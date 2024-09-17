#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define BOARD_NAME_PATH "/sys/devices/virtual/dmi/id/board_name"
#define BUFFER_SIZE 256
#define USEC 1000000

#define BOLD "\033[1m"
#define RESET "\033[0m"

int64_t get_cpuConsumptionUJoules()
{
    int64_t consumption = -1;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (file && fscanf(file, "%lld", &consumption) == 1)
        fclose(file);
    else
    {
        perror("Error reading RAPL energy file");

        if (file) fclose(file);
    }

    return consumption;
}

int64_t get_currentTimeUSec()
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) == 0)
        return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;

    perror("Error getting current time");

    return -1;
}

float calculate_cpu_power()
{
    int64_t initial_usage = get_cpuConsumptionUJoules();
    int64_t initial_time = get_currentTimeUSec();

    if (initial_usage == -1 || initial_time == -1)
    {
        fprintf(stderr, "Failed to read initial CPU consumption or time data\n");

        return 0.0f;
    }

    usleep(USEC);

    int64_t final_usage = get_cpuConsumptionUJoules();
    int64_t final_time = get_currentTimeUSec();

    if (final_usage == -1 || final_time == -1 || final_time < initial_time)
    {
        fprintf(stderr, "Failed to read final CPU consumption or time data, or time went backwards\n");

        return 0.0f;
    }

    int64_t time_diff_usec = final_time - initial_time;
    int64_t energy_diff_uj = final_usage - initial_usage;

    if (time_diff_usec <= 0 || energy_diff_uj < 0)
    {
        fprintf(stderr, "Invalid time or energy difference\n");

        return 0.0f;
    }

    return (float)(energy_diff_uj) / (time_diff_usec / (float)USEC);
}

int read_int_from_file(const char *path)
{
    FILE *file = fopen(path, "r");
    int value = -1;

    if (file && fscanf(file, "%d", &value) == 1)
        fclose(file);
    else
    {
        perror("Error reading file");

        if (file) fclose(file);
    }

    return value;
}

int get_nvme_device_model(const char *nvme_device, char *device_model, size_t size)
{
    char command[BUFFER_SIZE];

    snprintf(command, sizeof(command), "udevadm info --query=property --name=%sn1 | grep 'ID_MODEL='", nvme_device);
    FILE *fp = popen(command, "r");

    if (fp)
    {
        char line[BUFFER_SIZE];

        while (fgets(line, sizeof(line), fp))
        {
            if (strstr(line, "ID_MODEL=") != NULL)
            {
                char *start = strstr(line, "=");

                if (start)
                {
                    start++;
                    strncpy(device_model, start, size);

                    device_model[strcspn(device_model, "\n")] = 0;

                    pclose(fp);

                    return 0;
                }
            }
        }

        pclose(fp);
    }

    return -1;
}

int find_hwmon_path(const char *sensor_name, char *path, size_t size)
{
    char cmd[BUFFER_SIZE];

    snprintf(cmd, sizeof(cmd), "grep -l '%s' /sys/class/hwmon/hwmon*/name", sensor_name);
    FILE *fp = popen(cmd, "r");

    if (fp && fgets(path, size, fp))
    {
        pclose(fp);

        path[strcspn(path, "\n")] = 0;
        size_t len = strlen(path);

        if (len >= 5)
            path[len - 5] = '\0';
        else
            path[0] = '\0';

        return 0;
    }
    if (fp) pclose(fp);
    return -1;
}

int find_nvme_hwmon_path(const char *nvme_device, char *hwmon_path, size_t max_len)
{
    char command[BUFFER_SIZE];

    snprintf(command, sizeof(command), "udevadm info --query=path %s", nvme_device);
    FILE *fp = popen(command, "r");

    if (fp && fgets(hwmon_path, max_len, fp))
    {
        pclose(fp);

        hwmon_path[strcspn(hwmon_path, "\n")] = 0;

        snprintf(command, sizeof(command), "find /sys%s -type d -name 'hwmon*'", hwmon_path);
        fp = popen(command, "r");

        if (fp && fgets(hwmon_path, max_len, fp))
        {
            pclose(fp);

            hwmon_path[strcspn(hwmon_path, "\n")] = 0;

            return 0;
        }

        if (fp) pclose(fp);
    }

    return -1;
}

int read_board_name(char *board_name, size_t size)
{
    FILE *file = fopen(BOARD_NAME_PATH, "r");

    if (file)
    {
        if (fgets(board_name, size, file))
        {
            board_name[strcspn(board_name, "\n")] = 0;

            fclose(file);

            return 0;
        }

        fclose(file);
    }

    perror("Error reading board name file");

    return -1;
}

