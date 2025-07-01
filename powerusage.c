/*
 * powerusage - Utilitas untuk memantau penggunaan daya CPU dan GPU.
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
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pci/pci.h>
#include <signal.h>
#include <glob.h>
#include <limits.h>
#include <syslog.h>

#ifdef NVIDIA_GPU
#include <nvml.h>
#endif

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define MAX_NAME_LENGTH 300
#define USEC 1000000
#define KILO 1000
#define TO_GB (1024.0 * 1024.0)
#define VRAM_REGISTER_OFFSET 0x0000E2A8
#define HOTSPOT_REGISTER_OFFSET 0x0002046c
#define NVIDIA_VRAM_TEMP_MASK 0x00000fff
#define NVIDIA_VRAM_TEMP_DIVISOR 32
#define NVIDIA_HOTSPOT_TEMP_SHIFT 8
#define NVIDIA_HOTSPOT_TEMP_MASK 0xff
#define NVIDIA_HOTSPOT_VALID_MAX 0x7f
#define PG_SZ sysconf(_SC_PAGE_SIZE)
#define MEM_PATH "/dev/mem"
#define MAX_DEVICES 1
#define DEBUG 0
#define BUFFER_SIZE 32

float get_memory_usage(void);
int64_t get_cpuConsumptionUJoules(void);
int64_t get_currentTimeUSec(void);
char *execute_command(const char *command);
void print_cpu_info(void);
void print_amd_gpu_info(void);

#ifdef NVIDIA_GPU
void print_nvidia_gpu_info(void);
#endif

int detect_gpu_type(void);

enum GpuType
{
    GPU_TYPE_UNINITIALIZED = -1,
    GPU_TYPE_NONE = 0,
    GPU_TYPE_AMD,
    GPU_TYPE_NVIDIA,
};

typedef struct
{
    unsigned long long total;
    unsigned long long idle;
} CpuTimes;

/**
 * @brief Menghitung penggunaan memori dalam GB.
 *
 * Fungsi ini membaca /proc/meminfo untuk mendapatkan total dan memori yang tersedia,
 * lalu menghitung memori yang digunakan dan mengembalikannya dalam gigabyte.
 *
 * @return float Penggunaan memori dalam GB, atau -1.0f jika terjadi kesalahan.
 */
float get_memory_usage(void)
{
    FILE *file = fopen("/proc/meminfo", "r");

    if (!file)
    {
        if (DEBUG)
            perror("fopen");

        return -1.0f;
    }

    char buffer[MAX_NAME_LENGTH];
    long total_memory = 0, available_memory = 0;

    while (fgets(buffer, sizeof(buffer), file))
    {
        if (sscanf(buffer, "MemTotal: %ld kB", &total_memory) == 1)
            continue;

        if (sscanf(buffer, "MemAvailable: %ld kB", &available_memory) == 1)
            break;
    }

    fclose(file);

    if (total_memory == 0 || available_memory == 0)
        return -1.0f;

    return (float)(total_memory - available_memory) / (1024.0f * 1024.0f);
}

/**
 * @brief Mengambil konsumsi energi CPU dalam microjoule.
 *
 * Fungsi ini membaca file RAPL untuk mendapatkan total energi yang dikonsumsi oleh CPU.
 *
 * @return int64_t Konsumsi energi dalam microjoule, atau -1 jika terjadi kesalahan.
 */
int64_t get_cpuConsumptionUJoules(void)
{
    int64_t consumption;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (!file || fscanf(file, "%ld", &consumption) != 1)
    {
        if (DEBUG)
            perror("Failed to read energy consumption!");

        if (file)
            fclose(file);

        return -1;
    }

    fclose(file);

    return consumption;
}

/**
 * @brief Mendapatkan waktu saat ini dalam mikrodetik.
 *
 * Fungsi ini menggunakan gettimeofday untuk mengambil waktu sistem saat ini dan
 * mengubahnya menjadi mikrodetik.
 *
 * @return int64_t Waktu saat ini dalam mikrodetik, atau -1 jika terjadi kesalahan.
 */
