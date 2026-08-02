# 🔌 Panduan Pinout & Wiring Diagram: Scream Meter Game

Dokumen ini berisi panduan lengkap pengkabelan (*wiring diagram*) dan pemetaan pin (*pinout*) untuk proyek **Scream Meter (Game Teriak)**. Panduan ini menyajikan instruksi detail baik untuk mikrokontroler **ESP32 Classic (WROOM-32)** maupun **ESP32-C3 Super Mini**.

---

## 📌 Ringkasan Komparatif Pinout

Tabel berikut memudahkan perbandingan cepat jalur pin antar modul untuk kedua jenis mikrokontroler:

| Modul / Komponen | Pin Modul | ESP32 Classic (30/38 Pin) | ESP32-C3 Super Mini | Jalur & Proteksi Spesial |
| :--- | :--- | :--- | :--- | :--- |
| **MAX4466 Mic** | OUT | **GPIO 34** (ADC1_CH6) | **GPIO 0** (ADC1_CH0) | Analog Input (ADC1 Wajib) |
| | VCC | **3V3** | **3V3** | Daya 3.3V Stabil (Bebas Noise) |
| | GND | **GND** | **GND** | Ground Bersama |
| **WS2812B NeoPixel**| DIN | **GPIO 27** | **GPIO 10** | Sinyal Data RGB LED |
| | VCC | **5V / 3V3** | **5V / 3V3** | Daya Utama LED Strip |
| | GND | **GND** | **GND** | Ground Bersama |
| **DFPlayer Mini** | RX (Pin 2) | **GPIO 17** (TX2) | **GPIO 6** (TX1) | **Wajib Seri Resistor 1kΩ** |
| | TX (Pin 3) | **GPIO 16** (RX2) | **GPIO 7** (RX1) | Jalur Data Serial RX ESP |
| | VCC (Pin 1)| **5V / VIN** | **5V / VCC** | Daya 5V (Amp Speaker 3W) |
| | GND (Pin 7)| **GND** | **GND** | Ground Bersama |
| **Tombol Start** | PIN | **GPIO 26** | **GPIO 4** | Terhubung ke GND (`INPUT_PULLUP`) |
| **Tombol Reset** | PIN | **GPIO 25** | **GPIO 5** | Terhubung ke GND (`INPUT_PULLUP`) |

---

## 1. 🟦 Skema Wiring: ESP32 Classic (WROOM-32)

ESP32 Classic (DevKit v1 / 30-Pin / 38-Pin) memiliki banyak GPIO. Namun, pemilihan pin harus memperhatikan batasan hardware internal (seperti pin ADC1 vs ADC2, serta pin input-only).

### Tabel Detail Pinout ESP32 Classic

| Modul | Pin Modul | Pin ESP32 Classic | Tipe Pin | Fungsi & Alasan Pemilihan Pin |
| :--- | :--- | :--- | :--- | :--- |
| **MAX4466** | OUT | **GPIO 34** | ADC1_CH6 | **Input-Only (GPI)**. Sangat aman karena termasuk ADC1 yang tidak terganggu saat Wi-Fi aktif. |
| | VCC | **3V3** | Power | Tegangan 3.3V membuat DC bias mikrofon (1.65V) sangat stabil. |
| | GND | **GND** | Power | Ground |
| **WS2812B** | DIN | **GPIO 27** | Output | GPIO Output standar yang terisolasi dari memori flash dan bus SPI. |
| | VCC | **5V / 3V3** | Power | Gunakan 5V jika LED berjumlah banyak (62 LED) agar warna tidak pudar. |
| | GND | **GND** | Power | Ground |
| **DFPlayer** | RX | **GPIO 17** | UART2 TX | Menghubungkan TX hardware UART2 ke RX DFPlayer (**Wajib Resistor 1kΩ Seri**). |
| | TX | **GPIO 16** | UART2 RX | Menghubungkan RX hardware UART2 ke TX DFPlayer. |
| | VCC | **5V / VIN** | Power | DFPlayer butuh 5V untuk menghidupkan amplifier speaker internal (4Ω 3W). |
| | GND | **GND** | Power | Ground |
| **Tombol Start**| Terminal 1 | **GPIO 26** | Input | Menggunakan internal pull-up (`INPUT_PULLUP`). |
| | Terminal 2 | **GND** | Power | Dihubungkan ke GND saat ditekan. |
| **Tombol Reset**| Terminal 1 | **GPIO 25** | Input | Menggunakan internal pull-up (`INPUT_PULLUP`). |
| | Terminal 2 | **GND** | Power | Dihubungkan ke GND saat ditekan. |

### Diagram Diagram Blok Connection (ESP32 Classic)

