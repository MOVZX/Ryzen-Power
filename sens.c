/*
 * sens - Utilitas untuk memantau sensor perangkat keras.
 *
 * Hak Cipta (C) 2024 MOVZX
 *
 * Program ini adalah perangkat lunak bebas; Anda dapat menyebarluaskannya kembali
 * dan/atau memodifikasinya di bawah ketentuan Lisensi Publik Umum GNU
 * sebagaimana dipublikasikan oleh Free Software Foundation; baik versi 2
 * dari Lisensi, atau (sesuai pilihan Anda) versi yang lebih baru.
 *
 * Program ini didistribusikan dengan harapan akan bermanfaat,
 * tetapi TANPA JAMINAN APAPUN; bahkan tanpa jaminan tersirat
 * DAGANGAN atau KESESUAIAN UNTUK TUJUAN TERTENTU. Lihat
 * Lisensi Publik Umum GNU untuk lebih jelasnya.
 *
 * Anda seharusnya telah menerima salinan Lisensi Publik Umum GNU
 * bersama dengan program ini; jika tidak, tulislah ke Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <libgen.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <unistd.h>
#include <dirent.h>

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define BUFFER_SIZE 32
#define USEC 1000000
#define KILO 1000

#define BOLD "\033[1m"
#define RESET "\033[0m"

enum GpuType
{
    GPU_TYPE_UNINITIALIZED = -1,
    GPU_TYPE_NONE = 0,
    GPU_TYPE_AMD,
    GPU_TYPE_NVIDIA,
};

/**
 * @brief Menjalankan perintah shell dan mengembalikan outputnya.
 *
 * @param command Perintah shell yang akan dieksekusi.
 * @return char* Baris pertama dari output perintah, atau NULL jika gagal.
 *
 * @note Memori yang dikembalikan harus dibebaskan oleh pemanggil.
 *       Fungsi ini merupakan bottleneck kinerja karena forking proses.
 */
char *execute_command(const char *command)
{
    FILE *fp = popen(command, "r");

    if (!fp)
    {
        perror("popen");

        return NULL;
    }

    char *output = malloc(4096);

    if (!output)
    {
        pclose(fp);

        return NULL;
    }

    if (fgets(output, 4096, fp) == NULL)
    {
        free(output);

        output = NULL;
    }
    else
    {
        output[strcspn(output, "\n")] = 0;
    }

    pclose(fp);

    return output;
}

/**
 * @brief Mendapatkan konsumsi energi CPU saat ini dalam mikrojoule.
 *
 * @return int64_t Konsumsi energi dalam mikrojoule, atau -1 jika gagal.
 *
 * @note Membutuhkan antarmuka RAPL di RAPL_FILE_PATH.
 */
int64_t get_cpuConsumptionUJoules()
{
    int64_t consumption;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (!file || fscanf(file, "%ld", &consumption) != 1)
    {
        perror("Failed to read CPU energy consumption");

        if (file)
            fclose(file);

        return -1;
    }

    fclose(file);

    return consumption;
}

/**
 * @brief Mengembalikan waktu saat ini dalam mikrodetik.
 *
 * @return int64_t Waktu saat ini dalam mikrodetik, atau -1 jika gagal.
 */
int64_t get_currentTimeUSec()
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("Failed to get current time");

        return -1;
    }

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

/**
 * @brief Menghitung daya CPU rata-rata dalam Watt selama interval 1 detik.
 *
 * @return float Daya CPU dalam Watt, atau 0.0f jika gagal.
 */
