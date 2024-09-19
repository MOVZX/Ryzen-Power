#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/time.h>
#include <stdint.h>

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define MAX_PROCESSES 100
#define MAX_NAME_LENGTH 256
#define USEC 1000000
#define KILO 1000

float get_memory_usage()
{
    FILE *file = fopen("/proc/meminfo", "r");

    if (file == NULL)
    {
        perror("fopen");

        exit(EXIT_FAILURE);
    }

    long total_memory = 0;
    long free_memory = 0;

    fscanf(file, "MemTotal: %ld kB\n", &total_memory);
    fscanf(file, "MemFree: %ld kB\n", &free_memory);

    fclose(file);

    long used_memory = total_memory - free_memory;
    // *used_memory_percent = ((float)used_memory / total_memory) * 100.0f; -- persen
    return (float)used_memory / USEC;
}

int64_t get_cpuConsumptionUJoules()
{
    int64_t consumption;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (!file || fscanf(file, "%lld", &consumption) != 1)
    {
        perror("Error reading energy consumption!");

        if (file)
            fclose(file);

        return -1;
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

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

float calculate_cpu_power()
{
    int64_t initial_usage = get_cpuConsumptionUJoules();
    int64_t initial_time = get_currentTimeUSec();

    if (initial_usage == -1 || initial_time == -1)
        return -1.0f;

    usleep(USEC);

    int64_t final_usage = get_cpuConsumptionUJoules();
    int64_t final_time = get_currentTimeUSec();

    if (final_usage == -1 || final_time == -1 || final_time <= initial_time || final_usage <= initial_usage)
        return -1.0f;

    return (float)((final_usage - initial_usage) / ((final_time - initial_time) / USEC * USEC));
}

char* execute_command(const char* command)
{
    FILE* fp = popen(command, "r");

    if (!fp) return NULL;

    char* output = malloc(KILO);

    if (!output) return NULL;

    if (!fgets(output, KILO, fp))
    {
        free(output);
        output = NULL;
    }
    else
        strtok(output, "\n");

    pclose(fp);

    return output;
}

bool is_process_running_native(const char* process_name)
{
    DIR* dir = opendir("/proc");

    if (!dir)
        return false;

    struct dirent* entry;

    char path[MAX_NAME_LENGTH], pname[MAX_NAME_LENGTH], buffer[MAX_NAME_LENGTH];

    while ((entry = readdir(dir)))
    {
        if (isdigit(entry->d_name[0]))
        {
            snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);

            FILE* fp = fopen(path, "r");

            if (fp && fgets(buffer, sizeof(buffer), fp) && sscanf(buffer, "%s", pname) && strcmp(pname, process_name) == 0)
            {
                fclose(fp);
                closedir(dir);

                return true;
            }

            if (fp) fclose(fp);
        }
    }

    closedir(dir);

    return false;
}

int load_process_names(const char* config_file, char process_list[MAX_PROCESSES][MAX_NAME_LENGTH])
{
    FILE* fp = fopen(config_file, "r");

    if (!fp)
        return -1;

    char line[MAX_NAME_LENGTH];
    int count = 0;

    while (fgets(line, sizeof(line), fp) && count < MAX_PROCESSES)
    {
        strtok(line, "\n");
        strncpy(process_list[count++], line, MAX_NAME_LENGTH);
    }

    fclose(fp);

    return count;
}

bool is_any_process_running(char process_list[MAX_PROCESSES][MAX_NAME_LENGTH], int process_count)
{
    for (int i = 0; i < process_count; i++)
        if (is_process_running_native(process_list[i]))
            return true;

    return false;
}

void print_cpu_info()
{
    char* cpu_temperature1 = execute_command("sensors k10temp-pci-* | awk -F '[:°C]' '/Tctl:/ {print $2}'");
    char* cpu_temperature2 = execute_command("sensors k10temp-pci-* | awk -F '[:°C]' '/Tccd1:/ {print $2}'");
    float cpu_power = calculate_cpu_power();
    float used_memory_gb = get_memory_usage();

    if (cpu_temperature1 && cpu_temperature2 && used_memory_gb)
        printf("   %.0f GB |    %.0f °C |    %.0f °C | 󰚥 %.0f W\n", used_memory_gb, atof(cpu_temperature1), atof(cpu_temperature2), cpu_power);

    free(cpu_temperature1);
    free(cpu_temperature2);
}

void print_gpu_info()
{
    char* gpu_usage = execute_command("rocm-smi -d 0 --showuse | awk '/GPU use \\(%\\)/ {print $NF}'");
    char* gpu_temperature1 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor edge\\) \\(C\\):/ {print $NF}'");
    char* gpu_temperature2 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor junction\\) \\(C\\):/ {print $NF}'");
    char* gpu_temperature3 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor memory\\) \\(C\\):/ {print $NF}'");
    char* gpu_power = execute_command("rocm-smi -P | awk '/Average Graphics Package Power \\(W\\):/ {print $NF}'");

    if (gpu_temperature1 && gpu_temperature2 && gpu_temperature3 && gpu_usage)
        printf("   %.0f \% |    %.0f °C |    %.0f °C |    %.0f °C | 󰚥 %.0f W\n", atof(gpu_usage), atof(gpu_temperature1), atof(gpu_temperature2), atof(gpu_temperature3), atof(gpu_power));

    free(gpu_usage);
    free(gpu_temperature1);
    free(gpu_temperature2);
    free(gpu_temperature3);
    free(gpu_power);
}

int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Sintaks: powerusage CONFIG CPU_GPU, Contoh: powerusage ~/.config/daftar_hitam.conf cpu\n", argv[0]);

        return 1;
    }

    char process_list[MAX_PROCESSES][MAX_NAME_LENGTH];
    int process_count = load_process_names(argv[1], process_list);

    if (process_count == -1 || is_any_process_running(process_list, process_count))
        return 1;

    if (strcmp(argv[2], "cpu") == 0)
        print_cpu_info();
    else if (strcmp(argv[2], "gpu") == 0)
        print_gpu_info();
    else
        fprintf(stderr, "Salah mode: %s. Gunakan 'cpu' atau 'gpu'.\n", argv[2]);

    return 0;
}
