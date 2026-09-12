/*
 * sensors_common.h - Helper yang dipakai bersama semua utilitas di repo ini.
 *
 * Hak Cipta (C) 2024 MOVZX
 *
 * Program ini adalah perangkat lunak bebas; Anda dapat menyebarluaskannya
 * dan/atau memodifikasinya di bawah ketentuan Lisensi Publik Umum GNU
 * versi 2 sebagaimana dipublikasikan oleh Free Software Foundation.
 * Lihat file LICENSE untuk detailnya.
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

/* Batas indeks temp<N>_label yang dipindai. Boleh bolong, jadi dipindai lebar. */
#define MAX_TEMP_IDX 32

/* Buffer path hasil penggabungan direktori hwmon + nama file sensor.
 * Lebih besar dari PATH_MAX supaya hasil snprintf selalu muat. */
#define SENSOR_PATH_BUF (PATH_MAX + 32)

/* Tipe GPU yang terdeteksi di sistem. */
enum GpuType
{
    GPU_TYPE_UNINITIALIZED = -1,
    GPU_TYPE_NONE = 0,
    GPU_TYPE_AMD,
    GPU_TYPE_NVIDIA,
};

/* Satu sensor suhu hwmon beserta labelnya, mis. "Tctl", "Tccd1". */
typedef struct
{
    char label[32];
    int value;
} TempSensor;

/**
 * @brief Membaca nilai integer dari satu file.
 *
 * @param path Path ke file.
 * @return int Nilai yang dibaca, atau -1 jika gagal.
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
 * @brief Membaca satu file suhu hwmon.
 *
 * @param hwmon_path Direktori hwmon.
 * @param temp_file Nama file, mis. "temp1_input".
 * @return int Suhu dalam miliderajat Celsius, atau -1 jika gagal.
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
 * @brief Bulatkan suhu miliderajat Celsius menjadi derajat utuh.
 *
 * Sysfs mengirim suhu dalam miliderajat, misalnya 70965. Pembagian integer
 * memotong ke bawah sehingga tampil 70, padahal lm-sensors menampilkan 71.
 *
 * @param milli_c Suhu dalam miliderajat Celsius.
 * @return int Suhu dalam derajat Celsius, dibulatkan ke terdekat.
 */
static inline int temp_c_rounded(int milli_c)
{
    if (milli_c < 0)
        return -((-milli_c + 500) / 1000);

    return (milli_c + 500) / 1000;
}

/**
 * @brief Format angka MHz dengan pemisah ribuan gaya Indonesia (titik).
 *
 * Contoh: 5712 menjadi "5.712 MHz", 624 menjadi "624 MHz".
 *
 * @param buf   Buffer tujuan.
 * @param size  Ukuran buffer.
 * @param mhz   Nilai frekuensi dalam MHz.
 * @return const char * Pointer ke buffer.
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
    }
    while (v && n < (int)sizeof(digits));

    int pos = 0;

    if (neg && pos < (int)size - 1)
        buf[pos++] = '-';

    for (int i = n - 1; i >= 0; i--)
    {
        /* Titik dicetak di depan digit kalau sisa digit (termasuk yang ini)
         * habis dibagi 3, dan bukan di paling depan. */
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
 * @brief Jalankan perintah shell dan ambil baris pertamanya.
 *
 * Pemanggil wajib membebaskan memori yang dikembalikan. Fungsi ini melakukan
 * fork, jadi pemanggilan berulang itu mahal.
 *
 * @param command Perintah shell.
 * @return char* Baris pertama output tanpa whitespace akhir, atau NULL jika gagal.
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

        /* dmidecode dan nvidia-smi sering menambah padding di akhir baris. */
        while (len > 0 && isspace((unsigned char)output[len - 1]))
            output[--len] = '\0';
    }

    pclose(fp);

    return output;
}

/**
 * @brief Cari semua direktori hwmon yang namanya cocok persis.
 *
 * @param name Nama hwmon, mis. "k10temp" atau "spd5118".
 * @param out_paths Array penyimpanan path.
 * @param max_paths Jumlah maksimum path.
 * @return int Jumlah path yang ditemukan.
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
 * @brief Hitung jumlah input kipas sebuah perangkat hwmon.
 *
 * Dipakai untuk memilih chip super I/O utama kalau ada lebih dari satu.
 *
 * @param hwmon_path Direktori hwmon.
 * @return int Jumlah input kipas yang ada.
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
 * @brief Cari satu direktori hwmon dengan pencocokan awalan nama.
 *
 * Cocok untuk keluarga chip: "nct6" melatih nct6799 dan nct6798. Kalau
 * beberapa chip cocok, yang dipilih adalah chip dengan input kipas terbanyak.
 *
 * @param prefix Awalan nama hwmon.
 * @param out_path Buffer penyimpanan path.
 * @param out_path_size Ukuran buffer.
 * @return int 0 jika berhasil, -1 jika tidak ketemu.
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
 * @brief Cari direktori hwmon dengan nama persis, simpan ke buffer tunggal.
 *
 * @param name Nama hwmon, mis. "k10temp".
 * @param out_path Buffer penyimpanan path.
 * @param out_path_size Ukuran buffer.
 * @return int 0 jika berhasil, -1 jika tidak ketemu.
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
 * @brief Baca semua sensor suhu berlabel pada satu direktori hwmon.
 *
 * Indeks sensor boleh bolong. Pada 9950X3D misalnya: temp1=Tctl, temp3=Tccd1,
 * temp4=Tccd2, sedangkan temp2 tidak ada. Sensor tanpa label dilewati.
 *
 * @param hwmon_path Direktori hwmon.
 * @param sensors Array penyimpanan sensor.
 * @param max_sensors Kapasitas array sensors.
 * @return int Jumlah sensor yang terisi.
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
 * @brief Waktu saat ini dalam mikrodetik, jam realtime.
 *
 * @return int64_t Mikrodetik, atau -1 jika gagal.
 */
