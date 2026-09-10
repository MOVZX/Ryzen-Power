#!/usr/bin/env bash

# Suhu DRAM (sensor spd5118) mati secara default.
# Aktifkan dengan: ENABLE_DRAM=1 ./build.sh
DRAM_FLAGS=()

if [ -n "$ENABLE_DRAM" ]; then
    echo -e "\e[36m\u2139 DRAM temperature (spd5118) enabled.\e[0m"

    DRAM_FLAGS=(-DENABLE_DRAM)
fi

gcc -O3 -o ryzen ryzen.c -lm
gcc -O3 -o cpuf cpuf.c -lm

if [ -d /opt/cuda ]; then
    # NVIDIA
    echo -e "\e[32m✔ NVIDIA CUDA SDK found. Compiling with NVIDIA support. 🚀\e[0m"

    gcc -O3 -Wall -Werror -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wno-error=deprecated-declarations -Wvla -I/opt/cuda/include -o powerusage powerusage.c -lpci -lnvidia-ml -lm -DNVIDIA_GPU ${DRAM_FLAGS[@]+"${DRAM_FLAGS[@]}"}
    gcc -O3 -DNVIDIA_GPU -o sens sens.c -lpci -lm ${DRAM_FLAGS[@]+"${DRAM_FLAGS[@]}"}
else
    # AMD/Intel
    echo -e "\e[33mℹ NVIDIA CUDA SDK not found. Compiling without NVIDIA support. CPU only. 💻\e[0m"

    gcc -O3 -Wall -Werror -Wextra -Wshadow -Wpointer-arith -Wstrict-prototypes -Wmissing-prototypes -Wold-style-definition -Wvla -o powerusage powerusage.c -lpci -lm ${DRAM_FLAGS[@]+"${DRAM_FLAGS[@]}"}
    gcc -O3 -o sens sens.c -lm ${DRAM_FLAGS[@]+"${DRAM_FLAGS[@]}"}
fi
