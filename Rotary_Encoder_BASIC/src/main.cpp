#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_seesaw.h>
#include <FastLED.h>

// ==================== CONFIGURATION ====================
#define SDA_PIN 8
#define SCL_PIN 10

// CALIBRATION VALUES
float METERS_PER_ROTATION = 1.028;
const float CALORIES_PER_METER = 0.12;
const int ENCODER_RESOLUTION = 24;
bool CALIBRATION_MODE = false;  // Set to true to recalibrate

// ==================== LED RING ====================
#define NUM_LEDS   12
#define DATA_PIN   2      // LED ring data pin
CRGB leds[NUM_LEDS];

// LED colours by effort level
// Blue = slow warmup, Green = moderate, Yellow = good, Red = max effort
const CRGB COLOR_SLOW   = CRGB(0,   50,  255);  // Blue
const CRGB COLOR_MED    = CRGB(0,   255, 50);   // Green
const CRGB COLOR_FAST   = CRGB(255, 200, 0);    // Yellow
const CRGB COLOR_MAX    = CRGB(255, 30,  0);    // Red
const CRGB COLOR_OFF    = CRGB::Black;

// ==================== STROKE DETECTION ====================
const unsigned long STROKE_PAUSE_MS = 300;
const int32_t MIN_STROKE_PULSES = 6;

// ==================== HARDWARE ====================
Adafruit_seesaw encoder;

// ==================== SESSION DATA ====================
struct SessionData {
  float distance;
  float calories;
  unsigned long duration;
  int strokes;
  float avgStrokeDistance;
  int32_t totalPulses;
  float maxSpeed;
};

SessionData currentSession = {0, 0, 0, 0, 0, 0, 0};

// ==================== TRACKING VARIABLES ====================
int32_t lastPosition = 0;
unsigned long sessionStart = 0;
bool sessionActive = false;
unsigned long lastActivity = 0;
const unsigned long INACTIVITY_TIMEOUT = 30000;

bool inStroke = false;
int32_t strokePulses = 0;
unsigned long lastMovementTime = 0;

unsigned long lastSpeedCheck = 0;
int32_t lastSpeedPosition = 0;
float currentSpeedMps = 0;   // current speed in m/s (used for LEDs)

unsigned long lastLedUpdate = 0;
const unsigned long LED_UPDATE_MS = 50;  // update LEDs every 50ms

// ==================== FUNCTION DECLARATIONS ====================
void startSession();
void endSession();
void printLiveUpdate();
void printFinalSummary();
void runCalibration();
void updateLEDs();
void setAllLEDs(CRGB color);
void ledCelebration();
void ledIdle();

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(3000);

  // Init LEDs first so we can show status
  FastLED.addLeds<WS2812, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(80);
  FastLED.clear(true);

  // Startup flash - white sweep
  for (int i = 0; i < NUM_LEDS; i++) {
    leds[i] = CRGB::White;
    FastLED.show();
    delay(40);
  }
  FastLED.clear(true);
  FastLED.show();

  Serial.println("\n\n╔════════════════════════════════════════╗");
  Serial.println("║   ROWING MACHINE MONITOR v2.0          ║");
  Serial.println("║   With LED Ring Feedback               ║");
  Serial.println("╚════════════════════════════════════════╝\n");

  Wire.begin(SDA_PIN, SCL_PIN);
  Serial.println("⚙️  Initializing I2C...");
  Serial.printf("   GPIO%d (SDA), GPIO%d (SCL)\n", SDA_PIN, SCL_PIN);

  Serial.print("🔍 Looking for encoder...");
  if (!encoder.begin(0x36)) {
    Serial.println(" ❌ FAILED!");
    // Flash red to show error
    setAllLEDs(CRGB::Red);
    FastLED.show();
    Serial.println("\nCheck wiring!");
    while(1) delay(1000);
  }
  Serial.println(" ✅ OK!\n");

  encoder.enableEncoderInterrupt();

  if (CALIBRATION_MODE) {
    runCalibration();
    return;
  }

  // Idle breathing blue to show ready
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║  🚣 READY! START ROWING...             ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

