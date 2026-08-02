# 🔌 Panduan Pinout & Wiring Diagram: Scream Meter Game (PWM Audio Internal)

Dokumen ini berisi panduan lengkap pengkabelan (*wiring diagram*) dan pemetaan pin (*pinout*) untuk proyek **Scream Meter (Game Teriak)** menggunakan mikrokontroler **ESP32-C3 Super Mini** dan **ESP32 Classic** dengan **PWM Audio Engine internal** (tanpa modul DFPlayer Mini).

---

## 📌 Ringkasan Komparatif Pinout

Tabel berikut memandu penghubungan pin antar modul untuk kedua jenis mikrokontroler:

| Modul / Komponen | Pin Modul | ESP32-C3 Super Mini | ESP32 Classic (30/38 Pin) | Keterangan & Proteksi |
| :--- | :--- | :--- | :--- | :--- |
| **MAX4466 Mic** | OUT | **GPIO 0** (ADC1_CH0) | **GPIO 34** (ADC1_CH6) | Input Analog (ADC1 Wajib) |
| | VCC | **3V3** | **3V3** | Daya 3.3V Stabil (Bebas Noise) |
| | GND | **GND** | **GND** | Ground Bersama |
| **WS2812B NeoPixel**| DIN | **GPIO 10** | **GPIO 27** | Sinyal Data RGB LED Strip |
| | VCC | **5V / 3V3** | **5V / 3V3** | Daya Utama LED Strip |
| | GND | **GND** | **GND** | Ground Bersama |
| **Audio Out (AUX)** | Signal Out | **GPIO 1** (via Filter RC) | **GPIO 25** (via Filter RC) | Ke Pin Tip/Signal Speaker Aktif |
| | Ground | **GND** | **GND** | Ke Pin Sleeve/Ground Speaker |
| **Tombol Start** | PIN | **GPIO 4** | **GPIO 26** | Terhubung ke GND (`INPUT_PULLUP`) |
| **Tombol Reset** | PIN | **GPIO 5** | **GPIO 25** | Terhubung ke GND (`INPUT_PULLUP`) |

---

## 📻 Skema Rangkaian Filter Pasif RC Audio (GPIO 1 ke Speaker Aktif)

Untuk mengubah sinyal PWM frekuensi tinggi (250 kHz) dari **GPIO 1** menjadi sinyal audio analog jernih untuk colokan **AUX / Jack 3.5mm Speaker Aktif**, gunakan rangkaian filter pasif RC sederhana berikut:

```
                          1k Ohm
ESP32-C3 (GPIO 1) ------[ Resistor ]-------+-----[ 10uF Kapasitor ]-----> Sinyal AUX (Tip)
                                           |      (Kutub + ke ESP)
                                    [ 10nF / 100nF ]
                                    [  Kapasitor   ]
                                           |
GND ---------------------------------------+----------------------------> Ground AUX (Sleeve)
```

### Komponen Filter Pasif RC:
1. **Resistor 1kΩ**: Dipasang seri dari GPIO 1 ke titik temu filter.
2. **Kapasitor Keramik 10nF (atau 100nF)**: Dipasang pararel dari titik temu ke GND (berfungsi membuang frekuensi carrier PWM 250kHz ke ground).
3. **Kapasitor Elco 10uF** *(Opsional)*: Dipasang seri sebelum colokan AUX untuk memblokir DC Bias (DC Blocking Capacitor).

---

## 1. 🟧 Skema Wiring: ESP32-C3 Super Mini

### Visual Diagram Header Pinout ESP32-C3 Super Mini

```
                     +-------------------+
                     | [ USB Type-C ]    |
             5V  --- | [1]           [16]| --- GPIO 21
            GND  --- | [2]           [15]| --- GPIO 20
            3V3  --- | [3]           [14]| --- GPIO 10 (DIN NeoPixel)
  (Mic)  GPIO 0  --- | [4]           [13]| --- GPIO 9  (BOOT Strapping)
(Audio)  GPIO 1  --- | [5]           [12]| --- GPIO 8  (Strapping / CS)
         GPIO 2  --- | [6]           [11]| --- GPIO 7  
         GPIO 3  --- | [7]           [10]| --- GPIO 6  
(Start)  GPIO 4  --- | [8]           [9] | --- GPIO 5  (Reset Button)
                     +-------------------+
```

---

## 2. ⚡ Keuntungan Tanpa DFPlayer Mini

1. **Zero Latency**: Suara berputar secara instan tanpa delay inisialisasi serial UART.
2. **Hemat Komponen & Pin**: Hemat 2 pin UART dan tidak membutuhkan modul DFPlayer Mini / MicroSD Card reader fisik.
3. **Audio Terintegrasi di Flash**: Semua efek suara (`winning.h`, `losing.h`, `jingle.h`) tersimpan langsung di dalam memori Flash 4MB ESP32-C3 Super Mini (`const PROGMEM`).
