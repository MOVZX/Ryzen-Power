# Ryzen Power

Command-line utilities that read hardware sensors on Linux. They read sysfs, hwmon and
`/proc` directly. They call `dmidecode`, `nvidia-smi` or `rocm-smi` for the rest.

The tools print plain text. `powerusage` uses Nerd Font glyphs. Install a Nerd Font to
get the correct icons.

![Screenshot](screenshot.png)
![Screenshot](screenshot-stats.png)

## Tools

1. `ryzen` — CPU power draw in watts. It prints one number.
2. `cpuf` — live monitor for the CPU name, temperatures, power and per-core frequency.
3. `powerusage` — one status line for the CPU or the GPU. Use it for panels and status bars.
4. `sens` — full one-shot sensor report for the whole system.

## Prerequisites

1. `gcc`. Set `CC` to use another compiler.
2. AMD GPU: `rocm-smi`.
3. NVIDIA GPU: `nvidia-smi`, `libpci` and the NVIDIA Management Library (NVML).
4. CUDA SDK. The build script uses it only to find NVIDIA support.
5. `dmidecode`. This is optional. `sens` needs it only for the DRAM part number.

A CPU-only build needs nothing beyond a C compiler. The build script links `libpci` and
NVML only when the build has NVIDIA support.

## Build

```bash
./build.sh
```

The script looks for `/opt/cuda`. If it finds that directory, it builds `powerusage` and
`sens` with `-DNVIDIA_GPU`. Otherwise it builds them for AMD or Intel only.

All four tools compile with `-Wall -Werror -Wextra` and the other strict warning flags.

Environment variables:

| Variable      | Default      | Effect                                              |
| ------------- | ------------ | --------------------------------------------------- |
| `CC`          | `gcc`        | Compiler to use                                     |
| `ENABLE_DRAM` | off          | Also print the DRAM (spd5118) temperatures          |
| `NVIDIA`      | autodetect   | `0`, `false`, `no` or `off` forces a CPU-only build |
| `CUDA_DIR`    | `/opt/cuda`  | Directory of the CUDA SDK                           |
| `PREFIX`      | `/usr/local` | Install prefix for `./build.sh install`             |

### DRAM temperature (spd5118)

The DRAM sensor output is off by default. Enable it at build time:

```bash
ENABLE_DRAM=1 ./build.sh
```

The accepted "on" values are `1`, `true`, `yes` and `on`. `ENABLE_DRAM=0` keeps it off.
Any other value, and no value at all, also keeps it off. The flag adds two DRAM readings
to `powerusage cpu` and adds the DRAM section to `sens`. The compiler fixes this value,
so you must rebuild to change it.

## Install

The binaries run from the current directory. To install them on the PATH, run this
command as root:

```bash
sudo ./build.sh install
```

The command copies all four tools to `$PREFIX/bin`. Set `PREFIX` to install somewhere
else. Example: `PREFIX="$HOME/.local" ./build.sh install`.

Reinstall after every rebuild. If you do not reinstall, you keep testing the old copy.

## Usage

### `ryzen`

```bash
ryzen
```

The output is one number, in watts. Example: `41.77`

### `cpuf`

```bash
cpuf
```

The tool redraws the screen every second until you press Ctrl+C. It shows:

- CPU model name, read from `/proc/cpuinfo`
- Every k10temp label that it finds, for example Tctl, Tccd1 and Tccd2
- CPU package power in watts
- Current and peak frequency for every logical core

The `max` value per core counts from the moment you start the program.

### `powerusage`

The first argument selects the target:

```bash
powerusage cpu
powerusage gpu
```

`powerusage cpu` prints CPU usage, current core frequency, peak recorded frequency,
used memory, CPU temperature, the two PCH chipset temperatures, and power:

```
󰻠 1 % | 󰾾 5.696 MHz | 󰾾 5.857 MHz |  32.8 GB |  65 °C |  71 °C |  71 °C | 󰚥 56 W
```

`powerusage gpu` finds the GPU vendor, then prints utilization, core clock, memory clock,
framebuffer use, temperatures and power. AMD uses `rocm-smi`. NVIDIA uses NVML.

The NVIDIA memory and hotspot temperatures need root. NVML does not give these two
values. The tool reads them from the GPU BAR0 registers through `/dev/mem`:

```bash
sudo powerusage gpu
```

Your kernel must allow `/dev/mem` reads of device memory. If the read returns nothing,
add `iomem=relaxed` to the kernel command line. The memory and hotspot temperature then
show as 0.

### `sens`

```bash
./sens
```