// ==================== MAIN LOOP ====================
void loop() {
  unsigned long currentMillis = millis();
  int32_t currentPosition = encoder.getEncoderPosition();

  if (currentPosition != lastPosition) {
    if (!sessionActive) {
      startSession();
    }

    int32_t delta = currentPosition - lastPosition;

    if (delta > 0) {
      float rotations = (float)delta / ENCODER_RESOLUTION;
      float newDistance = rotations * METERS_PER_ROTATION;
      currentSession.distance += newDistance;
      currentSession.totalPulses += delta;

      strokePulses += delta;
      inStroke = true;
      lastMovementTime = currentMillis;

      // Update speed every second
      if (currentMillis - lastSpeedCheck >= 1000) {
        int32_t speedDelta = abs(currentPosition - lastSpeedPosition);
        float speedRotations = (float)speedDelta / ENCODER_RESOLUTION;
        currentSpeedMps = speedRotations * METERS_PER_ROTATION;

        if (currentSpeedMps > currentSession.maxSpeed) {
          currentSession.maxSpeed = currentSpeedMps;
        }

        lastSpeedCheck = currentMillis;
        lastSpeedPosition = currentPosition;
      }
    }

    currentSession.calories = currentSession.distance * CALORIES_PER_METER;
    lastPosition = currentPosition;
    lastActivity = currentMillis;

  } else if (inStroke && (currentMillis - lastMovementTime >= STROKE_PAUSE_MS)) {
    // Encoder stopped = complete stroke
    if (strokePulses >= MIN_STROKE_PULSES) {
      currentSession.strokes++;

      if (currentSession.strokes % 10 == 0) {
        printLiveUpdate();
      }
    }
    inStroke = false;
    strokePulses = 0;

    // Fade speed down when not actively pulling
    currentSpeedMps *= 0.7;
  }

  if (sessionActive) {
    currentSession.duration = (currentMillis - sessionStart) / 1000;
  }

  // Auto-end after 30 seconds of no movement
  if (sessionActive && (currentMillis - lastActivity > INACTIVITY_TIMEOUT)) {
    Serial.println("\n⏸️  No activity for 30 seconds - ending session...");
    endSession();
  }

  // Update LEDs regularly
  if (currentMillis - lastLedUpdate >= LED_UPDATE_MS) {
    updateLEDs();
    lastLedUpdate = currentMillis;
  }

  delay(10);
}

// ==================== LED FEEDBACK ====================
void updateLEDs() {
  if (!sessionActive) {
    // Idle: slow breathing blue pulse
    static uint8_t breath = 0;
    static int8_t dir = 1;
    breath += dir * 2;
    if (breath >= 80) dir = -1;
    if (breath <= 5) dir = 1;
    
    for (int i = 0; i < NUM_LEDS; i++) {
      leds[i] = CRGB(0, 0, breath);
    }
    FastLED.show();
    return;
  }

  // Map speed to number of LEDs lit (0 - 12)
  // Max expected speed ~4 m/s = all 12 LEDs
  float maxSpeed = 4.0;
  int numLit = (int)((currentSpeedMps / maxSpeed) * NUM_LEDS);
  if (numLit > NUM_LEDS) numLit = NUM_LEDS;
  if (numLit < 0) numLit = 0;

  // Choose colour based on effort
  CRGB color;
  if (numLit <= 3) {
    color = COLOR_SLOW;    // Blue - slow/warmup
  } else if (numLit <= 6) {
    color = COLOR_MED;     // Green - moderate
  } else if (numLit <= 9) {
    color = COLOR_FAST;    // Yellow - good pace
  } else {
    color = COLOR_MAX;     // Red - max effort!
  }

  // Fill LEDs in ring
  fill_solid(leds, NUM_LEDS, COLOR_OFF);
  for (int i = 0; i < numLit; i++) {
    leds[i] = color;
  }

  // Always show at least 1 dim LED during session
  if (numLit == 0 && sessionActive) {
    leds[0] = CRGB(0, 20, 60);
  }

  FastLED.show();
}

