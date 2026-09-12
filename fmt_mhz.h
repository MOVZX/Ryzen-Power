/*
 * fmt_mhz.h - Format angka MHz dengan pemisah ribuan.
 *
 * Hak Cipta (C) 2024 MOVZX
 *
 * Program ini adalah perangkat lunak bebas; Anda dapat menyebarluaskannya
 * dan/atau memodifikasinya di bawah ketentuan Lisensi Publik Umum GNU
 * versi 2 sebagaimana dipublikasikan oleh Free Software Foundation.
 * Lihat file LICENSE untuk detailnya.
 */

#ifndef FMT_MHZ_H
#define FMT_MHZ_H

#include <stddef.h>
#include <stdio.h>

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

#endif /* FMT_MHZ_H */
