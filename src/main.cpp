#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>

// ==========================================
// KONFIGURASI PIN & KONSTANTA
// ==========================================
#define MIC_PIN             34    // Pin analog untuk output sensor Mic MAX4466 (GPIO34 / ADC1_CH6)
#define DFPLAYER_RX         16    // Pin GPIO16 terhubung ke TX DFPlayer Mini (UART2 RX)
#define DFPLAYER_TX         17    // Pin GPIO17 terhubung ke RX DFPlayer Mini (UART2 TX - gunakan resistor 1kΩ seri)
#define NEOPIXEL_PIN        27    // Pin GPIO27 terhubung ke Data Input WS2812B
#define RESET_BUTTON_PIN    25    // Pin GPIO25 terhubung ke tombol reset high score (ke GND)
#define START_BUTTON_PIN    26    // Pin GPIO26 terhubung ke tombol start game (ke GND)

#define NUM_LEDS            62    // Jumlah total LED WS2812B
#define EEPROM_ADDR_SCORE   0     // Alamat EEPROM untuk menyimpan rekor tertinggi (2 byte)
#define DEFAULT_HIGH_SCORE  1000  // Nilai default rekor jika EEPROM kosong (skala 12-bit MAD)
#define MAX_SCREAM_VAL      1700  // Amplitudo suara maksimum yang diharapkan (skala 12-bit MAD)
#define SCREAM_DURATION_MS  5000  // Durasi perekaman teriakan dalam milidetik (5 detik)
#define SAMPLING_WINDOW_MS  50    // Window pembacaan sampel mic untuk mengukur amplitudo

// Konstanta Command & Status DFPlayer
#define DF_CMD_PLAY_TRACK     0x12    // Menggunakan command 0x12 untuk memutar dari folder /MP3 berdasarkan nama file
#define DF_CMD_SET_VOLUME     0x06
#define DF_CMD_PAUSE          0x0E
#define DF_CMD_STOP           0x16
#define DF_CMD_QUERY_STATUS   0x42
#define DF_CMD_QUERY_ONLINE   0x3F

#define DF_EVT_PLAY_FINISHED  0x3D
#define DF_EVT_ERROR          0x40
#define DF_EVT_STATUS_RESP    0x42

// ==========================================
// DEFINISI STATE
// ==========================================
enum GameState {
  STATE_IDLE,         // Standby, menunggu teriakan melampaui noise floor
  STATE_SCREAMING,    // Perekaman teriakan selama 5 detik, menampilkan level real-time
  STATE_RESULT,       // Menghitung hasil akhir teriakan
  STATE_CELEBRATION   // Perayaan rekor baru (musik & animasi LED)
};

