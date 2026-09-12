/*
 * powerusage - Tool to monitor CPU and GPU power usage.
 *
 * Copyright (C) 2026 MOVZX
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
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

/* The DRAM temperature (spd5118 sensor) is off by default.
 * Build with DRAM support:
 *   gcc -O3 -DENABLE_DRAM ...   or   ENABLE_DRAM=1 ./build.sh
 */

float get_memory_usage(void);
void print_cpu_info(void);
void update_cpu_freqs(int *out_cur, int *out_max);
void print_amd_gpu_info(void);

#ifdef NVIDIA_GPU
void print_nvidia_gpu_info(void);
#endif

/**
 * @brief Report whether debug output is enabled.
 *
 * A panel uses the main output line, so the tool keeps quiet when a read
 * fails. The variable RYZEN_POWER_DEBUG enables the messages when set to 1.
 *
 * @return bool true when the RYZEN_POWER_DEBUG variable is set.
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
 * @brief Calculate the memory usage in GB.
 *
 * The function reads /proc/meminfo to get the total memory and the available
 * memory. It then calculates the used memory and returns that value in
 * gigabytes.
 *
 * @return float Memory usage in GB, or -1.0f on error.
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
 * @brief Get the CPU frequency in MHz from all cores: average and maximum.
 *
 * The function reads scaling_cur_freq for every online core. The path is
 * "cpu" in most cases, with a "platform-cpufreq" fallback for kernel 6.10
 * and newer. The function then calculates the average frequency and the
 * highest frequency over all cores.
 *
 * @param out_avg Storage for the average frequency in MHz. -1 when no data exists.
 * @param out_max Storage for the maximum frequency in MHz. -1 when no data exists.
 */
static void get_cpu_freqs(int *out_avg, int *out_max)
{
    char path[PATH_MAX];
    int max_mhz = -1, sum_mhz = 0, count = 0;

    for (int cpu = 0;; cpu++)
    {
        int found = 0;

        /* two possible paths: "cpu" (common) and "platform-cpufreq" (kernel 6.10+) */
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
                continue; /* core offline or no data */

            int mhz = (int)(khz / 1000);

            if (mhz > max_mhz)
                max_mhz = mhz;

            sum_mhz += mhz;
            count++;
            found = 1;
        }

        if (found == 0)
        {
            /* the next core does not always exist, so stop when even its directory is missing */
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
 * @brief Read, merge, and store the CPU frequency statistics in one locked operation.
 *
 * The file is /tmp/cpu_stats.txt with the format "KEY: number" on each line
 * (AVG, MAX, CUR). MAX is the highest frequency ever recorded and can only
 * rise. AVG is the average of all cores on the last sample. CUR is the
 * highest core frequency on the last sample.
 *
 * The code uses flock() because this application runs one time per call and
 * can run in turn, for example from a panel poller. Without the lock, two
 * writers can overwrite each other, so MAX can look like a decrease.
 *
 * @param out_cur Storage for the highest core frequency now (CUR) in MHz.
 * @param out_max Storage for the merged MAX value in MHz. This is history and can only rise.
 */
void update_cpu_freqs(int *out_cur, int *out_max)
{
    int cur_avg = -1, cur_max = -1;

    get_cpu_freqs(&cur_avg, &cur_max);

    /* O_NOFOLLOW: do not follow a symlink that another user planted in /tmp. */
    int lock_fd = open(CPU_STATS_PATH, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0644);

    if (lock_fd < 0)
    {
        /* Cannot open the file: show only the sampling result. */
        *out_cur = cur_max;
        *out_max = cur_max;

        return;
    }

    flock(lock_fd, LOCK_EX);

    /* Read again INSIDE the lock so the merged values are really the latest. */
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

    /* MAX: can only rise. AVG: the newest sample. */

    int max_mhz = cur_max;

    if (max_mhz < 0)
        max_mhz = old_max;
    else if (old_max > max_mhz)
        max_mhz = old_max;

    int avg_mhz = cur_avg >= 0 ? cur_avg : old_avg;

    /* CUR: the highest core frequency on the last sample, with no history. */
    int cur_mhz = cur_max >= 0 ? cur_max : old_cur;

    /* Write only when something changed: a higher MAX, or a changed AVG/CUR.
     * An old value never loses to a worse number. Write through a unique
     * temporary file, then use rename (atomic). */
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

    if (f != NULL)
        fclose(f);
    else
        close(lock_fd);

    *out_cur = cur_mhz;
    *out_max = max_mhz;
}

/**
 * @brief Get the total and idle CPU time from /proc/stat.
 *
 * @param times Pointer to the CpuTimes structure that stores the result.
 * @return bool true on success, false on error.
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
 * @brief Print the CPU usage, temperature, and memory usage information.
 *
 * The function measures the CPU power draw, the CPU usage, and the CPU
 * temperature over a one second interval, then prints them to standard output.
 */
void print_cpu_info(void)
{
    /* The RAPL energy and the CPU time come from the same one second window. */
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
        /* An energy counter error must not remove the output line of the panel. */
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
        used_memory_gb = 0.0f; /* the panel still receives one complete line */

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
 * @brief Print the AMD GPU usage, temperature, and power information.
 *
 * The function uses rocm-smi to get and print the metrics for the AMD GPU.
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
 * @brief Print the NVIDIA GPU usage, temperature, and power information.
 *
 * The function uses NVML and direct PCI access to get and print the metrics
 * for the NVIDIA GPU, including the VRAM and hotspot temperatures.
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

        /* Current GPU core frequency (MHz). */
        unsigned int gpu_clock = 0;

        if (nvmlDeviceGetClock(device, NVML_CLOCK_GRAPHICS, NVML_CLOCK_ID_CURRENT, &gpu_clock) != NVML_SUCCESS)
            gpu_clock = 0;

        /* Current VRAM frequency (MHz). */
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

            /* The code only reads the VRAM temperature register, so write access is not needed. */
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
 * @brief Program entry point.
 *
 * The function reads the command line arguments to decide whether to show
 * the CPU information or the GPU information.
 *
 * @return int 0 on success, 1 on error.
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
