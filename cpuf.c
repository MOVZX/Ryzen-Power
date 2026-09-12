/*
 * cpuf - Tool to monitor CPU frequency, temperature, and power.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sensors_common.h"

#define MAX_CPU_SENSORS 16

#define BOLD "\033[1m"
#define RESET "\033[0m"
#define CLEAR_SCREEN "\033[2J"
#define CURSOR_HOME "\033[H"

typedef struct
{
    int last;
    int max;
    long long sum;
    long count;
} FreqStats;

/**
 * @brief Read the current frequency of every CPU core.
 *
 * @param freqs Array that stores the frequency of each core in MHz.
 * @param cpu_count Number of logical CPU cores.
 */
static void get_cpu_frequencies(int *freqs, int cpu_count)
{
    for (int i = 0; i < cpu_count; i++)
    {
        char freq_path[PATH_MAX];

        snprintf(freq_path, sizeof(freq_path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);

        int freq_khz = read_int_from_file(freq_path);
        freqs[i] = (freq_khz != -1) ? freq_khz / 1000 : 0;
    }
}

/**
 * @brief Record one frequency sample in the per-core statistics.
 *
 * The function ignores the value 0. That value means a failed read, and a
 * failed read must not damage the maximum value.
 *
 * @param stats Array of per-core frequency statistics.
 * @param freqs Array of core frequencies in MHz.
 * @param cpu_count Number of logical CPU cores.
 */
static void record_sample(FreqStats *stats, const int *freqs, int cpu_count)
{
    for (int i = 0; i < cpu_count; i++)
    {
        int freq = freqs[i];

        if (freq == 0)
            continue;

        if (freq > stats[i].max)
            stats[i].max = freq;

        stats[i].last = freq;
        stats[i].sum += freq;
        stats[i].count++;
    }
}

/**
 * @brief Print the formatted CPU information.
 *
 * @param sensors Array of CPU temperature sensors.
 * @param sensor_count Number of CPU temperature sensors.
 * @param cpu_power CPU power in watts.
 * @param stats Array of per-core frequency statistics.
 * @param cpu_count Number of logical CPU cores.
 * @param cpu_name CPU model name.
 */
static void print_cpu_info(const TempSensor *sensors, int sensor_count, float cpu_power, const FreqStats *stats, int cpu_count, const char *cpu_name)
{
    printf(BOLD "%s" RESET "\n\n", cpu_name);

    for (int i = 0; i < sensor_count; i++)
        printf("%-7s : %8d°C\n", sensors[i].label, sensors[i].value != -1 ? temp_c_rounded(sensors[i].value) : 0);

    if (cpu_power >= 0)
        printf("Power   : %8.2f W\n", cpu_power);
    else
        printf("Power   :       N/A\n");

    printf("\n");

    for (int i = 0; i < cpu_count; i++)
    {
        char flast[24], fmax[24];

        printf("CPU %2d : %10s  (max %8s)\n",
               i,
               fmt_mhz(flast, sizeof(flast), stats[i].last),
               fmt_mhz(fmax, sizeof(fmax), stats[i].max));
    }

    printf("\n");
}

/**
 * @brief Program entry point.
 *
 * The program runs a loop. Each second it reads the temperature, the power,
 * and the frequencies, then it prints the status. The maximum value per core
 * counts from the start of the program. Press Ctrl+C to stop.
 *
 * @return int 0 on success, 1 on error.
 */
int main(void)
{
    char cpu_name[256];
    char hwmon_path[PATH_MAX] = "";
    TempSensor sensors[MAX_CPU_SENSORS];

    if (get_cpu_model_name(cpu_name, sizeof(cpu_name)) != 0)
        snprintf(cpu_name, sizeof(cpu_name), "CPU");

    int cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);

    if (cpu_count <= 0)
        cpu_count = 1;

    FreqStats *stats = calloc(cpu_count, sizeof(FreqStats));

    if (stats == NULL)
    {
        perror("Failed to allocate CPU frequency stats");

        return 1;
    }

    int *cpu_freqs = malloc(sizeof(int) * cpu_count);

    if (cpu_freqs == NULL)
    {
        perror("Failed to allocate CPU frequency array");

        free(stats);

        return 1;
    }

    printf(CLEAR_SCREEN);

    while (1)
    {
        /* The program resolves the k10temp path one time only. The hwmon index
         * can change after a module reload, so resolve the path again only when
         * the temperature read fails. */
        if (hwmon_path[0] == '\0' && find_hwmon_path_by_name("k10temp", hwmon_path, sizeof(hwmon_path)) != 0)
        {
            fprintf(stderr, "k10temp sensor module not found!\n");

            free(cpu_freqs);
            free(stats);

            return 1;
        }

        int sensor_count = read_labelled_temps(hwmon_path, sensors, MAX_CPU_SENSORS);

        if (sensor_count == 0)
        {
            hwmon_path[0] = '\0';

            if (find_hwmon_path_by_name("k10temp", hwmon_path, sizeof(hwmon_path)) != 0)
            {
                fprintf(stderr, "k10temp sensor module not found!\n");

                free(cpu_freqs);
                free(stats);

                return 1;
            }

            sensor_count = read_labelled_temps(hwmon_path, sensors, MAX_CPU_SENSORS);

            if (sensor_count == 0)
            {
                fprintf(stderr, "Failed to read CPU temperatures.\n");

                free(cpu_freqs);
                free(stats);

                return 1;
            }
        }

        float cpu_power = measure_cpu_power();

        /* A short error, for example a RAPL counter wrap, must not stop the
         * monitor. Print N/A for this tick and continue. */

        get_cpu_frequencies(cpu_freqs, cpu_count);
        record_sample(stats, cpu_freqs, cpu_count);

        printf(CURSOR_HOME);
        print_cpu_info(sensors, sensor_count, cpu_power, stats, cpu_count, cpu_name);
    }
}