static inline int64_t get_time_usec(void)
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
        return -1;

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

/**
 * @brief Baca counter energi RAPL.
 *
 * @return int64_t Energi dalam mikrojoule, atau -1 jika gagal.
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
 * @brief Baca rentang (wrap range) counter energi RAPL dalam mikrojoule.
 *
 * Pada kernel baru, energy_uj ber-wrap setiap max_energy_range_uj, bukan
 * kumulatif. Userspace harus menangani wrap-nya sendiri.
 *
 * @return int64_t Rentang counter, atau -1 jika gagal.
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
 * @brief Hitung daya rata-rata dari dua pembacaan counter energi.
 *
 * Fungsi ini juga mengoreksi wrap counter memakai max_energy_range_uj.
 * Pemanggil yang mengukur sendiri jendelanya memakai fungsi ini supaya
 * koreksi wrap tidak ditulis ulang di setiap utilitas.
 *
 * @param energy_start Energi pada awal jendela (mikrojoule).
 * @param time_start Waktu awal jendela (mikrodetik).
 * @param energy_end Energi pada akhir jendela (mikrojoule).
 * @param time_end Waktu akhir jendela (mikrodetik).
 * @return float Daya dalam Watt, atau -1.0f jika datanya tidak valid.
 */
static inline float cpu_power_from_delta(int64_t energy_start, int64_t time_start,
                                         int64_t energy_end, int64_t time_end)
{
    if (energy_start < 0 || energy_end < 0 || time_end <= time_start)
        return -1.0f;

    int64_t energy_delta = energy_end - energy_start;

    /* Counter bisa ber-wrap di dalam interval; kembalikan rentangnya.
     * Maksimal satu wrap per detik (rentang ~65 kJ vs energi ~kJ/detik). */
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
 * @brief Ukur daya CPU sekali, selama interval satu detik.
 *
 * @return float Daya dalam Watt, atau -1.0f jika gagal.
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
 * @brief Ukur daya CPU, dengan satu kali pengukuran ulang saat gagal.
 *
 * Rentang wrap yang dilaporkan kernel bisa sedikit lebih kecil dari titik wrap
 * sebenarnya, sehingga selang satu detik yang memotong wrap bisa menghasilkan
 * selisih negatif palsu. Ukur sekali lagi sebelum menyerah.
 *
 * @return float Daya dalam Watt, atau -1.0f jika tetap gagal.
 */
static inline float measure_cpu_power(void)
{
    float power = measure_cpu_power_once();

    if (power < 0)
        power = measure_cpu_power_once();

    return power;
}

/**
 * @brief Nama model CPU dari /proc/cpuinfo.
 *
 * Tidak butuh root, jadi ini pilihan utama dibanding dmidecode.
 *
 * @param buffer Buffer tujuan.
 * @param size Ukuran buffer.
 * @return int 0 jika berhasil, -1 jika gagal.
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
 * @brief Baca satu field DMI dari sysfs tanpa butuh root.
 *
 * Field seperti board_vendor dan board_name punya mode 0444, jadi terbaca
 * oleh pengguna biasa. Field lain (mis. bios_date) hanya terbaca oleh root.
 *
 * @param field Nama field, mis. "board_vendor".
 * @param buffer Buffer tujuan.
 * @param size Ukuran buffer.
 * @return int 0 jika berhasil, -1 jika gagal.
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
 * @brief Hitung kunci urut dari alamat PCI sebuah direktori hwmon.
 *
 * Path hwmon selalu mengandung alamat PCI perangkatnya, misalnya
 * ".../0000:11:00.0/hwmon/hwmon7". Alamat terakhir di path yang dipakai supaya
 * urutan stabil dan tidak bergantung pada urutan glob, yang mengurutkan nama
 * sebagai string sehingga hwmon10 muncul sebelum hwmon7.
 *
 * @param hwmon_path Path direktori hwmon.
 * @return long long Kunci urut, atau -1 jika tidak ada alamat PCI.
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
 * @brief Urutkan daftar path hwmon berdasarkan alamat PCI, terkecil dulu.
 *
 * @param paths Array path, dimutasi.
 * @param keys Array kunci, dimutasi.
 * @param count Jumlah entri.
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
 * @brief Baca suhu chipset PCH dari sensor prom21_xhci.
 *
 * Board AM5 punya dua chip prom21_xhci, di PCI 11:00.0 dan 13:00.0. lm-sensors
 * menamai keduanya "PCH Chipset #1" dan "PCH Chipset #2", tapi label di sysfs
 * kosong. Urutan ditentukan dari alamat PCI supaya sama dengan penomoran
 * lm-sensors: 11:00.0 -> PCH1, 13:00.0 -> PCH2.
 *
 * @param pch1 Penyimpanan suhu PCH1 (miliderajat Celsius), -1 jika tidak ada.
 * @param pch2 Penyimpanan suhu PCH2 (miliderajat Celsius), -1 jika tidak ada.
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
 * @brief Deteksi jenis GPU di sistem. Hasilnya di-cache.
 *
 * GPU AMD dikenali dari hwmon "amdgpu" tanpa perlu fork. GPU NVIDIA dikenali
 * lewat nvidia-smi, hanya kalau program dibangun dengan -DNVIDIA_GPU.
 *
 * @return int Salah satu nilai enum GpuType.
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