int64_t get_currentTimeUSec(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
    {
        if (DEBUG)
            perror("Failed to get current time!");

        return -1;
    }

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

/**
 * @brief Menjalankan perintah shell dan mengembalikan outputnya.
 *
 * Fungsi ini membuka sebuah proses dengan perintah yang diberikan, membaca baris pertama
 * dari output, dan mengembalikannya sebagai string yang dialokasikan secara dinamis.
 * Pemanggil bertanggung jawab untuk membebaskan memori.
 *
 * @param command Perintah yang akan dijalankan.
 * @return char* Output dari perintah, atau NULL jika terjadi kesalahan.
 */
char *execute_command(const char *command)
{
    FILE *fp = popen(command, "r");

    if (!fp)
    {
        if (DEBUG)
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
 * @brief Menemukan semua direktori hwmon dengan nama yang cocok.
 *
 * Fungsi ini memindai melalui /sys/class/hwmon untuk menemukan perangkat keras
 * monitor yang cocok dengan nama yang diberikan dan menyimpan path mereka.
 *
 * @param name Nama hwmon yang dicari (misalnya, "k10temp").
 * @param out_paths Array untuk menyimpan path yang ditemukan.
 * @param max_paths Jumlah maksimum path yang akan disimpan.
 * @return int Jumlah path yang ditemukan.
 */
static int find_all_hwmon_by_name(const char *name, char (*out_paths)[PATH_MAX], int max_paths)
{
    glob_t hwmon_paths;
    int count = 0;

    if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &hwmon_paths) != 0)
        return 0;

    for (size_t i = 0; i < hwmon_paths.gl_pathc && count < max_paths; i++)
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

            if (strcmp(buffer, name) == 0)
            {
                strncpy(out_paths[count], hwmon_paths.gl_pathv[i], PATH_MAX - 1);

                out_paths[count][PATH_MAX - 1] = '\0';

                count++;
            }
        }

        fclose(f);
    }

    globfree(&hwmon_paths);

    return count;
}

/**
 * @brief Membaca suhu dari file input suhu hwmon.
 *
 * @param hwmon_path Path ke direktori hwmon.
 * @param temp_file Nama file input suhu (misalnya, "temp1_input").
 * @return int Suhu dalam mili-derajat Celsius, atau -1 jika terjadi kesalahan.
 */
static int read_hwmon_temp(const char *hwmon_path, const char *temp_file)
{
    char full_path[PATH_MAX];
    int ret = snprintf(full_path, sizeof(full_path), "%s/%s", hwmon_path, temp_file);

    if (ret < 0 || (size_t)ret >= sizeof(full_path))
        return -1;

    FILE *f = fopen(full_path, "r");

    if (!f)
        return -1;

    int temp;

    if (fscanf(f, "%d", &temp) != 1)
        temp = -1;

    fclose(f);

    return temp;
}

/**
 * @brief Mengambil waktu CPU total dan idle dari /proc/stat.
 *
 * @param times Pointer ke struktur CpuTimes untuk menyimpan hasilnya.
 * @return bool true jika berhasil, false jika gagal.
 */
static bool get_cpu_times(CpuTimes *times)
{
    FILE *f = fopen("/proc/stat", "r");

    if (!f)
    {
        if (DEBUG)
            perror("fopen /proc/stat");

        return false;
    }

    char buffer[256];

    if (!fgets(buffer, sizeof(buffer), f))
    {
        fclose(f);

        return false;
    }

    fclose(f);

    unsigned long long user, nice, system, idle, iowait, irq, softirq, steal = 0;
    int ret = sscanf(buffer, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                     &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);

    if (ret < 4)
        return false;

    times->idle = idle + iowait;
    times->total = user + nice + system + times->idle + irq + softirq + steal;

    return true;
}

/**
 * @brief Mencetak informasi penggunaan CPU, suhu, dan penggunaan memori.
 *
 * Fungsi ini mengukur penggunaan daya CPU, penggunaan CPU, dan suhu
 * selama interval satu detik dan mencetaknya ke output standar.
 */
