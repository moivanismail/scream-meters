#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <EEPROM.h>
#include "esp_timer.h"

// Include file audio WAV C-Array yang dihasilkan oleh xxd
#include "winning.h"
#include "losing.h"
#include "jingle.h"

// ==========================================
// KONFIGURASI PIN & KONSTANTA
// ==========================================
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(ARDUINO_ESP32C3_DEV) || defined(ESP32C3)
  // Pinout ESP32-C3 Super Mini
  #define MIC_PIN             0     // Pin analog sensor Mic MAX4466 (GPIO0 / ADC1_CH0)
  #define AUDIO_PWM_PIN       1     // Pin Output Audio PWM ke Filter RCPasif & Speaker Aktif (GPIO1)
  #define NEOPIXEL_PIN        10    // Pin GPIO10 terhubung ke Data Input WS2812B
  #define RESET_BUTTON_PIN    5     // Pin GPIO5 terhubung ke tombol reset high score (ke GND)
  #define START_BUTTON_PIN    4     // Pin GPIO4 terhubung ke tombol start game (ke GND)
#else
  // Pinout ESP32 Classic (WROOM-32)
  #define MIC_PIN             34    // Pin analog untuk output sensor Mic MAX4466 (GPIO34 / ADC1_CH6)
  #define AUDIO_PWM_PIN       25    // Pin Output Audio PWM (GPIO25)
  #define NEOPIXEL_PIN        27    // Pin GPIO27 terhubung ke Data Input WS2812B
  #define RESET_BUTTON_PIN    25    // Pin GPIO25 terhubung ke tombol reset high score (ke GND)
  #define START_BUTTON_PIN    26    // Pin GPIO26 terhubung ke tombol start game (ke GND)
#endif

#define NUM_LEDS            62    // Jumlah total LED WS2812B
#define EEPROM_ADDR_SCORE   0     // Alamat EEPROM untuk menyimpan rekor tertinggi (2 byte)
#define DEFAULT_HIGH_SCORE  1000  // Nilai default rekor jika EEPROM kosong (skala 12-bit MAD)
#define MAX_SCREAM_VAL      1700  // Amplitudo suara maksimum yang diharapkan (skala 12-bit MAD)
#define SCREAM_DURATION_MS  5000  // Durasi perekaman teriakan dalam milidetik (5 detik)
#define SAMPLING_WINDOW_MS  50    // Window pembacaan sampel mic untuk mengukur amplitudo

// Konstanta Audio PWM Engine
#define LEDC_PWM_CHANNEL    0
#define LEDC_PWM_FREQ       250000 // Frekuensi carrier PWM 250 kHz (di atas rentang pendengaran manusia)
#define LEDC_PWM_RES        8      // Resolusi 8-bit (0 s.d. 255)

// ==========================================
// DEFINISI STATE PERMAINAN
// ==========================================
enum GameState {
  STATE_IDLE,         // Standby, menunggu teriakan melampaui noise floor
  STATE_SCREAMING,    // Perekaman teriakan selama 5 detik, menampilkan level real-time
  STATE_RESULT,       // Menghitung hasil akhir teriakan
  STATE_CELEBRATION   // Perayaan rekor baru (musik & animasi LED)
};

