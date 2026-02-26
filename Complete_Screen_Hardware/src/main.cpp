/*
 * ROWING MACHINE MONITOR - FIXED VERSION
 * - Proper distance calculation (no overflow!)
 * - Button to end session
 * - Button to dismiss summary and start new session
 */

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_seesaw.h>
#include <FastLED.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <MCUFRIEND_kbv.h>

// ╔════════════════════════════════════════════════════════════╗
// ║                   PIN CONFIGURATION                       ║
// ╚════════════════════════════════════════════════════════════╝

#define SDA_PIN          21
#define SCL_PIN          22
#define SEESAW_ADDR      0x36
#define LED_DATA_PIN     23
#define NUM_LEDS         12
#define LED_BRIGHTNESS   10
#define BUTTON_PIN       5   // External button - FREE PIN with internal pull-up!

// ╔════════════════════════════════════════════════════════════╗
// ║           🎯 LAP DISTANCE - CHANGE THIS!                  ║
// ╚════════════════════════════════════════════════════════════╝
const float LAP_DISTANCE_METERS = 10.0f;  

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

const unsigned long DISPLAY_UPDATE_MS = 500;  // Slower = smoother (was 200)

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
#define YELLOW  0xFFE0
#define WHITE   0xFFFF
#define BROWN   0x8200
#define SKYBLUE 0x867D
#define DARKBLUE 0x0010

// ==================== SESSION DATA ====================
struct SessionData {
  float         distance;
  float         calories;
  unsigned long duration;
  int           strokes;
  float         avgStrokeDistance;
  float         maxSpeed;
};

SessionData currentSession = {};

// ==================== STATE ====================
enum Mode { MODE_CALIBRATING, MODE_RUNNING, MODE_SUMMARY };
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

// Display state
unsigned long lastDisplayUpdate = 0;
int           completedLaps = 0;
int           lastBoatX = 20;
float         lastDisplayedDistance = -1;
int           lastDisplayedStrokes = -1;
int           lastDisplayedTime = -1;
int           lastDisplayedLap = -1;
bool          rowingScreenInitialized = false;  // Track if rowing screen is drawn
bool          idleScreenDrawn = false;  // Track if idle screen is drawn
bool          summaryScreenDrawn = false;  // Track if summary screen is drawn

bool          spinnerActive      = false;
int           spinnerLed         = 0;
unsigned long lastSpinTime       = 0;
const unsigned long SPIN_INTERVAL_MS = 80;

// Button state
bool lastButtonState = HIGH;
unsigned long lastButtonPressTime = 0;
bool waitingForDoubleClick = false;
const unsigned long DOUBLE_CLICK_TIMEOUT = 300;  // 300ms for double click

// ==================== BUTTON HELPER ====================

// Returns: 0 = no press, 1 = single click, 2 = double click
int getButtonClick() {
  bool currentState = digitalRead(BUTTON_PIN);
  
  // Detect button press (HIGH → LOW transition)
  if (currentState == LOW && lastButtonState == HIGH) {
    delay(50);  // Debounce
    if (digitalRead(BUTTON_PIN) == LOW) {
      unsigned long now = millis();
      
      if (waitingForDoubleClick && (now - lastButtonPressTime < DOUBLE_CLICK_TIMEOUT)) {
        // Double click detected!
        waitingForDoubleClick = false;
        lastButtonState = LOW;
        return 2;  // Double click
      } else {
        // First click - wait to see if double click coming
        waitingForDoubleClick = true;
        lastButtonPressTime = now;
        lastButtonState = LOW;
        return 0;  // Waiting...
      }
    }
  }
  
  // Check if double-click timeout expired (single click confirmed)
  if (waitingForDoubleClick && (millis() - lastButtonPressTime > DOUBLE_CLICK_TIMEOUT)) {
    waitingForDoubleClick = false;
    return 1;  // Single click
  }
  
  lastButtonState = currentState;
  return 0;  // No click
}

// ==================== DISPLAY FUNCTIONS ====================

void drawBoat(int x, int y) {
  tft.fillTriangle(x, y + 10, x + 40, y + 10, x + 20, y, BROWN);
  tft.fillRect(x + 5, y + 10, 30, 15, BROWN);
  tft.fillRect(x + 20, y - 30, 2, 30, BLACK);
  tft.fillTriangle(x + 22, y - 30, x + 22, y, x + 45, y - 15, WHITE);
  tft.fillRect(x + 22, y - 40, 10, 5, RED);
}

