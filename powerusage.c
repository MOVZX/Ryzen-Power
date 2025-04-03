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
#include <nvml.h>

#define RAPL_FILE_PATH          "/sys/class/powercap/intel-rapl:0/energy_uj"
#define MAX_PROCESSES           100
#define MAX_NAME_LENGTH         300
#define USEC                    1000000
#define KILO                    1000
#define TO_GB                   (1024.0 * 1024.0)
#define VRAM_REGISTER_OFFSET    0x0000E2A8
#define HOTSPOT_REGISTER_OFFSET 0x0002046c
#define PG_SZ                   sysconf(_SC_PAGE_SIZE)
#define MEM_PATH                "/dev/mem"
#define MAX_DEVICES             1 // Sesuaikan dengan jumlah GPU yang ada

float get_cpu_usage(void);
int64_t get_memory_usage(void);
int64_t get_cpuConsumptionUJoules(void);
int64_t get_currentTimeUSec(void);
float calculate_cpu_power(void);
char *execute_command(const char *command);
bool is_process_running_native(const char *process_name);
int load_process_names(const char *config_file, char process_list[MAX_PROCESSES][MAX_NAME_LENGTH]);
bool is_any_process_running(char process_list[MAX_PROCESSES][MAX_NAME_LENGTH], int process_count);
void print_cpu_info(void);
void print_amd_gpu_info(void);
void print_nvidia_gpu_info(void);
int detect_gpu_type(void);

typedef struct
{
    unsigned int vram_temp;
    unsigned int hotspot_temp;
    unsigned int gpu_temp;
    unsigned int power_usage;
    unsigned int gpu_util;
    float fb_used;
} NvidiaDeviceData;

static NvidiaDeviceData nvidia_devices[MAX_DEVICES];
static int nvidia_fd            = -1;
static void *nvidia_map_base    = MAP_FAILED;
static int gpu_type             = 0;

float get_cpu_usage(void)
{
    char *cpu_usage = execute_command("top -bn1 | grep 'Cpu(s)' | awk '{print $2}'");

    if (!cpu_usage)
        return -1.0f;

    float usage = atof(cpu_usage);

    free(cpu_usage);

    return usage;
}

