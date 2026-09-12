/*
 * sens - Utilitas untuk memantau sensor perangkat keras.
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>
#include <limits.h>
#include <glob.h>

#ifdef NVIDIA_GPU
#include <stdbool.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <pci/pci.h>
#endif

#include "sensors_common.h"

#define BUFFER_SIZE 255
#define MAX_CPU_SENSORS 16

#ifdef NVIDIA_GPU
#define NVIDIA_MEM_PATH "/dev/mem"
#define NVIDIA_VENDOR_ID 0x10de
#define NVIDIA_VRAM_TEMP_MASK 0x00000fff
#define NVIDIA_VRAM_TEMP_DIVISOR 32
#endif

#define BOLD "\033[1m"
#define RESET "\033[0m"

/**
 * @brief Mencetak informasi sistem (produsen dan produk).
 *
 * Sysfs dibaca lebih dulu karena board_vendor dan board_name punya mode 0444,
 * jadi terbaca tanpa root. dmidecode hanya dipakai kalau sysfs tidak lengkap.
 */
static void print_system_info(void)
{
    char vendor[128] = "";
    char product[128] = "";

    int have_vendor = (read_dmi_field("board_vendor", vendor, sizeof(vendor)) == 0);
    int have_product = (read_dmi_field("board_name", product, sizeof(product)) == 0);

    if (!have_vendor)
    {
        char *dmidecode_vendor = execute_command("dmidecode -s system-manufacturer");

        if (dmidecode_vendor != NULL)
        {
            snprintf(vendor, sizeof(vendor), "%s", dmidecode_vendor);

            free(dmidecode_vendor);
        }
    }

    if (!have_product)
    {
        char *dmidecode_product = execute_command("dmidecode -s system-product-name");

        if (dmidecode_product != NULL)
        {
            snprintf(product, sizeof(product), "%s", dmidecode_product);

            free(dmidecode_product);
        }
    }

    if (vendor[0] == '\0' && product[0] == '\0')
        printf(BOLD "System" RESET "\n");
    else
        printf(BOLD "%s %s" RESET "\n", vendor, product);
}

/**
 * @brief Mencetak suhu motherboard, VRM, chipset, dan kecepatan kipas.
 *
 * Channel sensor pada chip super I/O keluarga NCT67xx:
 *   temp1 = SYSTIN  : suhu motherboard (sensor SYS on board)
 *   temp3 = AUXTIN0 : VRM / MOS
 * Channel lain pada board ini nilainya tidak masuk akal (mis. AUXTIN1 17 °C),
 * jadi tidak dipakai. Suhu chipset diambil dari chip prom21_xhci, bukan dari
 * super I/O, karena label PCH_CHIP_TEMP pada nct6799 selalu 0 di AM5.
 */
