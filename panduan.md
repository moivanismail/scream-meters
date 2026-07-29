# 🎙️ Panduan Lengkap Proyek Scream Meter (ESP32 Classic)

Panduan ini berisi semua instruksi yang diperlukan untuk membangun, merakit, dan mengonfigurasi proyek **Scream Meter** (Game Teriak) menggunakan mikrokomputer **ESP32 Classic** (CP2102), sensor suara **MAX4466**, **DFPlayer Mini**, dan strip **NeoPixel WS2812B**.

---

## 🛠️ Komponen yang Diperlukan

1. **Mikrokontroler**: ESP32 Classic Development Board (30-Pin / 38-Pin CP2102)
2. **Sensor Suara**: MAX4466 Electret Microphone Preamplifier
3. **LED Bar**: WS2812B NeoPixel RGB LED Strip (62 LED)
4. **Modul Suara**: DFPlayer Mini MP3 Player + Speaker (4Ω 3W)
5. **Tombol**: 2x Push Button (Start & Reset)
6. **Resistor**: 1x Resistor 1kΩ (Wajib dipasang seri pada jalur RX DFPlayer)
7. **Kabel Jumper** & Breadboard secukupnya.

---

## 🔌 Skema Perkabelan (Wiring Diagram)

Untuk menghindari pin JTAG debug, jalur memori flash internal, dan pin *boot strapping* (yang dapat menyebabkan ESP32 gagal boot), gunakan pemetaan pin yang aman di bawah ini:

| Komponen | Pin Komponen | Pin ESP32 Classic | Deskripsi / Fungsi |
| :--- | :--- | :--- | :--- |
| **MAX4466 Mic** | VCC | **3V3** | Daya 3.3V (Sangat disarankan untuk stabilitas analog) |
| | GND | **GND** | Ground Bersama |
| | OUT | **GPIO 34** | Input Analog ADC1_CH6 (Input-only, tanpa pull-up internal) |
| **WS2812B LED** | VCC | **3V3** | Daya 3.3V (Membantu pencocokan level logika 3.3V) |
| | GND | **GND** | Ground Bersama |
| | DIN (Data In) | **GPIO 27** | Jalur sinyal data LED |
| **Tombol Start** | PIN | **GPIO 26** | Dihubungkan ke GND saat ditekan (INPUT_PULLUP) |
| **Tombol Reset** | PIN | **GPIO 25** | Dihubungkan ke GND saat ditekan (INPUT_PULLUP) |
| **DFPlayer Mini**| VCC | **5V / VIN** | Membutuhkan tegangan 5V stabil untuk amplifier speaker |
| | GND | **GND** | Ground Bersama |
| | TX | **GPIO 16** | Masuk ke UART2 RX2 ESP32 |
| | RX | **GPIO 17** | Dari UART2 TX2 ESP32 (Wajib dipasang resistor 1kΩ seri) |

> [!CAUTION]
> **Pasang Resistor 1kΩ secara seri** pada kabel dari pin **GPIO 17 (TX2 ESP32)** menuju pin **RX DFPlayer Mini**. Siasat ini sangat krusial untuk meredam kebisingan arus balik digital (*serial noise*) dan melindungi port input DFPlayer Mini (yang beroperasi pada tegangan logika 3.3V sensitif).

---

## 💻 Pengaturan Lingkungan Software (PlatformIO)

Gunakan konfigurasi berikut pada file `platformio.ini` Anda. Sangat penting menggunakan versi platform `@6.6.0` karena menggunakan **Arduino Core 2.0.14** yang terbukti sangat stabil dengan driver RMT NeoPixel (versi Core 3.0.x terbaru memiliki masalah stack overflow yang sering membuat NeoPixel crash).

```ini
[env:esp32dev]
platform = espressif32 @ 6.6.0
board = esp32dev
framework = arduino
monitor_speed = 9600
lib_deps =
    adafruit/Adafruit NeoPixel
```

---

## 🔬 Algoritma & Konfigurasi Khusus Kode