void print_cpu_info(void)
{
    int64_t initial_energy_uj = get_cpuConsumptionUJoules();
    int64_t initial_time_us = get_currentTimeUSec();
    CpuTimes initial_cpu_times;
    bool initial_times_ok = get_cpu_times(&initial_cpu_times);

    if (initial_energy_uj == -1 || initial_time_us == -1 || !initial_times_ok)
    {
        if (DEBUG)
            fprintf(stderr, "Failed to get initial CPU stats.\n");

        return;
    }

    sleep(1);

    int64_t final_energy_uj = get_cpuConsumptionUJoules();
    int64_t final_time_us = get_currentTimeUSec();
    CpuTimes final_cpu_times;
    bool final_times_ok = get_cpu_times(&final_cpu_times);

    if (final_energy_uj == -1 || final_time_us == -1 || !final_times_ok)
    {
        if (DEBUG)
            fprintf(stderr, "Failed to get final CPU stats.\n");

        return;
    }

    float cpu_power = 0.0f;
    int64_t energy_delta_uj = final_energy_uj - initial_energy_uj;
    int64_t time_delta_us = final_time_us - initial_time_us;

    if (energy_delta_uj >= 0 && time_delta_us > 0)
        cpu_power = (float)energy_delta_uj / (float)time_delta_us;

    float cpu_usage = 0.0f;
    unsigned long long total_diff = final_cpu_times.total - initial_cpu_times.total;
    unsigned long long idle_diff = final_cpu_times.idle - initial_cpu_times.idle;

    if (total_diff > 0)
        cpu_usage = 100.0f * (float)(total_diff - idle_diff) / (float)total_diff;

    float used_memory_gb = get_memory_usage();
    int cpu_temperature1 = -1;
    int dram_temperature1 = -1;
    int dram_temperature2 = -1;
    char cpu_hwmon_paths[1][PATH_MAX];
    char dram_paths[2][PATH_MAX];

    if (find_all_hwmon_by_name("k10temp", cpu_hwmon_paths, 1) > 0)
        cpu_temperature1 = read_hwmon_temp(cpu_hwmon_paths[0], "temp1_input");
    else if (find_all_hwmon_by_name("coretemp", cpu_hwmon_paths, 1) > 0)
        cpu_temperature1 = read_hwmon_temp(cpu_hwmon_paths[0], "temp1_input");

    int num_dram_sensors = find_all_hwmon_by_name("spd5118", dram_paths, 2);

    if (num_dram_sensors > 0)
        dram_temperature1 = read_hwmon_temp(dram_paths[0], "temp1_input");

    if (num_dram_sensors > 1)
        dram_temperature2 = read_hwmon_temp(dram_paths[1], "temp1_input");

    if (used_memory_gb >= 0)
        printf("󰻠   %.0f %% |    %.1f GB |    %d °C |    %d °C |    %d °C | 󰚥 %.0f W\n",
               cpu_usage, used_memory_gb,
               cpu_temperature1 != -1 ? cpu_temperature1 / 1000 : 0,
               dram_temperature1 != -1 ? dram_temperature1 / 1000 : 0,
               dram_temperature2 != -1 ? dram_temperature2 / 1000 : 0,
               cpu_power);
}

/**
 * @brief Mencetak informasi penggunaan GPU AMD, suhu, dan penggunaan daya.
 *
 * Fungsi ini menggunakan rocm-smi untuk mengambil dan mencetak berbagai metrik
 * untuk GPU AMD.
 */
void print_amd_gpu_info(void)
{
    char *gpu_usage = execute_command("rocm-smi -d 0 --showuse | awk '/GPU use \\(%\\)/ {print $NF}'");
    char *gpu_vram_usage = execute_command("rocm-smi -d 0 --showmemuse | awk '/GPU Memory Allocated \\(VRAM%\\)/ {print $NF}'");
    char *gpu_temperature1 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor edge\\) \\(C\\):/ {print $NF}'");
    char *gpu_temperature2 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor junction\\) \\(C\\):/ {print $NF}'");
    char *gpu_temperature3 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor memory\\) \\(C\\):/ {print $NF}'");
    char *gpu_power = execute_command("rocm-smi -P | awk '/Average Graphics Package Power \\(W\\):/ {print $NF}'");

    if (gpu_temperature1 && gpu_usage && gpu_vram_usage)
    {
        printf("󰻠   %.0f %% | 󰻠   %.0f %% |    %.0f °C |    %.0f °C |    %.0f °C | 󰚥 %.0f W\n",
               atof(gpu_usage),
               atof(gpu_vram_usage),
               atof(gpu_temperature1),
               gpu_temperature2 ? atof(gpu_temperature2) : 0.0f,
               gpu_temperature3 ? atof(gpu_temperature3) : 0.0f,
               gpu_power ? atof(gpu_power) : 0.0f);
    }

    if (gpu_usage)
        free(gpu_usage);

    if (gpu_vram_usage)
        free(gpu_vram_usage);

    if (gpu_temperature1)
        free(gpu_temperature1);

    if (gpu_temperature2)
        free(gpu_temperature2);

    if (gpu_temperature3)
        free(gpu_temperature3);

    if (gpu_power)
        free(gpu_power);
}