static void print_motherboard_and_fan_info(void)
{
    char hwmon_path[PATH_MAX];
    int mobo_temp = -1, vrm_temp = -1;
    int pch_temp1 = -1, pch_temp2 = -1;
    int radiator_fan = -1, front_fan = -1, rear_fan = -1, pump_fan = -1, bottom1_fans = -1, bottom2_fans = -1;

    if (find_hwmon_path_by_prefix("nct6", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        mobo_temp = read_hwmon_temp(hwmon_path, "temp1_input");
        vrm_temp = read_hwmon_temp(hwmon_path, "temp3_input");

        /* fan1 = Radiator, fan2 = Front, fan3 = Rear, fan4 = AIO Pump,
         * fan6 = Bottom 1, fan7 = Bottom 2 (fan5 kosong) */
        radiator_fan = read_hwmon_temp(hwmon_path, "fan1_input");
        front_fan = read_hwmon_temp(hwmon_path, "fan2_input");
        rear_fan = read_hwmon_temp(hwmon_path, "fan3_input");
        pump_fan = read_hwmon_temp(hwmon_path, "fan4_input");
        bottom1_fans = read_hwmon_temp(hwmon_path, "fan6_input");
        bottom2_fans = read_hwmon_temp(hwmon_path, "fan7_input");
    }
    else
    {
        fprintf(stderr, "Super I/O (nct6x) sensor module not found!\n");
    }

    get_pch_temps(&pch_temp1, &pch_temp2);

    printf("%-8s : %.2f°C\n", "Mobo", mobo_temp != -1 ? mobo_temp / 1000.0 : 0.0);
    printf("%-8s : %.2f°C\n", "VRM", vrm_temp != -1 ? vrm_temp / 1000.0 : 0.0);
    printf("%-8s : %.2f°C\n", "PCH 1", pch_temp1 != -1 ? pch_temp1 / 1000.0 : 0.0);
    printf("%-8s : %.2f°C\n", "PCH 2", pch_temp2 != -1 ? pch_temp2 / 1000.0 : 0.0);
    printf("\n");

    printf("%-8s : %d RPM\n", "Radiator", radiator_fan != -1 ? radiator_fan : 0);
    printf("%-8s : %d RPM\n", "Front", front_fan != -1 ? front_fan : 0);
    printf("%-8s : %d RPM\n", "Rear", rear_fan != -1 ? rear_fan : 0);
    printf("%-8s : %d RPM\n", "Pump", pump_fan != -1 ? pump_fan : 0);
    printf("%-8s : %d RPM\n", "Bottom 1", bottom1_fans != -1 ? bottom1_fans : 0);
    printf("%-8s : %d RPM\n", "Bottom 2", bottom2_fans != -1 ? bottom2_fans : 0);
    printf("\n");
}

/**
 * @brief Mencetak suhu dan daya CPU.
 *
 * Nama CPU diambil dari /proc/cpuinfo supaya tidak butuh root. dmidecode hanya
 * dipakai sebagai cadangan.
 */
static void print_cpu_info(void)
{
    char hwmon_path[PATH_MAX];
    char cpu_name[256] = "";
    TempSensor sensors[MAX_CPU_SENSORS];
    int sensor_count = 0;
    float cpu_power = -1.0f;

    if (find_hwmon_path_by_name("k10temp", hwmon_path, sizeof(hwmon_path)) == 0)
    {
        sensor_count = read_labelled_temps(hwmon_path, sensors, MAX_CPU_SENSORS);
        cpu_power = measure_cpu_power();
    }
    else
    {
        fprintf(stderr, "k10temp sensor module not found!\n");
    }

    if (get_cpu_model_name(cpu_name, sizeof(cpu_name)) != 0)
    {
        char *processor_name = execute_command("dmidecode -s processor-version");

        if (processor_name != NULL)
        {
            snprintf(cpu_name, sizeof(cpu_name), "%s", processor_name);

            free(processor_name);
        }
    }

    printf(BOLD "%s" RESET "\n", cpu_name[0] != '\0' ? cpu_name : "CPU");

    for (int i = 0; i < sensor_count; i++)
        printf("%-8s : %.2f°C\n", sensors[i].label, sensors[i].value != -1 ? sensors[i].value / 1000.0 : 0.0);

    if (cpu_power >= 0)
        printf("Power    : %.2f W\n", cpu_power);
    else
        printf("Power    : N/A\n");

    printf("\n");
}

#ifdef ENABLE_DRAM
/**
 * @brief Mencetak model dan suhu DRAM.
 *
 * Nomor bagian DIMM hanya ada di tabel SMBIOS, jadi bagian ini yang masih
 * membutuhkan dmidecode (dan root). Suhu dibaca dari sensor spd5118 di sysfs.
 */
static void print_dram_info(void)
{
    char *dram_model = execute_command("dmidecode -t memory | grep -m1 'Part Number:' | sed 's/.*Part Number:[[:space:]]*//' | sed 's/[[:space:]]*$//'");

    printf(BOLD "%s" RESET "\n", dram_model ? dram_model : "DRAM");

    int dram_temps[2] = {-1, -1};
    char dram_paths[2][PATH_MAX];
    int dram_count = find_all_hwmon_by_name("spd5118", dram_paths, 2);

    for (int i = 0; i < dram_count; i++)
        dram_temps[i] = read_hwmon_temp(dram_paths[i], "temp1_input");

    printf("DRAM 1   : %.2f°C\n", dram_temps[0] != -1 ? dram_temps[0] / 1000.0 : 0.0);
    printf("DRAM 2   : %.2f°C\n", dram_temps[1] != -1 ? dram_temps[1] / 1000.0 : 0.0);
    printf("\n");

    free(dram_model);
}
#endif /* ENABLE_DRAM */

#ifdef NVIDIA_GPU
/**
 * @brief Membaca suhu dan daya GPU NVIDIA menggunakan nvidia-smi.
 *
 * @param temperature Pointer untuk menyimpan suhu GPU.
 * @param power Pointer untuk menyimpan penarikan daya GPU.
 * @param bus_id Buffer untuk bus_id GPU; boleh NULL.
 * @param bus_id_size Ukuran buffer bus_id.
 *
 * @note Tidak efisien karena memanggil `nvidia-smi` melalui `popen`.
 */
static void read_nvidia_gpu_info(float *temperature, float *power, char *bus_id, size_t bus_id_size)
{
    /* nounits perlu supaya power.draw tidak membawa " W" di belakang angka;
     * kalau tidak, field ketiga (pci.bus_id) tidak akan ter-parse. */
    char command[BUFFER_SIZE] = "nvidia-smi --query-gpu=temperature.gpu,power.draw,pci.bus_id --format=csv,noheader,nounits";
    FILE *fp = popen(command, "r");

    if (bus_id != NULL && bus_id_size > 0)
        bus_id[0] = '\0';

    if (fp)
    {
        char line[BUFFER_SIZE];

        if (fgets(line, sizeof(line), fp))
        {
            char parsed_id[32] = "";
            int fields = sscanf(line, "%f, %f, %31s", temperature, power, parsed_id);

            if (fields >= 2 && bus_id != NULL && bus_id_size > 0 && parsed_id[0] != '\0')
                snprintf(bus_id, bus_id_size, "%s", parsed_id);
        }

        pclose(fp);
    }
    else
    {
        *temperature = -1;
        *power = -1;
    }
}

/**
 * @brief Mencocokkan bus_id dari nvidia-smi dengan perangkat PCI hasil scan libpci.
 *
 * Format bus_id nvidia-smi: "00000000:01:00.0" (domain:bus:dev.func), heksadesimal.
 *
 * @param bus_id String bus_id; NULL atau kosong berarti tanpa filter.
 * @param dev    Perangkat PCI yang diperiksa.
 * @return bool true kalau cocok (atau kalau bus_id tidak tersedia).
 */
static bool nvidia_bus_id_matches(const char *bus_id, struct pci_dev *dev)
{
    unsigned int domain, bus, devnum, func;

    if (bus_id == NULL || bus_id[0] == '\0')
        return true;

    if (sscanf(bus_id, "%x:%x:%x.%x", &domain, &bus, &devnum, &func) != 4)
        return true;

    return dev->domain == (int)domain && dev->bus == (int)bus &&
           dev->dev == (int)devnum && dev->func == (int)func;
}

/**
 * @brief Tabel GPU NVIDIA beserta offset register suhu VRAM pada BAR0.
 */
static const struct
{
    uint32_t offset;
    uint16_t dev_id;
    const char *name;
} vram_dev_table[] =
{
    { 0x0000E2A8, 0x2684, "RTX 4090" },
    { 0x0000E2A8, 0x2702, "RTX 4080 Super" },
    { 0x0000E2A8, 0x2704, "RTX 4080" },
    { 0x0000E2A8, 0x2705, "RTX 4070 Ti Super" },
    { 0x0000E2A8, 0x2782, "RTX 4070 Ti" },
    { 0x0000E2A8, 0x2783, "RTX 4070 Super" },
    { 0x0000E2A8, 0x2786, "RTX 4070" },
    { 0x0000E2A8, 0x2860, "RTX 4070 Max-Q / Mobile" },
    { 0x0000E2A8, 0x2203, "RTX 3090 Ti" },
    { 0x0000E2A8, 0x2204, "RTX 3090" },
    { 0x0000E2A8, 0x2208, "RTX 3080 Ti" },
    { 0x0000E2A8, 0x2206, "RTX 3080" },
    { 0x0000E2A8, 0x2216, "RTX 3080 LHR" },
    { 0x0000EE50, 0x2484, "RTX 3070" },
    { 0x0000EE50, 0x2488, "RTX 3070 LHR" },
    { 0x0000E2A8, 0x2531, "RTX A2000" },
    { 0x0000E2A8, 0x2571, "RTX A2000" },
    { 0x0000E2A8, 0x2232, "RTX A4500" },
    { 0x0000E2A8, 0x2231, "RTX A5000" },
    { 0x0000E2A8, 0x26B1, "RTX A6000" },
    { 0x0000E2A8, 0x27B8, "L4" },
    { 0x0000E2A8, 0x26B9, "L40S" },
    { 0x0000E2A8, 0x2236, "A10" },
};

/**
 * @brief Membaca suhu VRAM GPU NVIDIA dari register BAR0 melalui /dev/mem.
 *
 * Fungsi ini memindai bus PCI untuk menemukan GPU NVIDIA yang kompatibel
 * (tercantum dalam vram_dev_table), memetakan halaman register suhu VRAM
 * pada BAR0 melalui /dev/mem, lalu membaca nilainya dan mengkonversinya
 * ke derajat Celsius: (nilai & 0xFFF) / 32. Memerlukan eksekusi sebagai root.
 *
 * @param bus_id    bus_id GPU dari nvidia-smi; NULL berarti ambil GPU NVIDIA pertama.
 * @param vram_temp Pointer untuk menyimpan suhu VRAM (°C), -1 jika gagal.
 */
static void read_nvidia_vram_temp(const char *bus_id, int *vram_temp)
{
    *vram_temp = -1;

    struct pci_access *pacc = pci_alloc();

    if (!pacc)
        return;

    pci_init(pacc);
    pci_scan_bus(pacc);

    int fd = open(NVIDIA_MEM_PATH, O_RDONLY);

    if (fd < 0)
    {
        pci_cleanup(pacc);
        return;
    }

    long page_size = sysconf(_SC_PAGE_SIZE);

    /* Lintasan 0: hanya GPU yang cocok dengan bus_id dari nvidia-smi.
     * Lintasan 1: abaikan bus_id, ambil GPU NVIDIA pertama yang dikenal.
     * Fallback ini menjaga pembacaan tetap jalan kalau bus_id tidak terbaca. */
    for (int pass = 0; pass < 2 && *vram_temp < 0; pass++)
    {
        for (struct pci_dev *dev = pacc->devices; dev; dev = dev->next)
        {
            pci_fill_info(dev, PCI_FILL_IDENT | PCI_FILL_BASES);

            if (dev->vendor_id != NVIDIA_VENDOR_ID)
                continue;

            uint32_t reg_offset = 0;
            bool compatible = false;

            for (size_t i = 0; i < sizeof(vram_dev_table) / sizeof(vram_dev_table[0]); i++)
            {
                if (dev->device_id == vram_dev_table[i].dev_id)
                {
                    reg_offset = vram_dev_table[i].offset;
                    compatible = true;

                    break;
                }
            }

            if (!compatible)
                continue;

            if (pass == 0 && !nvidia_bus_id_matches(bus_id, dev))
                continue;

            uint32_t vram_addr = (dev->base_addr[0] & 0xFFFFFFFFu) + reg_offset;
            uint32_t page_off = vram_addr & (uint32_t)(page_size - 1);
            uint32_t page_base = vram_addr - page_off;

            void *vram_base = mmap(NULL, page_size, PROT_READ, MAP_SHARED, fd, (off_t)page_base);

            if (vram_base != MAP_FAILED)
            {
                uint32_t *vram_reg = (uint32_t *)((char *)vram_base + page_off);
                *vram_temp = (int)((*vram_reg & NVIDIA_VRAM_TEMP_MASK) / NVIDIA_VRAM_TEMP_DIVISOR);

                munmap(vram_base, page_size);
            }

            if (*vram_temp >= 0)
                break;
        }
    }

    close(fd);
    pci_cleanup(pacc);
}
#endif

/**
 * @brief Mendeteksi dan mencetak informasi suhu dan daya GPU.
 */
static void print_gpu_info(void)
{
    int gpu_type = detect_gpu_type();

    if (gpu_type == GPU_TYPE_AMD)
    {
        char hwmon_path[PATH_MAX];
        float gpu_edge = 0.0f, gpu_junction = 0.0f, gpu_mem = 0.0f, gpu_power = 0.0f;

        if (find_hwmon_path_by_name("amdgpu", hwmon_path, sizeof(hwmon_path)) == 0)
        {
            gpu_edge = read_hwmon_temp(hwmon_path, "temp1_input");
            gpu_junction = read_hwmon_temp(hwmon_path, "temp2_input");
            gpu_mem = read_hwmon_temp(hwmon_path, "temp3_input");
            gpu_power = read_hwmon_temp(hwmon_path, "power1_average");

            printf(BOLD "AMD Radeon GPU" RESET "\n");
            printf("Edge     : %.2f°C\n", gpu_edge != -1 ? gpu_edge / 1000.0 : 0.0);
            printf("Junction : %.2f°C\n", gpu_junction != -1 ? gpu_junction / 1000.0 : 0.0);
            printf("Mem      : %.2f°C\n", gpu_mem != -1 ? gpu_mem / 1000.0 : 0.0);
            printf("Power    : %.2f W\n", gpu_power != -1 ? gpu_power / 1000000.0 : 0.0);
            printf("\n");
        }
    }
#ifdef NVIDIA_GPU
    else if (gpu_type == GPU_TYPE_NVIDIA)
    {
        float gpu_temp_nvidia = -1.0f, gpu_power_nvidia = -1.0f;
        char nvidia_bus_id[32] = "";

        read_nvidia_gpu_info(&gpu_temp_nvidia, &gpu_power_nvidia, nvidia_bus_id, sizeof(nvidia_bus_id));

        /* Cukup suhu yang valid. Daya tidak diminta lagi di sini supaya blok GPU
         * tetap tampil saat driver melapor 0 W pada idle. */
        if (gpu_temp_nvidia > 0)
        {
            char *nvidia_gpu_name = execute_command("nvidia-smi --query-gpu=gpu_name --format=csv,noheader");
            int vram_temp = -1;

            read_nvidia_vram_temp(nvidia_bus_id, &vram_temp);

            printf(BOLD "%s" RESET "\n", nvidia_gpu_name ? nvidia_gpu_name : "NVIDIA GPU");
            printf("Temp     : %.2f°C\n", gpu_temp_nvidia);
            printf("VRAM     : %.2f°C\n", vram_temp != -1 ? (float)vram_temp : 0.0f);
            printf("Power    : %.2f W\n", gpu_power_nvidia > 0 ? gpu_power_nvidia : 0.0);
            printf("\n");

            free(nvidia_gpu_name);
        }
    }
#endif
}

/**
 * @brief Mencetak suhu SSD NVMe.
 */
static void print_nvme_info(void)
{
    char temp_path[PATH_MAX];
    glob_t nvme_paths;

    if (glob("/sys/class/nvme/nvme*", 0, NULL, &nvme_paths) == 0)
    {
        for (size_t i = 0; i < nvme_paths.gl_pathc; i++)
        {
            char model[128] = "NVMe SSD";
            char model_path[PATH_MAX];

            snprintf(model_path, sizeof(model_path), "%s/model", nvme_paths.gl_pathv[i]);

            FILE *f_model = fopen(model_path, "r");

            if (f_model)
            {
                if (fgets(model, sizeof(model), f_model))
                {
                    model[strcspn(model, "\n")] = 0;

                    // Buang padding spasi dari sysfs model file
                    size_t len = strlen(model);

                    while (len > 0 && isspace((unsigned char)model[len - 1]))
                        model[--len] = '\0';
                }

                fclose(f_model);
            }

            char nvme_hwmon_path[PATH_MAX];

            snprintf(nvme_hwmon_path, sizeof(nvme_hwmon_path), "%s/hwmon*", nvme_paths.gl_pathv[i]);

            glob_t nvme_hwmon_glob;

            if (glob(nvme_hwmon_path, 0, NULL, &nvme_hwmon_glob) == 0)
            {
                if (nvme_hwmon_glob.gl_pathc > 0)
                {
                    snprintf(temp_path, sizeof(temp_path), "%s/temp1_input", nvme_hwmon_glob.gl_pathv[0]);

                    int nvme_temp = read_int_from_file(temp_path);

                    printf(BOLD "%s" RESET "\n", model);
                    printf("NAND     : %.2f°C\n", nvme_temp != -1 ? nvme_temp / 1000.0 : 0.0);
                    printf("\n");
                }

                globfree(&nvme_hwmon_glob);
            }
        }
    }

    globfree(&nvme_paths);
}

/**
 * @brief Titik masuk utama untuk program pembacaan sensor.
 *
 * @return int 0 jika berhasil.
 */
int main(void)
{
    print_system_info();
    print_motherboard_and_fan_info();
    print_cpu_info();
#ifdef ENABLE_DRAM
    print_dram_info();
#endif
    print_gpu_info();
    print_nvme_info();

    return 0;
}
