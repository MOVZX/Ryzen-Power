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
#include <stdbool.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <limits.h>

#ifdef NVIDIA_GPU
#include <sys/mman.h>
#include <pci/pci.h>
#include <nvml.h>
#endif

#include "sensors_common.h"

#define MAX_NAME_LENGTH 300
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
#define DEBUG_ENV_VAR "RYZEN_POWER_DEBUG"
#define CPU_STATS_PATH "/tmp/cpu_stats.txt"

/* Suhu DRAM (sensor spd5118) dimatikan secara default.
 * Build dengan dukungan DRAM:
 *   gcc -O3 -DENABLE_DRAM ...   atau   ENABLE_DRAM=1 ./build.sh
 */

float get_memory_usage(void);
void print_cpu_info(void);
void update_cpu_freqs(int *out_cur, int *out_max);
void print_amd_gpu_info(void);

#ifdef NVIDIA_GPU
void print_nvidia_gpu_info(void);
#endif

/**
 * @brief Cek apakah pesan diagnostik boleh dicetak ke stderr.
 *
 * Baris output utama dipakai panel, jadi kegagalan sengaja diam secara default.
 * Nyalakan dengan RYZEN_POWER_DEBUG=1 pada environment.
 *
 * @return bool true kalau variabel RYZEN_POWER_DEBUG terpasang.
 */
static bool dbg_enabled(void)
{
    static int cached = -1;

    if (cached < 0)
        cached = (getenv(DEBUG_ENV_VAR) != NULL) ? 1 : 0;

    return cached == 1;
}

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
        if (dbg_enabled())
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
 * @brief Mengambil frekuensi CPU (MHz) dari seluruh core: rata-rata, maksimum.
 *
 * Fungsi ini membaca scaling_cur_freq setiap core yang online (jalur "cpu", dengan
 * fallback "platform-cpufreq" untuk kernel 6.10+), lalu menghitung frekuensi
 * terendah, rata-rata (frekuensi saat ini) dan tertinggi di semua core.
 *
 * @param out_avg  Penyimpanan frekuensi rata-rata MHz (-1 jika tidak ada data).
 * @param out_max  Penyimpanan frekuensi maksimum MHz (-1 jika tidak ada data).
 */