`sens` runs without root. The board name comes from `/sys/class/dmi/id`, the CPU name
from `/proc/cpuinfo`, and every other value from sysfs. Two readings need root:

1. The DRAM part number, which lives in the SMBIOS tables (`dmidecode`).
2. The NVIDIA VRAM temperature, which comes from the GPU BAR0 registers.

Without root you still get a full report. Those two lines show a generic label or 0. Run
`sens` as root to fill them in.

The report covers:

- Board vendor and product name
- Board temperatures: `Mobo`, `VRM` and the two PCH chipset sensors
- Fan speeds for the radiator, front, rear, pump and bottom positions
- CPU model, temperatures and power
- DRAM part number and temperature, only with `ENABLE_DRAM=1`
- GPU name, temperature, VRAM temperature and power
- Each NVMe drive and its NAND temperature

```
ASRock X870E Nova WiFi
Mobo     : 50.00°C
VRM      : 50.00°C
PCH 1    : 70.97°C
PCH 2    : 70.06°C

Radiator : 920 RPM
Front    : 1104 RPM
Rear     : 1062 RPM
Pump     : 2343 RPM
Bottom 1 : 845 RPM
Bottom 2 : 882 RPM

AMD Ryzen 9 9950X3D 16-Core Processor
Tctl     : 65.50°C
Tccd1    : 63.00°C
Tccd2    : 46.75°C
Power    : 57.60 W
```

## Notes

- **RAPL path.** The tools read `/sys/class/powercap/intel-rapl:0/energy_uj`. That path
  also works on AMD. Your user must be able to read that file. New kernels wrap the
  counter at `max_energy_range_uj`, so the tools correct for the wrap. If your user
  cannot read `energy_uj`, a kernel patch can make the file world-readable.
- **`/tmp/cpu_stats.txt`.** `powerusage cpu` stores the `AVG`, `MAX` and `CUR` frequency
  in that file. `MAX` can only rise, so the peak survives between runs. Delete the file
  to reset the peak. Panel pollers and manual runs share the file, so the tool locks it
  with `flock()`. The tool also opens it with `O_NOFOLLOW`, so a symlink in `/tmp` cannot
  redirect the write. If the tool cannot open the file, it still prints one line and
  shows the current sample instead of the stored peak.
- **Debug output.** `powerusage` says nothing when a sensor read fails. A panel then
  prints such a message as its own line. Set `RYZEN_POWER_DEBUG=1` to send those messages
  to stderr.
- **PCH chipset sensors.** `powerusage cpu` reads both `prom21_xhci` chips and prints
  them after the CPU temperature. Their sysfs labels are empty, so the order comes from
  the PCI address. The first PCH field is `11:00.0` and the second is `13:00.0`. That
  matches the "PCH Chipset #1" and "PCH Chipset #2" names that `sensors` shows. A missing
  chip prints 0.
- **Temperature rounding.** `powerusage cpu` and `cpuf` round temperatures to the nearest
  degree, so `70965` millidegrees shows as `71`. That matches `sensors`. Integer division
  shows `70` instead.
- **Board channels (ASRock X870E Nova WiFi).** `sens` uses these super I/O channels:

    | Line             | Source                                       | Verification                                             |
    | ---------------- | -------------------------------------------- | -------------------------------------------------------- |
    | `Mobo`           | `nct6799` `temp1` = `SYSTIN`                 | `sensors` gives the same channel the label "Motherboard" |
    | `VRM`            | `nct6799` `temp3` = `AUXTIN0`                | `sensors` gives no label. Measurement confirms it        |
    | `PCH 1`, `PCH 2` | `prom21_xhci` at PCI `11:00.0` and `13:00.0` | matches `sensors` "PCH Chipset #1/#2"                    |

    The other NCT67xx channels on this board read 17 °C, 0 °C or −62 °C, so the tools do
    not use them. On a different board, examine the channels before you trust the labels.

- **Fan and chip selection.** `sens` matches super I/O chips on the `nct6` name prefix
  and selects the instance with the most fan inputs. The fan index to header mapping is
  fixed: fan1 radiator, fan2 front, fan3 rear, fan4 pump, fan6 bottom 1 and fan7 bottom 2.
- **Shared code.** Common helpers live in `sensors_common.h`: RAPL power with wrap
  correction, hwmon lookup, labelled temperature scan, PCH lookup and command execution.
  Each tool stays one `.c` file, so you can compile any tool by hand. Example:
  `gcc -O3 -o cpuf cpuf.c`.

## License

GNU General Public License v2.0. See [LICENSE](LICENSE).