// ==========================================
// INSTANSIALISASI OBJEK
// ==========================================
#define dfSerial Serial2
Adafruit_NeoPixel pixels(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// ==========================================
// VARIABEL GLOBAL
// ==========================================
GameState currentState = STATE_IDLE;
unsigned int highScore = DEFAULT_HIGH_SCORE;
unsigned int currentPeak = 0;
unsigned int calibratedNoiseFloor = 50;
unsigned int micDCOffset = 2048;      // DC bias tengah default (VCC/2)
unsigned long stateStartTime = 0;
bool dfPlayerOnline = false;
bool dfplayerPlaying = false;
unsigned long dfplayerPlayStartTime = 0;

// ==========================================
// DEKLARASI FUNGSI HELPER
// ==========================================
unsigned int getSoundLevel(unsigned long durationMs);
void displayVolumeLevel(int numLedsLit);
void runCelebrationAnimation(unsigned long durationMs);
void runNormalResultAnimation(int finalLeds);
uint32_t Wheel(byte WheelPos);
void calibrateNoiseFloor();
void runCountdownAnimation();
void displaySimpleColor(int count, uint32_t color);

// Fungsi Driver DFPlayer Custom
void sendDFPlayerCmd(uint8_t cmd, uint16_t parameter);
void setDFPlayerVolume(uint8_t volume);
void playDFPlayerTrack(uint16_t track);
void stopDFPlayer();
void parseDFPlayer();
void safeDelay(unsigned long ms);

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(9600);
  while (!Serial) {
    ; // Tunggu serial monitor terhubung
  }
  Serial.println(F("--- Scream Meter Game Initializing ---"));

  // Inisialisasi EEPROM Flash untuk ESP32
  EEPROM.begin(32);

  // Konfigurasi Pin
  pinMode(MIC_PIN, INPUT);
  analogSetAttenuation(ADC_11db); // Atur atenuasi ADC ke 11dB agar dapat membaca tegangan penuh 0-3.3V
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);

  // Inisialisasi LED Strip
  pixels.begin();
  pixels.clear();
  pixels.show();

  // Inisialisasi DFPlayer Mini (menggunakan Hardware Serial1)
  dfSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX, DFPLAYER_TX);
  Serial.println(F("Menghubungkan ke DFPlayer Mini..."));
  
  // Beri waktu 2 detik sejak Arduino boot agar DFPlayer selesai booting fisik (Siasat Booting Time Lambat)
  delay(2000); 
  
  // Bersihkan buffer serial
  while (dfSerial.available()) {
    dfSerial.read();
  }
  
  // Kirim status query untuk mendeteksi DFPlayer
  sendDFPlayerCmd(DF_CMD_QUERY_STATUS, 0);
  
  // Tunggu respon (maksimal 500 ms)
  unsigned long startWait = millis();
  while (millis() - startWait < 500) {
    parseDFPlayer();
    if (dfPlayerOnline) {
      break;
    }
    delay(10);
  }
  
  if (!dfPlayerOnline) {
    Serial.println(F("DFPlayer Mini tidak merespon/terdeteksi. Silakan periksa kabel!"));
  } else {
    Serial.println(F("DFPlayer Mini terdeteksi & online!"));
    
    // Set volume awal (dibatasi maks 30)
    setDFPlayerVolume(15);
    delay(100);
    
    Serial.println(F("Memutar lagu startup 0003.mp3 dari folder /MP3..."));
    playDFPlayerTrack(3);
  }

  // Membaca Rekor Tertinggi dari EEPROM
  unsigned int savedScore;
  EEPROM.get(EEPROM_ADDR_SCORE, savedScore);
  if (savedScore == 0xFFFFFFFF || savedScore == 0xFFFF || savedScore == 0) {
    highScore = DEFAULT_HIGH_SCORE;
    EEPROM.put(EEPROM_ADDR_SCORE, highScore);
    EEPROM.commit();
  } else {
    highScore = savedScore;
  }
  Serial.print(F("Rekor Tertinggi Saat Ini: "));
  Serial.println(highScore);

  // Kalibrasi Suara Sekitar
  calibrateNoiseFloor();
  
  Serial.println(F("Permainan Siap! Mulailah berteriak..."));
}

