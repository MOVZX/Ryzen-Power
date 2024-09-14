#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define NUM_CPUS 16
#define BUFFER_SIZE 256

int64_t get_cpuConsumptionUJoules()
{
    int64_t consumption = -1;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (file == NULL)
    {
        perror("Error opening RAPL energy file!");

        return -1;
    }

    if (fscanf(file, "%lld", &consumption) != 1)
    {
        perror("Error reading energy consumption!");

        consumption = -1;
    }

    fclose(file);

    return consumption;
}

int64_t get_currentTimeUSec()
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("Error getting current time!");

        return -1;
    }

    return ((int64_t)tv.tv_sec * 1000000) + tv.tv_usec;
}

float calculate_cpu_power()
{
    int64_t initial_usage = get_cpuConsumptionUJoules();
    int64_t initial_time = get_currentTimeUSec();

    if (initial_usage == -1 || initial_time == -1)
    {
        fprintf(stderr, "Failed to read initial CPU consumption or time data!\n");

        return -1.0f;
    }

    usleep(1000000);

    int64_t final_usage = get_cpuConsumptionUJoules();
    int64_t final_time = get_currentTimeUSec();

    if (final_usage == -1 || final_time == -1)
    {
        fprintf(stderr, "Failed to read final CPU consumption or time data!\n");

        return -1.0f;
    }

    if (final_time < initial_time)
    {
        fprintf(stderr, "Time went backwards!\n");

        return -1.0f;
    }

    int64_t time_diff_usec = final_time - initial_time;
    int64_t energy_diff_uj = final_usage - initial_usage;

    if (time_diff_usec <= 0)
    {
        fprintf(stderr, "Invalid time difference!\n");

        return -1.0f;
    }

    if (energy_diff_uj < 0)
    {
        fprintf(stderr, "Energy consumption decreased!\n");

        return -1.0f;
    }

    double time_diff_sec = time_diff_usec / 1000000.0;

    return (float)(energy_diff_uj / (time_diff_sec * 1e6));
}

int read_int_from_file(const char *path)
{
    int value = 0;
    FILE *file = fopen(path, "r");

    if (file == NULL)
    {
        perror("Error opening file!");

        return -1;
    }

    if (fscanf(file, "%d", &value) != 1)
    {
        perror("Error reading integer from file!");

        fclose(file);

        return -1;
    }

    fclose(file);

    return value;
}

int read_int_from_command(const char *command)
{
    FILE *fp;
    char output[BUFFER_SIZE];
    int value = -1;

    fp = popen(command, "r");

    if (fp == NULL)
    {
        perror("Error running command!");

        return -1;
    }

    if (fgets(output, sizeof(output), fp) != NULL)
    {
        char *endptr;
        value = strtol(output, &endptr, 10);

        if (*endptr != '\0')
        {
            fprintf(stderr, "Error converting command output to integer!\n");

            value = -1;
        }
    }
    else
    {
        perror("Error reading output from command!");
    }

    pclose(fp);

    return value;
}

int main()
{
    char hwmon_path[BUFFER_SIZE];
    FILE *fp;
    int cpu_tctl = -1, cpu_tccd = -1;
    float cpu_power = -1.0f;
    int cpu_freq[NUM_CPUS];

    fp = popen("grep -l 'k10temp' /sys/class/hwmon/hwmon*/name", "r");

    if (fp == NULL || fgets(hwmon_path, sizeof(hwmon_path), fp) == NULL)
    {
        printf("k10temp sensor module not found!\n");

        pclose(fp);

        return 1;
    }

    pclose(fp);

    hwmon_path[strcspn(hwmon_path, "\n")] = 0;

    size_t len = strlen(hwmon_path);

    if (len > 5)
    {
        hwmon_path[len - 5] = '\0';
    }
    else
    {
        fprintf(stderr, "Invalid hwmon_path format!\n");

        return 1;
    }

    char temp1_path[BUFFER_SIZE];
    char temp3_path[BUFFER_SIZE];

    snprintf(temp1_path, sizeof(temp1_path), "%s/temp1_input", hwmon_path);
    snprintf(temp3_path, sizeof(temp3_path), "%s/temp3_input", hwmon_path);

    cpu_tctl = read_int_from_file(temp1_path) / 1000;
    cpu_tccd = read_int_from_file(temp3_path) / 1000;

    if (cpu_tctl == -1 || cpu_tccd == -1)
    {
        printf("Failed to read temperatures!\n");

        return 1;
    }

    cpu_power = calculate_cpu_power();

    if (cpu_power == -1.0f)
    {
        printf("Failed to calculate CPU power!\n");

        return 1;
    }

    for (int i = 0; i < NUM_CPUS; i++)
    {
        char freq_path[BUFFER_SIZE];

        snprintf(freq_path, sizeof(freq_path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);

        cpu_freq[i] = read_int_from_file(freq_path) / 1000;
    }

    printf("\nRyzen 7 7800X3D\n\n");
    printf("Tctl    : %8d°C\n", cpu_tctl);
    printf("Tccd    : %8d°C\n", cpu_tccd);
    printf("Power   : %8.2f W\n", cpu_power);
    printf("\n");

    for (int i = 0; i < NUM_CPUS; i++)
    {
        printf("CPU %2d  : %6d MHz\n", i + 1, cpu_freq[i]);
    }

    return 0;
}
