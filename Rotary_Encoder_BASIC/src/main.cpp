#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Seesaw.h>

// ==================== CONFIGURATION ====================
#define SDA_PIN 21
#define SCL_PIN 22

// CALIBRATION VALUES - Will be set during first run
float METERS_PER_ROTATION = 1.5;      // Adjust after calibration
const float CALORIES_PER_METER = 0.12; // Conservative estimate for elderly
const int ENCODER_RESOLUTION = 24;     // Adafruit encoder: 24 pulses per rotation

// Set to true for first run to calibrate
bool CALIBRATION_MODE = true;  // CHANGE TO false AFTER FIRST CALIBRATION

// ==================== HARDWARE ====================
Adafruit_Seesaw encoder;

// ==================== SESSION DATA ====================
struct SessionData {
  float distance;           // meters
  float calories;           // kcal
  unsigned long duration;   // seconds
  int strokes;             // number of rowing strokes
  float avgStrokeDistance;  // meters per stroke
  int32_t totalPulses;
  float maxSpeed;          // max speed during session
};

SessionData currentSession = {0, 0, 0, 0, 0, 0, 0};

// ==================== TRACKING VARIABLES ====================
int32_t lastPosition = 0;
unsigned long sessionStart = 0;
bool sessionActive = false;
unsigned long lastActivity = 0;
const unsigned long INACTIVITY_TIMEOUT = 30000; // 30 seconds

// Stroke detection
int32_t strokeStartPosition = 0;
bool inStroke = false;
const int32_t STROKE_THRESHOLD = 12; // Half rotation to detect stroke

// Speed tracking
unsigned long lastSpeedCheck = 0;
int32_t lastSpeedPosition = 0;

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(2000);
  
  Serial.println("\n\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║   ROWING MACHINE MONITOR v1.0          ║");
  Serial.println("║   For Elderly Fitness Tracking         ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println();
  
  // Initialize I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  Serial.println("⚙️  Initializing I2C bus...");
  
  // Initialize Encoder
  Serial.print("🔍 Looking for rotary encoder...");
  if (!encoder.begin(0x36)) {
    Serial.println(" ❌ FAILED!");
    Serial.println();
    Serial.println("ERROR: Encoder not found!");
    Serial.println("═══════════════════════════");
    Serial.println("Check your wiring:");
    Serial.println("  Encoder → ESP32");
    Serial.println("  SDA (Blue)   → GPIO 21");
    Serial.println("  SCL (Yellow) → GPIO 22");
    Serial.println("  VCC (Red)    → 3.3V");
    Serial.println("  GND (Black)  → GND");
    Serial.println();
    Serial.println("Halting... Fix wiring and reset ESP32");
    Serial.println("═══════════════════════════");
    while(1) {
      delay(1000);
      Serial.print(".");
    }
  }
  Serial.println(" ✅ OK!");
  
  // Configure encoder
  encoder.pinMode(24, INPUT_PULLUP); // Button pin
  encoder.enableEncoderInterrupt();
  
  Serial.println("✅ Hardware initialized successfully!");
  Serial.println();
  
  // Check if calibration mode
  if (CALIBRATION_MODE) {
    runCalibration();
    return; // Stop here until code is updated
  }
  
  // Show current settings
  Serial.println("📊 Current Settings:");
  Serial.println("───────────────────────────────────────");
  Serial.printf("  Meters per rotation: %.2f m\n", METERS_PER_ROTATION);
  Serial.printf("  Calories per meter:  %.2f kcal\n", CALORIES_PER_METER);
  Serial.printf("  Encoder resolution:  %d pulses/rotation\n", ENCODER_RESOLUTION);
  Serial.printf("  Inactivity timeout:  %d seconds\n", INACTIVITY_TIMEOUT/1000);
  Serial.println("───────────────────────────────────────");
  Serial.println();
  
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║  🚣 READY! START ROWING TO BEGIN...    ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println();
}