```
                       +-------------------------+
                       |   ESP32 Classic 30-Pin  |
                       |                         |
MAX4466 OUT ---------->| GPIO 34 (ADC1_CH6)      |
WS2812B DIN <----------| GPIO 27                 |
Tombol Start --------->| GPIO 26 (PULLUP)        |
Tombol Reset --------->| GPIO 25 (PULLUP)        |
                       |                         |
DFPlayer TX ---------->| GPIO 16 (RX2)           |
DFPlayer RX <---[1kΩ]--| GPIO 17 (TX2)           |
                       |                         |
3.3V Out --------------| 3V3                     |---> VCC MAX4466
5V / VIN Out ----------| 5V / VIN                |---> VCC DFPlayer & WS2812B
GND -------------------| GND                     |---> GND (Semua Modul)
                       +-------------------------+
```

---

## 2. 🟧 Skema Wiring: ESP32-C3 Super Mini

ESP32-C3 Super Mini memiliki ukuran sangat ringkas (22.5 x 18 mm) dengan total 16 pin header (8 pin di sisi kiri, 8 pin di sisi kanan). Karena jumlah GPIO terbatas dan beberapa pin merupakan *strapping pins*, pemilihan pin harus sangat teliti.

### Visual Diagram Header Pinout ESP32-C3 Super Mini

```
                     +-------------------+
                     | [ USB Type-C ]    |
             5V  --- | [1]           [16]| --- GPIO 21 (U0TXD)
            GND  --- | [2]           [15]| --- GPIO 20 (U0RXD)
            3V3  --- | [3]           [14]| --- GPIO 10 (DIN NeoPixel)
  (Mic)  GPIO 0  --- | [4]           [13]| --- GPIO 9  (BOOT Strapping)
         GPIO 1  --- | [5]           [12]| --- GPIO 8  (Strapping / CS)
 (LED)   GPIO 2  --- | [6]           [11]| --- GPIO 7  (DFPlayer TX)
         GPIO 3  --- | [7]           [10]| --- GPIO 6  (DFPlayer RX via 1kΩ)
(Start)  GPIO 4  --- | [8]           [9] | --- GPIO 5  (Reset Button)
                     +-------------------+
```

### Tabel Detail Pinout ESP32-C3 Super Mini

| Modul | Pin Modul | Pin ESP32-C3 | Tipe Pin | Fungsi & Alasan Pemilihan Pin |
| :--- | :--- | :--- | :--- | :--- |
| **MAX4466** | OUT | **GPIO 0** | ADC1_CH0 | Channel ADC1 paling presisi. Bebas dari masalah *strapping mode*. |
| | VCC | **3V3** | Power | Tegangan 3.3V teregulasi dari board. |
| | GND | **GND** | Power | Ground |
| **WS2812B** | DIN | **GPIO 10** | Output | Pin digital output aman tanpa fungsi strapping khusus saat boot. |
| | VCC | **5V / 3V3** | Power | Dihubungkan ke pin 5V board ESP32-C3 Super Mini. |
| | GND | **GND** | Power | Ground |
| **DFPlayer** | RX | **GPIO 6** | UART1 TX | Berfungsi sebagai Hardware Serial TX (**Wajib Seri Resistor 1kΩ**). |
| | TX | **GPIO 7** | UART1 RX | Berfungsi sebagai Hardware Serial RX. |
| | VCC | **5V** | Power | Dihubungkan ke pin 5V (USB VBUS). |
| | GND | **GND** | Power | Ground |
| **Tombol Start**| Terminal 1 | **GPIO 4** | Input | Menggunakan internal pull-up (`INPUT_PULLUP`). |
| | Terminal 2 | **GND** | Power | Dihubungkan ke GND saat ditekan. |
| **Tombol Reset**| Terminal 1 | **GPIO 5** | Input | Menggunakan internal pull-up (`INPUT_PULLUP`). |
| | Terminal 2 | **GND** | Power | Dihubungkan ke GND saat ditekan. |

> [!WARNING]
> **Hindari Penggunaan Pin Strapping Berikut untuk Sakelar / Tombol:**
> - **GPIO 9**: Pin tombol BOOT internal. Jika tertarik ke GND saat booting, ESP32-C3 akan masuk ke mode *ROM Flash Downloader* dan gagal menjalankan program!
> - **GPIO 8**: Harus dalam keadaan HIGH saat booting agar ESP32-C3 dapat membaca memori Flash SPI internal.
> - **GPIO 2**: Terhubung dengan On-board LED biru pada mayoritas modul ESP32-C3 Super Mini.

---

## 3. ⚙️ Penyesuaian Kode Program (C++ / PlatformIO)

Agar kode program dapat dikompilasi secara fleksibel baik untuk ESP32 Classic maupun ESP32-C3 Super Mini, Anda dapat menggunakan direktif kompilator (`#ifdef` / `#if defined`) pada file `src/main.cpp`:

```cpp
// ==========================================
// KONFIGURASI PIN OTOMATIS BERDASARKAN BOARD
// ==========================================
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV)
  // Konfigurasi Pin untuk ESP32-C3 Super Mini
  #define MIC_PIN             0     // GPIO 0 (ADC1_CH0)
  #define DFPLAYER_RX         7     // GPIO 7 terhubung ke TX DFPlayer
  #define DFPLAYER_TX         6     // GPIO 6 terhubung ke RX DFPlayer (via 1kΩ)
  #define NEOPIXEL_PIN        10    // GPIO 10 terhubung ke DIN WS2812B
  #define START_BUTTON_PIN    4     // GPIO 4 terhubung ke Tombol Start
  #define RESET_BUTTON_PIN    5     // GPIO 5 terhubung ke Tombol Reset

  // ESP32-C3 hanya memiliki Hardware Serial0 dan Serial1
  #define dfSerial Serial1

#else
  // Konfigurasi Pin Default untuk ESP32 Classic (WROOM-32)
  #define MIC_PIN             34    // GPIO 34 (ADC1_CH6)
  #define DFPLAYER_RX         16    // GPIO 16 (UART2 RX)
  #define DFPLAYER_TX         17    // GPIO 17 (UART2 TX via 1kΩ)
  #define NEOPIXEL_PIN        27    // GPIO 27 terhubung ke DIN WS2812B
  #define START_BUTTON_PIN    26    // GPIO 26 terhubung ke Tombol Start
  #define RESET_BUTTON_PIN    25    // GPIO 25 terhubung ke Tombol Reset

  // ESP32 Classic menggunakan Serial2 (UART2)
  #define dfSerial Serial2
#endif
```

---

## 4. ⚡ Panduan Catu Daya & Proteksi Elektronik

> [!IMPORTANT]
> **1. Resistor 1kΩ Seri pada RX DFPlayer Mini (WAJIB)**
> * Pasang resistor **1kΩ** secara seri di jalur transmisi dari ESP (GPIO 17 pada Classic atau GPIO 6 pada C3) menuju pin **RX DFPlayer Mini**.
> * **Alasan**: Meredam derau arus balik sinyal serial (*serial noise/humming* pada speaker) dan menyesuaikan toleransi logika tegangan 3.3V ke 5V.

> [!TIP]
> **2. Skema Grounding (Common Ground)**
> * Pastikan **semua pin GND** dari MAX4466, WS2812B, DFPlayer Mini, tombol, dan ESP32 terhubung menjadi **satu titik ground bersama** (Common Ground).
> * Ground yang terpisah akan menyebabkan pembacaan sinyal analog mikrofon menjadi fluktuatif (*noisy*) serta komunikasi UART DFPlayer sering *mismatch*.

> [!CAUTION]
> **3. Kebutuhan Arus Listrik WS2812B & DFPlayer Mini**
> * Strip WS2812B dengan 62 LED pada warna putih penuh dapat menyedot arus hingga **~3.5 Ampere** pada tegangan 5V.
> * Jika menggunakan USB port komputer/laptop biasa (maksimal 500mA - 1A), disarankan membatasi kecerahan LED di kode program (misal `pixels.setBrightness(120);`) atau gunakan adaptor daya eksternal 5V (2A - 5A) khusus untuk LED Strip.

---

## 5. 🔍 Langkah Pengujian & Diagnostik Jalur Pin

Setelah melakukan perakitan kabel, lakukan pengujian bertahap melalui **Serial Monitor (Baud rate: 9600)**:

1. **Uji Mikrofon (MAX4466)**:
   * Saat diam, periksa log `[ADC Debug] Bias: ...`.
   * Pada ESP32 Classic (3.3V ADC1), nilai bias ideal berada di kisaran **~1900 - 2100**.
   * Pada ESP32-C3 Super Mini, nilai bias ideal berada di kisaran **~1900 - 2200**.
2. **Uji DFPlayer Mini**:
   * Jika pesan `DFPlayer Mini terdeteksi & online!` muncul di serial, berarti jalur TX/RX dan resistor 1kΩ sudah terpasang dengan benar.
   * Jika muncul pesan error `DFPlayer Mini tidak merespon/terdeteksi`, tukar posisi kabel TX dan RX DFPlayer.
3. **Uji Tombol Start & Reset**:
   * Tekan tombol Start (GPIO 26/4) -> Serial Monitor harus menampilkan pesan `Tombol Start Ditekan. Memulai countdown...`.
   * Tekan tombol Reset (GPIO 25/5) -> Serial Monitor harus menampilkan pesan `Tombol Reset Ditekan. Menghapus rekor tertinggi...`.
