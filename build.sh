#!/usr/bin/env bash
#
# build.sh - Bangun keempat utilitas Ryzen Power.
#
# Variabel environment yang dimengerti:
#   CC           kompilator (default: gcc)
#   ENABLE_DRAM  1/true/yes/on untuk ikut mencetak suhu DRAM (spd5118)
#   NVIDIA       0/false/off untuk memaksa build tanpa dukungan NVIDIA
#   CUDA_DIR     direktori CUDA (default: /opt/cuda)
#   PREFIX       tujuan "install" (default: /usr/local)
#
# Contoh:
#   ./build.sh                      bangun saja
#   ENABLE_DRAM=1 ./build.sh        bangun dengan suhu DRAM
#   sudo ./build.sh install         bangun lalu pasang ke $PREFIX/bin

set -euo pipefail

CC="${CC:-gcc}"
PREFIX="${PREFIX:-/usr/local}"
CUDA_DIR="${CUDA_DIR:-/opt/cuda}"

OPT_FLAGS=(-O3)
STRICT_FLAGS=(
    -Wall
    -Werror
    -Wextra
    -Wshadow
    -Wpointer-arith
    -Wstrict-prototypes
    -Wmissing-prototypes
    -Wold-style-definition
    -Wvla
    -Wno-error=deprecated-declarations
)

# Suhu DRAM (sensor spd5118) mati secara default. Nilai 0/false/off/no berarti
# tetap mati, jadi ENABLE_DRAM=0 tidak mengaktifkannya.
DRAM_FLAGS=()
case "${ENABLE_DRAM:-0}" in
    1 | true | True | TRUE | yes | Yes | YES | on | On | ON)
        echo -e "\e[36m\u2139 DRAM temperature (spd5118) enabled.\e[0m"
        DRAM_FLAGS=(-DENABLE_DRAM)
        ;;
esac

# Dukungan NVIDIA aktif kalau SDK ada, kecuali diminta mati lewat NVIDIA=0.
NVIDIA_FLAGS=()
NVIDIA_LIBS=()
USE_NVIDIA=1
NVIDIA_DISABLED_BY_ENV=0

case "${NVIDIA:-1}" in
    0 | false | False | FALSE | no | No | NO | off | Off | OFF)
        USE_NVIDIA=0
        NVIDIA_DISABLED_BY_ENV=1
        ;;
esac

if [ "$USE_NVIDIA" = "1" ] && [ ! -d "$CUDA_DIR" ]; then
    USE_NVIDIA=0
fi

if [ "$NVIDIA_DISABLED_BY_ENV" = "1" ]; then
    echo -e "\e[33m\u2139 NVIDIA support disabled by NVIDIA=${NVIDIA}. CPU only. 💻\e[0m"
elif [ "$USE_NVIDIA" = "1" ]; then
    echo -e "\e[32m✔ NVIDIA CUDA SDK found at $CUDA_DIR. Compiling with NVIDIA support. 🚀\e[0m"
    NVIDIA_FLAGS=(-DNVIDIA_GPU -I"$CUDA_DIR/include")
    NVIDIA_LIBS=(-lpci -lnvidia-ml)
else
    echo -e "\e[33mℹ NVIDIA CUDA SDK not found. Compiling without NVIDIA support. CPU only. 💻\e[0m"
fi

# build <nama-target> <file-sumber> [library tambahan]
build()
{
    local target="$1"
    shift

    "$CC" "${OPT_FLAGS[@]}" "${STRICT_FLAGS[@]}" \
        ${NVIDIA_FLAGS[@]+"${NVIDIA_FLAGS[@]}"} \
        ${DRAM_FLAGS[@]+"${DRAM_FLAGS[@]}"} \
        -o "$target" "$target.c" -lpci "$@" -lm
}

# Akses register GPU lewat libpci dan NVML hanya dipakai pada build NVIDIA,
# jadi build CPU-only tidak butuh library tambahan.
build ryzen
build cpuf
build sens "${NVIDIA_LIBS[@]+"${NVIDIA_LIBS[@]}"}"
build powerusage "${NVIDIA_LIBS[@]+"${NVIDIA_LIBS[@]}"}"

echo -e "\e[32m✔ Build selesai.\e[0m"

case "${1:-}" in
    install)
        install -d "$PREFIX/bin"
        install -m 0755 ryzen cpuf powerusage sens "$PREFIX/bin/"

        echo -e "\e[32m✔ Terpasang ke $PREFIX/bin\e[0m"
        ;;
    "")
        ;;
    *)
        echo "Usage: $0 [install]" >&2

        exit 1
        ;;
esac