// ==================== MAIN LOOP ====================
void loop() {
  unsigned long currentMillis = millis();
  
  // Read encoder
  int32_t currentPosition = encoder.getEncoderPosition();
  
  // Detect movement
  if (currentPosition != lastPosition) {
    
    // Start session if not active
    if (!sessionActive) {
      startSession();
    }
    
    // Calculate change
    int32_t delta = currentPosition - lastPosition;
    
    // Update distance
    float rotations = (float)abs(delta) / ENCODER_RESOLUTION;
    float newDistance = rotations * METERS_PER_ROTATION;
    currentSession.distance += newDistance;
    currentSession.totalPulses += abs(delta);
    
    // Calculate current speed (every second)
    if (currentMillis - lastSpeedCheck >= 1000) {
      int32_t speedDelta = abs(currentPosition - lastSpeedPosition);
      float speedRotations = (float)speedDelta / ENCODER_RESOLUTION;
      float currentSpeed = speedRotations * METERS_PER_ROTATION; // meters per second
      
      if (currentSpeed > currentSession.maxSpeed) {
        currentSession.maxSpeed = currentSpeed;
      }
      
      lastSpeedCheck = currentMillis;
      lastSpeedPosition = currentPosition;
    }
    
    // Detect strokes (forward pull motion)
    if (abs(delta) > 2) { // Filter out noise
      if (!inStroke && delta > 0) {
        // Start of pull stroke
        inStroke = true;
        strokeStartPosition = currentPosition;
      } else if (inStroke && currentPosition - strokeStartPosition >= STROKE_THRESHOLD) {
        // Complete stroke detected
        currentSession.strokes++;
        inStroke = false;
        
        // Print live update every 10 strokes
        if (currentSession.strokes % 10 == 0) {
          printLiveUpdate();
        }
      }
    }
    
    // Update calories
    currentSession.calories = currentSession.distance * CALORIES_PER_METER;
    
    lastPosition = currentPosition;
    lastActivity = currentMillis;
  }
  
  // Update session time
  if (sessionActive) {
    currentSession.duration = (currentMillis - sessionStart) / 1000;
  }
  
  // Check for inactivity timeout
  if (sessionActive && (currentMillis - lastActivity > INACTIVITY_TIMEOUT)) {
    Serial.println("\n⏸️  No activity detected for 30 seconds...");
    endSession();
  }
  
  // Check button for manual end
  if (!encoder.digitalRead(24)) {
    delay(50); // Debounce
    if (!encoder.digitalRead(24) && sessionActive) {
      Serial.println("\n🛑 Button pressed - ending session...");
      endSession();
      while(!encoder.digitalRead(24)) delay(10); // Wait for release
    }
  }
  
  delay(10); // Small delay to reduce CPU load
}

// ==================== SESSION MANAGEMENT ====================
void startSession() {
  sessionActive = true;
  sessionStart = millis();
  lastActivity = millis();
  lastSpeedCheck = millis();
  
  // Reset session data
  currentSession.distance = 0;
  currentSession.calories = 0;
  currentSession.duration = 0;
  currentSession.strokes = 0;
  currentSession.totalPulses = 0;
  currentSession.avgStrokeDistance = 0;
  currentSession.maxSpeed = 0;
  
  lastPosition = encoder.getEncoderPosition();
  lastSpeedPosition = lastPosition;
  encoder.setEncoderPosition(0); // Reset encoder to 0
  
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║       🚣 SESSION STARTED! 🚣           ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println();
  Serial.println("Live updates every 10 strokes...");
  Serial.println();
}

void endSession() {
  sessionActive = false;
  
  // Calculate averages
  if (currentSession.strokes > 0) {
    currentSession.avgStrokeDistance = currentSession.distance / currentSession.strokes;
  }
  
  // Print final summary
  printFinalSummary();
  
  // Reset for next session
  delay(3000);
  Serial.println();
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║   ✅ READY FOR NEXT SESSION...         ║");
  Serial.println("╚════════════════════════════════════════╝");
  Serial.println();
}

// ==================== DISPLAY FUNCTIONS ====================
void printLiveUpdate() {
  int minutes = currentSession.duration / 60;
  int seconds = currentSession.duration % 60;
  
  Serial.printf("⚡ Strokes: %3d | Distance: %6.1f m | Time: %d:%02d | Cal: %5.1f kcal\n", 
                currentSession.strokes,
                currentSession.distance,
                minutes, seconds,
                currentSession.calories);
}

