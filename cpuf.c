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
 * @brief Mendapatkan frekuensi saat ini untuk setiap inti CPU.
 *
 * @param freqs Array untuk menyimpan frekuensi setiap inti dalam MHz.
 * @param cpu_count Jumlah logik core CPU.
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
 * @brief Mencatat satu sampel frekuensi ke statistik per-core.
 *
 * Nilai 0 (gagal baca) diabaikan agar tidak merusak max.
 *
 * @param stats Array statistik frekuensi per-core.
 * @param freqs Array frekuensi inti CPU dalam MHz.
 * @param cpu_count Jumlah logik core CPU.
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
 * @brief Mencetak informasi CPU yang diformat.
 *
 * @param sensors Array sensor suhu CPU.
 * @param sensor_count Jumlah sensor suhu CPU.
 * @param cpu_power Daya CPU dalam Watt.
 * @param stats Array statistik frekuensi per-core.
 * @param cpu_count Jumlah logik core CPU.
 * @param cpu_name Nama model CPU.
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
 * @brief Titik masuk utama untuk program.
 *
 * Menjalankan loop terus: tiap detik membaca suhu, daya, dan
 * frekuensi, lalu mencetak status. Max per-core dihitung
 * sejak program dijalankan. Berhenti dengan Ctrl+C.
 *
 * @return int 0 jika berhasil, 1 jika gagal.
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
        /* Path k10temp dicari sekali saja. Indeks hwmon bisa berubah setelah
         * modul dimuat ulang, jadi baca ulang path hanya kalau suhunya gagal. */
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

        /* Kegagalan sesaat (mis. wrap counter RAPL) tidak boleh mematikan
         * monitor; tampilkan N/A untuk tick ini dan lanjut. */

        get_cpu_frequencies(cpu_freqs, cpu_count);
        record_sample(stats, cpu_freqs, cpu_count);

        printf(CURSOR_HOME);
        print_cpu_info(sensors, sensor_count, cpu_power, stats, cpu_count, cpu_name);
    }
}