// ==========================================
// LOOP UTAMA
// ==========================================
void loop() {
  // Cek pesan detail/error dari DFPlayer Mini secara non-blocking
  parseDFPlayer();

  // Jalankan periodic polling & safety timeout check
  if (dfPlayerOnline) {
    static unsigned long lastQueryTime = 0;
    if (dfplayerPlaying && (millis() - lastQueryTime >= 1500)) {
      lastQueryTime = millis();
      sendDFPlayerCmd(DF_CMD_QUERY_STATUS, 0);
    }
    
    // Safety timeout check (300 detik)
    if (dfplayerPlaying && (millis() - dfplayerPlayStartTime >= 300000UL)) {
      Serial.println(F("DFPlayer Safety Timeout tercapai! Memaksa stop..."));
      stopDFPlayer();
    }
  }

  // Cek jika tombol reset ditekan
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    safeDelay(50); // Debouncing
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
      Serial.println(F("Tombol Reset Ditekan. Menghapus rekor tertinggi..."));
      highScore = DEFAULT_HIGH_SCORE;
      EEPROM.put(EEPROM_ADDR_SCORE, highScore);
      EEPROM.commit();
      
      // Putar lagu 0003.mp3 saat reset
      if (dfPlayerOnline) {
        playDFPlayerTrack(3);
      }
      
      // Kedipkan LED warna merah 3 kali sebagai indikasi reset sukses
      for (int i = 0; i < 3; i++) {
        for (int l = 0; l < NUM_LEDS; l++) {
          pixels.setPixelColor(l, pixels.Color(255, 0, 0));
        }
        pixels.show();
        safeDelay(200);
        pixels.clear();
        pixels.show();
        safeDelay(100);
      }
      
      Serial.print(F("Rekor direset ke: "));
      Serial.println(highScore);
      calibrateNoiseFloor(); // Rekalibrasi noise floor setelah reset
    }
  }

  switch (currentState) {
    case STATE_IDLE: {
      // Cek jika tombol start ditekan untuk memulai permainan
      if (digitalRead(START_BUTTON_PIN) == LOW) {
        safeDelay(50); // Debounce
        if (digitalRead(START_BUTTON_PIN) == LOW) {
          Serial.println(F("Tombol Start Ditekan. Memulai countdown..."));
          
          // Hentikan lagu jika DFPlayer sedang memutar musik
          if (dfPlayerOnline) {
            stopDFPlayer();
            delay(100); // Beri jeda kecil agar perintah stop diproses penuh oleh DFPlayer
          }
          
          runCountdownAnimation();
          currentState = STATE_SCREAMING;
          stateStartTime = millis();
          currentPeak = 0;
          break; // Keluar dari case agar loop berikutnya langsung masuk ke STATE_SCREAMING
        }
      }

      // Jalankan pembaruan LED hanya setiap 30ms agar tidak mengganggu komunikasi serial
      static unsigned long lastPixelUpdate = 0;
      if (millis() - lastPixelUpdate >= 30) {
        lastPixelUpdate = millis();

        // Buat indikator letak rekor tertinggi berdenyut (breathing effect) warna biru
        unsigned long now = millis();
        float breath = (exp(sin(now / 1000.0 * PI)) - 0.36787944) * 108.0;
        uint8_t brightness = map(breath, 0, 255, 30, 180); // Skala nyaman namun terlihat di luar ruangan

        pixels.clear();
        // Petakan letak rekor tertinggi ke index LED (Quadratic Mapping agar selaras dengan game)
        float recordRatio = 0.0;
        if (highScore > calibratedNoiseFloor && MAX_SCREAM_VAL > calibratedNoiseFloor) {
          recordRatio = (float)(highScore - calibratedNoiseFloor) / (MAX_SCREAM_VAL - calibratedNoiseFloor);
        }
        recordRatio = constrain(recordRatio, 0.0, 1.0);
        int recordLedIndex = recordRatio * recordRatio * (NUM_LEDS - 1);
        recordLedIndex = constrain(recordLedIndex, 0, NUM_LEDS - 1);
        pixels.setPixelColor(recordLedIndex, pixels.Color(0, 0, brightness)); // Warna biru
        pixels.show();
      }
      break;
    }

    case STATE_SCREAMING: {
      unsigned int currentSound = getSoundLevel(SAMPLING_WINDOW_MS);

      // Update nilai puncak jika mendeteksi suara yang lebih keras
      if (currentSound > currentPeak) {
        currentPeak = currentSound;
      }

      // Tampilkan level suara real-time di Serial Monitor (VU meter)
      Serial.print(F("Scream Level: "));
      Serial.print(currentSound);
      Serial.print(F("\t ["));
      int bars = map(currentSound, calibratedNoiseFloor, MAX_SCREAM_VAL, 0, 20);
      bars = constrain(bars, 0, 20);
      for (int i = 0; i < 20; i++) {
        if (i < bars) Serial.print('=');
        else Serial.print(' ');
      }
      Serial.print(F("] Peak: "));
      Serial.println(currentPeak);

      // Tampilkan kekuatan suara secara real-time pada LED Bar (Quadratic Mapping)
      float ratio = 0.0;
      if (currentSound > calibratedNoiseFloor && MAX_SCREAM_VAL > calibratedNoiseFloor) {
        ratio = (float)(currentSound - calibratedNoiseFloor) / (MAX_SCREAM_VAL - calibratedNoiseFloor);
      }
      ratio = constrain(ratio, 0.0, 1.0);
      int ledsToLight = ratio * ratio * NUM_LEDS;
      displayVolumeLevel(ledsToLight);

      // Cek apakah durasi 5 detik sudah berakhir
      if (millis() - stateStartTime >= SCREAM_DURATION_MS) {
        currentState = STATE_RESULT;
      }
      break;
    }

    case STATE_RESULT: {
      Serial.print(F("Teriakan selesai. Puncak Terukur: "));
      Serial.print(currentPeak);
      Serial.print(F(" | Rekor Saat Ini: "));
      Serial.println(highScore);

      // Hitung level LED dari puncak teriakan untuk animasi hasil (Quadratic Mapping)
      float finalRatio = 0.0;
      if (currentPeak > calibratedNoiseFloor && MAX_SCREAM_VAL > calibratedNoiseFloor) {
        finalRatio = (float)(currentPeak - calibratedNoiseFloor) / (MAX_SCREAM_VAL - calibratedNoiseFloor);
      }
      finalRatio = constrain(finalRatio, 0.0, 1.0);
      int finalLeds = finalRatio * finalRatio * NUM_LEDS;

      if (currentPeak > highScore) {
        // Melampaui rekor tertinggi!
        currentState = STATE_CELEBRATION;
      } else {
        // Gagal melampaui rekor, putar musik/efek suara gagal 0002.mp3 dari folder /MP3
        if (dfPlayerOnline) {
          playDFPlayerTrack(2); // Memutar file indeks ke-2 (0002.mp3)
        }
        
        // Jalankan animasi biasa
        runNormalResultAnimation(finalLeds);

        currentState = STATE_IDLE;
        Serial.println(F("Kembali ke Standby. Coba teriak lebih keras!"));
      }
      break;
    }

    case STATE_CELEBRATION: {
      Serial.print(F("--- REKOR BARU TERCAPAI! --- "));
      Serial.print(highScore);
      Serial.print(F(" -> "));
      Serial.println(currentPeak);

      // Simpan rekor baru ke RAM & EEPROM
      highScore = currentPeak;
      EEPROM.put(EEPROM_ADDR_SCORE, highScore);
      EEPROM.commit();

      // Play lagu perayaan di DFPlayer (File indeks ke-1)
      if (dfPlayerOnline) {
        playDFPlayerTrack(1); // Memutar file indeks ke-1
      }

      // Jalankan animasi perayaan pelangi (rainbow chase) selama 6 detik
      runCelebrationAnimation(6000);



      // Beri jeda kecil lalu kembali ke Standby
      safeDelay(500);
      currentState = STATE_IDLE;
      Serial.println(F("Kembali ke Standby. Siap untuk penantang berikutnya!"));
      break;
    }
  }
}