void drawFinishFlag(int x, int y) {
  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < 6; j++) {
      uint16_t color = ((i + j) % 2 == 0) ? WHITE : BLACK;
      tft.fillRect(x + i * 5, y + j * 5, 5, 5, color);
    }
  }
  tft.fillRect(x + 15, y + 30, 2, 40, BLACK);
}

void updateBoatPosition() {
  float distanceInLap = currentSession.distance - (completedLaps * LAP_DISTANCE_METERS);
  int newBoatX = 20 + (int)((distanceInLap / LAP_DISTANCE_METERS) * 400);
  if (newBoatX > 440) newBoatX = 440;
  
  // Only redraw if boat moved 5+ pixels (smoother)
  if (abs(newBoatX - lastBoatX) >= 5) {
    tft.fillRect(lastBoatX - 5, 180, 60, 100, DARKBLUE);
    drawBoat(newBoatX, 250);
    lastBoatX = newBoatX;
  }
}

void checkForLapCompletion() {
  int currentLap = (int)(currentSession.distance / LAP_DISTANCE_METERS);
  
  if (currentLap > completedLaps) {
    completedLaps = currentLap;
    
    tft.fillRect(100, 100, 280, 80, YELLOW);
    tft.setTextColor(BLACK);
    tft.setTextSize(5);
    tft.setCursor(140, 120);
    tft.print((int)(completedLaps * LAP_DISTANCE_METERS));
    tft.print("m!");
    
    delay(1000);
    
    tft.fillRect(0, 0, 480, 320, DARKBLUE);
    lastDisplayedDistance = -1;
    lastDisplayedStrokes = -1;
    lastDisplayedTime = -1;
    lastDisplayedLap = -1;
    lastBoatX = 20;
  }
}

void updateStats() {
  // Distance + Lap - only update when changes by 1m or more
  if (abs(currentSession.distance - lastDisplayedDistance) >= 1.0 || completedLaps != lastDisplayedLap) {
    tft.fillRect(10, 10, 460, 35, BLACK);
    tft.setTextColor(CYAN);
    tft.setTextSize(4);
    tft.setCursor(10, 10);
    tft.print(currentSession.distance, 1);
    tft.print("m");
    
    if (completedLaps > 0) {
      tft.setTextSize(2);
      tft.setTextColor(WHITE);
      tft.setCursor(250, 20);
      tft.print("Lap ");
      tft.print(completedLaps + 1);
    }
    
    lastDisplayedDistance = currentSession.distance;
    lastDisplayedLap = completedLaps;
  }
  
  // Strokes - only when count changes
  if (currentSession.strokes != lastDisplayedStrokes) {
    tft.fillRect(10, 55, 250, 30, BLACK);
    tft.setTextColor(GREEN);
    tft.setTextSize(3);
    tft.setCursor(10, 55);
    tft.print("Strokes: ");
    tft.print(currentSession.strokes);
    lastDisplayedStrokes = currentSession.strokes;
  }
  
  // Time - only when second changes
  if (currentSession.duration != lastDisplayedTime) {
    tft.fillRect(10, 95, 250, 30, BLACK);
    tft.setTextColor(YELLOW);
    tft.setTextSize(3);
    tft.setCursor(10, 95);
    int m = currentSession.duration / 60;
    int s = currentSession.duration % 60;
    tft.print("Time: ");
    tft.print(m);
    tft.print(":");
    if (s < 10) tft.print("0");
    tft.print(s);
    lastDisplayedTime = currentSession.duration;
  }
  
  // Right side stats - update every 2 seconds (less frequent)
  static unsigned long lastRightUpdate = 0;
  if (millis() - lastRightUpdate > 2000) {
    tft.fillRect(280, 10, 190, 120, BLACK);
    tft.setTextColor(WHITE);
    tft.setTextSize(2);
    
    tft.setCursor(280, 10);
    tft.print("Speed: ");
    tft.print(filteredSpeed, 1);
    tft.print(" m/s");
    
    tft.setCursor(280, 40);
    tft.print("Cal: ");
    tft.print(currentSession.calories, 0);
    
    if (currentSession.duration > 0 && currentSession.strokes > 0) {
      float spm = (currentSession.strokes * 60.0f) / currentSession.duration;
      tft.setCursor(280, 70);
      tft.print("SPM: ");
      tft.print(spm, 1);
    }
    
    tft.setCursor(280, 100);
    tft.print("Max: ");
    tft.print(currentSession.maxSpeed, 1);
    
    lastRightUpdate = millis();
  }
}

