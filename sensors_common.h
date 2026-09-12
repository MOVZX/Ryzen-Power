/*
 * sensors_common.h - Helpers that all tools in this repo share.
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

#ifndef SENSORS_COMMON_H
#define SENSORS_COMMON_H

#include <ctype.h>
#include <glob.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef NVIDIA_GPU
#include <stdbool.h>
#endif

#define RAPL_ENERGY_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define RAPL_RANGE_PATH "/sys/class/powercap/intel-rapl:0/max_energy_range_uj"
#define USEC 1000000

/* Scan limit for the temp<N>_label index. Indexes can have gaps, so scan wide. */
#define MAX_TEMP_IDX 32

/* Buffer for a path made from an hwmon directory and a sensor file name.
 * It is larger than PATH_MAX so the snprintf result always fits. */
#define SENSOR_PATH_BUF (PATH_MAX + 32)

/* GPU type detected on the system. */
enum GpuType
{
    GPU_TYPE_UNINITIALIZED = -1,
    GPU_TYPE_NONE = 0,
    GPU_TYPE_AMD,
    GPU_TYPE_NVIDIA,
};

/* One hwmon temperature sensor with its label, for example "Tctl" or "Tccd1". */
typedef struct
{
    char label[32];
    int value;
} TempSensor;

/**
 * @brief Read an integer value from one file.
 *
 * @param path Path to the file.
 * @return int The value that was read, or -1 on error.
 */
static inline int read_int_from_file(const char *path)
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
 * @brief Read one hwmon temperature file.
 *
 * @param hwmon_path The hwmon directory.
 * @param temp_file File name, for example "temp1_input".
 * @return int Temperature in millidegrees Celsius, or -1 on error.
 */
static inline int read_hwmon_temp(const char *hwmon_path, const char *temp_file)
{
    char full_path[SENSOR_PATH_BUF];
    int ret = snprintf(full_path, sizeof(full_path), "%s/%s", hwmon_path, temp_file);

    if (ret < 0 || (size_t)ret >= sizeof(full_path))
        return -1;

    return read_int_from_file(full_path);
}

/**
 * @brief Round a millidegree Celsius temperature to a whole degree.
 *
 * Sysfs gives temperatures in millidegrees, for example 70965. Integer
 * division cuts down, so the result shows 70 while lm-sensors shows 71.
 * Round to the nearest degree so both give the same value.
 *
 * @param milli_c Temperature in millidegrees Celsius.
 * @return int Temperature in degrees Celsius, rounded to the nearest degree.
 */
static inline int temp_c_rounded(int milli_c)
{
    if (milli_c < 0)
        return -((-milli_c + 500) / 1000);

    return (milli_c + 500) / 1000;
}

/**
 * @brief Format an MHz value with a dot as the thousands separator.
 *
 * Example: 5712 becomes "5.712 MHz", 624 becomes "624 MHz".
 *
 * @param buf   Destination buffer.
 * @param size  Size of the buffer.
 * @param mhz   Frequency value in MHz.
 * @return const char * Pointer to the buffer.
 */
static inline const char *fmt_mhz(char *buf, size_t size, int mhz)
{
    char digits[16];
    int neg = mhz < 0;
    unsigned int v = neg ? (unsigned int)-(long)mhz : (unsigned int)mhz;
    int n = 0;

    do
    {
        digits[n++] = (char)('0' + (v % 10));
        v /= 10;
    } while (v && n < (int)sizeof(digits));

    int pos = 0;

    if (neg && pos < (int)size - 1)
        buf[pos++] = '-';

    for (int i = n - 1; i >= 0; i--)
    {
        /* Print the dot before a digit when the digits that remain, this one
         * included, fill a group of three, and this is not the first digit. */
        int remaining = i + 1;

        if (pos > (neg ? 1 : 0) && remaining % 3 == 0 && pos < (int)size - 1)
            buf[pos++] = '.';

        if (pos < (int)size - 1)
            buf[pos++] = digits[i];
    }

    snprintf(buf + pos, size - (size_t)pos, " MHz");

    return buf;
}