int main()
{
    char board_name[BUFFER_SIZE], hwmon_path[BUFFER_SIZE], nvme_device_model[BUFFER_SIZE], temp_path[BUFFER_SIZE];
    int mobo_temp, vrm_temp, pch_temp;
    int radiator_fan, top_fans, bottom1_fans, bottom2_fans;
    int cpu_tctl, cpu_tccd;
    float cpu_power = 0.0f, gpu_edge = 0.0f, gpu_junction = 0.0f, gpu_mem = 0.0f, gpu_power = 0.0f;
    int nvme_temps[4] = {-1, -1, -1, -1};
    int dram_temps[2] = {-1, -1};

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
        fprintf(stderr, "nct668* sensor module not found!\n");

        return 1;
    }

    if (find_hwmon_path("k10temp", hwmon_path, sizeof(hwmon_path)) == 0)
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

        return 1;
    }

    if (find_hwmon_path("amdgpu", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", hwmon_path);
        gpu_edge = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/temp2_input", hwmon_path);
        gpu_junction = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/temp3_input", hwmon_path);
        gpu_mem = read_int_from_file(temp_path);

        snprintf(temp_path, sizeof(temp_path), "%s/power1_average", hwmon_path);
        gpu_power = read_int_from_file(temp_path);
    }
    else
    {
        fprintf(stderr, "amdgpu sensor module not found!\n");

        return 1;
    }

    if (read_board_name(board_name, sizeof(board_name)) == 0)
    {
        printf("\n" BOLD "%s" RESET "\n", board_name);
    }
    else
    {
        printf(BOLD "Unknown Motherboard" RESET "\n");
    }

    printf("Mobo     : %.2f°C\n", mobo_temp >= 0 ? mobo_temp / 1000.0 : 0.0);
    printf("VRM      : %.2f°C\n", vrm_temp >= 0 ? vrm_temp / 1000.0 : 0.0);
    printf("Chipset  : %.2f°C\n", pch_temp >= 0 ? pch_temp / 1000.0 : 0.0);
    printf("\n");

    printf(BOLD "AMD Ryzen 7 7800X3D" RESET "\n");
    printf("Tctl     : %.2f°C\n", cpu_tctl >= 0 ? cpu_tctl / 1000.0 : 0.0);
    printf("Tccd     : %.2f°C\n", cpu_tccd >= 0 ? cpu_tccd / 1000.0 : 0.0);
    printf("Power    : %.2f W\n", cpu_power / USEC);
    printf("\n");

    printf(BOLD "AMD Radeon RX 6800 XT" RESET "\n");
    printf("Edge     : %.2f°C\n", gpu_edge >= 0 ? gpu_edge / 1000.0 : 0.0);
    printf("Junction : %.2f°C\n", gpu_junction >= 0 ? gpu_junction / 1000.0 : 0.0);
    printf("Mem      : %.2f°C\n", gpu_mem >= 0 ? gpu_mem / 1000.0 : 0.0);
    printf("Power    : %.2f W\n", gpu_power / USEC);
    printf("\n");

    printf(BOLD "G-SKILL Trident Z5 Neo" RESET "\n");

    for (int i = 0; i < 2; i++)
    {
        snprintf(hwmon_path, sizeof(hwmon_path), "/sys/class/hwmon/hwmon%d", 8 + i);

        snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", hwmon_path);
        dram_temps[i] = read_int_from_file(temp_path);

        printf("DRAM %d   : %.2f°C\n", i + 1, dram_temps[i] >= 0 ? dram_temps[i] / 1000.0 : 0.0);
    }

    printf("\n");

    for (int i = 0; i < 4; i++)
    {
        char nvme_device[BUFFER_SIZE];

        snprintf(nvme_device, sizeof(nvme_device), "/dev/nvme%d", i);

        if (find_nvme_hwmon_path(nvme_device, hwmon_path, sizeof(hwmon_path)) == 0)
        {
            snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", hwmon_path);
            nvme_temps[i] = read_int_from_file(temp_path);

            if (get_nvme_device_model(nvme_device, nvme_device_model, sizeof(nvme_device_model)) == 0)
                printf(BOLD "%s" RESET "\n", nvme_device_model);
            else
                printf(BOLD "NVMe %d: Model name not found" RESET "\n", i + 1);

            printf("NAND     : %.2f°C\n", nvme_temps[i] >= 0 ? nvme_temps[i] / 1000.0 : 0.0);
            printf("\n");
        }
        else
            printf("Failed to find hwmon path for %s\n", nvme_device);
    }

    printf(BOLD "Lian Li Lancool II" RESET "\n");
    printf("Radiator : %d RPM\n", radiator_fan >= 0 ? radiator_fan : 0);
    printf("Top      : %d RPM\n", top_fans >= 0 ? top_fans : 0);
    printf("Bottom 1 : %d RPM\n", bottom1_fans >= 0 ? bottom1_fans : 0);
    printf("Bottom 2 : %d RPM\n", bottom2_fans >= 0 ? bottom2_fans : 0);

    return 0;
}