float calculate_cpu_power()
{
    int64_t initial_energy_uj = get_cpuConsumptionUJoules();
    int64_t initial_time_us = get_currentTimeUSec();

    if (initial_energy_uj == -1 || initial_time_us == -1)
    {
        fprintf(stderr, "Failed to read initial CPU consumption or time data\n");

        return 0.0f;
    }

    usleep(USEC);

    int64_t final_energy_uj = get_cpuConsumptionUJoules();
    int64_t final_time_us = get_currentTimeUSec();

    if (final_energy_uj == -1 || final_time_us == -1)
    {
        fprintf(stderr, "Failed to read final CPU consumption or time data\n");

        return 0.0f;
    }

    int64_t energy_delta_uj = final_energy_uj - initial_energy_uj;
    int64_t time_delta_us = final_time_us - initial_time_us;

    if (time_delta_us <= 0 || energy_delta_uj < 0)
    {
        fprintf(stderr, "Invalid time or energy difference (time: %ld us, energy: %ld uJ)\n", time_delta_us, energy_delta_uj);

        return 0.0f;
    }

    return (float)energy_delta_uj / (float)time_delta_us;
}

/**
 * @brief Membaca nilai integer dari file yang ditentukan.
 *
 * @param path Path ke file.
 * @return int Nilai integer yang dibaca dari file, atau -1 jika gagal.
 */
int read_int_from_file(const char *path)
{
    int value = -1;
    FILE *file = fopen(path, "r");

    if (!file)
        return -1;

    if (fscanf(file, "%d", &value) != 1)
        value = -1;

    fclose(file);

    return value;
}

/**
 * @brief Menemukan path hwmon berdasarkan nama.
 *
 * @param name Nama hwmon yang dicari.
 * @param out_path Buffer untuk menyimpan path yang ditemukan.
 * @param out_path_size Ukuran buffer out_path.
 * @return int 0 jika berhasil, -1 jika gagal.
 */
static int find_hwmon_path_by_name(const char *name, char *out_path, size_t out_path_size)
{
    glob_t hwmon_paths;
    int found = -1;
    int max_fan_count = -1;
    char best_path[PATH_MAX] = {0};

    if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &hwmon_paths) != 0)
        return -1;

    for (size_t i = 0; i < hwmon_paths.gl_pathc; i++)
    {
        char name_path[PATH_MAX];

        snprintf(name_path, sizeof(name_path), "%s/name", hwmon_paths.gl_pathv[i]);

        FILE *f = fopen(name_path, "r");

        if (!f)
            continue;

        char buffer[BUFFER_SIZE];

        if (fgets(buffer, sizeof(buffer), f))
        {
            buffer[strcspn(buffer, "\n")] = 0;

            if (strncmp(buffer, name, strlen(name)) == 0)
            {
                // Count fan inputs for this hwmon device
                int fan_count = 0;
                for (int fan_num = 1; fan_num <= 10; fan_num++)
                {
                    char fan_path[PATH_MAX];
                    snprintf(fan_path, sizeof(fan_path), "%s/fan%d_input", hwmon_paths.gl_pathv[i], fan_num);
                    if (access(fan_path, F_OK) == 0)
                        fan_count++;
                }

                // For nct6799, prefer the device with more fan inputs
                if (strcmp(buffer, name) == 0 && fan_count > max_fan_count)
                {
                    strncpy(best_path, hwmon_paths.gl_pathv[i], sizeof(best_path) - 1);
                    best_path[sizeof(best_path) - 1] = '\0';
                    max_fan_count = fan_count;
                    found = 0;
                }
                // For other devices, use the first match
                else if (strcmp(buffer, name) == 0 && found == -1)
                {
                    strncpy(best_path, hwmon_paths.gl_pathv[i], sizeof(best_path) - 1);
                    best_path[sizeof(best_path) - 1] = '\0';
                    max_fan_count = fan_count;
                    found = 0;
                }
            }
        }

        fclose(f);
    }

    if (found == 0)
    {
        strncpy(out_path, best_path, out_path_size - 1);
        out_path[out_path_size - 1] = '\0';
    }

    globfree(&hwmon_paths);

    return found;
}

/**
 * @brief Mendeteksi jenis GPU yang ada di sistem (AMD, NVIDIA, atau tidak ada).
 *
 * @return enum GpuType Jenis GPU yang terdeteksi.
 */
static enum GpuType detect_gpu_type(void)
{
    static enum GpuType cached_gpu_type = GPU_TYPE_UNINITIALIZED;