void drawCalibrationScreen() {
  static int lastCalibDisplayed = -1;
  
  if (calibStrokeCount != lastCalibDisplayed) {
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
    
    tft.fillRect(0, 180, 480, 30, BLUE);
    tft.setCursor(130, 180);
    tft.print("Stroke ");
    tft.print(calibStrokeCount);
    tft.print("/");
    tft.print(CALIBRATION_STROKES);
    
    lastCalibDisplayed = calibStrokeCount;
  }
}

void drawIdleScreen() {
  tft.fillScreen(SKYBLUE);
  tft.fillRect(0, 200, 480, 120, DARKBLUE);
  
  drawBoat(50, 250);
  
  tft.setTextColor(WHITE);
  tft.setTextSize(4);
  tft.setCursor(140, 60);
  tft.print("READY!");
  
  tft.setTextSize(2);
  tft.setCursor(100, 120);
  tft.print("Start rowing...");
  
  // Button instructions
  tft.setTextColor(CYAN);
  tft.setTextSize(1);
  tft.setCursor(140, 160);
  tft.print("Click: View last session");
  tft.setCursor(130, 175);
  tft.print("Double-click: Calibrate");
}

void drawSummaryScreen() {
  tft.fillScreen(BLACK);
  
  tft.setTextColor(YELLOW);
  tft.setTextSize(3);
  tft.setCursor(60, 20);
  tft.print("SESSION COMPLETE!");
  
  drawBoat(200, 120);
  drawFinishFlag(380, 100);
  
  tft.setTextColor(GREEN);
  tft.setTextSize(2);
  
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;
  
  tft.setCursor(20, 180);
  tft.print("Time: ");
  tft.print(m);
  tft.print(":");
  if (s < 10) tft.print("0");
  tft.print(s);
  
  tft.setCursor(20, 210);
  tft.print("Distance: ");
  tft.print(currentSession.distance, 1);
  tft.print(" m");
  
  tft.setCursor(20, 240);
  tft.print("Laps: ");
  tft.print(completedLaps);
  tft.print(" x ");
  tft.print((int)LAP_DISTANCE_METERS);
  tft.print("m");
  
  tft.setCursor(20, 270);
  tft.print("Strokes: ");
  tft.print(currentSession.strokes);
  
  if (currentSession.strokes > 0 && currentSession.duration > 0) {
    float spm = (currentSession.strokes * 60.0f) / currentSession.duration;
    tft.setCursor(250, 210);
    tft.print("SPM: ");
    tft.print(spm, 1);
    
    tft.setCursor(250, 240);
    tft.print("Max: ");
    tft.print(currentSession.maxSpeed, 1);
    tft.print(" m/s");
  }
  
  // Show button instruction
  tft.setTextColor(CYAN);
  tft.setTextSize(2);
  tft.setCursor(100, 300);
  tft.print("Press to return to ready");
}

void initRowingScreen() {
  tft.fillScreen(DARKBLUE);
  drawFinishFlag(460, 200);
  
  for (int y = 280; y < 320; y += 10) {
    tft.drawFastHLine(0, y, 480, CYAN);
  }
  
  lastBoatX = 20;
  lastDisplayedDistance = -1;
  lastDisplayedStrokes = -1;
  lastDisplayedTime = -1;
  lastDisplayedLap = -1;
}

void updateDisplay() {
  unsigned long now = millis();
  
  if (mode == MODE_CALIBRATING) {
    // Calibration screen updates every time (needs to show stroke count)
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
      drawCalibrationScreen();
      lastDisplayUpdate = now;
    }
    
  } else if (mode == MODE_SUMMARY) {
    // Summary screen - draw once and wait for button
    if (!summaryScreenDrawn) {
      // Summary already drawn in endSession(), just set flag
      summaryScreenDrawn = true;
    }
    
  } else if (sessionActive) {
    // Rowing screen - draw background once, update stats frequently
    if (!rowingScreenInitialized) {
      initRowingScreen();
      rowingScreenInitialized = true;
    }
    
    if (now - lastDisplayUpdate >= DISPLAY_UPDATE_MS) {
      checkForLapCompletion();
      updateStats();
      updateBoatPosition();
      lastDisplayUpdate = now;
    }
    
  } else {
    // Idle screen - draw once and stop refreshing!
    if (!idleScreenDrawn) {
      drawIdleScreen();
      idleScreenDrawn = true;
      lastDisplayUpdate = now;
    }
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
  Serial.printf("⚡ %3d strokes | %6.2f m (Lap %d) | %d:%02d | %5.1f kcal\n",
                currentSession.strokes, currentSession.distance, completedLaps + 1,
                m, s, currentSession.calories);
}

