# Ryzen Power

Kumpulan utilitas baris perintah sederhana untuk memantau berbagai sensor perangkat keras pada sistem Linux, dengan fokus pada CPU AMD Ryzen. Utilitas ini membaca data langsung dari antarmuka sysfs dan mengeksekusi beberapa perintah eksternal untuk mendapatkan informasi yang komprehensif.

![Screenshot](screenshot.png)
![Screenshot](screenshot-stats.png)

## Fitur

Proyek ini menyediakan empat utilitas terpisah:

1.  `ryzen`: Menampilkan konsumsi daya CPU saat ini dalam Watt.
2.  `cpuf`: Menampilkan frekuensi, suhu, dan daya CPU secara real-time.
3.  `powerusage`: Menampilkan statistik penggunaan untuk CPU atau GPU.
4.  `sens`: Utilitas pemantauan sensor lengkap untuk seluruh sistem.

## Prasyarat

- `gcc` (GNU Compiler Collection)
- `libpci-dev` (atau yang setara) untuk `powerusage`
- `lm-sensors` (opsional, untuk beberapa data sensor)
- `rocm-smi` (untuk GPU AMD)
- `nvidia-ml` (NVML) dan `CUDA SDK` (untuk GPU NVIDIA)
- `dmidecode`

## Kompilasi

Untuk mengkompilasi semua utilitas, jalankan skrip build:

```bash
./build.sh
```

Skrip akan secara otomatis mendeteksi keberadaan NVIDIA CUDA SDK dan mengkompilasi `powerusage` dengan dukungan untuk GPU NVIDIA jika ditemukan.

## Penggunaan

### 1. `ryzen`

Utilitas paling dasar. Cukup jalankan untuk mendapatkan konsumsi daya CPU saat ini.

```bash
./ryzen
```

Outputnya adalah nilai tunggal dalam Watt.

### 2. `cpuf`

Menampilkan informasi terperinci tentang CPU, termasuk nama model, suhu (Tctl/Tccd), konsumsi daya, dan frekuensi setiap inti.

```bash
./cpuf
```

### 3. `powerusage`

Menyediakan statistik untuk CPU atau GPU. Anda harus menentukan target (`cpu` atau `gpu`).

**Untuk CPU:**

```bash
./powerusage cpu
```

Menampilkan penggunaan CPU (%), penggunaan memori (GB), suhu CPU, suhu DRAM, dan konsumsi daya CPU.

**Untuk GPU:**

```bash
./powerusage gpu
```

Menampilkan penggunaan GPU (%), penggunaan VRAM (%), suhu (tepi, sambungan, memori), dan konsumsi daya GPU. Mendukung GPU AMD (melalui `rocm-smi`) dan NVIDIA (melalui `NVML`).

### 4. `sens`

Alat pemantauan sensor terlengkap. Memberikan gambaran umum tentang berbagai suhu dan kecepatan kipas di seluruh sistem.

```bash
./sens
```

Output mencakup:
- Informasi sistem (Motherboard)
- Suhu Motherboard (Mobo, VRM, Chipset)
- Kecepatan Kipas
- Informasi CPU (Nama, Suhu, Daya)
- Informasi DRAM (Model, Suhu)
- Informasi GPU (Suhu, Daya)
- Informasi SSD NVMe (Suhu)

## Lisensi

Proyek ini dilisensikan di bawah Lisensi Publik Umum GNU v2.0. Lihat file `LICENSE` untuk detailnya.
