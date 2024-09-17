#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define BUFFER_SIZE 256

#define BOLD "\033[1m"
#define RESET "\033[0m"

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

float read_float_from_command(const char *command)
{
    FILE *fp;
    char output[BUFFER_SIZE];
    float value = -1.0f;

    fp = popen(command, "r");

    if (fp == NULL)
    {
        perror("Error running command!");

        return value;
    }

    if (fgets(output, sizeof(output), fp) != NULL)
    {
        value = strtof(output, NULL);
    }
    else
    {
        perror("Error reading command output!");
    }

    pclose(fp);

    return value;
}

int read_int_from_file(const char *path)
{
    FILE *file = fopen(path, "r");
    int value = -1;

    if (file == NULL)
    {
        perror("Error opening file!");

        return value;
    }

    if (fscanf(file, "%d", &value) != 1)
    {
        perror("Error reading file!");

        value = -1;
    }

    fclose(file);

    return value;
}

int find_hwmon_path(const char *sensor_name, char *path, size_t size)
{
    FILE *fp;
    char cmd[BUFFER_SIZE];
    char result[BUFFER_SIZE];

    snprintf(cmd, sizeof(cmd), "grep -l '%s' /sys/class/hwmon/hwmon*/name", sensor_name);

    fp = popen(cmd, "r");

    if (fp == NULL)
    {
        perror("Error running grep command!");

        return -1;
    }

    if (fgets(result, sizeof(result), fp) == NULL)
    {
        pclose(fp);

        return -1;
    }

    pclose(fp);

    result[strcspn(result, "\n")] = 0;

    size_t len = strlen(result);

    if (len >= 5)
    {
        snprintf(path, size, "%s", result);
        path[len - 5] = '\0';
    }
    else
    {
        path[0] = '\0';
    }

    return 0;
}