#ifdef NVIDIA_GPU
/**
 * @brief Mencetak informasi penggunaan GPU NVIDIA, suhu, dan penggunaan daya.
 *
 * Fungsi ini menggunakan NVML dan akses PCI langsung untuk mengambil dan mencetak
 * berbagai metrik untuk GPU NVIDIA, termasuk suhu VRAM dan hotspot.
 */
void print_nvidia_gpu_info(void)
{
    nvmlReturn_t result;
    struct pci_access *pacc = NULL;
    unsigned int device_count = 0;
    result = nvmlInit();

    if (result != NVML_SUCCESS)
    {
        if (DEBUG)
            fprintf(stderr, "Failed to initialize NVML: %s\n", nvmlErrorString(result));

        return;
    }

    result = nvmlDeviceGetCount(&device_count);

    if (result != NVML_SUCCESS || device_count == 0)
    {
        if (DEBUG && result != NVML_SUCCESS)
            fprintf(stderr, "Failed to get device count: %s\n", nvmlErrorString(result));

        goto cleanup_nvml;
    }

    pacc = pci_alloc();

    if (!pacc)
    {
        if (DEBUG)
            fprintf(stderr, "Failed to allocate pci_access\n");

        goto cleanup_nvml;
    }

    pci_init(pacc);
    pci_scan_bus(pacc);

    for (unsigned int i = 0; i < device_count && i < MAX_DEVICES; i++)
    {
        nvmlDevice_t device;

        if (nvmlDeviceGetHandleByIndex(i, &device) != NVML_SUCCESS)
            continue;

        nvmlPciInfo_t pciInfo;

        if (nvmlDeviceGetPciInfo(device, &pciInfo) != NVML_SUCCESS)
            continue;

        unsigned int gpu_util = 0, gpu_temp = 0, power_usage = 0, vram_temp = 0, hotspot_temp = 0;
        float fb_used = 0.0f;

        nvmlUtilization_t util;

        if (nvmlDeviceGetUtilizationRates(device, &util) == NVML_SUCCESS)
            gpu_util = util.gpu;

        nvmlMemory_t mem;

        if (nvmlDeviceGetMemoryInfo(device, &mem) == NVML_SUCCESS)
            fb_used = (float)mem.used / (1024.0f * 1024.0f * 1024.0f);

        unsigned int temp;

        if (nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS)
            gpu_temp = temp;

        unsigned int power;

        if (nvmlDeviceGetPowerUsage(device, &power) == NVML_SUCCESS)
            power_usage = power / 1000;

        for (struct pci_dev *dev = pacc->devices; dev; dev = dev->next)
        {
            pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);

            if ((unsigned int)((dev->device_id << 16) | dev->vendor_id) != pciInfo.pciDeviceId ||
                (unsigned int)dev->domain != pciInfo.domain ||
                dev->bus != pciInfo.bus ||
                dev->dev != pciInfo.device)
            {
                continue;
            }

            int nvidia_fd = open(MEM_PATH, O_RDWR | O_SYNC);

            if (nvidia_fd < 0)
                break;

            uint32_t vram_addr = (dev->base_addr[0] & 0xFFFFFFFF) + VRAM_REGISTER_OFFSET;
            void *nvidia_map_base = mmap(NULL, PG_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, nvidia_fd, vram_addr & ~(PG_SZ - 1));

            if (nvidia_map_base != MAP_FAILED)
            {
                uint32_t *vram_reg = (uint32_t *)((char *)nvidia_map_base + (vram_addr & (PG_SZ - 1)));
                vram_temp = (*vram_reg & NVIDIA_VRAM_TEMP_MASK) / NVIDIA_VRAM_TEMP_DIVISOR;

                munmap(nvidia_map_base, PG_SZ);
            }

            uint32_t hotspot_addr = (dev->base_addr[0] & 0xFFFFFFFF) + HOTSPOT_REGISTER_OFFSET;
            void *hotspot_base = mmap(NULL, PG_SZ, PROT_READ, MAP_SHARED, nvidia_fd, hotspot_addr & ~(PG_SZ - 1));

            if (hotspot_base != MAP_FAILED)
            {
                uint32_t *hotspot_reg = (uint32_t *)((char *)hotspot_base + (hotspot_addr & (PG_SZ - 1)));
                uint32_t temp_hotspot = (*hotspot_reg >> NVIDIA_HOTSPOT_TEMP_SHIFT) & NVIDIA_HOTSPOT_TEMP_MASK;
                hotspot_temp = (temp_hotspot < NVIDIA_HOTSPOT_VALID_MAX) ? temp_hotspot : 0;

                munmap(hotspot_base, PG_SZ);
            }

            close(nvidia_fd);

            break;
        }

        printf("󰻠   %u %% |    %.1f GB |    %u °C |    %u °C |    %u °C | 󰚥 %u W\n",
               gpu_util, fb_used, gpu_temp, hotspot_temp, vram_temp, power_usage);

        break;
    }

    pci_cleanup(pacc);