Program ini dirancang khusus untuk menangani karakteristik ADC ESP32 dan modul suara secara optimal:

### 1. Atenuasi ADC 11dB (Rentang Tegangan 0V - 3.3V)
Secara default, ADC ESP32 hanya mampu membaca tegangan hingga 1.1V. Karena MAX4466 memiliki tegangan diam (DC bias) sebesar **1.65V**, sirkuit akan langsung mentok di angka maksimum 4095 jika menggunakan setelan bawaan.
* Di dalam `setup()`, ditambahkan perintah **`analogSetAttenuation(ADC_11db);`** untuk memperluas jangkauan ADC menjadi **0 s.d. 3.3V**, sehingga sensor suara dapat dibaca penuh tanpa terpotong (*clipping*).

### 2. Kalibrasi Bias DC Statis
Alih-alih menebak nilai tengah sinyal analog secara dinamis saat bermain, sistem mengukur bias rata-rata secara presisi sebanyak 1.000 kali sampel selama kondisi hening saat booting (`micDCOffset`). Nilai ini dikunci dan digunakan sebagai jangkar tengah deteksi gelombang AC mikrofon.

### 3. Filter Mean Absolute Deviation (MAD)
Untuk menghindari lonjakan liar/saturasi instan akibat satu atau dua riak kebisingan (*glitch* frekuensi tinggi), program tidak menggunakan kalkulasi puncak-ke-puncak (Max - Min).
Sebagai gantinya, program menghitung **MAD (Mean Absolute Deviation)**:
$$\text{MAD} = \frac{1}{N} \sum_{i=1}^{N} |x_i - \text{Bias}|$$
Metode rata-rata deviasi mutlak ini sangat stabil dan toleran terhadap spike noise acak, menghasilkan nilai amplitudo yang linier.

### 4. Pemetaan LED Kuadratik (Quadratic Curve)
Untuk memberikan pengalaman bermain yang menantang (arcade feel), kenaikan LED bar tidak bersifat linear melainkan kuadratik (`ratio * ratio`).
* Suara sedang/teriakan kecil hanya akan menaikkan beberapa LED hijau.
* Untuk menyalakan LED kuning/merah bagian atas hingga penuh (LED ke-62), pemain dipaksa untuk berteriak dengan energi penuh (level suara mendekati batas maksimal `1700`).

---

## 🕹️ Panduan Penggunaan & Pengoperasian

1. **Inisialisasi Booting**: Saat board dinyalakan, diamkan sirkuit selama 2 detik untuk kalibrasi otomatis hening. Sistem akan memutar musik startup (`0003.mp3`) dan NeoPixel akan menampilkan efek putaran oranye lalu berkedip hijau 3 kali saat siap digunakan.
2. **Reset Rekor**: Tekan tombol Reset (GPIO 25) sekali untuk menyetel ulang rekor tertinggi tersimpan di EEPROM ke nilai default **`1000`** (LED merah akan berkedip 3 kali).
3. **Mulai Bermain**:
   * Tekan tombol Start (GPIO 26). Musik yang berjalan akan langsung dimatikan secara instan dan countdown visual dimulai.
   * Saat tulisan "MULAI BERTERIAK!" muncul di serial, berteriaklah ke arah mikrofon selama 5 detik.
   * Indikator LED Bar akan naik secara dinamis.
4. **Hasil Rekor**:
   * Jika rekor baru tercapai, musik perayaan/menang (`0001.mp3`) diputar dengan animasi pelangi (*rainbow chase*) selama 6 detik.
   * Jika gagal melampaui rekor, musik gagal (`0002.mp3`) diputar dengan visualisasi puncak teriakan Anda.
   * Setelah itu sistem kembali ke standby (`STATE_IDLE`). Musik perayaan/gagal tetap akan berputar sampai selesai (atau mati instan jika tombol Start ditekan kembali).
5. **Jarak Uji Coba**: Jarak ideal berteriak adalah **30 cm s.d. 50 cm** dari mikrofon.