void setAllLEDs(CRGB color) {
  fill_solid(leds, NUM_LEDS, color);
  FastLED.show();
}

void ledCelebration() {
  // Rainbow spin around the ring
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < NUM_LEDS; i++) {
      fill_solid(leds, NUM_LEDS, COLOR_OFF);
      leds[i] = CHSV((i * 255 / NUM_LEDS), 255, 255);
      leds[(i + 1) % NUM_LEDS] = CHSV(((i+1) * 255 / NUM_LEDS), 200, 180);
      FastLED.show();
      delay(60);
    }
  }
  FastLED.clear(true);
  FastLED.show();
}

// ==================== SESSIONS ====================
void startSession() {
  sessionActive = true;
  sessionStart = millis();
  lastActivity = millis();
  lastSpeedCheck = millis();
  lastMovementTime = millis();

  currentSession = {0, 0, 0, 0, 0, 0, 0};
  inStroke = false;
  strokePulses = 0;
  currentSpeedMps = 0;

  lastPosition = encoder.getEncoderPosition();
  lastSpeedPosition = lastPosition;
  encoder.setEncoderPosition(0);

  // Flash green to signal session start
  setAllLEDs(CRGB::Green);
  delay(300);
  FastLED.clear(true);
  FastLED.show();

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       🚣 SESSION STARTED!              ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  Serial.println("Live updates every 10 strokes...\n");
  Serial.println("LED GUIDE:");
  Serial.println("  🔵 Blue  = Slow/Warmup");
  Serial.println("  🟢 Green = Moderate pace");
  Serial.println("  🟡 Yellow = Good pace");
  Serial.println("  🔴 Red   = Max effort!\n");
}

void endSession() {
  if (inStroke && strokePulses >= MIN_STROKE_PULSES) {
    currentSession.strokes++;
  }
  inStroke = false;
  strokePulses = 0;
  sessionActive = false;
  currentSpeedMps = 0;

  if (currentSession.strokes > 0) {
    currentSession.avgStrokeDistance = currentSession.distance / currentSession.strokes;
  }

  printFinalSummary();

  // Celebration LEDs!
  ledCelebration();

  delay(3000);
  Serial.println("\n✅ Ready for next session - start rowing!\n");
}

// ==================== DISPLAY ====================
void printLiveUpdate() {
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;

  Serial.printf("⚡ %3d strokes | %6.1f m | %d:%02d | %5.1f kcal | %.1f m/s\n",
                currentSession.strokes, currentSession.distance, m, s,
                currentSession.calories, currentSpeedMps);
}

