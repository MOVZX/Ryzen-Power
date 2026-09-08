/*
 * ryzen - Utilitas untuk memantau konsumsi daya CPU.
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

#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

#define RAPL_PATH "/sys/class/powercap/intel-rapl:0/energy_uj"
#define RAPL_RANGE_PATH "/sys/class/powercap/intel-rapl:0/max_energy_range_uj"
#define USEC 1000000

/**
 * @brief Mendapatkan waktu monotonik dalam mikrodetik.
 *
 * @return int64_t Waktu saat ini dalam mikrodetik.
 */
int64_t get_monotonic_time_usec()
{
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);

    return time.tv_sec * USEC + time.tv_nsec / 1000;
}

/**
 * @brief Mengambil konsumsi energi CPU dalam microjoule dengan caching.
 *
 * Fungsi ini membaca file RAPL untuk mendapatkan total energi yang dikonsumsi oleh CPU.
 * Hasilnya di-cache selama satu detik untuk mengurangi pembacaan file.
 *
 * @return int64_t Konsumsi energi dalam microjoule, atau -1 jika terjadi kesalahan.
 */
int64_t get_cpu_consumption_ujoules()
{
    static int64_t last_read_time = 0;
    static int64_t cached_consumption = -1;
    int64_t current_time = get_monotonic_time_usec();

    if (current_time - last_read_time >= USEC || cached_consumption == -1)
    {
        FILE *file = fopen(RAPL_PATH, "r");

        if (file != NULL)
        {
            if (fscanf(file, "%ld", &cached_consumption) != 1)
            {
                perror("Failed to read CPU consumption");

                cached_consumption = -1;
            }

            fclose(file);
        }
        else
        {
            perror("Failed to open RAPL path");

            cached_consumption = -1;
        }

        last_read_time = current_time;
    }

    return cached_consumption;
}

/**
 * @brief Membaca rentang (wrap range) counter energi RAPL dalam mikrojoule.
 *
 * Pada kernel 7.x, counter ber-wrap setiap max_energy_range_uj,
 * bukan kumulatif. Userspace harus menangani wrap-nya.
 *
 * @return int64_t Rentang counter dalam mikrojoule, atau -1 jika gagal.
 */
int64_t get_rapl_range_uj()
{
    int64_t range = -1;
    FILE *file = fopen(RAPL_RANGE_PATH, "r");

    if (file == NULL)
        return -1;

    if (fscanf(file, "%ld", &range) != 1)
        range = -1;

    fclose(file);

    return range;
}

/**
 * @brief Menghitung konsumsi daya CPU saat ini dalam Watt.
 *
 * Fungsi ini mengukur perubahan konsumsi energi selama interval satu detik
 * untuk menghitung penggunaan daya rata-rata.
 *
 * @return float Penggunaan daya dalam Watt, atau -1.0f jika terjadi kesalahan.
 */
float get_cpu_consumption_watts()
{
    int64_t initial_usage = get_cpu_consumption_ujoules();
    int64_t initial_timestamp = get_monotonic_time_usec();

    if (initial_usage < 0)
        return -1.0f;

    sleep(1);

    int64_t final_usage = get_cpu_consumption_ujoules();
    int64_t final_timestamp = get_monotonic_time_usec();

    if (final_usage < 0)
        return -1.0f;

    int64_t energy_diff_uj = final_usage - initial_usage;
    int64_t time_diff_us = final_timestamp - initial_timestamp;

    if (time_diff_us <= 0)
        return 0;

    /* Counter bisa ber-wrap di dalam interval; kembalikan rentangnya.
     * Maksimal satu wrap per detik (rentang ~65 kJ vs energi ~kJ/detik). */
    if (energy_diff_uj < 0)
    {
        int64_t range = get_rapl_range_uj();

        if (range > 0)
            energy_diff_uj += range;
    }

    if (energy_diff_uj < 0)
        return -1.0f;

    float watts = (float)energy_diff_uj / (float)time_diff_us;

    return watts;
}

/**
 * @brief Titik masuk utama program.
 *
 * Mengambil dan mencetak konsumsi daya CPU saat ini dalam Watt.
 *
 * @return int 0 jika berhasil, 1 jika terjadi kesalahan.
 */
int main()
{
    float power = get_cpu_consumption_watts();

    if (power < 0)
    {
        fprintf(stderr, "Failed to get CPU power consumption.\n");

        return 1;
    }

    printf("%.2f\n", power);

    return 0;
}
