/*
 * cpuf - Utilitas untuk memantau frekuensi, suhu, dan daya CPU.
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
#include <sys/time.h>
#include <stdint.h>
#include <glob.h>
#include <limits.h>
#include <fcntl.h>
#include <libgen.h>

#define RAPL_FILE_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define BUFFER_SIZE 256
#define USEC 1000000

#define BOLD "\033[1m"
#define RESET "\033[0m"
#define CLEAR_SCREEN "\033[2J"
#define CURSOR_HOME "\033[H"

#define MAX_CPU_SENSORS 8
#define MAX_TEMP_IDX 16

typedef struct
{
    char label[32];
    int value;
} TempSensor;

typedef struct
{
    int last;
    int min;
    int max;
    long long sum;
    long count;
} FreqStats;

/**
 * @brief Mendapatkan konsumsi energi CPU saat ini dalam mikrojoule.
 *
 * @return int64_t Konsumsi energi dalam mikrojoule, atau -1 jika gagal.
 */
int64_t get_cpu_consumption_ujoules()
{
    int64_t consumption = -1;
    FILE *file = fopen(RAPL_FILE_PATH, "r");

    if (file == NULL)
    {
        perror("Error opening RAPL energy file");

        return -1;
    }

    if (fscanf(file, "%ld", &consumption) != 1)
    {
        perror("Error reading energy consumption");

        consumption = -1;
    }

    fclose(file);

    return consumption;
}

/**
 * @brief Mengembalikan waktu saat ini dalam mikrodetik.
 *
 * @return int64_t Waktu saat ini dalam mikrodetik, atau -1 jika gagal.
 */
int64_t get_current_time_usec()
{
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0)
    {
        perror("Error getting current time");

        return -1;
    }

    return ((int64_t)tv.tv_sec * USEC) + tv.tv_usec;
}

/**
 * @brief Menghitung daya CPU rata-rata dalam Watt selama interval 1 detik.
 *
 * @return float Daya CPU dalam Watt, atau -1.0f jika gagal.
 */
float calculate_cpu_power()
{
    int64_t initial_usage = get_cpu_consumption_ujoules();
    int64_t initial_time = get_current_time_usec();

    if (initial_usage == -1 || initial_time == -1)
    {
        fprintf(stderr, "Failed to read initial CPU consumption or time data\n");

        return -1.0f;
    }

    sleep(1);

    int64_t final_usage = get_cpu_consumption_ujoules();
    int64_t final_time = get_current_time_usec();

    if (final_usage == -1 || final_time == -1)
    {
        fprintf(stderr, "Failed to read final CPU consumption or time data\n");

        return -1.0f;
    }

    if (final_time <= initial_time)
    {
        fprintf(stderr, "Time did not advance or went backwards!\n");

        return -1.0f;
    }

    int64_t energy_diff_uj = final_usage - initial_usage;
    int64_t time_diff_usec = final_time - initial_time;

    if (energy_diff_uj < 0)
    {
        fprintf(stderr, "Energy consumption decreased, which is not possible.\n");

        return -1.0f;
    }

    return (float)energy_diff_uj / (float)time_diff_usec;
}

/**
 * @brief Membaca nilai integer dari file yang ditentukan.
 *
 * @param path Path ke file.
 * @return int Nilai integer yang dibaca dari file, atau -1 jika gagal.
 */
int read_int_from_file(const char *path)
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
 * @brief Mendapatkan suhu CPU (Tctl, Tccd1, Tccd2, ...) dari k10temp.
 *
 * @param sensors Array untuk menyimpan sensor suhu.
 * @param max_sensors Ukuran maksimum array sensors.
 * @return int Jumlah sensor ditemukan, atau -1 jika gagal.
 */