    if (cached_gpu_type != GPU_TYPE_UNINITIALIZED)
        return cached_gpu_type;

    char hwmon_path[PATH_MAX];

    if (find_hwmon_path_by_name("amdgpu", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        cached_gpu_type = GPU_TYPE_AMD;

        return cached_gpu_type;
    }

#ifdef NVIDIA_GPU
    FILE *fp = popen("nvidia-smi -L >/dev/null 2>&1 && echo 'NVIDIA'", "r");

    if (fp)
    {
        char buffer[32];

        if (fgets(buffer, sizeof(buffer), fp) != NULL && strstr(buffer, "NVIDIA"))
            cached_gpu_type = GPU_TYPE_NVIDIA;

        pclose(fp);

        if (cached_gpu_type == GPU_TYPE_NVIDIA)
            return cached_gpu_type;
    }
#endif

    cached_gpu_type = GPU_TYPE_NONE;

    return cached_gpu_type;
}

#ifdef NVIDIA_GPU
/**
 * @brief Membaca suhu dan daya GPU NVIDIA menggunakan nvidia-smi.
 *
 * @param temperature Pointer ke float untuk menyimpan suhu GPU.
 * @param power Pointer ke float untuk menyimpan penarikan daya GPU.
 *
 * @note Fungsi ini tidak efisien karena memanggil `nvidia-smi` melalui `popen`.
 *       Untuk penggunaan produksi, pustaka NVML direkomendasikan.
 */
void read_nvidia_gpu_info(float *temperature, float *power)
{
    char command[BUFFER_SIZE] = "nvidia-smi --query-gpu=temperature.gpu,power.draw --format=csv,noheader";
    FILE *fp = popen(command, "r");

    if (fp)
    {
        char line[BUFFER_SIZE];

        if (fgets(line, sizeof(line), fp))
            sscanf(line, "%f, %f", temperature, power);

        pclose(fp);
    }
    else
    {
        *temperature = -1;
        *power = -1;
    }
}
#endif

/**
 * @brief Mencetak informasi sistem (produsen dan produk).
 */
static void print_system_info(void)
{
    char *board_manufacturer = execute_command("dmidecode -s system-manufacturer");
    char *board_product = execute_command("dmidecode -s system-product-name");

    printf(BOLD "%s %s" RESET "\n", board_manufacturer ? board_manufacturer : "System", board_product ? board_product : "");

    free(board_manufacturer);
    free(board_product);
}

/**
 * @brief Mencetak suhu motherboard dan kecepatan kipas.
 */
static void print_motherboard_and_fan_info(void)
{
    char hwmon_path[PATH_MAX], temp_path[PATH_MAX];
    int mobo_temp = -1, vrm_temp = -1, pch_temp = -1;
    int radiator_fan = -1, pump_fan = -1, top_fans = -1, bottom1_fans = -1, bottom2_fans = -1;

    if (find_hwmon_path_by_name("nct6799", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        snprintf(temp_path, sizeof(temp_path), "%s/temp2_input", hwmon_path);

        mobo_temp = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/temp3_input", hwmon_path);

        vrm_temp = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/temp4_input", hwmon_path);

        pch_temp = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan1_input", hwmon_path);

        radiator_fan = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan2_input", hwmon_path);

        pump_fan = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan3_input", hwmon_path);

        top_fans = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan4_input", hwmon_path);

        bottom1_fans = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan5_input", hwmon_path);

        bottom2_fans = read_int_from_file(temp_path);
    }
    else
    {
        fprintf(stderr, "NCT679x sensor module not found!\n");
    }

    printf("Mobo     : %.2f°C\n", mobo_temp != -1 ? mobo_temp / 1000.0 : 0.0);
    printf("VRM      : %.2f°C\n", vrm_temp != -1 ? vrm_temp / 1000.0 : 0.0);
    printf("Chipset  : %.2f°C\n", pch_temp != -1 ? pch_temp / 1000.0 : 0.0);
    printf("\n");

    printf(BOLD "Lian Li Lancool II" RESET "\n");
    printf("Radiator : %d RPM\n", radiator_fan != -1 ? radiator_fan : 0);
    printf("Pump     : %d RPM\n", pump_fan != -1 ? pump_fan : 0);
    printf("Top      : %d RPM\n", top_fans != -1 ? top_fans : 0);
    printf("Bottom 1 : %d RPM\n", bottom1_fans != -1 ? bottom1_fans : 0);
    printf("Bottom 2 : %d RPM\n", bottom2_fans != -1 ? bottom2_fans : 0);
    printf("\n");
}

/**
 * @brief Mencetak suhu dan daya CPU.
 */
static void print_cpu_info(void)
{
    char hwmon_path[PATH_MAX], temp_path[PATH_MAX];
    int cpu_tctl = -1, cpu_tccd = -1;
    float cpu_power = 0.0f;

    if (find_hwmon_path_by_name("k10temp", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", hwmon_path);

        cpu_tctl = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/temp3_input", hwmon_path);

        cpu_tccd = read_int_from_file(temp_path);
        cpu_power = calculate_cpu_power();
    }
    else
    {
        fprintf(stderr, "k10temp sensor module not found!\n");
    }

    char *processor_name = execute_command("dmidecode -s processor-version");

    printf(BOLD "%s" RESET "\n", processor_name ? processor_name : "CPU");
    printf("Tctl     : %.2f°C\n", cpu_tctl != -1 ? cpu_tctl / 1000.0 : 0.0);
    printf("Tccd     : %.2f°C\n", cpu_tccd != -1 ? cpu_tccd / 1000.0 : 0.0);
    printf("Power    : %.2f W\n", cpu_power);
    printf("\n");

    free(processor_name);
}

/**
 * @brief Mencetak suhu DRAM.
 */
static void print_dram_info(void)
{
    char temp_path[PATH_MAX];
    char *dram_model = execute_command("dmidecode -t memory | grep \"Part Number:\" | awk '{$1=\"\"; print $0}' | sed 's/^ *//' | head -n 1");

    printf(BOLD "%s" RESET "\n", dram_model ? dram_model : "DRAM");

    int dram_temps[2] = {-1, -1};
    glob_t spd_paths;

    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &spd_paths) == 0)
    {
        int dram_idx = 0;

        for (size_t i = 0; i < spd_paths.gl_pathc && dram_idx < 2; i++)
        {
            FILE *f = fopen(spd_paths.gl_pathv[i], "r");

            if (!f)
                continue;

            char name_buf[32];

            if (fgets(name_buf, sizeof(name_buf), f) && strncmp(name_buf, "spd5118", 7) == 0)
            {
                char *dir_path = dirname(spd_paths.gl_pathv[i]);

                snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", dir_path);

                dram_temps[dram_idx++] = read_int_from_file(temp_path);
            }

            fclose(f);
        }
    }

    globfree(&spd_paths);

    printf("DRAM 1   : %.2f°C\n", dram_temps[0] != -1 ? dram_temps[0] / 1000.0 : 0.0);
    printf("DRAM 2   : %.2f°C\n", dram_temps[1] != -1 ? dram_temps[1] / 1000.0 : 0.0);
    printf("\n");

    free(dram_model);
}

/**
 * @brief Mendeteksi dan mencetak informasi suhu dan daya GPU.
 */
static void print_gpu_info(void)
{
    enum GpuType gpu_type = detect_gpu_type();

    if (gpu_type == GPU_TYPE_AMD)
    {
        char hwmon_path[PATH_MAX], temp_path[PATH_MAX];
        float gpu_edge = 0.0f, gpu_junction = 0.0f, gpu_mem = 0.0f, gpu_power = 0.0f;

        if (find_hwmon_path_by_name("amdgpu", hwmon_path, sizeof(hwmon_path)) == 0)
        {
            snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", hwmon_path);

            gpu_edge = read_int_from_file(temp_path);

            snprintf(temp_path, sizeof(temp_path), "%s/temp2_input", hwmon_path);

            gpu_junction = read_int_from_file(temp_path);

            snprintf(temp_path, sizeof(temp_path), "%s/temp3_input", hwmon_path);

            gpu_mem = read_int_from_file(temp_path);

            snprintf(temp_path, sizeof(temp_path), "%s/power1_average", hwmon_path);

            gpu_power = read_int_from_file(temp_path);

            printf(BOLD "AMD Radeon GPU" RESET "\n");
            printf("Edge     : %.2f°C\n", gpu_edge != -1 ? gpu_edge / 1000.0 : 0.0);
            printf("Junction : %.2f°C\n", gpu_junction != -1 ? gpu_junction / 1000.0 : 0.0);
            printf("Mem      : %.2f°C\n", gpu_mem != -1 ? gpu_mem / 1000.0 : 0.0);
            printf("Power    : %.2f W\n", gpu_power != -1 ? gpu_power / 1000000.0 : 0.0);
            printf("\n");
        }
    }
#ifdef NVIDIA_GPU
    else if (gpu_type == GPU_TYPE_NVIDIA)
    {
        float gpu_temp_nvidia = -1.0f, gpu_power_nvidia = -1.0f;

        read_nvidia_gpu_info(&gpu_temp_nvidia, &gpu_power_nvidia);

        if (gpu_temp_nvidia > 0 && gpu_power_nvidia > 0)
        {
            char *nvidia_gpu_name = execute_command("nvidia-smi --query-gpu=gpu_name --format=csv,noheader");

            printf(BOLD "%s" RESET "\n", nvidia_gpu_name ? nvidia_gpu_name : "NVIDIA GPU");
            printf("Temp     : %.1f°C\n", gpu_temp_nvidia);
            printf("Power    : %.1f W\n", gpu_power_nvidia);
            printf("\n");

            free(nvidia_gpu_name);
        }
    }
#endif
}

/**
 * @brief Mencetak suhu SSD NVMe.
 */
static void print_nvme_info(void)
{
    char temp_path[PATH_MAX];
    glob_t nvme_paths;

    if (glob("/sys/class/nvme/nvme*", 0, NULL, &nvme_paths) == 0)
    {
        for (size_t i = 0; i < nvme_paths.gl_pathc; i++)
        {
            char model[128] = "NVMe SSD";
            char model_path[PATH_MAX];

            snprintf(model_path, sizeof(model_path), "%s/model", nvme_paths.gl_pathv[i]);

            FILE *f_model = fopen(model_path, "r");

            if (f_model)
            {
                if (fgets(model, sizeof(model), f_model))
                    model[strcspn(model, "\n")] = 0;

                fclose(f_model);
            }

            char nvme_hwmon_path[PATH_MAX];

            snprintf(nvme_hwmon_path, sizeof(nvme_hwmon_path), "%s/hwmon*", nvme_paths.gl_pathv[i]);

            glob_t nvme_hwmon_glob;

            if (glob(nvme_hwmon_path, 0, NULL, &nvme_hwmon_glob) == 0)
            {
                if (nvme_hwmon_glob.gl_pathc > 0)
                {
                    snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", nvme_hwmon_glob.gl_pathv[0]);

                    int nvme_temp = read_int_from_file(temp_path);

                    printf(BOLD "%s" RESET "\n", model);
                    printf("NAND     : %.2f°C\n", nvme_temp != -1 ? nvme_temp / 1000.0 : 0.0);
                    printf("\n");
                }

                globfree(&nvme_hwmon_glob);
            }
        }
    }

    globfree(&nvme_paths);
}

/**
 * @brief Titik masuk utama untuk program pembacaan sensor.
 *
 * @return int 0 jika berhasil.
 */
int main()
{
    print_system_info();
    print_motherboard_and_fan_info();
    print_cpu_info();
    print_dram_info();
    print_gpu_info();
    print_nvme_info();

    return 0;
}