// ==========================================
// DEFINISI FUNGSI HELPER
// ==========================================

/**
 * Membaca nilai puncak-ke-puncak dari sensor suara selama rentang waktu tertentu.
 * Digunakan untuk mengukur kekuatan/amplitudo getaran suara AC dari mic.
 */
unsigned int getSoundLevel(unsigned long durationMs) {
  unsigned long start = millis();
  unsigned long sumDiff = 0;
  unsigned long sampleCount = 0;

  while (millis() - start < durationMs) {
    unsigned int val = analogRead(MIC_PIN);
    sumDiff += abs((int)val - (int)micDCOffset);
    sampleCount++;
  }

  unsigned int mad = (sampleCount > 0) ? (sumDiff / sampleCount) : 0;
  
  // Gunakan nilai MAD langsung (tanpa dikali 2) agar tidak mudah mentok ke maksimum
  unsigned int soundLevel = mad;

  // Debug output untuk menganalisis karakteristik sinyal analog
  static unsigned long lastDebugPrint = 0;
  if (millis() - lastDebugPrint >= 200) { // Cetak 5 kali per detik
    lastDebugPrint = millis();
    Serial.print(F("[ADC Debug] Bias: "));
    Serial.print(micDCOffset);
    Serial.print(F(" | MAD: "));
    Serial.print(mad);
    Serial.print(F(" | Level: "));
    Serial.println(soundLevel);
  }

  return soundLevel;
}

