/*
 * ryzen - Tool to monitor CPU power consumption.
 *
 * Copyright (C) 2026 MOVZX
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>

#include "sensors_common.h"

/**
 * @brief Program entry point.
 *
 * The program measures the CPU power for one second, then prints the result
 * in watts. The file sensors_common.h handles the RAPL counter wrap and the retry.
 *
 * @return int 0 on success, 1 on error.
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
