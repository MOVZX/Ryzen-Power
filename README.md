# Ryzen Power

Command-line utilities that read hardware sensors on Linux. They read sysfs, hwmon and
`/proc` directly, and call `dmidecode`, `nvidia-smi` or `rocm-smi` for the rest.

The tools print plain text. `powerusage` uses Nerd Font glyphs, so install a Nerd Font
for correct icons.

![Screenshot](screenshot.png)
![Screenshot](screenshot-stats.png)

## Tools

1. `ryzen` — CPU power draw in watts. Prints one number.
2. `cpuf` — live monitor: CPU name, temperatures, power, and per-core frequency.
3. `powerusage` — one status line for the CPU or the GPU. Made for panels and bars.
4. `sens` — full one-shot sensor report for the whole system.

## Prerequisites

- `gcc` (or set `CC` to another compiler)
- AMD GPU: `rocm-smi`
- NVIDIA GPU: `nvidia-smi`, `libpci`, and the NVIDIA Management Library (NVML)
- CUDA SDK: used only to detect NVIDIA support at build time
- `dmidecode`: optional, only for the DRAM part number in `sens`

A CPU-only build needs nothing beyond a C compiler. `libpci` and NVML are linked only
when the build has NVIDIA support.

## Build

```bash
./build.sh
```

The script looks for `/opt/cuda`. If it finds that directory, it builds `powerusage` and
`sens` with `-DNVIDIA_GPU`. Otherwise it builds them for AMD or Intel only.

All four tools build with `-Wall -Werror -Wextra` and the other strict warnings.

Environment variables:

| Variable | Default | Effect |
| --- | --- | --- |
| `CC` | `gcc` | Compiler to use |
| `ENABLE_DRAM` | off | Also print DRAM (spd5118) temperatures |
| `NVIDIA` | autodetect | `0`, `false`, `no` or `off` forces a CPU-only build |
| `CUDA_DIR` | `/opt/cuda` | Where to look for the CUDA SDK |
| `PREFIX` | `/usr/local` | Install prefix for `./build.sh install` |

### DRAM temperature (spd5118)

DRAM sensor output is off by default. Enable it at build time:

```bash
ENABLE_DRAM=1 ./build.sh
```

Accepted "on" values are `1`, `true`, `yes` and `on`. `ENABLE_DRAM=0` stays off, as does
any other value or leaving the variable unset. The flag adds two DRAM readings to
`powerusage cpu` and adds the DRAM section to `sens`. It is fixed at compile time, so
rebuild to change it.

## Install

The binaries run from the current directory. To install them on the PATH:

```bash
sudo ./build.sh install
```

That copies all four tools to `$PREFIX/bin`. Set `PREFIX` to install somewhere else, for
example `PREFIX="$HOME/.local" ./build.sh install`.

Reinstall after every rebuild. Otherwise you keep testing the old copy.

## Usage

### `ryzen`

```bash
ryzen
```

Output: one float, watts. Example: `41.77`

### `cpuf`

```bash
cpuf
```

Redraws every second until you press Ctrl+C. It shows:

- CPU model name, read from `/proc/cpuinfo`
- Every k10temp label it finds (Tctl, Tccd1, Tccd2, ...)
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
󰻠 8 % | 󰾾 5.523 MHz | 󰾾 5.745 MHz |  37.2 GB |  67 °C |  70 °C |  70 °C | 󰚥 84 W
```

`powerusage gpu` detects the GPU vendor and prints utilization, core clock, memory
clock, framebuffer use, temperatures, and power. AMD uses `rocm-smi`. NVIDIA uses NVML.

**NVIDIA memory and hotspot temperatures need root.** These are not in NVML. The tool
reads them straight from the GPU BAR0 registers through `/dev/mem`:

```bash
sudo powerusage gpu
```

Your kernel must allow `/dev/mem` reads of device memory. Add `iomem=relaxed` to the
kernel command line if the read returns nothing. Memory and hotspot temperature then
show as 0.

### `sens`

```bash
./sens
```

`sens` runs without root. The board name comes from `/sys/class/dmi/id`, the CPU name
from `/proc/cpuinfo`, and every other value from sysfs. Two readings need root:

1. The DRAM part number, which lives in the SMBIOS tables (`dmidecode`).
2. The NVIDIA VRAM temperature, which is read from GPU BAR0 registers.

Without root you still get a full report. Those two lines show a generic label or 0.
Run `sudo sens` to fill them in.

The report covers:

- Board vendor and product name
- Board temperatures: `Mobo`, `VRM`, and the two PCH chipset sensors
- Fan speeds (radiator, front, rear, pump, bottom)
- CPU model, temperatures and power
- DRAM part number and temperature (only with `ENABLE_DRAM=1`)
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
  also works on AMD. The file must be readable by your user. Kernel 7.x wraps that
  counter at `max_energy_range_uj`, so the tools correct for the wrap. Without root, a
  kernel patch may be needed to make `energy_uj` world-readable.
- **`/tmp/cpu_stats.txt`.** `powerusage cpu` stores `AVG`, `MAX` and `CUR` frequency
  there. `MAX` only ratchets upward, so the peak survives between runs. The file is
  locked with `flock()` because panel pollers and manual runs share it, and it is opened
  with `O_NOFOLLOW` so a symlink planted in `/tmp` cannot redirect the writes. Delete the
  file to reset the peak. If the file cannot be opened, the tool still prints a line and
  shows the current sample instead of the stored peak.
- **Debug output.** `powerusage` keeps quiet when a sensor read fails, because a panel
  would show the noise. Set `RYZEN_POWER_DEBUG=1` to send those messages to stderr.
- **PCH chipset sensors.** `powerusage cpu` reads both `prom21_xhci` chips and prints
  them right after the CPU temperature. Their sysfs labels are empty, so the order comes
  from the PCI address: the first PCH field is `11:00.0`, the second is `13:00.0`. That
  matches the "PCH Chipset #1" and "PCH Chipset #2" names that `sensors` shows. A missing
  chip prints 0.
- **Temperature rounding.** `powerusage cpu` and `cpuf` round temperatures to the nearest
  degree, so `70965` millidegrees shows as `71`. That matches `sensors`. Integer division
  would show `70` instead.
- **Board channels (ASRock X870E Nova WiFi).** `sens` uses these super I/O channels:

  | Line | Source | Value check |
  | --- | --- | --- |
  | `Mobo` | `nct6799` `temp1` = `SYSTIN` | `sensors` labels the same channel "Motherboard" |
  | `VRM` | `nct6799` `temp3` = `AUXTIN0` | not labelled by `sensors`; confirmed by measurement |
  | `PCH 1`, `PCH 2` | `prom21_xhci` at PCI `11:00.0` and `13:00.0` | matches `sensors` "PCH Chipset #1/#2" |

  Other NCT67xx channels on this board read 17 °C, 0 °C or −62 °C, so they stay unused.
  On a different board, check the channels before trusting the labels.
- **Shared code.** Common helpers (RAPL power with wrap correction, hwmon lookup,
  labelled temperature scan, PCH lookup, command execution) live in `sensors_common.h`.
  Each tool stays one `.c` file, so you can still compile any of them by hand:
  `gcc -O3 -o cpuf cpuf.c`.
- **Fan and chip selection.** `sens` matches super I/O chips by the `nct6` name prefix
  and picks the instance with the most fan inputs. The fan index to header mapping is
  fixed: fan1 radiator, fan2 front, fan3 rear, fan4 pump, fan6 bottom 1, fan7 bottom 2.

## License

GNU General Public License v2.0. See [LICENSE](LICENSE).