cleanup_nvml:
    nvmlShutdown();
}
#endif

/**
 * @brief Mendeteksi jenis GPU yang ada di sistem (AMD, NVIDIA, atau tidak ada).
 *
 * Fungsi ini mencoba mendeteksi GPU AMD menggunakan rocm-smi dan GPU NVIDIA
 * menggunakan nvidia-smi. Hasilnya di-cache untuk panggilan berikutnya.
 *
 * @return int Enum GpuType yang menunjukkan jenis GPU yang terdeteksi.
 */
int detect_gpu_type(void)
{
    static enum GpuType cached_gpu_type = GPU_TYPE_UNINITIALIZED;

    if (cached_gpu_type != GPU_TYPE_UNINITIALIZED)
        return cached_gpu_type;

    char *amd_check = execute_command("rocm-smi --showid 2>/dev/null | grep -i 'GPU' >/dev/null && echo 'AMD'");

    if (amd_check)
    {
        if (strstr(amd_check, "AMD"))
        {
            free(amd_check);
            cached_gpu_type = GPU_TYPE_AMD;

            return GPU_TYPE_AMD;
        }

        free(amd_check);
    }

#ifdef NVIDIA_GPU
    char *nvidia_check = execute_command("nvidia-smi >/dev/null 2>&1 && echo 'NVIDIA'");

    if (nvidia_check)
    {
        if (strstr(nvidia_check, "NVIDIA"))
        {
            free(nvidia_check);
            cached_gpu_type = GPU_TYPE_NVIDIA;

            return GPU_TYPE_NVIDIA;
        }

        free(nvidia_check);
    }
#endif

    cached_gpu_type = GPU_TYPE_NONE;

    return GPU_TYPE_NONE;
}

/**
 * @brief Titik masuk utama program.
 *
 * Menganalisis argumen baris perintah untuk menentukan apakah akan menampilkan
 * informasi CPU atau GPU.
 *
 * @param argc Jumlah argumen baris perintah.
 * @param argv Array argumen baris perintah.
 * @return int 0 jika berhasil, 1 jika terjadi kesalahan.
 */
int main(int argc, char *argv[])
{
    if (argc < 2 || (strcmp(argv[1], "cpu") && strcmp(argv[1], "gpu")))
    {
        fprintf(stderr, "Syntax: %s [cpu|gpu], Example: powerusage cpu\n", argv[0]);
        return 1;
    }

    if (!strcmp(argv[1], "cpu"))
    {
        print_cpu_info();
    }
    else if (!strcmp(argv[1], "gpu"))
    {
        int gpu_type = detect_gpu_type();

        switch (gpu_type)
        {
        case GPU_TYPE_AMD:
            print_amd_gpu_info();

            break;
#ifdef NVIDIA_GPU
        case GPU_TYPE_NVIDIA:
            print_nvidia_gpu_info();

            break;
#endif
        default:
            fprintf(stderr, "No compatible GPU found!\n");
            return 1;
        }
    }

    return 0;
}
