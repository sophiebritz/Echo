/*
 * ROWING MACHINE MONITOR with TFT Display + Boat Animation
 * For ESP32 with ILI9486 480x320 8-bit parallel display
 * 
 * Hardware:
 * - ESP32 Dev Board
 * - ILI9486 3.5" TFT (8-bit parallel)
 * - Adafruit I2C Rotary Encoder (0x36)
 * - WS2812 LED Ring (12 LEDs)
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_seesaw.h>
#include <FastLED.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>  // For 8-bit parallel ILI9486

// ╔════════════════════════════════════════════════════════════╗
// ║                   PIN CONFIGURATION                       ║
// ╚════════════════════════════════════════════════════════════╝

// I2C Encoder
#define SDA_PIN          21
#define SCL_PIN          22
#define SEESAW_ADDR      0x36

// LED Ring  
#define LED_DATA_PIN     23
#define NUM_LEDS         12
#define LED_BRIGHTNESS   10

// TFT Display uses these pins automatically on ESP32:
// D0-D7: Standard 8-bit bus
// Control pins will be auto-detected by MCUFRIEND_kbv library

// ╔════════════════════════════════════════════════════════════╗
// ║                   ROWING CONFIGURATION                    ║
// ╚════════════════════════════════════════════════════════════╝

const float DRUM_DIAMETER_MM   = 90.0f;
const int   ENCODER_RESOLUTION = 20;
const float ROPE_PER_PULSE_M   = (DRUM_DIAMETER_MM * PI) / ENCODER_RESOLUTION / 1000.0f;
const float CALORIES_PER_METER = 0.12f;

const int   CALIBRATION_STROKES        = 3;
const float FULL_RING_AT_FRACTION      = 0.85f;

const unsigned long STROKE_PAUSE_MS   = 300;
const int32_t       MIN_STROKE_PULSES = 6;
const unsigned long INACTIVITY_TIMEOUT = 30000;

const float SPEED_GREEN_MS = 1.0f;
const float SPEED_RED_MS   = 2.2f;
const float SPEED_SMOOTHING  = 0.88f;
const float SPEED_MIN_DT_SEC = 0.02f;

// Display update rate
const unsigned long DISPLAY_UPDATE_MS = 100;

// ╔════════════════════════════════════════════════════════════╗
// ║                    HARDWARE OBJECTS                       ║
// ╚════════════════════════════════════════════════════════════╝

CRGB leds[NUM_LEDS];
Adafruit_seesaw encoder;
MCUFRIEND_kbv tft;

// Colors
#define BLACK   0x0000
#define BLUE    0x001F
#define RED     0xF800
#define GREEN   0x07E0
#define CYAN    0x07FF
#define MAGENTA 0xF81F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF
#define ORANGE  0xFD20
#define BROWN   0x8200
#define SKYBLUE 0x867D

// ==================== SESSION DATA ====================
struct SessionData {
  float         distance;
  float         calories;
  unsigned long duration;
  int           strokes;
  float         avgStrokeDistance;
  int32_t       totalPulses;
  float         maxSpeed;
};

SessionData currentSession = {};

// ==================== STATE ====================
enum Mode { MODE_CALIBRATING, MODE_RUNNING };
Mode mode = MODE_CALIBRATING;

float calibratedStrokeMeters = 0.0f;
float metersPerLED           = 0.0f;
int   calibStrokeCount       = 0;
float calibSumStrokeMeters   = 0.0f;

int32_t lastPosition = 0;

unsigned long sessionStart  = 0;
bool          sessionActive = false;
unsigned long lastActivity  = 0;

bool          inStroke         = false;
int32_t       strokePulses     = 0;
unsigned long lastMovementTime = 0;

float         filteredSpeed       = 0.0f;
unsigned long lastSpeedTime       = 0;
int32_t       lastSpeedPos        = 0;
float         peakSpeedThisStroke = 0.0f;

unsigned long lastDisplayUpdate = 0;
int           boatX = 20;
int           flagWave = 0;
int           wavePhase = 0;

bool          spinnerActive      = false;
int           spinnerLed         = 0;
unsigned long lastSpinTime       = 0;
const unsigned long SPIN_INTERVAL_MS = 80;

// ==================== DISPLAY FUNCTIONS ====================

void drawWaves(int yStart) {
  // Animated water waves
  for (int x = 0; x < 480; x += 8) {
    int y1 = yStart + sin((x + wavePhase) * 0.05) * 3;
    int y2 = yStart + 10 + sin((x + wavePhase + 50) * 0.04) * 2;
    tft.drawLine(x, y1, x + 8, y1, CYAN);
    tft.drawLine(x, y2, x + 8, y2, SKYBLUE);
  }
  wavePhase = (wavePhase + 2) % 200;
}

void drawBoat(int x, int y) {
  // Hull (brown)
  tft.fillTriangle(x, y + 10, x + 40, y + 10, x + 20, y, BROWN);
  tft.fillRect(x + 5, y + 10, 30, 15, BROWN);
  tft.drawRect(x + 5, y + 10, 30, 15, BLACK);
  
  // Mast (black pole)
  tft.fillRect(x + 20, y - 30, 2, 30, BLACK);
  
  // Sail (white triangle)
  tft.fillTriangle(x + 22, y - 30, x + 22, y, x + 45, y - 15, WHITE);
  tft.drawTriangle(x + 22, y - 30, x + 22, y, x + 45, y - 15, BLACK);
  
  // Flag on top (red, animated wave)
  int flagY = y - 35;
  for (int i = 0; i < 10; i++) {
    int wave = sin((flagWave + i * 30) * 0.1) * 2;
    tft.drawLine(x + 22, flagY - i, x + 32 + wave, flagY - i, RED);
  }
  flagWave = (flagWave + 5) % 360;
}

void drawFinishLine(int x, int y) {
  // Checkered finish flag
  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < 6; j++) {
      uint16_t color = ((i + j) % 2 == 0) ? WHITE : BLACK;
      tft.fillRect(x + i * 5, y + j * 5, 5, 5, color);
    }
  }
  // Flag pole
  tft.fillRect(x + 15, y + 30, 2, 40, BLACK);
}

void drawCalibrationScreen() {
  tft.fillScreen(BLUE);
  
  tft.setTextColor(YELLOW);
  tft.setTextSize(3);
  tft.setCursor(80, 80);
  tft.print("CALIBRATING");
  
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(100, 140);
  tft.print("Do ");
  tft.print(CALIBRATION_STROKES);
  tft.print(" strokes");
  
  tft.setCursor(130, 180);
  tft.print("Stroke ");
  tft.print(calibStrokeCount);
  tft.print("/");
  tft.print(CALIBRATION_STROKES);
  
  drawWaves(270);
}

void drawIdleScreen() {
  tft.fillScreen(SKYBLUE);
  
  // Sky
  tft.fillRect(0, 0, 480, 200, SKYBLUE);
  
  // Water
  tft.fillRect(0, 200, 480, 120, BLUE);
  drawWaves(200);
  
  // Static boat
  drawBoat(50, 220);
  
  // Text
  tft.setTextColor(WHITE);
  tft.setTextSize(4);
  tft.setCursor(80, 60);
  tft.print("READY!");
  
  tft.setTextSize(2);
  tft.setCursor(120, 120);
  tft.print("Start rowing...");
}

void updateRowingDisplay() {
  // Clear previous boat area
  tft.fillRect(0, 150, 480, 100, BLUE);
  
  // Calculate boat position based on distance (500m = full width)
  int maxDistance = 500;
  boatX = 20 + (int)((currentSession.distance / maxDistance) * 400);
  if (boatX > 440) boatX = 440;
  
  // Draw water
  drawWaves(250);
  
  // Draw boat
  drawBoat(boatX, 220);
  
  // Draw finish line when close
  if (currentSession.distance > 400) {
    drawFinishLine(460, 180);
  }
  
  // Stats area at top (black background)
  tft.fillRect(0, 0, 480, 150, BLACK);
  
  // Distance (big and prominent)
  tft.setTextColor(CYAN);
  tft.setTextSize(4);
  tft.setCursor(10, 10);
  tft.print(currentSession.distance, 1);
  tft.print("m");
  
  // Strokes
  tft.setTextColor(GREEN);
  tft.setTextSize(3);
  tft.setCursor(10, 55);
  tft.print("Strokes: ");
  tft.print(currentSession.strokes);
  
  // Time
  tft.setTextColor(YELLOW);
  tft.setCursor(10, 95);
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;
  tft.print("Time: ");
  tft.print(m);
  tft.print(":");
  if (s < 10) tft.print("0");
  tft.print(s);
  
  // Speed and calories (smaller, right side)
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(280, 10);
  tft.print("Speed: ");
  tft.print(filteredSpeed, 1);
  tft.print(" m/s");
  
  tft.setCursor(280, 40);
  tft.print("Cal: ");
  tft.print(currentSession.calories, 0);
  tft.print(" kcal");
  
  // Strokes per minute
  if (currentSession.duration > 0 && currentSession.strokes > 0) {
    float spm = (currentSession.strokes * 60.0f) / currentSession.duration;
    tft.setCursor(280, 70);
    tft.print("SPM: ");
    tft.print(spm, 1);
  }
  
  // Max speed
  tft.setCursor(280, 100);
  tft.print("Max: ");
  tft.print(currentSession.maxSpeed, 1);
  tft.print(" m/s");
}

void drawSummaryScreen() {
  tft.fillScreen(BLACK);
  
  // Title
  tft.setTextColor(YELLOW);
  tft.setTextSize(3);
  tft.setCursor(60, 20);
  tft.print("SESSION COMPLETE!");
  
  // Celebration boat
  drawBoat(200, 100);
  drawFinishLine(380, 80);
  
  // Stats
  tft.setTextColor(GREEN);
  tft.setTextSize(2);
  
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;
  
  tft.setCursor(20, 170);
  tft.print("Time: ");
  tft.print(m);
  tft.print(":");
  if (s < 10) tft.print("0");
  tft.print(s);
  
  tft.setCursor(20, 200);
  tft.print("Distance: ");
  tft.print(currentSession.distance, 1);
  tft.print(" m");
  
  tft.setCursor(20, 230);
  tft.print("Calories: ");
  tft.print(currentSession.calories, 1);
  tft.print(" kcal");
  
  tft.setCursor(20, 260);
  tft.print("Strokes: ");
  tft.print(currentSession.strokes);
  
  if (currentSession.strokes > 0 && currentSession.duration > 0) {
    float spm = (currentSession.strokes * 60.0f) / currentSession.duration;
    
    tft.setCursor(250, 200);
    tft.print("SPM: ");
    tft.print(spm, 1);
    
    tft.setCursor(250, 230);
    tft.print("Max: ");
    tft.print(currentSession.maxSpeed, 1);
    tft.print(" m/s");
  }
}

void updateDisplay() {
  unsigned long now = millis();
  if (now - lastDisplayUpdate < DISPLAY_UPDATE_MS) return;
  lastDisplayUpdate = now;
  
  if (mode == MODE_CALIBRATING) {
    drawCalibrationScreen();
  } else if (sessionActive) {
    updateRowingDisplay();
  } else {
    drawIdleScreen();
  }
}

// ==================== LED HELPERS ====================

static inline float pulsesToMeters(int32_t pulses) {
  return (float)pulses * ROPE_PER_PULSE_M;
}

static inline void clearLEDs() {
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();
}

static inline CRGB colourFromSpeed(float speedMs) {
  float denom = SPEED_RED_MS - SPEED_GREEN_MS;
  if (denom <= 1e-6f) return CRGB::Yellow;
  float x = constrain((speedMs - SPEED_GREEN_MS) / denom, 0.0f, 1.0f);
  return CHSV((uint8_t)(96.0f * (1.0f - x)), 255, 255);
}

static inline void showStrokeProgress(float strokeMeters, CRGB colour) {
  if (metersPerLED <= 0.0f) return;
  int lit = constrain((int)floor(strokeMeters / metersPerLED), 0, NUM_LEDS);
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  for (int i = 0; i < lit; i++) leds[i] = colour;
  FastLED.show();
}

static inline void resetSpeedTracking() {
  filteredSpeed       = 0.0f;
  lastSpeedTime       = millis();
  lastSpeedPos        = 0;
  peakSpeedThisStroke = 0.0f;
}

static inline void resetStroke() {
  inStroke            = false;
  strokePulses        = 0;
  peakSpeedThisStroke = 0.0f;
}

// ==================== SERIAL OUTPUT ====================

void printLiveUpdate() {
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;
  Serial.printf("⚡ %3d strokes | %6.2f m | %d:%02d | %5.1f kcal | %.2f m/s peak\n",
                currentSession.strokes, currentSession.distance,
                m, s, currentSession.calories, currentSession.maxSpeed);
}

void printFinalSummary() {
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;
  Serial.println("\n╔════════════════════════════════════════════════════╗");
  Serial.println("║          🎉 SESSION COMPLETE!                      ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf( "║ Duration:     %2d min %02d sec                       ║\n", m, s);
  Serial.printf( "║ Distance:     %-8.2f meters                     ║\n", currentSession.distance);
  Serial.printf( "║ Calories:     %-8.1f kcal                       ║\n", currentSession.calories);
  Serial.printf( "║ Strokes:      %-8d                             ║\n", currentSession.strokes);
  if (currentSession.strokes > 0 && currentSession.duration > 0) {
    float spm = (currentSession.strokes * 60.0f) / (float)currentSession.duration;
    Serial.printf("║ Avg/Stroke:   %-8.2f meters                     ║\n", currentSession.avgStrokeDistance);
    Serial.printf("║ Strokes/Min:  %-8.1f                             ║\n", spm);
    Serial.printf("║ Peak Speed:   %-8.2f m/s                         ║\n", currentSession.maxSpeed);
  }
  Serial.println("╚════════════════════════════════════════════════════╝");
}

// ==================== SESSION CONTROL ====================

void startSession() {
  currentSession = {};
  sessionActive  = true;
  sessionStart   = millis();
  lastActivity   = millis();

  encoder.setEncoderPosition(0);
  lastPosition = 0;

  resetSpeedTracking();
  resetStroke();
  clearLEDs();
  
  boatX = 20;  // Reset boat to start

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       🚣 SESSION STARTED!              ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

void endSession() {
  if (inStroke && strokePulses >= MIN_STROKE_PULSES) {
    currentSession.strokes++;
  }
  resetStroke();
  sessionActive = false;
  clearLEDs();

  if (currentSession.strokes > 0) {
    currentSession.avgStrokeDistance = currentSession.distance / currentSession.strokes;
    currentSession.calories          = currentSession.distance * CALORIES_PER_METER;
  }
  
  printFinalSummary();
  drawSummaryScreen();

  delay(5000);
  Serial.println("\n✅ Ready for next session!\n");
}

// ==================== CALIBRATION ====================

void beginCalibration() {
  mode                   = MODE_CALIBRATING;
  calibStrokeCount       = 0;
  calibSumStrokeMeters   = 0.0f;
  calibratedStrokeMeters = 0.0f;
  metersPerLED           = 0.0f;

  encoder.setEncoderPosition(0);
  lastPosition = 0;
  resetStroke();

  spinnerActive = true;
  spinnerLed    = 0;
  lastSpinTime  = millis();
  clearLEDs();

  Serial.println("\n╔════════════════════════════════════════════════════╗");
  Serial.println("║          🎯 AUTO CALIBRATION                       ║");
  Serial.println("╚════════════════════════════════════════════════════╝");
  Serial.printf( "Do %d normal strokes.\n", CALIBRATION_STROKES);
  
  drawCalibrationScreen();
}

void finishCalibration() {
  calibratedStrokeMeters   = calibSumStrokeMeters / (float)CALIBRATION_STROKES;
  float fullRingMeters     = calibratedStrokeMeters * FULL_RING_AT_FRACTION;
  metersPerLED             = fullRingMeters / (float)NUM_LEDS;

  Serial.println("\n╔════════════════════════════════════════════════════╗");
  Serial.println("║          ✅ CALIBRATION COMPLETE                   ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf( "║ Avg stroke:  %-8.2f m                             ║\n", calibratedStrokeMeters);
  Serial.printf( "║ Full ring:   %-8.2f m                             ║\n", fullRingMeters);
  Serial.println("╚════════════════════════════════════════════════════╝\n");

  // Flash LEDs
  for (int i = 0; i < 3; i++) {
    fill_solid(leds, NUM_LEDS, CRGB::Blue);
    FastLED.show();
    delay(150);
    clearLEDs();
    delay(150);
  }
  
  mode = MODE_RUNNING;
  encoder.setEncoderPosition(0);
  lastPosition = 0;
  resetSpeedTracking();
  resetStroke();
  
  drawIdleScreen();
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(2000);

  // Initialize TFT
  uint16_t ID = tft.readID();
  Serial.print("TFT ID = 0x");
  Serial.println(ID, HEX);
  
  if (ID == 0xD3D3) ID = 0x9486;  // Force ILI9486
  
  tft.begin(ID);
  tft.setRotation(1);  // Landscape mode
  tft.fillScreen(BLACK);
  
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(100, 150);
  tft.print("Initializing...");

  // Initialize LEDs
  FastLED.addLeds<WS2812, LED_DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  clearLEDs();

  // Initialize I2C
  Wire.begin(SDA_PIN, SCL_PIN);

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║   ROWING MACHINE + TFT DISPLAY         ║");
  Serial.println("╚════════════════════════════════════════╝\n");

  Serial.print("🔍 Looking for encoder...");
  if (!encoder.begin(SEESAW_ADDR)) {
    Serial.println(" ❌ FAILED!");
    tft.fillScreen(RED);
    tft.setCursor(100, 150);
    tft.print("ENCODER ERROR!");
    while (1) delay(1000);
  }
  Serial.println(" ✅ OK!");

  encoder.enableEncoderInterrupt();
  
  beginCalibration();
}

// ==================== MAIN LOOP ====================

void loop() {
  unsigned long now             = millis();
  int32_t       currentPosition = encoder.getEncoderPosition();

  if (sessionActive) {
    currentSession.duration = (now - sessionStart) / 1000;
  }

  // Spinner animation during calibration
  if (spinnerActive && (now - lastSpinTime >= SPIN_INTERVAL_MS)) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    leds[(spinnerLed + NUM_LEDS - 2) % NUM_LEDS] = CHSV(160, 255, 60);
    leds[(spinnerLed + NUM_LEDS - 1) % NUM_LEDS] = CHSV(160, 255, 140);
    leds[spinnerLed]                              = CHSV(160, 255, 255);
    FastLED.show();
    spinnerLed   = (spinnerLed + 1) % NUM_LEDS;
    lastSpinTime = now;
  }

  // Movement detected
  if (currentPosition != lastPosition) {
    int32_t delta = currentPosition - lastPosition;

    if (mode == MODE_RUNNING && !sessionActive) {
      startSession();
    }

    if (delta > 0) {
      if (spinnerActive) {
        spinnerActive = false;
        clearLEDs();
      }

      if (!inStroke) peakSpeedThisStroke = 0.0f;

      strokePulses    += delta;
      inStroke         = true;
      lastMovementTime = now;
      lastActivity     = now;

      // Speed calculation
      float dt = (now - lastSpeedTime) / 1000.0f;
      if (dt >= SPEED_MIN_DT_SEC) {
        int32_t dPulses = max((int32_t)0, currentPosition - lastSpeedPos);
        float rawSpeed  = pulsesToMeters(dPulses) / dt;
        filteredSpeed   = SPEED_SMOOTHING * filteredSpeed + (1.0f - SPEED_SMOOTHING) * rawSpeed;
        lastSpeedTime   = now;
        lastSpeedPos    = currentPosition;

        if (filteredSpeed > peakSpeedThisStroke) {
          peakSpeedThisStroke = filteredSpeed;
        }
      }

      // Update LEDs
      if (mode == MODE_RUNNING) {
        showStrokeProgress(pulsesToMeters(strokePulses), colourFromSpeed(filteredSpeed));
      }

      // Update distance
      if (mode == MODE_RUNNING && sessionActive) {
        currentSession.totalPulses += delta;
        currentSession.distance     = pulsesToMeters(currentSession.totalPulses);
        currentSession.calories     = currentSession.distance * CALORIES_PER_METER;
      }
    }

    lastPosition = currentPosition;
  }

  // Stroke end
  if (inStroke && (now - lastMovementTime >= STROKE_PAUSE_MS)) {
    if (strokePulses >= MIN_STROKE_PULSES) {
      float strokeMeters = pulsesToMeters(strokePulses);

      if (mode == MODE_CALIBRATING) {
        calibStrokeCount++;
        calibSumStrokeMeters += strokeMeters;
        Serial.printf("  ✓ Calib stroke %d/%d (%.2f m)\n", calibStrokeCount, CALIBRATION_STROKES, strokeMeters);
        if (calibStrokeCount >= CALIBRATION_STROKES) finishCalibration();

      } else {
        currentSession.strokes++;
        if (peakSpeedThisStroke > currentSession.maxSpeed) {
          currentSession.maxSpeed = peakSpeedThisStroke;
        }
        if (currentSession.strokes % 10 == 0) printLiveUpdate();
      }
    }

    resetStroke();
    if (mode == MODE_RUNNING) clearLEDs();
  }

  // Inactivity timeout
  if (mode == MODE_RUNNING && sessionActive && (now - lastActivity > INACTIVITY_TIMEOUT)) {
    Serial.println("\n⏸️  Session ended (inactivity)");
    endSession();
  }

  // Update display
  updateDisplay();

  delay(10);
}
