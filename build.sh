#!/usr/bin/env bash
#
# build.sh - Build the four Ryzen Power tools.
#
# Environment variables:
#   CC           Compiler (default: gcc)
#   ENABLE_DRAM  1/true/yes/on also prints the DRAM (spd5118) temperature
#   NVIDIA       0/false/off forces a build without NVIDIA support
#   CUDA_DIR     Directory of the CUDA SDK (default: /opt/cuda)
#   PREFIX       Target of "install" (default: /usr/local)
#
# Example:
#   ./build.sh                      Build only
#   ENABLE_DRAM=1 ./build.sh        Build with the DRAM temperature
#   sudo ./build.sh install         Build, then install into $PREFIX/bin

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

# The DRAM temperature (spd5118 sensor) is off by default. Values 0/false/off/no
# keep it off, so ENABLE_DRAM=0 does not enable it.
DRAM_FLAGS=()
case "${ENABLE_DRAM:-0}" in
    1 | true | True | TRUE | yes | Yes | YES | on | On | ON)
        echo -e "\e[36m\u2139 DRAM temperature (spd5118) enabled.\e[0m"

        DRAM_FLAGS=(-DENABLE_DRAM)
        ;;
esac

# NVIDIA support is on when the SDK exists, unless NVIDIA=0 forces it off.
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

# build <target-name> [libraries...]
build()
{
    local target="$1"
    shift

    "$CC" "${OPT_FLAGS[@]}" "${STRICT_FLAGS[@]}" \
        ${NVIDIA_FLAGS[@]+"${NVIDIA_FLAGS[@]}"} \
        ${DRAM_FLAGS[@]+"${DRAM_FLAGS[@]}"} \
        -o "$target" "$target.c" "$@" -lm
}

# libpci and NVML are needed only for NVIDIA support. The tools read GPU
# registers through /dev/mem only with -DNVIDIA_GPU. NVIDIA_LIBS is empty
# for a CPU-only build, so a CPU-only build needs nothing beyond the
# compiler.
build ryzen
build cpuf
build sens "${NVIDIA_LIBS[@]+"${NVIDIA_LIBS[@]}"}"
build powerusage "${NVIDIA_LIBS[@]+"${NVIDIA_LIBS[@]}"}"

echo -e "\e[32m✔ Build completed.\e[0m"

case "${1:-}" in
    install)
        # The usual PREFIX is /usr/local, which needs root. A non-root run
        # with sudo available uses sudo automatically. Without sudo, the
        # install commands run as the current user and fail with a clear
        # message when the target is not writable.
        SUDO=""

        if [ "$(id -u)" -ne 0 ] && command -v sudo >/dev/null 2>&1; then
            SUDO="sudo"
        fi

        $SUDO install -d "$PREFIX/bin"
        $SUDO install -m 0755 ryzen cpuf powerusage sens "$PREFIX/bin/"

        echo -e "\e[32m✔ Installed to $PREFIX/bin\e[0m"

        # The systemd units start powerusage as a daemon. Install them into
        # /etc/systemd/system only when systemd exists and we may write there.
        # The ExecStart line names /usr/local/bin; rewrite it to $PREFIX/bin
        # for a custom PREFIX.
        if command -v systemctl >/dev/null 2>&1; then
            if [ -n "$SUDO" ] || [ -w /etc/systemd/system ]; then
                $SUDO install -m 0644 powerusage-cpu.service powerusage-gpu.service /etc/systemd/system/
                $SUDO sed -i "s|/usr/local/bin|$PREFIX/bin|" /etc/systemd/system/powerusage-cpu.service /etc/systemd/system/powerusage-gpu.service
                $SUDO systemctl daemon-reload

                # A running unit keeps the old binary in memory, so it needs
                # a restart to pick up the new one. On a stopped unit, restart
                # starts it. enable only records the boot symlink here,
                # because "enable --now" would not restart a running unit.
                $SUDO systemctl enable powerusage-cpu powerusage-gpu
                $SUDO systemctl restart powerusage-cpu powerusage-gpu

                echo -e "\e[32m✔ Enabled, started, and updated the powerusage services.\e[0m"
            else
                echo -e "\e[33mℹ /etc/systemd/system is not writable. Skip the systemd units.\e[0m"
            fi
        fi
        ;;
    "")
        ;;
    *)
        echo "Usage: $0 [install]" >&2

        exit 1
        ;;
esac