int main()
{
    char hwmon_path[BUFFER_SIZE];
    char temp_path[BUFFER_SIZE];
    int mobo_temp, vrm_temp, pch_temp;
    int radiator_fan, top_fans, bottom1_fans, bottom2_fans;
    int cpu_tctl, cpu_tccd;
    int mem1_temp, mem2_temp;
    float cpu_power, gpu_edge, gpu_junction, gpu_mem, gpu_power;
    int nvme1_temp, nvme2_temp, nvme3_temp, nvme4_temp;

    if (find_hwmon_path("nct668*", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        snprintf(temp_path, sizeof(temp_path), "%s/temp2_input", hwmon_path);
        mobo_temp = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/temp3_input", hwmon_path);
        vrm_temp = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/temp5_input", hwmon_path);
        pch_temp = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan1_input", hwmon_path);
        radiator_fan = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan4_input", hwmon_path);
        top_fans = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan5_input", hwmon_path);
        bottom1_fans = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/fan6_input", hwmon_path);
        bottom2_fans = read_int_from_file(temp_path);
    }
    else
    {
        fprintf(stderr, "nct668* sensor module not found! Ensure the sensor module is present or the path is correct!\n");

        return 1;
    }

    if (find_hwmon_path("k10temp", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", hwmon_path);

        cpu_tctl = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/temp3_input", hwmon_path);

        cpu_tccd = read_int_from_file(temp_path);

        cpu_power = calculate_cpu_power();

        if (cpu_power < 0)
        {
            cpu_power = 0;
        }
    }
    else
    {
        fprintf(stderr, "k10temp sensor module not found! Ensure the sensor module is present or the path is correct!\n");

        return 1;
    }

    if (find_hwmon_path("amdgpu", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", hwmon_path);

        gpu_edge = read_int_from_file(temp_path) / 1000.0;

        snprintf(temp_path, sizeof(temp_path), "%s/temp2_input", hwmon_path);

        gpu_junction = read_int_from_file(temp_path) / 1000.0;

        snprintf(temp_path, sizeof(temp_path), "%s/temp3_input", hwmon_path);

        gpu_mem = read_int_from_file(temp_path) / 1000.0;

        snprintf(temp_path, sizeof(temp_path), "%s/power1_average", hwmon_path);

        gpu_power = read_int_from_file(temp_path) / 1000000.0;

        if (gpu_power < 0)
        {
            gpu_power = 0;
        }
    }
    else
    {
        fprintf(stderr, "amdgpu sensor module not found! Ensure the sensor module is present or the path is correct!\n");

        return 1;
    }

    snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/hwmon8/temp1_input");

    mem1_temp = read_int_from_file(temp_path);

    snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/hwmon9/temp1_input");

    mem2_temp = read_int_from_file(temp_path);

    snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/hwmon5/temp1_input");

    nvme1_temp = read_int_from_file(temp_path);

    snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/hwmon3/temp1_input");

    nvme2_temp = read_int_from_file(temp_path);

    snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/hwmon2/temp1_input");

    nvme3_temp = read_int_from_file(temp_path);

    snprintf(temp_path, sizeof(temp_path), "/sys/class/hwmon/hwmon4/temp1_input");

    nvme4_temp = read_int_from_file(temp_path);

    printf("\n" BOLD "ASRock B650E Steel Legend" RESET "\n");
    printf("Mobo     : %.2f°C\n", mobo_temp >= 0 ? mobo_temp / 1000.0 : 0.0);
    printf("VRM      : %.2f°C\n", vrm_temp >= 0 ? vrm_temp / 1000.0 : 0.0);
    printf("Chipset  : %.2f°C\n", pch_temp >= 0 ? pch_temp / 1000.0 : 0.0);
    printf("\n");

    printf(BOLD "AMD Ryzen 7 7800X3D" RESET "\n");
    printf("Tctl     : %.2f°C\n", cpu_tctl >= 0 ? cpu_tctl / 1000.0 : 0.0);
    printf("Tccd     : %.2f°C\n", cpu_tccd >= 0 ? cpu_tccd / 1000.0 : 0.0);
    printf("Power    : %.2f W\n", cpu_power);
    printf("\n");

    printf(BOLD "AMD Radeon RX 6800 XT" RESET "\n");
    printf("Edge     : %.2f°C\n", gpu_edge >= 0 ? gpu_edge : 0.0);
    printf("Junction : %.2f°C\n", gpu_junction >= 0 ? gpu_junction : 0.0);
    printf("Mem      : %.2f°C\n", gpu_mem >= 0 ? gpu_mem : 0.0);
    printf("Power    : %.2f W\n", gpu_power);
    printf("\n");

    printf(BOLD "G-SKILL Trident Z5 Neo" RESET "\n");
    printf("Mem 1    : %.2f°C\n", mem1_temp >= 0 ? mem1_temp / 1000.0 : 0.0);
    printf("Mem 2    : %.2f°C\n", mem2_temp >= 0 ? mem2_temp / 1000.0 : 0.0);
    printf("\n");

    printf(BOLD "PNY CS3030 2TB" RESET "\n");
    printf("NAND     : %.2f°C\n", nvme1_temp >= 0 ? nvme1_temp / 1000.0 : 0.0);
    printf("\n");

    printf(BOLD "AddLink S70 2TB" RESET "\n");
    printf("NAND     : %.2f°C\n", nvme2_temp >= 0 ? nvme2_temp / 1000.0 : 0.0);
    printf("\n");

    printf(BOLD "AddLink S70 2TB" RESET "\n");
    printf("NAND     : %.2f°C\n", nvme3_temp >= 0 ? nvme3_temp / 1000.0 : 0.0);
    printf("\n");

    printf(BOLD "Transcend TS1TMTE220S 1TB" RESET "\n");
    printf("NAND     : %.2f°C\n", nvme4_temp >= 0 ? nvme4_temp / 1000.0 : 0.0);
    printf("\n");

    printf(BOLD "Lian Li Lancool II" RESET "\n");
    printf("Radiator : %d RPM\n", radiator_fan >= 0 ? radiator_fan : 0);
    printf("Top      : %d RPM\n", top_fans >= 0 ? top_fans : 0);
    printf("Bottom 1 : %d RPM\n", bottom1_fans >= 0 ? bottom1_fans : 0);
    printf("Bottom 2 : %d RPM\n", bottom2_fans >= 0 ? bottom2_fans : 0);

    return 0;
}