/**
 * @brief Run a shell command and take its first output line.
 *
 * The caller must free the returned memory. This function starts a new
 * process, so repeated calls are expensive.
 *
 * @param command The shell command.
 * @return char* First output line without trailing whitespace, or NULL on error.
 */
static inline char *execute_command(const char *command)
{
    FILE *fp = popen(command, "r");

    if (fp == NULL)
        return NULL;

    char *output = malloc(4096);

    if (output == NULL)
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
        size_t len = strlen(output);

        /* dmidecode and nvidia-smi often add padding at the end of the line. */
        while (len > 0 && isspace((unsigned char)output[len - 1]))
            output[--len] = '\0';
    }

    pclose(fp);

    return output;
}

/**
 * @brief Find every hwmon directory whose name matches exactly.
 *
 * @param name hwmon name, for example "k10temp" or "spd5118".
 * @param out_paths Array that stores the paths.
 * @param max_paths Maximum number of paths.
 * @return int Number of paths found.
 */
static inline int find_all_hwmon_by_name(const char *name, char (*out_paths)[PATH_MAX], int max_paths)
{
    glob_t hwmon_paths;
    int count = 0;

    if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &hwmon_paths) != 0)
        return 0;

    for (size_t i = 0; i < hwmon_paths.gl_pathc && count < max_paths; i++)
    {
        char name_path[SENSOR_PATH_BUF];

        snprintf(name_path, sizeof(name_path), "%s/name", hwmon_paths.gl_pathv[i]);

        FILE *f = fopen(name_path, "r");

        if (f == NULL)
            continue;

        char buffer[64];

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
 * @brief Count the fan inputs of one hwmon device.
 *
 * The tools use this to select the main super I/O chip when the system has
 * more than one chip.
 *
 * @param hwmon_path The hwmon directory.
 * @return int Number of fan inputs that exist.
 */
static inline int count_hwmon_fans(const char *hwmon_path)
{
    int fan_count = 0;

    for (int fan_num = 1; fan_num <= 20; fan_num++)
    {
        char fan_path[SENSOR_PATH_BUF];

        snprintf(fan_path, sizeof(fan_path), "%s/fan%d_input", hwmon_path, fan_num);

        if (access(fan_path, F_OK) == 0)
            fan_count++;
    }

    return fan_count;
}

/**
 * @brief Find one hwmon directory with a name prefix match.
 *
 * This fits a chip family: "nct6" matches nct6799 and nct6798. When several
 * chips match, the function selects the chip with the most fan inputs.
 *
 * @param prefix hwmon name prefix.
 * @param out_path Buffer that stores the path.
 * @param out_path_size Size of the buffer.
 * @return int 0 on success, -1 when nothing matches.
 */
static inline int find_hwmon_path_by_prefix(const char *prefix, char *out_path, size_t out_path_size)
{
    glob_t hwmon_paths;
    int found = -1;
    int max_fan_count = -1;
    char best_path[PATH_MAX] = {0};
    size_t prefix_len = strlen(prefix);

    if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &hwmon_paths) != 0)
        return -1;

    for (size_t i = 0; i < hwmon_paths.gl_pathc; i++)
    {
        char name_path[SENSOR_PATH_BUF];

        snprintf(name_path, sizeof(name_path), "%s/name", hwmon_paths.gl_pathv[i]);

        FILE *f = fopen(name_path, "r");

        if (f == NULL)
            continue;

        char buffer[64];

        if (fgets(buffer, sizeof(buffer), f))
        {
            buffer[strcspn(buffer, "\n")] = 0;

            if (strncmp(buffer, prefix, prefix_len) == 0)
            {
                int fan_count = count_hwmon_fans(hwmon_paths.gl_pathv[i]);

                if (fan_count > max_fan_count)
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
        snprintf(out_path, out_path_size, "%s", best_path);

    globfree(&hwmon_paths);

    return found;
}

/**
 * @brief Find an hwmon directory by exact name and store it in one buffer.
 *
 * @param name hwmon name, for example "k10temp".
 * @param out_path Buffer that stores the path.
 * @param out_path_size Size of the buffer.
 * @return int 0 on success, -1 when nothing matches.
 */
static inline int find_hwmon_path_by_name(const char *name, char *out_path, size_t out_path_size)
{
    char paths[1][PATH_MAX];

    if (find_all_hwmon_by_name(name, paths, 1) < 1)
        return -1;

    snprintf(out_path, out_path_size, "%s", paths[0]);

    return 0;
}

/**
 * @brief Read every labelled temperature sensor in one hwmon directory.
 *
 * Sensor indexes can have gaps. On the 9950X3D, for example: temp1=Tctl,
 * temp3=Tccd1, temp4=Tccd2, while temp2 does not exist. The function skips a
 * sensor with no label.
 *
 * @param hwmon_path The hwmon directory.
 * @param sensors Array that stores the sensors.
 * @param max_sensors Capacity of the sensors array.
 * @return int Number of sensors filled.
 */
static inline int read_labelled_temps(const char *hwmon_path, TempSensor *sensors, int max_sensors)
{
    int count = 0;

    for (int idx = 1; idx <= MAX_TEMP_IDX && count < max_sensors; idx++)
    {
        char label_path[SENSOR_PATH_BUF];
        char temp_path[SENSOR_PATH_BUF];
        char label[32] = "";

        snprintf(label_path, sizeof(label_path), "%s/temp%d_label", hwmon_path, idx);

        FILE *label_file = fopen(label_path, "r");

        if (label_file != NULL)
        {
            if (fgets(label, sizeof(label), label_file))
                label[strcspn(label, "\n")] = 0;

            fclose(label_file);
        }

        if (label[0] == '\0')
            continue;

        snprintf(temp_path, sizeof(temp_path), "%s/temp%d_input", hwmon_path, idx);

        snprintf(sensors[count].label, sizeof(sensors[count].label), "%s", label);
        sensors[count].value = read_int_from_file(temp_path);
        count++;
    }

    return count;
}

/**
 * @brief Current time in microseconds, from the realtime clock.
 *
 * @return int64_t Microseconds, or -1 on error.
 */
static inline int64_t get_time_usec(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
        return -1;

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

/**
 * @brief Read the RAPL energy counter.
 *
 * @return int64_t Energy in microjoules, or -1 on error.
 */
static inline int64_t get_cpu_energy_uj(void)
{
    FILE *file = fopen(RAPL_ENERGY_PATH, "r");

    if (file == NULL)
        return -1;

    int64_t consumption = -1;

    if (fscanf(file, "%ld", &consumption) != 1)
        consumption = -1;

    fclose(file);

    return consumption;
}

/**
 * @brief Read the wrap range of the RAPL energy counter in microjoules.
 *
 * On new kernels, energy_uj wraps at max_energy_range_uj instead of growing
 * without limit. User space must handle the wrap itself.
 *
 * @return int64_t Counter range, or -1 on error.
 */
static inline int64_t get_rapl_range_uj(void)
{
    FILE *file = fopen(RAPL_RANGE_PATH, "r");

    if (file == NULL)
        return -1;

    int64_t range = -1;

    if (fscanf(file, "%ld", &range) != 1)
        range = -1;

    fclose(file);

    return range;
}

/**
 * @brief Calculate the average power from two readings of the energy counter.
 *
 * This function also corrects a counter wrap with max_energy_range_uj. A
 * caller that measures its own window uses this function, so the wrap
 * correction exists in one place only.
 *
 * @param energy_start Energy at the start of the window (microjoules).
 * @param time_start Time at the start of the window (microseconds).
 * @param energy_end Energy at the end of the window (microjoules).
 * @param time_end Time at the end of the window (microseconds).
 * @return float Power in watts, or -1.0f when the data is not valid.
 */
static inline float cpu_power_from_delta(int64_t energy_start, int64_t time_start,
                                         int64_t energy_end, int64_t time_end)
{
    if (energy_start < 0 || energy_end < 0 || time_end <= time_start)
        return -1.0f;

    int64_t energy_delta = energy_end - energy_start;

    /* The counter can wrap inside the window, so add the range back.
     * One wrap per second at most, because the range is about 65 kJ while
     * the energy per second is about one kJ. */
    if (energy_delta < 0)
    {
        int64_t range = get_rapl_range_uj();

        if (range > 0)
            energy_delta += range;
    }

    if (energy_delta < 0)
        return -1.0f;

    return (float)energy_delta / (float)(time_end - time_start);
}

/**
 * @brief Measure the CPU power one time, over a one second interval.
 *
 * @return float Power in watts, or -1.0f on error.
 */
static inline float measure_cpu_power_once(void)
{
    int64_t energy_start = get_cpu_energy_uj();
    int64_t time_start = get_time_usec();

    if (energy_start < 0 || time_start < 0)
        return -1.0f;

    sleep(1);

    int64_t energy_end = get_cpu_energy_uj();
    int64_t time_end = get_time_usec();

    return cpu_power_from_delta(energy_start, time_start, energy_end, time_end);
}

/**
 * @brief Measure the CPU power, with one repeat measurement after an error.
 *
 * The wrap range that the kernel reports can be a little smaller than the
 * real wrap point. A one second window that crosses that point can give a
 * false negative difference. Measure one more time before you give up.
 *
 * @return float Power in watts, or -1.0f when it still fails.
 */
static inline float measure_cpu_power(void)
{
    float power = measure_cpu_power_once();

    if (power < 0)
        power = measure_cpu_power_once();

    return power;
}

/**
 * @brief CPU model name from /proc/cpuinfo.
 *
 * This does not need root, so prefer it over dmidecode.
 *
 * @param buffer Destination buffer.
 * @param size Size of the buffer.
 * @return int 0 on success, -1 on error.
 */
static inline int get_cpu_model_name(char *buffer, size_t size)
{
    FILE *file = fopen("/proc/cpuinfo", "r");

    if (file == NULL)
        return -1;

    char line[256];
    int found = -1;

    while (fgets(line, sizeof(line), file))
    {
        if (strncmp(line, "model name", 10) != 0)
            continue;

        char *colon = strchr(line, ':');

        if (colon == NULL)
            break;

        colon++;

        while (*colon == ' ' || *colon == '\t')
            colon++;

        snprintf(buffer, size, "%s", colon);

        buffer[strcspn(buffer, "\n")] = 0;
        found = 0;

        break;
    }

    fclose(file);

    return found;
}

/**
 * @brief Read one DMI field from sysfs without root.
 *
 * Fields such as board_vendor and board_name have mode 0444, so a normal
 * user can read them. Other fields, for example bios_date, are readable only
 * by root.
 *
 * @param field Field name, for example "board_vendor".
 * @param buffer Destination buffer.
 * @param size Size of the buffer.
 * @return int 0 on success, -1 on error.
 */
static inline int read_dmi_field(const char *field, char *buffer, size_t size)
{
    char path[SENSOR_PATH_BUF];

    snprintf(path, sizeof(path), "/sys/class/dmi/id/%s", field);

    FILE *file = fopen(path, "r");

    if (file == NULL)
        return -1;

    if (fgets(buffer, (int)size, file) == NULL)
    {
        fclose(file);

        return -1;
    }

    fclose(file);

    buffer[strcspn(buffer, "\n")] = 0;

    return (buffer[0] != '\0' && strcmp(buffer, "To be filled by O.E.M.") != 0) ? 0 : -1;
}

/**
 * @brief Calculate an order key from the PCI address of an hwmon directory.
 *
 * The hwmon path always contains the PCI address of its device, for example
 * ".../0000:11:00.0/hwmon/hwmon7". The function uses the last address in the
 * path, so the order stays stable and does not depend on glob order. Glob
 * sorts names as strings, so hwmon10 comes before hwmon7.
 *
 * @param hwmon_path Path of the hwmon directory.
 * @return long long Order key, or -1 when there is no PCI address.
 */
static inline long long hwmon_pci_key(const char *hwmon_path)
{
    char real[PATH_MAX];

    if (realpath(hwmon_path, real) == NULL)
        return -1;

    long long key = -1;

    for (const char *p = real; *p; p++)
    {
        unsigned int domain, bus, dev, func;
        int n = 0;

        if (sscanf(p, "%4x:%2x:%2x.%1x%n", &domain, &bus, &dev, &func, &n) == 4 && n > 0)
        {
            key = ((long long)domain << 32) | ((long long)bus << 16) | ((long long)dev << 8) | func;
            p += n - 1;
        }
    }

    return key;
}

/**
 * @brief Sort a list of hwmon paths by PCI address, smallest first.
 *
 * @param paths Array of paths. The function changes this array.
 * @param keys Array of order keys. The function changes this array.
 * @param count Number of entries.
 */
static inline void sort_paths_by_pci_key(char (*paths)[PATH_MAX], long long *keys, int count)
{
    for (int i = 0; i < count; i++)
    {
        for (int j = i + 1; j < count; j++)
        {
            if (keys[j] >= keys[i])
                continue;

            long long tmp_key = keys[i];

            keys[i] = keys[j];
            keys[j] = tmp_key;

            char tmp_path[PATH_MAX];

            memcpy(tmp_path, paths[i], PATH_MAX);
            memcpy(paths[i], paths[j], PATH_MAX);
            memcpy(paths[j], tmp_path, PATH_MAX);
        }
    }
}

/**
 * @brief Read the PCH chipset temperature from the prom21_xhci sensors.
 *
 * An AM5 board has two prom21_xhci chips, on PCI 11:00.0 and 13:00.0.
 * lm-sensors names them "PCH Chipset #1" and "PCH Chipset #2", but their
 * sysfs labels are empty. The order comes from the PCI address, so it matches
 * the lm-sensors numbering: 11:00.0 is PCH1, 13:00.0 is PCH2.
 *
 * @param pch1 Storage for the PCH1 temperature in millidegrees Celsius, or -1 when absent.
 * @param pch2 Storage for the PCH2 temperature in millidegrees Celsius, or -1 when absent.
 */
static inline void get_pch_temps(int *pch1, int *pch2)
{
    char paths[4][PATH_MAX];
    long long keys[4];

    *pch1 = -1;
    *pch2 = -1;

    int found = find_all_hwmon_by_name("prom21_xhci", paths, 4);

    if (found <= 0)
        return;

    for (int i = 0; i < found; i++)
        keys[i] = hwmon_pci_key(paths[i]);

    sort_paths_by_pci_key(paths, keys, found);

    *pch1 = read_hwmon_temp(paths[0], "temp1_input");

    if (found > 1)
        *pch2 = read_hwmon_temp(paths[1], "temp1_input");
}

/**
 * @brief Detect the GPU type on the system. The function caches the result.
 *
 * The AMD GPU is known from the "amdgpu" hwmon device, with no new process.
 * The NVIDIA GPU is known from nvidia-smi, and only when the build defines
 * -DNVIDIA_GPU.
 *
 * @return int One of the enum GpuType values.
 */
static inline int detect_gpu_type(void)
{
    static int cached_gpu_type = GPU_TYPE_UNINITIALIZED;

    if (cached_gpu_type != GPU_TYPE_UNINITIALIZED)
        return cached_gpu_type;

    char hwmon_path[PATH_MAX];

    if (find_hwmon_path_by_name("amdgpu", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        cached_gpu_type = GPU_TYPE_AMD;

        return cached_gpu_type;
    }

#ifdef NVIDIA_GPU
    char *nvidia_check = execute_command("nvidia-smi -L >/dev/null 2>&1 && echo 'NVIDIA'");

    if (nvidia_check != NULL)
    {
        if (strstr(nvidia_check, "NVIDIA") != NULL)
            cached_gpu_type = GPU_TYPE_NVIDIA;

        free(nvidia_check);
    }
#endif

    if (cached_gpu_type == GPU_TYPE_UNINITIALIZED)
        cached_gpu_type = GPU_TYPE_NONE;

    return cached_gpu_type;
}

#endif /* SENSORS_COMMON_H */