void printFinalSummary() {
  int minutes = currentSession.duration / 60;
  int seconds = currentSession.duration % 60;
  
  Serial.println();
  Serial.println();
  Serial.println("╔════════════════════════════════════════════════════╗");
  Serial.println("║          🎉 SESSION COMPLETE! 🎉                   ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf("║ ⏱️  Duration:        %2d min %02d sec                 ║\n", minutes, seconds);
  Serial.printf("║ 📏 Distance:        %-8.1f meters              ║\n", currentSession.distance);
  Serial.printf("║ 🔥 Calories:        %-8.1f kcal                ║\n", currentSession.calories);
  Serial.printf("║ 🚣 Total Strokes:   %-8d                       ║\n", currentSession.strokes);
  
  if (currentSession.strokes > 0) {
    Serial.printf("║ 📊 Avg per Stroke:  %-8.2f meters              ║\n", currentSession.avgStrokeDistance);
    float strokesPerMin = (currentSession.strokes * 60.0) / currentSession.duration;
    Serial.printf("║ ⚡ Strokes/Minute:  %-8.1f                      ║\n", strokesPerMin);
    Serial.printf("║ 🏃 Max Speed:       %-8.2f m/s                 ║\n", currentSession.maxSpeed);
  }
  
  Serial.println("╚════════════════════════════════════════════════════╝");
  
  // CSV format for easy logging
  Serial.println();
  Serial.println("📊 CSV FORMAT (Copy to spreadsheet):");
  Serial.println("───────────────────────────────────────────────────");
  Serial.println("Duration(s),Distance(m),Calories(kcal),Strokes,Avg/Stroke(m),Strokes/Min,MaxSpeed(m/s)");
  
  float strokesPerMin = 0;
  if (currentSession.duration > 0) {
    strokesPerMin = (currentSession.strokes * 60.0) / currentSession.duration;
  }
  
  Serial.printf("%lu,%.2f,%.2f,%d,%.2f,%.1f,%.2f\n",
                currentSession.duration,
                currentSession.distance,
                currentSession.calories,
                currentSession.strokes,
                currentSession.avgStrokeDistance,
                strokesPerMin,
                currentSession.maxSpeed);
  Serial.println("───────────────────────────────────────────────────");
}

// ==================== CALIBRATION ====================
void runCalibration() {
  Serial.println();
  Serial.println("╔════════════════════════════════════════════════════╗");
  Serial.println("║          🎯 CALIBRATION MODE ACTIVE 🎯             ║");
  Serial.println("╚════════════════════════════════════════════════════╝");
  Serial.println();
  Serial.println("INSTRUCTIONS FOR CALIBRATION:");
  Serial.println("═════════════════════════════════════════════════════");
  Serial.println("Since your encoder is attached to the cable wheel:");
  Serial.println();
  Serial.println("METHOD 1 (Recommended - Simple):");
  Serial.println("  1. Row EXACTLY 10 complete strokes");
  Serial.println("  2. Count carefully - full pull and return = 1 stroke");
  Serial.println("  3. Press encoder button when done");
  Serial.println();
  Serial.println("The system assumes ~2 meters per stroke (typical)");
  Serial.println("and will calculate the correct calibration value.");
  Serial.println();
  Serial.println("═════════════════════════════════════════════════════");
  Serial.println();
  Serial.println("Press encoder button to start calibration...");
  
  // Wait for button press to start
  while(encoder.digitalRead(24)) {
    delay(100);
  }
  while(!encoder.digitalRead(24)) delay(10); // Wait for release
  
  Serial.println();
  Serial.println("🚣 START ROWING NOW! (10 strokes) 🚣");
  Serial.println();
  
  encoder.setEncoderPosition(0);
  int strokeCount = 0;
  int32_t lastPos = 0;
  int32_t strokeStart = 0;
  bool inStroke = false;
  
  // Count strokes
  while(strokeCount < 10) {
    int32_t pos = encoder.getEncoderPosition();
    int32_t delta = pos - lastPos;
    
    if (!inStroke && delta > 2) {
      inStroke = true;
      strokeStart = pos;
    } else if (inStroke && pos - strokeStart >= STROKE_THRESHOLD) {
      strokeCount++;
      inStroke = false;
      Serial.printf("  ✓ Stroke %d detected\n", strokeCount);
    }
    
    lastPos = pos;
    delay(10);
  }
  
  int32_t totalPulses = abs(encoder.getEncoderPosition());
  float rotations = (float)totalPulses / ENCODER_RESOLUTION;
  
  Serial.println();
  Serial.println("╔════════════════════════════════════════════════════╗");
  Serial.println("║           📊 CALIBRATION RESULTS 📊                ║");
  Serial.println("╠════════════════════════════════════════════════════╣");
  Serial.printf("║ Total encoder pulses:  %-8d                    ║\n", totalPulses);
  Serial.printf("║ Wheel rotations:       %-8.2f                    ║\n", rotations);
  Serial.println("╚════════════════════════════════════════════════════╝");
  Serial.println();
  
  // Estimate distance assuming 2 meters per stroke (average for elderly)
  float estimatedDistance = 20.0; // 10 strokes × 2m/stroke
  float calculatedValue = estimatedDistance / rotations;
  
  Serial.println("🎯 CALCULATED CALIBRATION VALUE:");
  Serial.println("═════════════════════════════════════════════════════");
  Serial.printf("  METERS_PER_ROTATION = %.3f;\n", calculatedValue);
  Serial.println("═════════════════════════════════════════════════════");
  Serial.println();
  
  Serial.println("📝 ACTION REQUIRED:");
  Serial.println("───────────────────────────────────────────────────");
  Serial.println("1. Copy the value above");
  Serial.println("2. Open main.cpp in your editor");
  Serial.println("3. Find this line (around line 9):");
  Serial.println("   float METERS_PER_ROTATION = 1.5;");
  Serial.printf("4. Replace with: float METERS_PER_ROTATION = %.3f;\n", calculatedValue);
  Serial.println();
  Serial.println("5. Find this line (around line 15):");
  Serial.println("   bool CALIBRATION_MODE = true;");
  Serial.println("6. Change to: bool CALIBRATION_MODE = false;");
  Serial.println();
  Serial.println("7. Save and re-upload to ESP32");
  Serial.println("───────────────────────────────────────────────────");
  Serial.println();
  Serial.println("System halted. Update code and reset ESP32.");
  Serial.println();
  
  while(1) {
    delay(1000);
    Serial.print(".");
  }
}