// ==========================================
// INSTANSIALISASI OBJEK & VARIABEL GLOBAL
// ==========================================
Adafruit_NeoPixel pixels(NUM_LEDS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

GameState currentState = STATE_IDLE;
unsigned int highScore = DEFAULT_HIGH_SCORE;
unsigned int currentPeak = 0;
unsigned int calibratedNoiseFloor = 50;
unsigned int micDCOffset = 2048;      // DC bias tengah default (VCC/2)
unsigned long stateStartTime = 0;

// ==========================================
// AUDIO PWM ENGINE (INTERNAL FLASH PLAYBACK)
// ==========================================
struct WavAudioInfo {
  const uint8_t* pcmData;
  size_t pcmSize;
  uint32_t sampleRate;
};

static WavAudioInfo currentAudio = { NULL, 0, 22050 };
static volatile size_t audioPlayIndex = 0;
static volatile bool isAudioPlaying = false;
static esp_timer_handle_t audioTimerHandle = NULL;

// Callback ISR Timer High-Resolution untuk Pemutaran Audio PWM
static void IRAM_ATTR audioTimerCallback(void* arg) {
  if (!isAudioPlaying || currentAudio.pcmData == NULL) {
    return;
  }
  
  if (audioPlayIndex < currentAudio.pcmSize) {
    uint8_t sample = currentAudio.pcmData[audioPlayIndex++];
    ledcWrite(LEDC_PWM_CHANNEL, sample);
  } else {
    // Selesai memutar file audio
    isAudioPlaying = false;
    ledcWrite(LEDC_PWM_CHANNEL, 128); // Kembali ke DC Bias tengah (128)
    if (audioTimerHandle != NULL) {
      esp_timer_stop(audioTimerHandle);
    }
  }
}

// Helper untuk membaca header WAV dan menemukan offset chunk raw PCM "data"
WavAudioInfo parseWavHeader(const uint8_t* wavBytes, size_t totalLen) {
  WavAudioInfo info = { NULL, 0, 22050 };
  if (totalLen < 44) return info;

  // Cari chunk "fmt " untuk ekstrak sample rate
  uint32_t sRate = 22050;
  for (size_t i = 0; i < totalLen - 16; i++) {
    if (wavBytes[i] == 'f' && wavBytes[i+1] == 'm' && wavBytes[i+2] == 't' && wavBytes[i+3] == ' ') {
      sRate = wavBytes[i + 10] | (wavBytes[i + 11] << 8) | (wavBytes[i + 12] << 16) | (wavBytes[i + 13] << 24);
      break;
    }
  }
  info.sampleRate = (sRate > 0) ? sRate : 22050;

  // Cari chunk "data"
  for (size_t i = 0; i < totalLen - 8; i++) {
    if (wavBytes[i] == 'd' && wavBytes[i+1] == 'a' && wavBytes[i+2] == 't' && wavBytes[i+3] == 'a') {
      uint32_t dataLen = wavBytes[i+4] | (wavBytes[i+5] << 8) | (wavBytes[i+6] << 16) | (wavBytes[i+7] << 24);
      info.pcmData = wavBytes + i + 8;
      info.pcmSize = (dataLen <= totalLen - i - 8) ? dataLen : (totalLen - i - 8);
      return info;
    }
  }
  return info;
}

void playWavTrack(uint8_t trackNum) {
  // Hentikan pemutaran audio jika sedang berjalan
  if (isAudioPlaying) {
    isAudioPlaying = false;
    if (audioTimerHandle != NULL) {
      esp_timer_stop(audioTimerHandle);
    }
  }

  const uint8_t* wavData = NULL;
  size_t wavLen = 0;

  if (trackNum == 1) { // Trek Perayaan Rekor (winning.h -> 0001.wav)
    wavData = __0001_wav;
    wavLen = __0001_wav_len;
  } else if (trackNum == 2) { // Trek Gagal (losing.h -> 0002.wav)
    wavData = __0002_wav;
    wavLen = __0002_wav_len;
  } else if (trackNum == 3) { // Trek Startup / Jingle (jingle.h -> 0003.wav)
    wavData = __0003_wav;
    wavLen = __0003_wav_len;
  }

  if (wavData == NULL || wavLen == 0) return;

  currentAudio = parseWavHeader(wavData, wavLen);
  if (currentAudio.pcmData == NULL || currentAudio.pcmSize == 0) return;

  audioPlayIndex = 0;
  isAudioPlaying = true;

  // Atur interval pemicuan timer mikrodetik berdasarkan Sample Rate audio
  uint64_t periodUs = 1000000ULL / currentAudio.sampleRate;
  esp_timer_start_periodic(audioTimerHandle, periodUs);
  
  Serial.print(F("Memutar Audio Track "));
  Serial.print(trackNum);
  Serial.print(F(" | Sample Rate: "));
  Serial.print(currentAudio.sampleRate);
  Serial.print(F(" Hz | Samples: "));
  Serial.println(currentAudio.pcmSize);
}

void stopWavTrack() {
  if (isAudioPlaying) {
    isAudioPlaying = false;
    if (audioTimerHandle != NULL) {
      esp_timer_stop(audioTimerHandle);
    }
    ledcWrite(LEDC_PWM_CHANNEL, 128); // Kembali ke DC bias hening
    Serial.println(F("Audio Dihentikan."));
  }
}

void audioInit() {
  // Inisialisasi LEDC PWM di Pin Audio
  ledcSetup(LEDC_PWM_CHANNEL, LEDC_PWM_FREQ, LEDC_PWM_RES);
  ledcAttachPin(AUDIO_PWM_PIN, LEDC_PWM_CHANNEL);
  ledcWrite(LEDC_PWM_CHANNEL, 128); // Set bias hening 128

  // Buat High-Resolution Hardware Timer untuk audio playback
  const esp_timer_create_args_t timerArgs = {
    .callback = &audioTimerCallback,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "audio_timer"
  };
  esp_timer_create(&timerArgs, &audioTimerHandle);
  Serial.println(F("Engine Audio PWM (Internal Flash) Berhasil Diinisialisasi!"));
}

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
void safeDelay(unsigned long ms);

// ==========================================
// SETUP
// ==========================================
void setup() {
  Serial.begin(9600);
  while (!Serial) {
    ; // Tunggu serial monitor terhubung
  }
  Serial.println(F("--- Scream Meter Game Initializing (Internal Audio PWM) ---"));

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

  // Inisialisasi Engine Audio Internal
  audioInit();

  // Putar lagu startup (jingle.h -> 0003.wav)
  playWavTrack(3);

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
  // Cek jika tombol reset ditekan
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    safeDelay(50); // Debouncing
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
      Serial.println(F("Tombol Reset Ditekan. Menghapus rekor tertinggi..."));
      highScore = DEFAULT_HIGH_SCORE;
      EEPROM.put(EEPROM_ADDR_SCORE, highScore);
      EEPROM.commit();
      
      // Putar lagu 0003.wav saat reset
      playWavTrack(3);
      
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
          
          // Hentikan audio jika sedang memutar musik
          stopWavTrack();
          
          runCountdownAnimation();
          currentState = STATE_SCREAMING;
          stateStartTime = millis();
          currentPeak = 0;
          break; // Keluar dari case agar loop berikutnya langsung masuk ke STATE_SCREAMING
        }
      }

      // Jalankan pembaruan LED hanya setiap 30ms agar tidak mengganggu sistem
      static unsigned long lastPixelUpdate = 0;
      if (millis() - lastPixelUpdate >= 30) {
        lastPixelUpdate = millis();

        // Buat indikator letak rekor tertinggi berdenyut (breathing effect) warna biru
        unsigned long now = millis();
        float breath = (exp(sin(now / 1000.0 * PI)) - 0.36787944) * 108.0;
        uint8_t brightness = map(breath, 0, 255, 30, 180);

        pixels.clear();
        // Petakan letak rekor tertinggi ke index LED (Quadratic Mapping)
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
        // Gagal melampaui rekor, putar musik gagal (losing.h -> 0002.wav)
        playWavTrack(2);
        
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

      // Play lagu perayaan (winning.h -> 0001.wav)
      playWavTrack(1);

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
  unsigned int soundLevel = mad;

  static unsigned long lastDebugPrint = 0;
  if (millis() - lastDebugPrint >= 200) {
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

void displayVolumeLevel(int numLedsLit) {
  pixels.clear();
  int greenMax = (NUM_LEDS * 25) / 55;
  int orangeMax = (NUM_LEDS * 42) / 55;
  for (int i = 0; i < NUM_LEDS; i++) {
    if (i < numLedsLit) {
      if (i < greenMax) {
        pixels.setPixelColor(i, pixels.Color(0, 255, 0));
      } else if (i < orangeMax) {
        pixels.setPixelColor(i, pixels.Color(255, 136, 0));
      } else {
        pixels.setPixelColor(i, pixels.Color(255, 0, 0));
      }
    } else {
      pixels.setPixelColor(i, pixels.Color(0, 0, 0));
    }
  }
  pixels.show();
}

void runNormalResultAnimation(int finalLeds) {
  if (finalLeds <= 0) {
    pixels.clear();
    pixels.show();
    return;
  }

  for (int flash = 0; flash < 3; flash++) {
    displayVolumeLevel(finalLeds);
    safeDelay(250);
    pixels.clear();
    pixels.show();
    safeDelay(150);
  }

  displayVolumeLevel(finalLeds);
  safeDelay(500);

  float barHeight = finalLeds;
  float barVelocity = 0.0;
  float barGravity = 0.3;

  float peakPos = finalLeds - 1;
  float peakVelocity = 0.0;
  float peakGravity = 0.15;
  int peakHoldFrames = 15;

  int greenMax = (NUM_LEDS * 25) / 55;
  int orangeMax = (NUM_LEDS * 42) / 55;

  while (barHeight > 0 || peakPos > 0) {
    if (barHeight > 0) {
      barVelocity += barGravity;
      barHeight -= barVelocity;
      if (barHeight < 0) barHeight = 0;
    }

    if (peakHoldFrames > 0) {
      peakHoldFrames--;
    } else {
      if (peakPos > 0) {
        peakVelocity += peakGravity;
        peakPos -= peakVelocity;
        if (peakPos < 0) peakPos = 0;
      }
    }

    pixels.clear();
    for (int i = 0; i < (int)barHeight; i++) {
      if (i < greenMax) {
        pixels.setPixelColor(i, pixels.Color(0, 255, 0));
      } else if (i < orangeMax) {
        pixels.setPixelColor(i, pixels.Color(255, 136, 0));
      } else {
        pixels.setPixelColor(i, pixels.Color(255, 0, 0));
      }
    }

    int roundedPeak = (int)peakPos;
    if (roundedPeak >= (int)barHeight && roundedPeak < NUM_LEDS) {
      if (roundedPeak < greenMax) {
        pixels.setPixelColor(roundedPeak, pixels.Color(0, 255, 0));
      } else if (roundedPeak < orangeMax) {
        pixels.setPixelColor(roundedPeak, pixels.Color(255, 120, 0));
      } else {
        pixels.setPixelColor(roundedPeak, pixels.Color(255, 0, 0));
      }
    }

    pixels.show();
    safeDelay(40);
  }

  pixels.clear();
  pixels.show();
}

void runCelebrationAnimation(unsigned long durationMs) {
  unsigned long start = millis();
  uint16_t colorOffset = 0;

  while (millis() - start < durationMs) {
    for (uint16_t i = 0; i < pixels.numPixels(); i++) {
      pixels.setPixelColor(i, Wheel(((i * 256 / pixels.numPixels()) + colorOffset) & 255));
    }
    pixels.show();
    safeDelay(15);
    colorOffset += 3;
  }

  pixels.clear();
  pixels.show();
}

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

void calibrateNoiseFloor() {
  Serial.println(F("Mengkalibrasi kebisingan suara sekitar..."));
  
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
    
    pixels.clear();
    for (int t = 0; t < 8; t++) {
      int idx = (calLed - t + NUM_LEDS) % NUM_LEDS;
      int brightness = 255 - (t * 30);
      if (brightness < 0) brightness = 0;
      pixels.setPixelColor(idx, pixels.Color(brightness, brightness / 2, 0));
    }
    pixels.show();
    calLed = (calLed + 1) % NUM_LEDS;
    safeDelay(10);
  }

  calibratedNoiseFloor = maxAmbient + 30;
  if (calibratedNoiseFloor < 40) calibratedNoiseFloor = 40;

  Serial.print(F("Kalibrasi Selesai. Noise Floor diset ke: "));
  Serial.println(calibratedNoiseFloor);

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

void playBeepTone(uint16_t freqHz, uint16_t durationMs) {
  stopWavTrack();

  // Ubah frekuensi PWM ke frekuensi nada Beep yang diinginkan
  ledcSetup(LEDC_PWM_CHANNEL, freqHz, LEDC_PWM_RES);
  ledcWrite(LEDC_PWM_CHANNEL, 128); // Gelombang kotak 50% duty cycle

  delay(durationMs);

  // Matikan nada Beep
  ledcWrite(LEDC_PWM_CHANNEL, 0);

  // Kembalikan konfigurasi PWM ke frekuensi carrier 250kHz untuk pemutaran audio WAV
  ledcSetup(LEDC_PWM_CHANNEL, LEDC_PWM_FREQ, LEDC_PWM_RES);
  ledcWrite(LEDC_PWM_CHANNEL, 128);
}

void displaySimpleColor(int count, uint32_t color) {
  pixels.clear();
  for (int i = 0; i < count; i++) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

void runCountdownAnimation() {
  // Hitung mundur 3 (LED Jingga + Beep 1000Hz)
  Serial.println(F("Countdown: 3"));
  displaySimpleColor(NUM_LEDS, pixels.Color(255, 85, 0));
  playBeepTone(1000, 150);
  safeDelay(850);
  
  // Hitung mundur 2 (LED Kuning + Beep 1000Hz)
  Serial.println(F("Countdown: 2"));
  displaySimpleColor((NUM_LEDS * 2) / 3, pixels.Color(255, 191, 0));
  playBeepTone(1000, 150);
  safeDelay(850);
  
  // Hitung mundur 1 (LED Merah + Beep 1000Hz)
  Serial.println(F("Countdown: 1"));
  displaySimpleColor((NUM_LEDS * 1) / 3, pixels.Color(255, 0, 0));
  playBeepTone(1000, 150);
  safeDelay(850);
  
  // MULAI! (LED Hijau + High Pitch Beep 2000Hz)
  Serial.println(F("MULAI BERTERIAK!"));
  displaySimpleColor(NUM_LEDS, pixels.Color(0, 255, 0));
  playBeepTone(2000, 400);
  safeDelay(100);
  
  pixels.clear();
  pixels.show();
}

void safeDelay(unsigned long ms) {
  delay(ms);
}