int64_t get_memory_usage(void)
{
    FILE *file = fopen("/proc/meminfo", "r");

    if (!file)
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

    while (line)
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

int64_t get_cpuConsumptionUJoules(void)
{
    int64_t consumption;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (!file || fscanf(file, "%ld", &consumption) != 1)
    {
        perror("Gagal membaca konsumsi energi/daya!");

        if (file)
            fclose(file);

        return -1;
    }

    fclose(file);

    return consumption;
}

int64_t get_currentTimeUSec(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("Gagal mendapatkan waktu saat ini!");

        return -1;
    }

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

float calculate_cpu_power(void)
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

char *execute_command(const char *command)
{
    FILE *fp = popen(command, "r");

    if (!fp)
        return NULL;

    char *output = malloc(KILO);

    if (!output)
        return NULL;

    if (!fgets(output, KILO, fp))
    {
        free(output);

        output = NULL;
    }
    else
    {
        strtok(output, "\n");
    }

    pclose(fp);

    return output;
}

bool is_process_running_native(const char *process_name)
{
    DIR *dir = opendir("/proc");

    if (!dir)
        return false;

    struct dirent *entry;
    char path[MAX_NAME_LENGTH], pname[MAX_NAME_LENGTH], buffer[MAX_NAME_LENGTH];

    while ((entry = readdir(dir)))
    {
        if (isdigit(entry->d_name[0]))
        {
            snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);

            FILE *fp = fopen(path, "r");

            if (fp && fgets(buffer, sizeof(buffer), fp) && sscanf(buffer, "%s", pname) && !strcmp(pname, process_name))
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

int load_process_names(const char *config_file, char process_list[MAX_PROCESSES][MAX_NAME_LENGTH])
{
    FILE *fp = fopen(config_file, "r");

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

void print_cpu_info(void)
{
    char *cpu_temperature1 = execute_command("sensors k10temp-pci-* | awk -F '[:°C]' '/Tctl:/ {print $2}'");
    char *cpu_temperature2 = execute_command("sensors k10temp-pci-* | awk -F '[:°C]' '/Tccd1:/ {print $2}'");
    float cpu_usage = get_cpu_usage();
    float cpu_power = calculate_cpu_power();
    float used_memory_gb = get_memory_usage();

    if (cpu_temperature1 && cpu_temperature2 && used_memory_gb >= 0 && cpu_usage >= 0)
        printf("󰻠   %.0f %% |    %.1f GB |    %.0f °C |    %.0f °C | 󰚥 %.0f W\n",
               cpu_usage, used_memory_gb, atof(cpu_temperature1), atof(cpu_temperature2), cpu_power);
    free(cpu_temperature1);
    free(cpu_temperature2);
}

void print_amd_gpu_info(void)
{
    char *gpu_usage = execute_command("rocm-smi -d 0 --showuse | awk '/GPU use \\(%\\)/ {print $NF}'");
    char *gpu_temperature1 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor edge\\) \\(C\\):/ {print $NF}'");
    char *gpu_temperature2 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor junction\\) \\(C\\):/ {print $NF}'");
    char *gpu_temperature3 = execute_command("rocm-smi -t | awk '/Temperature \\(Sensor memory\\) \\(C\\):/ {print $NF}'");
    char *gpu_power = execute_command("rocm-smi -P | awk '/Average Graphics Package Power \\(W\\):/ {print $NF}'");

    if (gpu_temperature1 && gpu_temperature2 && gpu_temperature3 && gpu_usage && gpu_power)
        printf("󰻠   %.0f %% |    %.0f °C |    %.0f °C |    %.0f °C | 󰚥 %.0f W\n",
               atof(gpu_usage), atof(gpu_temperature1), atof(gpu_temperature2), atof(gpu_temperature3), atof(gpu_power));

    free(gpu_usage);
    free(gpu_temperature1);
    free(gpu_temperature2);
    free(gpu_temperature3);
    free(gpu_power);
}

void print_nvidia_gpu_info(void)
{
    if (nvmlInit() != NVML_SUCCESS)
    {
        fprintf(stderr, "Failed to initialize NVML\n");

        return;
    }

    unsigned int device_count;

    if (nvmlDeviceGetCount(&device_count) != NVML_SUCCESS || device_count == 0)
    {
        nvmlShutdown();

        return;
    }

    struct pci_access *pacc = pci_alloc();

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

        // NVIDIA: Penggunaan GPU
        nvmlUtilization_t util;
        nvidia_devices[i].gpu_util = (nvmlDeviceGetUtilizationRates(device, &util) == NVML_SUCCESS) ? util.gpu : 0;

        // NVIDIA: Penggunaan VRAM
        nvmlMemory_t mem;
        nvidia_devices[i].fb_used = (nvmlDeviceGetMemoryInfo(device, &mem) == NVML_SUCCESS) ? mem.used / (1024.0f * 1024.0f) : 0;

        // NVIDIA: Suhu Core
        unsigned int temp;
        nvidia_devices[i].gpu_temp = (nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp) == NVML_SUCCESS) ? temp : 0;

        // NVIDIA: Power (Watt)
        unsigned int power;
        nvidia_devices[i].power_usage = (nvmlDeviceGetPowerUsage(device, &power) == NVML_SUCCESS) ? power / 1000 : 0;

        // NVML
        for (struct pci_dev *dev = pacc->devices; dev; dev = dev->next)
        {
            pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);

            unsigned int dev_id = (unsigned int)((dev->device_id << 16) | dev->vendor_id);

            if (dev_id != pciInfo.pciDeviceId ||
                (unsigned int)dev->domain != pciInfo.domain ||
                dev->bus != pciInfo.bus ||
                dev->dev != pciInfo.device)
            {
                continue;
            }

            nvidia_fd = open(MEM_PATH, O_RDWR | O_SYNC);

            if (nvidia_fd < 0)
                break;

            // NVIDIA: Suhu VRAM
            uint32_t vram_addr = (dev->base_addr[0] & 0xFFFFFFFF) + VRAM_REGISTER_OFFSET;
            nvidia_map_base = mmap(NULL, PG_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, nvidia_fd, vram_addr & ~(PG_SZ - 1));

            if (nvidia_map_base != MAP_FAILED)
            {
                uint32_t *vram_reg = (uint32_t *)((char *)nvidia_map_base + (vram_addr & (PG_SZ - 1)));
                nvidia_devices[i].vram_temp = (*vram_reg & 0x00000fff) / 0x20;

                munmap(nvidia_map_base, PG_SZ);

                nvidia_map_base = MAP_FAILED;
            }

            // NVIDIA: Suhu Hotspot
            uint32_t hotspot_addr = (dev->base_addr[0] & 0xFFFFFFFF) + HOTSPOT_REGISTER_OFFSET;
            void *hotspot_base = mmap(NULL, PG_SZ, PROT_READ, MAP_SHARED, nvidia_fd, hotspot_addr & ~(PG_SZ - 1));

            if (hotspot_base != MAP_FAILED)
            {
                uint32_t *hotspot_reg = (uint32_t *)((char *)hotspot_base + (hotspot_addr & (PG_SZ - 1)));
                uint32_t hotspot_temp = (*hotspot_reg >> 8) & 0xff;
                nvidia_devices[i].hotspot_temp = (hotspot_temp < 0x7f) ? hotspot_temp : 0;

                munmap(hotspot_base, PG_SZ);
            }

            close(nvidia_fd);

            nvidia_fd = -1;

            break;
        }

        printf("󰻠   %u %% |    %.1f GB |    %u °C |    %u °C |    %u °C | 󰚥 %u W\n",
               nvidia_devices[i].gpu_util,
               nvidia_devices[i].fb_used / 1000,
               nvidia_devices[i].gpu_temp,
               nvidia_devices[i].hotspot_temp,
               nvidia_devices[i].vram_temp,
               nvidia_devices[i].power_usage);
        break;
    }

    pci_cleanup(pacc);
    nvmlShutdown();
}

