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

#include <stdio.h>

#include "sensors_common.h"

/**
 * @brief Titik masuk utama program.
 *
 * Mengukur konsumsi daya CPU selama satu detik, lalu mencetaknya dalam Watt.
 * Wrap counter RAPL dan pengukuran ulang ditangani sensors_common.h, sama
 * seperti yang dilakukan cpuf dan sens.
 *
 * @return int 0 jika berhasil, 1 jika terjadi kesalahan.
 */
int main(void)
{
    float power = measure_cpu_power();

    if (power < 0)
    {
        fprintf(stderr, "Failed to get CPU power consumption.\n");

        return 1;
    }

    printf("%.2f\n", power);

    return 0;
}
