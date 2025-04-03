#!/usr/bin/env bash

gcc -O3 -o ryzen ryzen.c -lm
gcc -O3 -o cpuf cpuf.c -lm
gcc -O3 -o sens sens.c -lm
gcc -O3 -Wall -Werror -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wvla -I/opt/cuda/include -o powerusage powerusage.c -lpci -lnvidia-ml -lm