/**
 * Menampilkan bar level volume pada strip LED.
 * LED bawah: Hijau, Tengah: Kuning/Oranye, Atas: Merah.
 */
void displayVolumeLevel(int numLedsLit) {
  pixels.clear();
  int greenMax = (NUM_LEDS * 25) / 55;   // Skala dinamis berdasarkan rasio asli
  int orangeMax = (NUM_LEDS * 42) / 55;  // Skala dinamis berdasarkan rasio asli
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i < numLedsLit) {
      if (i < greenMax) {
        // LED Hijau (Full Brightness)
        pixels.setPixelColor(i, pixels.Color(0, 255, 0));
      } else if (i < orangeMax) {
        // LED Kuning/Oranye (Full Brightness)
        pixels.setPixelColor(i, pixels.Color(255, 136, 0));
      } else {
        // LED Merah (Full Brightness)
        pixels.setPixelColor(i, pixels.Color(255, 0, 0));
      }
    } else {
      pixels.setPixelColor(i, pixels.Color(0, 0, 0)); // Matikan LED sisa
    }
  }
  pixels.show();
}

/**
 * Animasi hasil teriakan biasa (jika tidak melampaui rekor).
 * Mengedipkan level yang dicapai sebanyak 3 kali, lalu bar utama runtuh secara cepat dengan efek gravitasi, 
 * diikuti oleh pixel puncak (peak hold) yang turun perlahan secara dinamis.
 */
void runNormalResultAnimation(int finalLeds) {
  if (finalLeds <= 0) {
    pixels.clear();
    pixels.show();
    return;
  }

  // 1. Kedipkan level hasil teriakan sebanyak 3 kali untuk memperjelas hasil
  for (int flash = 0; flash < 3; flash++) {
    displayVolumeLevel(finalLeds);
    safeDelay(250);
    pixels.clear();
    pixels.show();
    safeDelay(150);
  }

  // Tampilkan level volume puncak penuh sejenak sebelum animasi runtuh
  displayVolumeLevel(finalLeds);
  safeDelay(500);

  // 2. Animasi Runtuh Gravitasi (Gravity Collapse) dengan Peak Hold
  float barHeight = finalLeds;
  float barVelocity = 0.0;
  float barGravity = 0.3; // Gravitasi untuk bar utama agar jatuh dengan cepat

  float peakPos = finalLeds - 1;
  float peakVelocity = 0.0;
  float peakGravity = 0.15; // Gravitasi untuk pixel puncak agar melayang lebih lambat
  int peakHoldFrames = 15;   // Tahan pixel puncak selama ~600ms (15 frames * 40ms)

  int greenMax = (NUM_LEDS * 25) / 55;
  int orangeMax = (NUM_LEDS * 42) / 55;

  while (barHeight > 0 || peakPos > 0) {
    // Fisika kejatuhan bar utama
    if (barHeight > 0) {
      barVelocity += barGravity;
      barHeight -= barVelocity;
      if (barHeight < 0) barHeight = 0;
    }

    // Fisika kejatuhan pixel puncak (ditahan dulu beberapa frame)
    if (peakHoldFrames > 0) {
      peakHoldFrames--;
    } else {
      if (peakPos > 0) {
        peakVelocity += peakGravity;
        peakPos -= peakVelocity;
        if (peakPos < 0) peakPos = 0;
      }
    }

    // Gambar frame
    pixels.clear();
    
    // Gambar bar utama
    for (int i = 0; i < (int)barHeight; i++) {
      if (i < greenMax) {
        pixels.setPixelColor(i, pixels.Color(0, 255, 0));
      } else if (i < orangeMax) {
        pixels.setPixelColor(i, pixels.Color(255, 136, 0));
      } else {
        pixels.setPixelColor(i, pixels.Color(255, 0, 0));
      }
    }

    // Gambar pixel puncak (jika berada di atas tinggi bar utama saat ini)
    int roundedPeak = (int)peakPos;
    if (roundedPeak >= (int)barHeight && roundedPeak < NUM_LEDS) {
      // Warna pixel puncak yang dibuat sedikit lebih menyala
      if (roundedPeak < greenMax) {
        pixels.setPixelColor(roundedPeak, pixels.Color(0, 255, 0));
      } else if (roundedPeak < orangeMax) {
        pixels.setPixelColor(roundedPeak, pixels.Color(255, 120, 0));
      } else {
        pixels.setPixelColor(roundedPeak, pixels.Color(255, 0, 0));
      }
    }

    pixels.show();
    safeDelay(40); // Interval frame ~40ms (25 FPS)
  }

  // Pastikan bersih total
  pixels.clear();
  pixels.show();
}