void printFinalSummary() {
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;

  Serial.println("\n╔════════════════════════════════════════════════════╗");
  Serial.println("║          🎉 SESSION COMPLETE!                      ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf( "║ Duration:     %2d min %02d sec                       ║\n", m, s);
  Serial.printf( "║ Distance:     %-8.1f meters                     ║\n", currentSession.distance);
  Serial.printf( "║ Calories:     %-8.1f kcal                       ║\n", currentSession.calories);
  Serial.printf( "║ Strokes:      %-8d                             ║\n", currentSession.strokes);

  if (currentSession.strokes > 0) {
    float spm = (currentSession.strokes * 60.0) / currentSession.duration;
    Serial.printf("║ Avg/Stroke:   %-8.2f meters                     ║\n", currentSession.avgStrokeDistance);
    Serial.printf("║ Strokes/Min:  %-8.1f                             ║\n", spm);
    Serial.printf("║ Max Speed:    %-8.2f m/s                         ║\n", currentSession.maxSpeed);
  }

  Serial.println("╚════════════════════════════════════════════════════╝");

  Serial.println("\n📊 CSV (copy to spreadsheet):");
  Serial.println("Duration(s),Distance(m),Calories(kcal),Strokes,Avg/Stroke,SPM,MaxSpeed");

  float spm = currentSession.duration > 0 ? (currentSession.strokes * 60.0) / currentSession.duration : 0;
  Serial.printf("%lu,%.2f,%.2f,%d,%.2f,%.1f,%.2f\n",
                currentSession.duration, currentSession.distance, currentSession.calories,
                currentSession.strokes, currentSession.avgStrokeDistance, spm, currentSession.maxSpeed);
}

// ==================== CALIBRATION ====================
void runCalibration() {
  Serial.println("╔════════════════════════════════════════════════════╗");
  Serial.println("║          🎯 CALIBRATION MODE                       ║");
  Serial.println("╚════════════════════════════════════════════════════╝\n");
  Serial.println("Row exactly 10 complete strokes then wait.\n");

  // Yellow LEDs during calibration
  setAllLEDs(CRGB::Yellow);

  for (int i = 5; i > 0; i--) {
    Serial.printf("Starting in %d seconds...\n", i);
    delay(1000);
  }

  // Green = go!
  setAllLEDs(CRGB::Green);
  Serial.println("\n🚣 START ROWING NOW! (10 strokes)\n");

  encoder.setEncoderPosition(0);
  int strokeCount = 0;
  int32_t lastPos = 0;
  bool pulling = false;
  int32_t pulses = 0;
  unsigned long lastMove = millis();

  while(strokeCount < 10) {
    int32_t pos = encoder.getEncoderPosition();
    int32_t delta = pos - lastPos;

    if (delta > 0) {
      pulling = true;
      pulses += delta;
      lastMove = millis();

      // Light up LEDs as you pull
      int lit = map(strokeCount, 0, 10, 0, NUM_LEDS);
      fill_solid(leds, NUM_LEDS, COLOR_OFF);
      for (int i = 0; i < lit; i++) leds[i] = CRGB::Green;
      FastLED.show();

    } else if (pulling && (millis() - lastMove >= STROKE_PAUSE_MS)) {
      if (pulses >= MIN_STROKE_PULSES) {
        strokeCount++;
        Serial.printf("  ✓ Stroke %d complete (pulses: %d)\n", strokeCount, pulses);

        // Update LEDs to show progress
        fill_solid(leds, NUM_LEDS, COLOR_OFF);
        int lit = map(strokeCount, 0, 10, 0, NUM_LEDS);
        for (int i = 0; i < lit; i++) leds[i] = CRGB::Blue;
        FastLED.show();
      }
      pulling = false;
      pulses = 0;
    }

    lastPos = pos;
    delay(10);
  }

  int32_t totalPulses = abs(encoder.getEncoderPosition());
  float rots = (float)totalPulses / ENCODER_RESOLUTION;
  float value = 20.0 / rots;

  // Flash white = done!
  setAllLEDs(CRGB::White);
  delay(500);
  FastLED.clear(true);
  FastLED.show();

  Serial.println("\n╔════════════════════════════════════════════════════╗");
  Serial.println("║          📊 CALIBRATION RESULTS                    ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf( "║ Total pulses:   %-8d                             ║\n", totalPulses);
  Serial.printf( "║ Rotations:      %-8.2f                             ║\n", rots);
  Serial.println("╚════════════════════════════════════════════════════╝\n");

  Serial.println("🎯 UPDATE YOUR CODE:");
  Serial.println("════════════════════════════════════════");
  Serial.printf( "  METERS_PER_ROTATION = %.3f;\n", value);
  Serial.println("  CALIBRATION_MODE = false;");
  Serial.println("════════════════════════════════════════\n");
  Serial.println("Save and re-upload when done!");

  while(1) {
    delay(1000);
    Serial.print(".");
  }
}