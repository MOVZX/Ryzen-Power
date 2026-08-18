#!/usr/bin/env bash

gcc -O3 -o ryzen ryzen.c -lm
gcc -O3 -o cpuf cpuf.c -lm

if [ -d /opt/cuda ]; then
    # NVIDIA
    echo -e "\e[32m✔ NVIDIA CUDA SDK found. Compiling with NVIDIA support. 🚀\e[0m"

    gcc -O3 -Wall -Werror -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wno-error=deprecated-declarations -Wvla -I/opt/cuda/include -o powerusage powerusage.c -lpci -lnvidia-ml -lm -DNVIDIA_GPU
    gcc -O3 -DNVIDIA_GPU -o sens sens.c -lm
else
    # AMD/Intel
    echo -e "\e[33mℹ NVIDIA CUDA SDK not found. Compiling without NVIDIA support. CPU only. 💻\e[0m"

    gcc -O3 -Wall -Werror -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wvla -o powerusage powerusage.c -lpci -lm
    gcc -O3 -o sens sens.c -lm
fi