static void get_cpu_freqs(int *out_avg, int *out_max)
{
    char path[PATH_MAX];
    int max_mhz = -1, sum_mhz = 0, count = 0;

    for (int cpu = 0;; cpu++)
    {
        int found = 0;

        /* dua possible jalur: "cpu" (umum) dan "platform-cpufreq" (kernel 6.10+) */
        for (int attempt = 0; attempt < 2 && !found; attempt++)
        {
            if (attempt == 0)
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
            else
                snprintf(path, sizeof(path),
                         "/sys/devices/system/cpu/platform-cpufreq/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);

            FILE *cf = fopen(path, "r");

            if (!cf)
                continue;

            long long khz = 0;

            if (fscanf(cf, "%lld", &khz) != 1)
                khz = 0;

            fclose(cf);

            if (khz <= 0)
                continue; /* core offline atau tidak ada data */

            int mhz = (int)(khz / 1000);

            if (mhz > max_mhz)
                max_mhz = mhz;

            sum_mhz += mhz;
            count++;
            found = 1;
        }

        if (found == 0)
        {
            /* core berikutnya belum tentu ada; berhenti kalau direktorinya pun tidak ada */
            snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq", cpu);

            struct stat st;

            if (stat(path, &st) != 0)
                break;
        }
    }

    *out_max = max_mhz;
    *out_avg = count > 0 ? (int)((sum_mhz + count / 2) / count) : -1;
}

/**
 * @brief Baca - gabung - simpan statistik frekuensi CPU dalam satu operasi terkunci.
 *
 * File: /tmp/cpu_stats.txt dengan format "KUNCI: angka" per baris
 * (AVG, MAX, CUR). MAX adalah frekuensi tertinggi yang pernah tercatat (hanya boleh
 * naik), AVG adalah rata-rata semua core pada sampling terakhir, CUR adalah frekuensi
 * core tertinggi pada sampling terakhir.
 *
 * Penguncian pakai flock() karena aplikasi ini berjalan sekali jalan (single shot)
 * dan bisa dipanggil bergantian (misalnya poller panel) - tanpa lock, dua penulis
 * bisa saling menimpa sehingga MAX terlihat turun.
 *
 * @param out_cur  Penyimpanan frekuensi core tertinggi saat ini (CUR) MHz.
 * @param out_max  Penyimpanan MAX gabungan MHz (riwayat, hanya boleh naik).
 */
void update_cpu_freqs(int *out_cur, int *out_max)
{
    int cur_avg = -1, cur_max = -1;

    get_cpu_freqs(&cur_avg, &cur_max);

    /* O_NOFOLLOW: jangan ikut symlink yang ditanam pengguna lain di /tmp. */
    int lock_fd = open(CPU_STATS_PATH, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);

    if (lock_fd < 0)
    {
        /* Tidak bisa membuka file: tampilkan hasil sampling saja. */
        *out_cur = cur_max;
        *out_max = cur_max;

        return;
    }

    flock(lock_fd, LOCK_EX);

    /* Baca ulang DI DALAM lock supaya nilai yang digabung benar-benar terakhir. */
    int old_avg = -1, old_max = -1, old_cur = -1;
    char line[64];
    char key[16];
    int value;
    FILE *f = fdopen(lock_fd, "r");

    if (f)
    {
        if (fseek(f, 0, SEEK_SET) == 0)
        {
            while (fgets(line, sizeof(line), f))
            {
                if (sscanf(line, "%15[a-zA-Z_]: %d", key, &value) != 2)
                    continue;

                if (strcmp(key, "AVG") == 0)
                    old_avg = value;
                else if (strcmp(key, "MAX") == 0)
                    old_max = value;
                else if (strcmp(key, "CUR") == 0)
                    old_cur = value;
            }
        }

    }

    /* MAX: hanya boleh naik. AVG: sampel terbaru. */

    int max_mhz = cur_max;

    if (max_mhz < 0)
        max_mhz = old_max;
    else if (old_max > max_mhz)
        max_mhz = old_max;

    int avg_mhz = cur_avg >= 0 ? cur_avg : old_avg;

    /* CUR: frekuensi core tertinggi pada sampel terakhir, tanpa riwayat. */
    int cur_mhz = cur_max >= 0 ? cur_max : old_cur;

    /* Hanya tulis kalau ada yang berubah: MAX baru lebih tinggi, atau AVG/CUR berubah.
     * Nilai lama tidak pernah ditimpa oleh angka yang
     * lebih buruk. Tulis lewat file sementara unik lalu rename (atomic). */
    int changed = (max_mhz != old_max) || (avg_mhz != old_avg) || (cur_mhz != old_cur);

    if (changed && max_mhz >= 0)
    {
        char tmp_path[] = "/tmp/cpu_stats.XXXXXX";
        int tmp_fd = mkstemp(tmp_path);

        if (tmp_fd >= 0)
        {
            FILE *tf = fdopen(tmp_fd, "w");

            if (tf)
            {
                fprintf(tf, "AVG: %d\n", avg_mhz);
                fprintf(tf, "MAX: %d\n", max_mhz);
                fprintf(tf, "CUR: %d\n", cur_mhz);
                fclose(tf);
                rename(tmp_path, CPU_STATS_PATH);
            }
            else
            {
                close(tmp_fd);
                unlink(tmp_path);
            }
        }
    }

    flock(lock_fd, LOCK_UN);

    if (!f)
        close(lock_fd);

    *out_cur = cur_mhz;
    *out_max = max_mhz;
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
        if (dbg_enabled())
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
    /* Energi RAPL dan waktu CPU diambil pada jendela satu detik yang sama. */
    int64_t initial_energy_uj = get_cpu_energy_uj();
    int64_t initial_time_us = get_time_usec();
    CpuTimes initial_cpu_times;

    if (initial_energy_uj < 0 || initial_time_us < 0 || !get_cpu_times(&initial_cpu_times))
    {
        if (dbg_enabled())
            fprintf(stderr, "Failed to get initial CPU stats.\n");

        return;
    }

    sleep(1);

    int64_t final_energy_uj = get_cpu_energy_uj();
    int64_t final_time_us = get_time_usec();
    CpuTimes final_cpu_times;

    if (final_energy_uj < 0 || final_time_us < 0 || !get_cpu_times(&final_cpu_times))
    {
        if (dbg_enabled())
            fprintf(stderr, "Failed to get final CPU stats.\n");

        return;
    }

    float cpu_power = cpu_power_from_delta(initial_energy_uj, initial_time_us,
                                          final_energy_uj, final_time_us);

    if (cpu_power < 0)
    {
        /* Kegagalan counter energi tidak boleh menghapus baris output panel. */
        if (dbg_enabled())
            fprintf(stderr, "Failed to compute CPU power (energy counter anomaly)\n");

        cpu_power = 0.0f;
    }

    float cpu_usage = 0.0f;
    unsigned long long total_diff = final_cpu_times.total - initial_cpu_times.total;
    unsigned long long idle_diff = final_cpu_times.idle - initial_cpu_times.idle;

    if (total_diff > 0)
        cpu_usage = 100.0f * (float)(total_diff - idle_diff) / (float)total_diff;

    float used_memory_gb = get_memory_usage();

    if (used_memory_gb < 0)
        used_memory_gb = 0.0f; /* panel tetap menerima satu baris lengkap */

    int freq_cur = -1, freq_max = -1;
    int cpu_temperature1 = -1;
    int pch_temperature1 = -1, pch_temperature2 = -1;
#ifdef ENABLE_DRAM
    int dram_temperature1 = -1;
    int dram_temperature2 = -1;
    char dram_paths[2][PATH_MAX];
#endif
    char cpu_hwmon_paths[1][PATH_MAX];

    if (find_all_hwmon_by_name("k10temp", cpu_hwmon_paths, 1) > 0)
        cpu_temperature1 = read_hwmon_temp(cpu_hwmon_paths[0], "temp1_input");
    else if (find_all_hwmon_by_name("coretemp", cpu_hwmon_paths, 1) > 0)
        cpu_temperature1 = read_hwmon_temp(cpu_hwmon_paths[0], "temp1_input");

    get_pch_temps(&pch_temperature1, &pch_temperature2);

    update_cpu_freqs(&freq_cur, &freq_max);

#ifdef ENABLE_DRAM
    int num_dram_sensors = find_all_hwmon_by_name("spd5118", dram_paths, 2);

    if (num_dram_sensors > 0)
        dram_temperature1 = read_hwmon_temp(dram_paths[0], "temp1_input");

    if (num_dram_sensors > 1)
        dram_temperature2 = read_hwmon_temp(dram_paths[1], "temp1_input");
#endif

    {
        char favg[24], fmax[24];

        printf("󰻠 %.0f %% | 󰾾 %s | 󰾾 %s |  %.1f GB |  %d °C | "
               " %d °C |  %d °C | "
#ifdef ENABLE_DRAM
               " %d °C |  %d °C | "
#endif
               "󰚥 %.0f W\n",
               cpu_usage, fmt_mhz(favg, sizeof(favg), freq_cur),
               fmt_mhz(fmax, sizeof(fmax), freq_max), used_memory_gb,
               cpu_temperature1 != -1 ? temp_c_rounded(cpu_temperature1) : 0,
               pch_temperature1 != -1 ? temp_c_rounded(pch_temperature1) : 0,
               pch_temperature2 != -1 ? temp_c_rounded(pch_temperature2) : 0,
#ifdef ENABLE_DRAM
               dram_temperature1 != -1 ? temp_c_rounded(dram_temperature1) : 0,
               dram_temperature2 != -1 ? temp_c_rounded(dram_temperature2) : 0,
#endif
               cpu_power);
    }
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
    char *gpu_clock = execute_command("rocm-smi --showclocks | awk '/sclk/ {print $3; exit}'");
    char *gpu_mem_clock = execute_command("rocm-smi --showclocks | awk '/mclk/ {print $3; exit}'");

    if (gpu_temperature1 && gpu_usage && gpu_vram_usage)
    {
        char fclk[24], fmemclk[24];

        printf("󰻠 %.0f %% | 󰻠 %.0f %% | 󰾾 %s | 󰾾 %s |  %.0f °C |  %.0f °C |  %.0f °C | 󰚥 %.0f W\n",
               atof(gpu_usage),
               atof(gpu_vram_usage),
               fmt_mhz(fclk, sizeof(fclk), gpu_clock ? atoi(gpu_clock) : 0),
               fmt_mhz(fmemclk, sizeof(fmemclk), gpu_mem_clock ? atoi(gpu_mem_clock) : 0),
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

    if (gpu_clock)
        free(gpu_clock);

    if (gpu_mem_clock)
        free(gpu_mem_clock);
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
        if (dbg_enabled())
            fprintf(stderr, "Failed to initialize NVML: %s\n", nvmlErrorString(result));

        return;
    }

    result = nvmlDeviceGetCount(&device_count);

    if (result != NVML_SUCCESS || device_count == 0)
    {
        if (result != NVML_SUCCESS && dbg_enabled())
            fprintf(stderr, "Failed to get device count: %s\n", nvmlErrorString(result));

        goto cleanup_nvml;
    }

    pacc = pci_alloc();

    if (!pacc)
    {
        if (dbg_enabled())
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

        /* Frekuensi inti GPU saat ini (MHz). */
        unsigned int gpu_clock = 0;

        if (nvmlDeviceGetClock(device, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT, &gpu_clock) != NVML_SUCCESS)
            gpu_clock = 0;

        /* Frekuensi VRAM saat ini (MHz). */
        unsigned int mem_clock = 0;

        if (nvmlDeviceGetClock(device, NVML_CLOCK_MEM, NVML_CLOCK_ID_CURRENT, &mem_clock) != NVML_SUCCESS)
            mem_clock = 0;

        nvmlMemory_t mem;

        if (nvmlDeviceGetMemoryInfo(device, &mem) == NVML_SUCCESS)
            fb_used = (float)mem.used / (1024.0f * 1024.0f * 1024.0f);

        nvmlTemperature_t temperature;

        temperature.version = nvmlTemperature_v1;
        temperature.sensorType = NVML_TEMPERATURE_GPU;

        if (nvmlDeviceGetTemperatureV(device, &temperature) == NVML_SUCCESS)
            gpu_temp = (unsigned int)temperature.temperature;

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

            /* Register suhu VRAM hanya dibaca; tidak perlu akses tulis. */
            int nvidia_fd = open(MEM_PATH, O_RDONLY);

            if (nvidia_fd < 0)
                break;

            uint32_t vram_addr = (dev->base_addr[0] & 0xFFFFFFFF) + VRAM_REGISTER_OFFSET;
            void *nvidia_map_base = mmap(NULL, PG_SZ, PROT_READ, MAP_SHARED, nvidia_fd, vram_addr & ~(PG_SZ - 1));

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

        char fclk[24], fmemclk[24];

        printf("󰻠 %u %% | 󰾾 %s | 󰾾 %s |  %.1f GB |  %u °C |  %u °C |  %u °C | 󰚥 %u W\n",
               gpu_util, fmt_mhz(fclk, sizeof(fclk), (int)gpu_clock),
               fmt_mhz(fmemclk, sizeof(fmemclk), (int)mem_clock),
               fb_used, gpu_temp, hotspot_temp, vram_temp, power_usage);

        break;
    }

    pci_cleanup(pacc);
cleanup_nvml:
    nvmlShutdown();
}
#endif

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