/**
 * Animasi Pelangi (Rainbow Cycle) berputar untuk perayaan rekor baru.
 */
void runCelebrationAnimation(unsigned long durationMs) {
  unsigned long start = millis();
  uint16_t colorOffset = 0;

  while (millis() - start < durationMs) {
    for (uint16_t i = 0; i < pixels.numPixels(); i++) {
      // Menghasilkan perputaran spektrum warna di seluruh strip LED
      pixels.setPixelColor(i, Wheel(((i * 256 / pixels.numPixels()) + colorOffset) & 255));
    }
    pixels.show();
    safeDelay(15);
    colorOffset += 3; // Kecepatan putaran pelangi
  }

  // Bersihkan strip LED di akhir animasi
  pixels.clear();
  pixels.show();
}

/**
 * Menghasilkan warna RGB berdasarkan roda warna 0-255.
 */
uint32_t Wheel(byte WheelPos) {
  WheelPos = 255 - WheelPos;
  if (WheelPos < 85) {
    return pixels.Color(255 - WheelPos * 3, 0, WheelPos * 3);
  }
  if (WheelPos < 170) {
    WheelPos -= 85;
    return pixels.Color(0, WheelPos * 3, 255 - WheelPos * 3);
  }
  WheelPos -= 170;
  return pixels.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}

/**
 * Kalibrasi kebisingan suara sekitar saat booting atau setelah reset.
 * Berjalan selama 1.5 detik dengan visualisasi LED warna oranye berputar.
 */
void calibrateNoiseFloor() {
  Serial.println(F("Mengkalibrasi kebisingan suara sekitar..."));
  
  // 1. Kalibrasi DC Offset (bias tengah) dari sinyal analog Mic saat kondisi hening
  unsigned long biasSum = 0;
  for (int i = 0; i < 1000; i++) {
    biasSum += analogRead(MIC_PIN);
    delayMicroseconds(100);
  }
  micDCOffset = biasSum / 1000;
  Serial.print(F("Calibrated DC Bias Offset: "));
  Serial.println(micDCOffset);

  unsigned long calStart = millis();
  unsigned int maxAmbient = 0;
  int calLed = 0;

  while (millis() - calStart < 1500) {
    unsigned int level = getSoundLevel(10);
    if (level > maxAmbient) {
      maxAmbient = level;
    }
    
    // Tampilkan LED oranye berputar dengan efek ekor (trail) selama kalibrasi
    pixels.clear();
    for (int t = 0; t < 8; t++) {
      int idx = (calLed - t + NUM_LEDS) % NUM_LEDS;
      int brightness = 255 - (t * 30);
      if (brightness < 0) brightness = 0;
      pixels.setPixelColor(idx, pixels.Color(brightness, brightness / 2, 0));
    }
    pixels.show();
    calLed = (calLed + 1) % NUM_LEDS;
    safeDelay(10); // Gabungan dengan getSoundLevel(10) menghasilkan jeda ~20ms (50 FPS)
  }

  // Set noise floor 30 level di atas puncak kebisingan sekitar agar tidak gampang terpicu
  calibratedNoiseFloor = maxAmbient + 30;
  if (calibratedNoiseFloor < 40) calibratedNoiseFloor = 40; // Batas minimal noise floor

  Serial.print(F("Kalibrasi Selesai. Noise Floor diset ke: "));
  Serial.println(calibratedNoiseFloor);

  // Nyalakan LED Hijau seluruhnya 3 kali untuk tanda siap digunakan
  for (int i = 0; i < 3; i++) {
    for (int l = 0; l < NUM_LEDS; l++) {
      pixels.setPixelColor(l, pixels.Color(0, 255, 0));
    }
    pixels.show();
    safeDelay(200);
    pixels.clear();
    pixels.show();
    safeDelay(100);
  }
}