int get_cpu_temperatures(TempSensor *sensors, int max_sensors)
{
    char hwmon_path[BUFFER_SIZE];
    char label_path[BUFFER_SIZE];
    char temp_path[BUFFER_SIZE];
    char label[32];
    int found = 0;

    glob_t glob_result;

    if (glob("/sys/class/hwmon/hwmon*/name", 0, NULL, &glob_result) == 0)
    {
        for (size_t i = 0; i < glob_result.gl_pathc; i++)
        {
            FILE *name_file = fopen(glob_result.gl_pathv[i], "r");

            if (name_file)
            {
                char name[32];

                if (fgets(name, sizeof(name), name_file))
                {
                    name[strcspn(name, "\n")] = 0;

                    if (strcmp(name, "k10temp") == 0)
                    {
                        char *dir = dirname(glob_result.gl_pathv[i]);

                        strncpy(hwmon_path, dir, sizeof(hwmon_path) - 1);

                        hwmon_path[sizeof(hwmon_path) - 1] = '\0';
                        found = 1;
                    }
                }

                fclose(name_file);

                if (found)
                    break;
            }
        }
    }

    globfree(&glob_result);

    if (!found)
    {
        fprintf(stderr, "k10temp sensor module not found!\n");

        return -1;
    }

    // Baca semua sensor berdasarkan label (indeks bisa bolong,
    // mis. 9950X3D: temp1=Tctl, temp3=Tccd1, temp4=Tccd2)
    int count = 0;

    for (int idx = 1; idx <= MAX_TEMP_IDX && count < max_sensors; idx++)
    {
        label[0] = '\0';

        snprintf(label_path, sizeof(label_path), "%s/temp%d_label", hwmon_path, idx);

        FILE *label_file = fopen(label_path, "r");

        if (label_file)
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

    if (count == 0)
    {
        fprintf(stderr, "Failed to read CPU temperatures.\n");

        return -1;
    }

    return count;
}

/**
 * @brief Membaca nama model CPU dari /proc/cpuinfo.
 *
 * @param buffer Buffer untuk menyimpan nama CPU.
 * @param size Ukuran buffer.
 * @return int 0 jika berhasil, -1 jika gagal.
 */
int get_cpu_name(char *buffer, size_t size)
{
    FILE *file = fopen("/proc/cpuinfo", "r");

    if (file == NULL)
        return -1;

    char line[BUFFER_SIZE];
    int found = -1;

    while (fgets(line, sizeof(line), file))
    {
        if (strncmp(line, "model name", 10) == 0)
        {
            char *colon = strchr(line, ':');

            if (colon != NULL)
            {
                colon++;

                while (*colon == ' ' || *colon == '\t')
                    colon++;

                snprintf(buffer, size, "%s", colon);

                found = 0;
            }

            break;
        }
    }

    fclose(file);

    if (found == 0)
        buffer[strcspn(buffer, "\n")] = 0;

    return found;
}

/**
 * @brief Mendapatkan frekuensi saat ini untuk setiap inti CPU.
 *
 * @param freqs Array untuk menyimpan frekuensi setiap inti dalam MHz.
 * @param cpu_count Jumlah logik core CPU.
 */
void get_cpu_frequencies(int *freqs, int cpu_count)
{
    for (int i = 0; i < cpu_count; i++)
    {
        char freq_path[BUFFER_SIZE];

        snprintf(freq_path, sizeof(freq_path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", i);

        int freq_khz = read_int_from_file(freq_path);
        freqs[i] = (freq_khz != -1) ? freq_khz / 1000 : 0;
    }
}

/**
 * @brief Mencatat satu sampel frekuensi ke statistik per-core.
 *
 * Nilai 0 (gagal baca) diabaikan agar tidak merusak min.
 *
 * @param stats Array statistik frekuensi per-core.
 * @param freqs Array frekuensi inti CPU dalam MHz.
 * @param cpu_count Jumlah logik core CPU.
 */
void record_sample(FreqStats *stats, const int *freqs, int cpu_count)
{
    for (int i = 0; i < cpu_count; i++)
    {
        int freq = freqs[i];

        if (freq == 0)
            continue;

        if (stats[i].count == 0)
        {
            stats[i].min = freq;
            stats[i].max = freq;
        }
        else
        {
            if (freq < stats[i].min)
                stats[i].min = freq;

            if (freq > stats[i].max)
                stats[i].max = freq;
        }

        stats[i].last = freq;
        stats[i].sum += freq;
        stats[i].count++;
    }
}

/**
 * @brief Mencetak informasi CPU yang diformat.
 *
 * @param sensors Array sensor suhu CPU.
 * @param sensor_count Jumlah sensor suhu CPU.
 * @param cpu_power Daya CPU dalam Watt.
 * @param stats Array statistik frekuensi per-core.
 * @param cpu_count Jumlah logik core CPU.
 * @param cpu_name Nama model CPU.
 */
void print_cpu_info(const TempSensor *sensors, int sensor_count, float cpu_power, const FreqStats *stats, int cpu_count, const char *cpu_name)
{
    printf(BOLD "%s" RESET "\n\n", cpu_name);

    for (int i = 0; i < sensor_count; i++)
        printf("%-7s : %8d°C\n", sensors[i].label, sensors[i].value != -1 ? sensors[i].value / 1000 : 0);

    printf("Power   : %8.2f W\n", cpu_power);
    printf("\n");

    for (int i = 0; i < cpu_count; i++)
        printf("CPU %2d  : %6d MHz  (min %6d / max %6d)\n", i, stats[i].last, stats[i].min, stats[i].max);

    printf("\n");
}

/**
 * @brief Titik masuk utama untuk program.
 *
 * Menjalankan loop terus: tiap detik membaca suhu, daya, dan
 * frekuensi, lalu mencetak status. Min/max per-core dihitung
 * sejak program dijalankan. Berhenti dengan Ctrl+C.
 *
 * @return int 0 jika berhasil, 1 jika gagal.
 */
int main()
{
    char cpu_name[BUFFER_SIZE];
    TempSensor sensors[MAX_CPU_SENSORS];

    if (get_cpu_name(cpu_name, sizeof(cpu_name)) != 0)
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
        int sensor_count = get_cpu_temperatures(sensors, MAX_CPU_SENSORS);

        if (sensor_count < 0)
        {
            free(cpu_freqs);
            free(stats);

            return 1;
        }

        float cpu_power = calculate_cpu_power();

        if (cpu_power < 0)
        {
            free(cpu_freqs);
            free(stats);

            return 1;
        }

        get_cpu_frequencies(cpu_freqs, cpu_count);
        record_sample(stats, cpu_freqs, cpu_count);

        printf(CURSOR_HOME);
        print_cpu_info(sensors, sensor_count, cpu_power, stats, cpu_count, cpu_name);
    }
}
