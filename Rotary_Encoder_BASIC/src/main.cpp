#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_seesaw.h>

// ==================== CONFIGURATION ====================
#define SDA_PIN 8
#define SCL_PIN 10

// CALIBRATION VALUES
float METERS_PER_ROTATION = 1.028;
const float CALORIES_PER_METER = 0.12;
const int ENCODER_RESOLUTION = 24;

bool CALIBRATION_MODE = false;  // Set to false after calibration

// ==================== STROKE DETECTION ====================
// A stroke is counted when:
// 1. Encoder moves forward (pull)
// 2. Then STOPS moving for STROKE_PAUSE_MS milliseconds (return/rest)
// Adjust STROKE_PAUSE_MS if strokes are over or under counted
const unsigned long STROKE_PAUSE_MS = 300;   // ms of stillness = end of stroke
const int32_t MIN_STROKE_PULSES = 6;          // minimum pulses to count as a real stroke

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

// Stroke detection
bool inStroke = false;
int32_t strokePulses = 0;
unsigned long lastMovementTime = 0;

unsigned long lastSpeedCheck = 0;
int32_t lastSpeedPosition = 0;

// ==================== FUNCTION DECLARATIONS ====================
void startSession();
void endSession();
void printLiveUpdate();
void printFinalSummary();
void runCalibration();

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(3000);

  Serial.println("\n\n╔════════════════════════════════════════╗");
  Serial.println("║   ROWING MACHINE MONITOR v1.0          ║");
  Serial.println("╚════════════════════════════════════════╝\n");

  Wire.begin(SDA_PIN, SCL_PIN);
  Serial.println("⚙️  Initializing I2C...");
  Serial.printf("   GPIO%d (SDA), GPIO%d (SCL)\n", SDA_PIN, SCL_PIN);

  Serial.print("🔍 Looking for encoder...");
  if (!encoder.begin(0x36)) {
    Serial.println(" ❌ FAILED!");
    Serial.println("\nCheck wiring!");
    while(1) delay(1000);
  }
  Serial.println(" ✅ OK!\n");

  encoder.enableEncoderInterrupt();

  if (CALIBRATION_MODE) {
    runCalibration();
    return;
  }

  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║  🚣 READY! START ROWING...             ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

// ==================== MAIN LOOP ====================
void loop() {
  unsigned long currentMillis = millis();
  int32_t currentPosition = encoder.getEncoderPosition();

  if (currentPosition != lastPosition) {
    // Auto-start session on first movement
    if (!sessionActive) {
      startSession();
    }

    int32_t delta = currentPosition - lastPosition;

    // Only care about forward movement (encoder only goes one way)
    if (delta > 0) {
      // Accumulate distance
      float rotations = (float)delta / ENCODER_RESOLUTION;
      float newDistance = rotations * METERS_PER_ROTATION;
      currentSession.distance += newDistance;
      currentSession.totalPulses += delta;

      // Accumulate stroke pulses
      strokePulses += delta;
      inStroke = true;
      lastMovementTime = currentMillis;

      // Speed tracking
      if (currentMillis - lastSpeedCheck >= 1000) {
        int32_t speedDelta = abs(currentPosition - lastSpeedPosition);
        float speedRotations = (float)speedDelta / ENCODER_RESOLUTION;
        float currentSpeed = speedRotations * METERS_PER_ROTATION;

        if (currentSpeed > currentSession.maxSpeed) {
          currentSession.maxSpeed = currentSpeed;
        }

        lastSpeedCheck = currentMillis;
        lastSpeedPosition = currentPosition;
      }
    }

    currentSession.calories = currentSession.distance * CALORIES_PER_METER;
    lastPosition = currentPosition;
    lastActivity = currentMillis;

  } else if (inStroke && (currentMillis - lastMovementTime >= STROKE_PAUSE_MS)) {
    // Encoder STOPPED after a pull = complete stroke!
    if (strokePulses >= MIN_STROKE_PULSES) {
      currentSession.strokes++;

      if (currentSession.strokes % 10 == 0) {
        printLiveUpdate();
      }
    }
    // Reset for next stroke
    inStroke = false;
    strokePulses = 0;
  }

  if (sessionActive) {
    currentSession.duration = (currentMillis - sessionStart) / 1000;
  }

  // Auto-end after 30 seconds of no movement
  if (sessionActive && (currentMillis - lastActivity > INACTIVITY_TIMEOUT)) {
    Serial.println("\n⏸️  No activity for 30 seconds - ending session...");
    endSession();
  }

  delay(10);
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

  lastPosition = encoder.getEncoderPosition();
  lastSpeedPosition = lastPosition;
  encoder.setEncoderPosition(0);

  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       🚣 SESSION STARTED!              ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  Serial.println("Live updates every 10 strokes...\n");
}

void endSession() {
  // Count any in-progress stroke
  if (inStroke && strokePulses >= MIN_STROKE_PULSES) {
    currentSession.strokes++;
  }
  inStroke = false;
  strokePulses = 0;
  sessionActive = false;

  if (currentSession.strokes > 0) {
    currentSession.avgStrokeDistance = currentSession.distance / currentSession.strokes;
  }

  printFinalSummary();

  delay(3000);
  Serial.println("\n✅ Ready for next session - start rowing!\n");
}

// ==================== DISPLAY ====================
void printLiveUpdate() {
  int m = currentSession.duration / 60;
  int s = currentSession.duration % 60;

  Serial.printf("⚡ %3d strokes | %6.1f m | %d:%02d | %5.1f kcal\n",
                currentSession.strokes, currentSession.distance, m, s, currentSession.calories);
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
  Serial.println("Get ready to row exactly 10 complete strokes.");
  Serial.println("Pull the cable fully, let it return, repeat.\n");

  for (int i = 5; i > 0; i--) {
    Serial.printf("Starting in %d seconds...\n", i);
    delay(1000);
  }

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
    } else if (pulling && (millis() - lastMove >= STROKE_PAUSE_MS)) {
      if (pulses >= MIN_STROKE_PULSES) {
        strokeCount++;
        Serial.printf("  ✓ Stroke %d complete (pulses: %d)\n", strokeCount, pulses);
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

  Serial.println("\n╔════════════════════════════════════════════════════╗");
  Serial.println("║          📊 CALIBRATION RESULTS                    ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf( "║ Total pulses:   %-8d                             ║\n", totalPulses);
  Serial.printf( "║ Rotations:      %-8.2f                             ║\n", rots);
  Serial.println("╚════════════════════════════════════════════════════╝\n");

  Serial.println("🎯 UPDATE YOUR CODE WITH THESE VALUES:");
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