void printFinalSummary() {
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;
  Serial.println("\n╔════════════════════════════════════════════════════╗");
  Serial.println("║          🎉 SESSION COMPLETE!                      ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf( "║ Duration:     %2d min %02d sec                       ║\n", m, s);
  Serial.printf( "║ Distance:     %-8.2f meters                     ║\n", currentSession.distance);
  Serial.printf( "║ Laps:         %d x %.0fm                            ║\n", completedLaps, LAP_DISTANCE_METERS);
  Serial.printf( "║ Calories:     %-8.1f kcal                       ║\n", currentSession.calories);
  Serial.printf( "║ Strokes:      %-8d                             ║\n", currentSession.strokes);
  if (currentSession.strokes > 0 && currentSession.duration > 0) {
    float spm = (currentSession.strokes * 60.0f) / (float)currentSession.duration;
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

  // CRITICAL: Reset encoder to zero at session start
  encoder.setEncoderPosition(0);
  lastPosition = 0;

  resetSpeedTracking();
  resetStroke();
  clearLEDs();
  
  completedLaps = 0;
  lastBoatX = 20;
  rowingScreenInitialized = false;  // Reset flag to redraw screen
  idleScreenDrawn = false;  // Allow idle screen to redraw next time

  Serial.println("\n🚣 SESSION STARTED!");
  Serial.printf("Lap distance: %.0f meters\n", LAP_DISTANCE_METERS);
  Serial.println("Press button to end session");
  Serial.println("Double-click button to re-calibrate\n");
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
  
  mode = MODE_SUMMARY;
  summaryScreenDrawn = false;  // Allow summary to be drawn
  drawSummaryScreen();
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

  Serial.println("\n🎯 CALIBRATION MODE");
  Serial.printf("Do %d normal strokes.\n", CALIBRATION_STROKES);
  Serial.println("(Double-click button anytime to re-calibrate)\n");
  
  drawCalibrationScreen();
}

void finishCalibration() {
  calibratedStrokeMeters   = calibSumStrokeMeters / (float)CALIBRATION_STROKES;
  float fullRingMeters     = calibratedStrokeMeters * FULL_RING_AT_FRACTION;
  metersPerLED             = fullRingMeters / (float)NUM_LEDS;

  Serial.println("\n✅ CALIBRATION COMPLETE");
  Serial.printf("Avg stroke: %.2f m\n", calibratedStrokeMeters);
  Serial.printf("Full ring:  %.2f m\n", fullRingMeters);
  Serial.println("\n📌 BUTTON CONTROLS:");
  Serial.println("  • Click: View last session summary");
  Serial.println("  • Double-click: Re-calibrate");
  Serial.println("  • While rowing: Click to end session\n");

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
  
  idleScreenDrawn = false;  // Allow idle screen to draw
  drawIdleScreen();
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(2000);

  uint16_t ID = tft.readID();
  if (ID == 0xD3D3) ID = 0x9486;
  
  tft.begin(ID);
  tft.setRotation(1);
  tft.fillScreen(BLACK);
  
  tft.setTextColor(WHITE);
  tft.setTextSize(2);
  tft.setCursor(100, 150);
  tft.print("Initializing...");

  FastLED.addLeds<WS2812, LED_DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(LED_BRIGHTNESS);
  clearLEDs();

  Wire.begin(SDA_PIN, SCL_PIN);

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║   ROWING MACHINE + TFT DISPLAY         ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.printf("\nLap distance: %.0f meters\n", LAP_DISTANCE_METERS);

  if (!encoder.begin(SEESAW_ADDR)) {
    Serial.println("\n❌ ENCODER ERROR!");
    tft.fillScreen(RED);
    tft.setCursor(100, 150);
    tft.print("ENCODER ERROR!");
    while (1) delay(1000);
  }
  Serial.println("✅ Encoder OK\n");

  encoder.enableEncoderInterrupt();
  
  // External button on GPIO 5 (has internal pull-up)
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  Serial.println("✅ Button on GPIO 5");
  
  beginCalibration();
}

// ==================== MAIN LOOP ====================

void loop() {
  unsigned long now             = millis();
  int32_t       currentPosition = encoder.getEncoderPosition();

  if (sessionActive) {
    currentSession.duration = (now - sessionStart) / 1000;
  }

  // ===== BUTTON HANDLING =====
  int buttonClick = getButtonClick();
  
  if (buttonClick == 2) {
    // DOUBLE CLICK → Start calibration from any screen
    Serial.println("\n🎯 Double click - starting calibration");
    beginCalibration();
    
  } else if (buttonClick == 1) {
    // SINGLE CLICK → Depends on current screen
    
    if (mode == MODE_SUMMARY) {
      // On summary screen → Back to ready
      Serial.println("\n◀️  Button - back to ready");
      mode = MODE_RUNNING;
      completedLaps = 0;
      rowingScreenInitialized = false;
      idleScreenDrawn = false;
      summaryScreenDrawn = false;
      drawIdleScreen();
      
    } else if (sessionActive) {
      // During rowing → End session
      Serial.println("\n🛑 Button - ending session");
      endSession();
      
    } else if (mode == MODE_RUNNING && !sessionActive) {
      // On ready screen → Show summary (fake data if no previous session)
      Serial.println("\n📊 Button - showing summary");
      mode = MODE_SUMMARY;
      summaryScreenDrawn = false;
      drawSummaryScreen();
    }
  }

  // ===== LED SPINNER DURING CALIBRATION =====
  if (spinnerActive && (now - lastSpinTime >= SPIN_INTERVAL_MS)) {
    fill_solid(leds, NUM_LEDS, CRGB::Black);
    leds[(spinnerLed + NUM_LEDS - 2) % NUM_LEDS] = CHSV(160, 255, 60);
    leds[(spinnerLed + NUM_LEDS - 1) % NUM_LEDS] = CHSV(160, 255, 140);
    leds[spinnerLed]                              = CHSV(160, 255, 255);
    FastLED.show();
    spinnerLed   = (spinnerLed + 1) % NUM_LEDS;
    lastSpinTime = now;
  }

  // ===== ENCODER MOVEMENT DETECTION =====
  if (currentPosition != lastPosition) {
    int32_t delta = currentPosition - lastPosition;
    
    // CRITICAL FIX: Ignore huge jumps (encoder overflow/noise)
    if (abs(delta) > 1000) {
      Serial.printf("⚠️  Ignored encoder jump: %d\n", delta);
      lastPosition = currentPosition;
      return;
    }

    if (mode == MODE_RUNNING && !sessionActive) {
      startSession();
    }

    if (delta > 0) {  // Only count forward movement
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
        int32_t dPulses = abs(currentPosition - lastSpeedPos);
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

      // CRITICAL FIX: Use DELTA not absolute position
      if (mode == MODE_RUNNING && sessionActive) {
        currentSession.distance += pulsesToMeters(delta);
        currentSession.calories  = currentSession.distance * CALORIES_PER_METER;
        
        // Sanity check
        if (currentSession.distance > 10000.0f) {
          Serial.println("❌ Distance overflow - resetting");
          currentSession.distance = 0;
          encoder.setEncoderPosition(0);
          lastPosition = 0;
        }
      }
    }

    lastPosition = currentPosition;
  }

  // ===== STROKE END DETECTION =====
  if (inStroke && (now - lastMovementTime >= STROKE_PAUSE_MS)) {
    if (strokePulses >= MIN_STROKE_PULSES) {
      float strokeMeters = pulsesToMeters(strokePulses);

      if (mode == MODE_CALIBRATING) {
        calibStrokeCount++;
        calibSumStrokeMeters += strokeMeters;
        Serial.printf("✓ Stroke %d/%d (%.2f m)\n", calibStrokeCount, CALIBRATION_STROKES, strokeMeters);
        
        drawCalibrationScreen();  // Force update
        
        if (calibStrokeCount >= CALIBRATION_STROKES) {
          finishCalibration();
        }

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

  // ===== INACTIVITY TIMEOUT =====
  if (mode == MODE_RUNNING && sessionActive && (now - lastActivity > INACTIVITY_TIMEOUT)) {
    Serial.println("\n⏸️  Inactivity timeout");
    endSession();
  }

  // ===== UPDATE DISPLAY =====
  updateDisplay();

  delay(10);
}
