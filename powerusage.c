#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>
#include <stdbool.h>
#include <unistd.h>
#include <sys/time.h>

#define RAPL_FILE_PATH  "/sys/class/powercap/intel-rapl:0/energy_uj" // Path ke file konsumsi energi CPU
#define MAX_PROCESSES   100 // Jumlah maksimum proses yang diperiksa
#define MAX_NAME_LENGTH 256 // Panjang maksimum nama proses
#define USEC            1000000 // Mikrodetik dalam satu detik
#define KILO            1000 // Ukuran buffer dalam byte
#define TO_GB           (1024.0 * 1024.0) // Konversi dari kB ke GB

int64_t get_memory_usage();
int64_t get_cpuConsumptionUJoules();
int64_t get_currentTimeUSec();
float calculate_cpu_power();
char* execute_command(const char* command);
bool is_process_running_native(const char* process_name);
int load_process_names(const char* config_file, char process_list[MAX_PROCESSES][MAX_NAME_LENGTH]);
bool is_any_process_running(char process_list[MAX_PROCESSES][MAX_NAME_LENGTH], int process_count);
void print_cpu_info();
void print_amd_gpu_info();
void print_nvidia_gpu_info();
int detect_gpu_type();

int64_t get_memory_usage()
{
    FILE *file = fopen("/proc/meminfo", "r");

    if (file == NULL)
    {
        perror("fopen");

        return -1;
    }

    char buffer[MAX_NAME_LENGTH];
    int64_t total_memory = 0, available_memory = 0;
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, file);
    buffer[bytes_read] = '\0';

    fclose(file);

    char *line = strtok(buffer, "\n");

    while (line != NULL)
    {
        if (sscanf(line, "MemTotal: %ld kB", &total_memory) == 1)
        {
            // Read total memory
        }
        else if (sscanf(line, "MemAvailable: %ld kB", &available_memory) == 1)
        {
            break;
        }

        line = strtok(NULL, "\n");
    }

    if (total_memory == 0 || available_memory == 0)
        return -1;

    return (total_memory - available_memory) / TO_GB;
}

int64_t get_cpuConsumptionUJoules()
{
    int64_t consumption;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (!file || fscanf(file, "%lld", &consumption) != 1)
    {
        perror("Gagal membaca konsumsi energi/daya!");

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
        perror("Gagal mendapatkan waktu saat ini!");

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

    sleep(1);

    int64_t final_usage = get_cpuConsumptionUJoules();
    int64_t final_time = get_currentTimeUSec();

    if (final_usage == -1 || final_time == -1 || final_time <= initial_time || final_usage <= initial_usage)
        return -1.0f;

    return (float)((final_usage - initial_usage) / ((final_time - initial_time) / USEC * USEC));
}

char* execute_command(const char* command)
{
    FILE* fp = popen(command, "r");

    if (!fp)
        return NULL;

    char* output = malloc(KILO);

    if (!output)
        return NULL;

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

            if (fp)
                fclose(fp);
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

    if (cpu_temperature1 && cpu_temperature2 && used_memory_gb >= 0)
        printf("   %.1f GB |    %.0f °C |    %.0f °C | 󰚥 %.0f W\n",
               used_memory_gb, atof(cpu_temperature1), atof(cpu_temperature2), cpu_power);

    free(cpu_temperature1);
    free(cpu_temperature2);
}

void print_amd_gpu_info()
{
    char* gpu_usage = execute_command("rocm-smi -d 0 --showuse | awk '/GPU use \\(%\\)/ {print $NF}'");
    char* gpu_temperature1 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor edge\\) \\(C\\):/ {print $NF}'");
    char* gpu_temperature2 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor junction\\) \\(C\\):/ {print $NF}'");
    char* gpu_temperature3 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor memory\\) \\(C\\):/ {print $NF}'");
    char* gpu_power = execute_command("rocm-smi -P | awk '/Average Graphics Package Power \\(W\\):/ {print $NF}'");

    if (gpu_temperature1 && gpu_temperature2 && gpu_temperature3 && gpu_usage && gpu_power)
        printf("   %.0f %% |    %.0f °C |    %.0f °C |    %.0f °C | 󰚥 %.0f W\n",
               atof(gpu_usage), atof(gpu_temperature1), atof(gpu_temperature2), atof(gpu_temperature3), atof(gpu_power));

    free(gpu_usage);
    free(gpu_temperature1);
    free(gpu_temperature2);
    free(gpu_temperature3);
    free(gpu_power);
}

void print_nvidia_gpu_info()
{
    char* gpu_usage = execute_command("nvidia-smi --query-gpu=utilization.gpu --format=csv,noheader,nounits");
    char* gpu_vram = execute_command("nvidia-smi --query-gpu=memory.used --format=csv,noheader,nounits");
    char* gpu_temperature1 = execute_command("nvidia-smi --query-gpu=temperature.gpu --format=csv,noheader,nounits");
    char* gpu_power = execute_command("nvidia-smi --query-gpu=power.draw --format=csv,noheader,nounits");

    if (gpu_temperature1 && gpu_usage && gpu_power && gpu_vram)
    {
        float vram_gb = atof(gpu_vram) / 1024.0;

        printf("   %.0f %% |    %.1f GB |    %.0f °C | 󰚥 %.0f W\n",
               atof(gpu_usage), vram_gb, atof(gpu_temperature1), atof(gpu_power));
    }

    free(gpu_usage);
    free(gpu_temperature1);
    free(gpu_power);
}

int detect_gpu_type()
{
    char* amd_check = execute_command("rocm-smi --showid 2>/dev/null | grep -i 'GPU' >/dev/null && echo 'AMD'");

    if (amd_check && strstr(amd_check, "AMD"))
    {
        free(amd_check);

        return 1; // AMD
    }

    free(amd_check);

    char* nvidia_check = execute_command("nvidia-smi >/dev/null 2>&1 && echo 'NVIDIA'");

    if (nvidia_check && strstr(nvidia_check, "NVIDIA"))
    {
        free(nvidia_check);

        return 2; // NVIDIA
    }

    free(nvidia_check);

    return 0;
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
    {
        int gpu_type = detect_gpu_type();

        switch (gpu_type)
        {
            case 1: // AMD
                print_amd_gpu_info();

                break;
            case 2: // NVIDIA
                print_nvidia_gpu_info();

                break;
            default:
                fprintf(stderr, "Tidak ada GPU yang kompatibel!\n");

                return 1;
        }
    } else {
        fprintf(stderr, "Salah mode: %s. Gunakan 'cpu' atau 'gpu'.\n", argv[2]);

        return 1;
    }

    return 0;
}