int detect_gpu_type(void)
{
    if (gpu_type)
        return gpu_type;

    char *amd_check = execute_command("rocm-smi --showid 2>/dev/null | grep -i 'GPU' >/dev/null && echo 'AMD'");

    if (amd_check && strstr(amd_check, "AMD"))
    {
        free(amd_check);

        return 1; // AMD
    }

    free(amd_check);

    char *nvidia_check = execute_command("nvidia-smi >/dev/null 2>&1 && echo 'NVIDIA'");

    if (nvidia_check && strstr(nvidia_check, "NVIDIA"))
    {
        free(nvidia_check);

        return 2; // NVIDIA
    }

    free(nvidia_check);

    return 0;
}

int main(int argc, char *argv[])
{
    if (argc < 3)
    {
        fprintf(stderr, "Sintaks: powerusage CONFIG CPU_GPU, Contoh: powerusage ~/.config/daftar_hitam.conf cpu\n");

        return 1;
    }

    char process_list[MAX_PROCESSES][MAX_NAME_LENGTH];
    int process_count = load_process_names(argv[1], process_list);

    if (process_count == -1 || is_any_process_running(process_list, process_count))
        return 1;

    if (!strcmp(argv[2], "cpu"))
    {
        print_cpu_info();
    }
    else if (!strcmp(argv[2], "gpu"))
    {
        gpu_type = detect_gpu_type();

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
    }
    else
    {
        fprintf(stderr, "Salah mode: %s. Gunakan 'cpu' atau 'gpu'.\n", argv[2]);

        return 1;
    }

    return 0;
}
