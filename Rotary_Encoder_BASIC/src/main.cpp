#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Seesaw.h>

// ==================== CONFIGURATION ====================
#define SDA_PIN 8
#define SCL_PIN 10

// CALIBRATION VALUES
float METERS_PER_ROTATION = 1.5;
const float CALORIES_PER_METER = 0.12;
const int ENCODER_RESOLUTION = 24;

bool CALIBRATION_MODE = true;  // Set to false after calibration

// ==================== HARDWARE ====================
Adafruit_seesaw encoder;  // <- THIS LINE WAS MISSING!

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

int32_t strokeStartPosition = 0;
bool inStroke = false;
const int32_t STROKE_THRESHOLD = 12;

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
  
  encoder.pinMode(24, INPUT_PULLUP);
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
    if (!sessionActive) {
      startSession();
    }
    
    int32_t delta = currentPosition - lastPosition;
    
    // Only count FORWARD movement for distance
    if (delta > 2) {  // Pull stroke
      float rotations = (float)delta / ENCODER_RESOLUTION;
      float newDistance = rotations * METERS_PER_ROTATION;
      
      currentSession.distance += newDistance;
      currentSession.totalPulses += delta;
      
      // Mark that we're in a pull stroke
      if (!inStroke) {
        inStroke = true;
        strokeStartPosition = currentPosition;
      }
      
    } else if (delta < -2 && inStroke) {  // Return stroke after pull
      // Complete stroke detected!
      currentSession.strokes++;
      inStroke = false;
      
      if (currentSession.strokes % 10 == 0) {
        printLiveUpdate();
      }
    }
    
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
    
    currentSession.calories = currentSession.distance * CALORIES_PER_METER;
    lastPosition = currentPosition;
    lastActivity = currentMillis;
  }
  
  if (sessionActive) {
    currentSession.duration = (currentMillis - sessionStart) / 1000;
  }
  
  if (sessionActive && (currentMillis - lastActivity > INACTIVITY_TIMEOUT)) {
    Serial.println("\n⏸️  Inactivity timeout...");
    endSession();
  }
  
  if (!encoder.digitalRead(24)) {
    delay(50);
    if (!encoder.digitalRead(24) && sessionActive) {
      Serial.println("\n🛑 Button pressed...");
      endSession();
      while(!encoder.digitalRead(24)) delay(10);
    }
  }
  
  delay(10);
}
// ==================== SESSIONS ====================
void startSession() {
  sessionActive = true;
  sessionStart = millis();
  lastActivity = millis();
  lastSpeedCheck = millis();
  
  currentSession = {0, 0, 0, 0, 0, 0, 0};
  
  lastPosition = encoder.getEncoderPosition();
  lastSpeedPosition = lastPosition;
  encoder.setEncoderPosition(0);
  
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       🚣 SESSION STARTED! 🚣           ║");
  Serial.println("╚════════════════════════════════════════╝\n");
}

void endSession() {
  sessionActive = false;
  
  if (currentSession.strokes > 0) {
    currentSession.avgStrokeDistance = currentSession.distance / currentSession.strokes;
  }
  
  printFinalSummary();
  
  delay(3000);
  Serial.println("\n✅ Ready for next session...\n");
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
  Serial.println("║          🎉 SESSION COMPLETE! 🎉                   ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf("║ Duration:     %2d:%02d                               ║\n", m, s);
  Serial.printf("║ Distance:     %.1f m                             ║\n", currentSession.distance);
  Serial.printf("║ Calories:     %.1f kcal                          ║\n", currentSession.calories);
  Serial.printf("║ Strokes:      %d                                  ║\n", currentSession.strokes);
  
  if (currentSession.strokes > 0) {
    Serial.printf("║ Avg/Stroke:   %.2f m                             ║\n", currentSession.avgStrokeDistance);
    float spm = (currentSession.strokes * 60.0) / currentSession.duration;
    Serial.printf("║ Strokes/Min:  %.1f                               ║\n", spm);
    Serial.printf("║ Max Speed:    %.2f m/s                           ║\n", currentSession.maxSpeed);
  }
  
  Serial.println("╚════════════════════════════════════════════════════╝");
  
  Serial.println("\n📊 CSV:");
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
  Serial.println("Row exactly 10 complete strokes (pull + return).\n");
  Serial.println("Press button to start (or wait 3 seconds)...");
  
  // Wait for button or timeout
  unsigned long startWait = millis();
  while(encoder.digitalRead(24) && (millis() - startWait < 3000)) {
    delay(100);
  }
  if (!encoder.digitalRead(24)) {
    while(!encoder.digitalRead(24)) delay(10);
  }
  
  Serial.println("\n🚣 START ROWING! (10 complete strokes)\n");
  
  encoder.setEncoderPosition(0);
  int strokeCount = 0;
  int32_t lastPos = 0;
  bool pulling = false;
  
  while(strokeCount < 10) {
    int32_t pos = encoder.getEncoderPosition();
    int32_t delta = pos - lastPos;
    
    // Detect pull stroke
    if (delta > 2 && !pulling) {
      pulling = true;
    }
    // Detect return after pull = complete stroke
    else if (delta < -2 && pulling) {
      strokeCount++;
      pulling = false;
      Serial.printf("  ✓ Stroke %d complete\n", strokeCount);
    }
    
    lastPos = pos;
    delay(10);
  }
  
  int32_t totalPulses = abs(encoder.getEncoderPosition());
  float rots = (float)totalPulses / ENCODER_RESOLUTION;
  float value = 20.0 / rots;
  
  Serial.println("\n📊 RESULTS:");
  Serial.printf("   Total pulses: %d\n", totalPulses);
  Serial.printf("   Rotations: %.2f\n\n", rots);
  Serial.println("🎯 UPDATE CODE:");
  Serial.printf("   METERS_PER_ROTATION = %.3f;\n", value);
  Serial.println("   CALIBRATION_MODE = false;\n");
  Serial.println("Re-upload after updating!\n");
  
  while(1) {
    delay(1000);
    Serial.print(".");
  }
}