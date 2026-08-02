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
#define LEDC_PWM_FREQ       64000  // Frekuensi carrier PWM 64 kHz (sangat stabil pada LEDC ESP32-C3)
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

// Deklarasi Prototipe Fungsi Audio PWM & Animasi
void stopWavTrack();
void playWavTrack(uint8_t trackNum);
void playBeepTone(uint16_t freqHz, uint16_t durationMs);
void playFullCountdownAudio();
void audioInit();
void rampUpPWM(uint8_t targetLevel);
void rampDownPWM(uint8_t startLevel);
uint32_t Wheel(byte WheelPos);
void runStartupJingleAnimation();
void displaySimpleColorFade(int count, uint32_t color, int fadeMs);

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

// Helper Anti-Pop (Soft Fade-In / Ramp-Up Tegangan PWM selama 50 ms)
void rampUpPWM(uint8_t targetLevel) {
  for (int lvl = 0; lvl <= (int)targetLevel; lvl++) {
    ledcWrite(LEDC_PWM_CHANNEL, lvl);
    delayMicroseconds(390);
  }
  ledcWrite(LEDC_PWM_CHANNEL, targetLevel);
}

// Helper Anti-Pop (Soft Fade-Out / Ramp-Down Tegangan PWM selama 50 ms)
void rampDownPWM(uint8_t startLevel) {
  for (int lvl = (int)startLevel; lvl >= 0; lvl--) {
    ledcWrite(LEDC_PWM_CHANNEL, lvl);
    delayMicroseconds(390);
  }
  ledcWrite(LEDC_PWM_CHANNEL, 0);
}

// Callback ISR Timer High-Resolution untuk Pemutaran Audio PWM
static void IRAM_ATTR audioTimerCallback(void* arg) {
  if (!isAudioPlaying || currentAudio.pcmData == NULL) {
    return;
  }
  
  if (audioPlayIndex < currentAudio.pcmSize) {
    uint8_t sample = currentAudio.pcmData[audioPlayIndex++];
    ledcWrite(LEDC_PWM_CHANNEL, sample);
  } else {
    // Selesai memutar file audio -> tahan di DC bias 128 (Zero DC Jump / Zero Pop)
    isAudioPlaying = false;
    if (audioTimerHandle != NULL) {
      esp_timer_stop(audioTimerHandle);
    }
    ledcWrite(LEDC_PWM_CHANNEL, 128);
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
  stopWavTrack();

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
    Serial.println(F("Audio Dihentikan."));
  }
  // Selalu tahan di DC bias 128 (Constant DC Bias / Zero Pop)
  ledcWrite(LEDC_PWM_CHANNEL, 128);
}