/**
 * Menyalakan sejumlah LED tertentu dengan warna solid.
 * Digunakan untuk visualisasi countdown.
 */
void displaySimpleColor(int count, uint32_t color) {
  pixels.clear();
  for (int i = 0; i < count; i++) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

/**
 * Menjalankan animasi countdown visual 3-2-1 sebelum mulai merekam teriakan.
 */
void runCountdownAnimation() {
  // Hitung mundur 3 (Menyalakan seluruh 3/3 LED - Jingga)
  Serial.println(F("Countdown: 3"));
  displaySimpleColor(NUM_LEDS, pixels.Color(255, 85, 0));
  safeDelay(1000);
  
  // Hitung mundur 2 (Menyalakan 2/3 LED - Kuning)
  Serial.println(F("Countdown: 2"));
  displaySimpleColor((NUM_LEDS * 2) / 3, pixels.Color(255, 191, 0));
  safeDelay(1000);
  
  // Hitung mundur 1 (Menyalakan 1/3 LED - Merah)
  Serial.println(F("Countdown: 1"));
  displaySimpleColor((NUM_LEDS * 1) / 3, pixels.Color(255, 0, 0));
  safeDelay(1000);
  
  // MULAI! (Menyalakan seluruh LED - Hijau)
  Serial.println(F("MULAI BERTERIAK!"));
  displaySimpleColor(NUM_LEDS, pixels.Color(0, 255, 0));
  safeDelay(500);
  
  pixels.clear();
  pixels.show();
}

// ====================================================================
// IMPLEMENTASI FUNGSI DRIVER DFPLAYER CUSTOM (SIASAT STABILITAS)
// ====================================================================

/**
 * Mengirim paket perintah 10-byte ke DFPlayer Mini dengan checksum manual.
 */
void sendDFPlayerCmd(uint8_t cmd, uint16_t parameter) {
  uint8_t packet[10];
  packet[0] = 0x7E; // Start Byte
  packet[1] = 0xFF; // Version
  packet[2] = 0x06; // Length
  packet[3] = cmd;
  packet[4] = 0x00; // Feedback (0x00 = No Feedback)
  packet[5] = (parameter >> 8) & 0xFF; // Parameter High
  packet[6] = parameter & 0xFF;        // Parameter Low
  
  // Hitung Checksum
  int16_t checksum = 0;
  for (int i = 1; i <= 6; i++) {
    checksum -= packet[i];
  }
  
  packet[7] = (checksum >> 8) & 0xFF; // Checksum High
  packet[8] = checksum & 0xFF;        // Checksum Low
  packet[9] = 0xEF; // End Byte
  
  dfSerial.write(packet, 10);
  
  Serial.print(F("DFPlayer Sent - CMD: 0x"));
  Serial.print(cmd, HEX);
  Serial.print(F(", Param: 0x"));
  Serial.println(parameter, HEX);
}

/**
 * Mengatur volume DFPlayer secara aman (dibatasi 0-30).
 */
void setDFPlayerVolume(uint8_t volume) {
  volume = constrain(volume, 0, 30); // Siasat Volume Overflow
  sendDFPlayerCmd(DF_CMD_SET_VOLUME, volume);
}

/**
 * Memutar trek lagu berdasarkan indeks file.
 */
void playDFPlayerTrack(uint16_t track) {
  sendDFPlayerCmd(DF_CMD_PLAY_TRACK, track);
  dfplayerPlaying = true;
  dfplayerPlayStartTime = millis();
}

/**
 * Menghentikan pemutaran lagu pada DFPlayer Mini.
 */
void stopDFPlayer() {
  sendDFPlayerCmd(DF_CMD_STOP, 0);
  dfplayerPlaying = false;
}

/**
 * Parser masukan serial asinkron non-blocking dari DFPlayer.
 * Menerapkan siasat 1-byte read alignment untuk menjaga sinkronisasi byte serial.
 */
void parseDFPlayer() {
  while (dfSerial.available() > 0) {
    // Siasat 1: Cari start byte 0x7E satu per satu
    if (dfSerial.peek() != 0x7E) {
      dfSerial.read(); // Buang byte yang tidak selaras
      continue;
    }
    
    // Siasat 2: Setelah start byte terdeteksi, pastikan sisa paket (10 byte total) sudah lengkap
    if (dfSerial.available() < 10) {
      break; // Tunggu sisa byte pada iterasi loop berikutnya
    }
    
    // Baca penuh 10 byte paket
    uint8_t packet[10];
    for (int i = 0; i < 10; i++) {
      packet[i] = dfSerial.read();
    }
    
    // Validasi struktur akhir paket dan checksum
    if (packet[9] == 0xEF) {
      int16_t checksum = 0;
      for (int i = 1; i <= 6; i++) {
        checksum -= packet[i];
      }
      
      uint16_t receivedChecksum = (packet[7] << 8) | packet[8];
      if ((uint16_t)checksum == receivedChecksum) {
        uint8_t cmd = packet[3];
        uint16_t param = (packet[5] << 8) | packet[6];
        
        // Tandai modul online karena telah merespon komunikasi serial dengan valid
        dfPlayerOnline = true;
        
        if (cmd == DF_EVT_PLAY_FINISHED) {
          Serial.print(F("DFPlayer Event: Selesai Memutar Lagu "));
          Serial.println(param);
          dfplayerPlaying = false;
        } else if (cmd == 0x3A) {
          Serial.println(F("DFPlayer Event: Kartu Memori Dimasukkan!"));
        } else if (cmd == 0x3B) {
          Serial.println(F("DFPlayer Event: Kartu Memori Dicabut!"));
        } else if (cmd == DF_EVT_STATUS_RESP) {
          if (param == 0) { // 0 = stopped/idle, 1 = playing, 2 = paused
            Serial.println(F("DFPlayer Status: STOPPED (Query 0x42)"));
            dfplayerPlaying = false;
          } else if (param == 1) {
            dfplayerPlaying = true;
          }
        } else if (cmd == DF_EVT_ERROR) {
          Serial.print(F("DFPlayer Event - Error: "));
          switch (param) {
            case 1: Serial.println(F("Modul Sibuk / Kartu tidak terdeteksi")); break;
            case 2: Serial.println(F("Modul Sedang Tidur (Sleep Mode)")); break;
            case 3: Serial.println(F("Format Komunikasi Serial Salah!")); break;
            case 4: Serial.println(F("Checksum Tidak Cocok!")); break;
            case 5: Serial.println(F("Indeks File di Luar Batas")); break;
            case 6: Serial.println(F("File Musik Tidak Ditemukan!")); break;
            case 7: Serial.println(F("Sedang dalam mode Iklan (Advertise)")); break;
            default:
              Serial.print(F("Kode Error Tidak Dikenal: "));
              Serial.println(param);
              break;
          }
          dfplayerPlaying = false;
        }
      } else {
        Serial.println(F("DFPlayer RX: Checksum Mismatch!"));
      }
    } else {
      Serial.println(F("DFPlayer RX: End Byte (0xEF) Mismatch!"));
    }
  }
}

/**
 * Fungsi delay aman non-blocking yang tetap menjalankan parser data DFPlayer 
 * dan pengecekan timeout di latar belakang selama masa tunggu.
 */
void safeDelay(unsigned long ms) {
  unsigned long start = millis();
  while (millis() - start < ms) {
    parseDFPlayer();
    
    if (dfPlayerOnline) {
      static unsigned long lastQueryTime = 0;
      if (dfplayerPlaying && (millis() - lastQueryTime >= 1500)) {
        lastQueryTime = millis();
        sendDFPlayerCmd(DF_CMD_QUERY_STATUS, 0);
      }
      if (dfplayerPlaying && (millis() - dfplayerPlayStartTime >= 300000UL)) {
        Serial.println(F("DFPlayer Safety Timeout tercapai! Memaksa stop..."));
        stopDFPlayer();
      }
    }
    
    delay(5); // Jeda singkat agar tidak membebani prosesor secara penuh
  }
}