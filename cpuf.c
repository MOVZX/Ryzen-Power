/*
 * cpuf - Utilitas untuk memantau frekuensi, suhu, dan daya CPU.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>
#include <glob.h>
#include <limits.h>
#include <fcntl.h>
#include <libgen.h>

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define NUM_CPUS 16
#define BUFFER_SIZE 256
#define USEC 1000000

#define BOLD "\033[1m"
#define RESET "\033[0m"

/**
 * @brief Mendapatkan konsumsi energi CPU saat ini dalam mikrojoule.
 *
 * @return int64_t Konsumsi energi dalam mikrojoule, atau -1 jika gagal.
 */
int64_t get_cpu_consumption_ujoules()
{
    int64_t consumption = -1;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (file == NULL)
    {
        perror("Error opening RAPL energy file");

        return -1;
    }

    if (fscanf(file, "%ld", &consumption) != 1)
    {
        perror("Error reading energy consumption");

        consumption = -1;
    }

    fclose(file);

    return consumption;
}

/**
 * @brief Mengembalikan waktu saat ini dalam mikrodetik.
 *
 * @return int64_t Waktu saat ini dalam mikrodetik, atau -1 jika gagal.
 */
int64_t get_current_time_usec()
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("Error getting current time");

        return -1;
    }

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

/**
 * @brief Menghitung daya CPU rata-rata dalam Watt selama interval 1 detik.
 *
 * @return float Daya CPU dalam Watt, atau -1.0f jika gagal.
 */
float calculate_cpu_power()
{
    int64_t initial_usage = get_cpu_consumption_ujoules();
    int64_t initial_time = get_current_time_usec();

    if (initial_usage == -1 || initial_time == -1)
    {
        fprintf(stderr, "Failed to read initial CPU consumption or time data\n");

        return -1.0f;
    }

    sleep(1);

    int64_t final_usage = get_cpu_consumption_ujoules();
    int64_t final_time = get_current_time_usec();

    if (final_usage == -1 || final_time == -1)
    {
        fprintf(stderr, "Failed to read final CPU consumption or time data\n");

        return -1.0f;
    }

    if (final_time <= initial_time)
    {
        fprintf(stderr, "Time did not advance or went backwards!\n");

        return -1.0f;
    }

    int64_t energy_diff_uj = final_usage - initial_usage;
    int64_t time_diff_usec = final_time - initial_time;

    if (energy_diff_uj < 0)
    {
        fprintf(stderr, "Energy consumption decreased, which is not possible.\n");

        return -1.0f;
    }

    return (float)energy_diff_uj / (float)time_diff_usec;
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

    if (file == NULL)
        return -1;

    if (fscanf(file, "%d", &value) != 1)
        value = -1;

    fclose(file);

    return value;
}

/**
 * @brief Mendapatkan suhu CPU (Tctl dan Tccd).
 *
 * @param cpu_tctl Pointer untuk menyimpan suhu Tctl.
 * @param cpu_tccd Pointer untuk menyimpan suhu Tccd.
 * @return int 0 jika berhasil, -1 jika gagal.
 */
int get_cpu_temperatures(int *cpu_tctl, int *cpu_tccd)
{
    char hwmon_path[BUFFER_SIZE];
    char temp_path[BUFFER_SIZE];
    int found = 0;

    glob_t glob_result;

    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &glob_result) == 0)
    {
        for (size_t i = 0; i < glob_result.gl_pathc; i++)
        {
            FILE *name_file = fopen(glob_result.gl_pathv[i], "r");

            if (name_file)
            {
                char name[32];

                if (fgets(name, sizeof(name), name_file))
                {
                    name[strcspn(name, "\n")] = 0;

                    if (strcmp(name, "k10temp") == 0)
                    {
                        char *dir = dirname(glob_result.gl_pathv[i]);

                        strncpy(hwmon_path, dir, sizeof(hwmon_path) - 1);

                        hwmon_path[sizeof(hwmon_path) - 1] = '\0';
                        found = 1;
                    }
                }

                fclose(name_file);

                if (found)
                    break;
            }
        }
    }

    globfree(&glob_result);

    if (!found)
    {
        fprintf(stderr, "k10temp sensor module not found!\n");

        return -1;
    }

    snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", hwmon_path);

    *cpu_tctl = read_int_from_file(temp_path);

    snprintf(temp_path, sizeof(temp_path), "%s/temp3_input", hwmon_path);

    *cpu_tccd = read_int_from_file(temp_path);

    if (*cpu_tctl == -1 || *cpu_tccd == -1)
    {
        fprintf(stderr, "Failed to read Tctl/Tccd temperatures.\n");

        return -1;
    }

    return 0;
}

/**
 * @brief Mendapatkan frekuensi saat ini untuk setiap inti CPU.
 *
 * @param freqs Array untuk menyimpan frekuensi setiap inti dalam MHz.
 */
void get_cpu_frequencies(int *freqs)
{
    for (int i = 0; i < NUM_CPUS; i++)
    {
        char freq_path[BUFFER_SIZE];

        snprintf(freq_path, sizeof(freq_path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);

        int freq_khz = read_int_from_file(freq_path);
        freqs[i] = (freq_khz != -1) ? freq_khz / 1000 : 0;
    }
}

/**
 * @brief Mencetak informasi CPU yang diformat.
 *
 * @param cpu_tctl Suhu CPU Tctl.
 * @param cpu_tccd Suhu CPU Tccd.
 * @param cpu_power Daya CPU dalam Watt.
 * @param cpu_freqs Array frekuensi inti CPU.
 */
void print_cpu_info(int cpu_tctl, int cpu_tccd, float cpu_power, const int *cpu_freqs)
{
    printf(BOLD "Ryzen 7 7800X3D" RESET "\n\n");
    printf("Tctl    : %8d°C\n", cpu_tctl / 1000);
    printf("Tccd    : %8d°C\n", cpu_tccd / 1000);
    printf("Power   : %8.2f W\n", cpu_power);
    printf("\n");

    for (int i = 0; i < NUM_CPUS; i++)
    {
        printf("CPU %2d  : %6d MHz\n", i, cpu_freqs[i]);
    }
}

/**
 * @brief Titik masuk utama untuk program.
 *
 * @return int 0 jika berhasil, 1 jika gagal.
 */
int main()
{
    int cpu_tctl = -1, cpu_tccd = -1;
    int cpu_freqs[NUM_CPUS] = {0};

    if (get_cpu_temperatures(&cpu_tctl, &cpu_tccd) != 0)
        return 1;

    float cpu_power = calculate_cpu_power();

    if (cpu_power < 0)
        return 1;

    get_cpu_frequencies(cpu_freqs);
    print_cpu_info(cpu_tctl, cpu_tccd, cpu_power, cpu_freqs);

    return 0;
}