void audioInit() {
  // Inisialisasi LEDC PWM di Pin Audio (GPIO 1)
  double actualFreq = ledcSetup(LEDC_PWM_CHANNEL, LEDC_PWM_FREQ, LEDC_PWM_RES);
  ledcAttachPin(AUDIO_PWM_PIN, LEDC_PWM_CHANNEL);
  
  // Ramp-up halus sekali saja saat booting dari 0V ke 1.65V (Bias Tengah 128)
  for (int lvl = 0; lvl <= 128; lvl++) {
    ledcWrite(LEDC_PWM_CHANNEL, lvl);
    delayMicroseconds(400);
  }

  // Buat High-Resolution Hardware Timer untuk audio playback
  const esp_timer_create_args_t timerArgs = {
    .callback = &audioTimerCallback,
    .arg = NULL,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "audio_timer"
  };
  esp_timer_create(&timerArgs, &audioTimerHandle);

  Serial.print(F("Engine Audio PWM (Constant DC Bias 128) Diinisialisasi di GPIO "));
  Serial.print(AUDIO_PWM_PIN);
  Serial.print(F(" | Actual Freq: "));
  Serial.print(actualFreq);
  Serial.println(F(" Hz"));
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

  // Putar lagu startup (jingle.h -> 0003.wav) dengan Animasi LED Equalizer Pelangi yang menari mengikuti Beat
  runStartupJingleAnimation();

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
      
      // Putar lagu jingle.h (0003.wav) dengan Animasi Beat Equalizer saat Reset
      runStartupJingleAnimation();
      
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

void runStartupJingleAnimation() {
  playWavTrack(3); // Putar lagu startup / reset (jingle.h -> 0003.wav)

  uint16_t colorOffset = 0;
  float smoothAmp = 0.0f;

  // Visualizer Equalizer Pelangi yang bergerak real-time mengikuti beat lagu
  while (isAudioPlaying) {
    size_t idx = audioPlayIndex;
    uint8_t sample = (idx < currentAudio.pcmSize && currentAudio.pcmData != NULL) ? currentAudio.pcmData[idx] : 128;
    
    // Hitung amplitudo real-time (selisih dari bias 128)
    int amplitude = abs((int)sample - 128);
    
    // Smooth moving average agar gerakan bar LED terlihat elastis & empuk
    smoothAmp = (smoothAmp * 0.75f) + ((float)amplitude * 0.25f);
    
    // Pemetaan amplitudo ke jumlah LED yang menyala
    int numLedsLit = map((int)(smoothAmp * 3.5f), 0, 100, 2, NUM_LEDS);
    numLedsLit = constrain(numLedsLit, 2, NUM_LEDS);

    pixels.clear();
    for (int i = 0; i < NUM_LEDS; i++) {
      if (i < numLedsLit) {
        uint32_t color = Wheel(((i * 256 / NUM_LEDS) + colorOffset) & 255);
        pixels.setPixelColor(i, color);
      }
    }
    pixels.show();
    
    colorOffset += 5;
    delay(20);

    // Bisa diinterupsi jika tombol Start ditekan
    if (digitalRead(START_BUTTON_PIN) == LOW) {
      stopWavTrack();
      break;
    }
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

  pixels.clear();
  pixels.show();
}

void playBeepTone(uint16_t freqHz, uint16_t durationMs) {
  stopWavTrack();

  // Sintesis Gelombang Sinus Murni dengan Amplop S-Curve (Raised Cosine / Hann Fade) - Zero Click/Pop
  uint32_t sampleRate = 22050;
  size_t numSamples = (sampleRate * (size_t)durationMs) / 1000;
  
  static uint8_t synthBuffer[4410]; // Maksimal buffer 200ms pada 22.05kHz
  if (numSamples > 4410) numSamples = 4410;

  float omega = 2.0f * PI * (float)freqHz / (float)sampleRate;
  float amplitude = 28.0f; // Amplitudo volume empuk & sangat jernih
  size_t fadeLen = (size_t)(sampleRate * 0.040f); // Amplop S-Curve Fade 40ms (882 sampel)

  for (size_t i = 0; i < numSamples; i++) {
    float env = 1.0f;
    if (i < fadeLen) {
      // S-Curve (Raised Cosine) smooth fade-in tanpa sudut/kink
      env = 0.5f * (1.0f - cosf(PI * (float)i / (float)fadeLen));
    } else if (i > numSamples - fadeLen) {
      // S-Curve (Raised Cosine) smooth fade-out tanpa sudut/kink
      size_t rem = numSamples - 1 - i;
      env = 0.5f * (1.0f - cosf(PI * (float)rem / (float)fadeLen));
    }
    
    float val = 128.0f + (amplitude * env * sinf(omega * (float)i));
    synthBuffer[i] = (uint8_t)constrain(val, 0.0f, 255.0f);
  }

  currentAudio.pcmData = synthBuffer;
  currentAudio.pcmSize = numSamples;
  currentAudio.sampleRate = sampleRate;

  audioPlayIndex = 0;
  isAudioPlaying = true;

  uint64_t periodUs = 1000000ULL / sampleRate;
  esp_timer_start_periodic(audioTimerHandle, periodUs);

  // Tunggu hingga nada selesai dimainkan
  safeDelay(durationMs);
}

void displaySimpleColor(int count, uint32_t color) {
  pixels.clear();
  for (int i = 0; i < count; i++) {
    pixels.setPixelColor(i, color);
  }
  pixels.show();
}

void displaySimpleColorFade(int count, uint32_t color, int fadeMs) {
  uint8_t targetR = (color >> 16) & 0xFF;
  uint8_t targetG = (color >> 8) & 0xFF;
  uint8_t targetB = color & 0xFF;

  int steps = 12;
  int stepDelay = fadeMs / steps;
  if (stepDelay < 1) stepDelay = 1;

  // Soft Fade-In Kecerahan LED (Anti-Power Spike / Kemiringan Arus Halus)
  for (int s = 1; s <= steps; s++) {
    uint8_t curR = (targetR * s) / steps;
    uint8_t curG = (targetG * s) / steps;
    uint8_t curB = (targetB * s) / steps;

    pixels.clear();
    for (int i = 0; i < count; i++) {
      pixels.setPixelColor(i, pixels.Color(curR, curG, curB));
    }
    pixels.show();
    delay(stepDelay);
  }
}

void playFullCountdownAudio() {
  stopWavTrack();

  uint32_t sampleRate = 22050;
  size_t totalSamples = (sampleRate * 3500) / 1000; // 3.5 detik = 77175 sampel
  
  static uint8_t countdownBuffer[77175];

  // Isi seluruh buffer dengan 128 (Constant 1.65V DC Bias Silence)
  memset(countdownBuffer, 128, totalSamples);

  // Helper lambda untuk menyisipkan Beep murni S-Curve ke offset waktu tertentu
  auto addBeepToBuffer = [&](uint32_t startMs, uint16_t freqHz, uint16_t durationMs) {
    size_t startIdx = (sampleRate * startMs) / 1000;
    size_t numSamples = (sampleRate * (size_t)durationMs) / 1000;
    float omega = 2.0f * PI * (float)freqHz / (float)sampleRate;
    float amplitude = 28.0f;
    size_t fadeLen = (size_t)(sampleRate * 0.040f); // Amplop S-Curve Fade 40ms

    for (size_t i = 0; i < numSamples; i++) {
      if (startIdx + i >= totalSamples) break;
      float env = 1.0f;
      if (i < fadeLen) {
        env = 0.5f * (1.0f - cosf(PI * (float)i / (float)fadeLen));
      } else if (i > numSamples - fadeLen) {
        size_t rem = numSamples - 1 - i;
        env = 0.5f * (1.0f - cosf(PI * (float)rem / (float)fadeLen));
      }
      float val = 128.0f + (amplitude * env * sinf(omega * (float)i));
      countdownBuffer[startIdx + i] = (uint8_t)constrain(val, 0.0f, 255.0f);
    }
  };

  // Sintesis 3x Beep Rendah (1000Hz) & 1x Beep Tinggi (2000Hz) dalam 1 Stream Kontinyu
  addBeepToBuffer(0,    1000, 150); // Beep 3 (0.0s)
  addBeepToBuffer(1000, 1000, 150); // Beep 2 (1.0s)
  addBeepToBuffer(2000, 1000, 150); // Beep 1 (2.0s)
  addBeepToBuffer(3000, 2000, 400); // Beep MULAI! (3.0s)

  currentAudio.pcmData = countdownBuffer;
  currentAudio.pcmSize = totalSamples;
  currentAudio.sampleRate = sampleRate;

  audioPlayIndex = 0;
  isAudioPlaying = true;

  uint64_t periodUs = 1000000ULL / sampleRate;
  esp_timer_start_periodic(audioTimerHandle, periodUs);
}

void runCountdownAnimation() {
  Serial.println(F("Memulai Countdown 3-2-1-MULAI (1 Stream Kontinyu 3.5 Detik)..."));
  
  // Putar 1 stream audio kontinyu 3.5 detik tanpa jeda timer hardware
  playFullCountdownAudio();

  unsigned long startAnim = millis();
  int lastPhase = -1;

  while (isAudioPlaying && (millis() - startAnim < 3600)) {
    unsigned long elapsed = millis() - startAnim;

    int currentPhase = 0;
    if (elapsed < 1000) currentPhase = 3;       // 0.0s - 1.0s: Hitung Mundur 3
    else if (elapsed < 2000) currentPhase = 2;  // 1.0s - 2.0s: Hitung Mundur 2
    else if (elapsed < 3000) currentPhase = 1;  // 2.0s - 3.0s: Hitung Mundur 1
    else currentPhase = 0;                      // 3.0s - 3.5s: MULAI BERTERIAK!

    if (currentPhase != lastPhase) {
      lastPhase = currentPhase;
      if (currentPhase == 3) {
        Serial.println(F("Countdown: 3"));
        displaySimpleColorFade(NUM_LEDS, pixels.Color(255, 85, 0), 40);
      } else if (currentPhase == 2) {
        Serial.println(F("Countdown: 2"));
        displaySimpleColorFade((NUM_LEDS * 2) / 3, pixels.Color(255, 191, 0), 40);
      } else if (currentPhase == 1) {
        Serial.println(F("Countdown: 1"));
        displaySimpleColorFade((NUM_LEDS * 1) / 3, pixels.Color(255, 0, 0), 40);
      } else {
        Serial.println(F("MULAI BERTERIAK!"));
        displaySimpleColorFade(NUM_LEDS, pixels.Color(0, 255, 0), 40);
      }
    }

    delay(20);
  }

  pixels.clear();
  pixels.show();
}

void safeDelay(unsigned long ms) {
  delay(ms);
}