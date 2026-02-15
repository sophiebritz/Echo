#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_Seesaw.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_NeoPixel.h>

// Pin Definitions
#define SDA_PIN 21
#define SCL_PIN 22
#define NEOPIXEL_PIN 25
#define NEOPIXEL_COUNT 12

// Hardware Objects
Adafruit_Seesaw encoder;
Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_NeoPixel pixels(NEOPIXEL_COUNT, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// Rowing Metrics
int32_t encoderPosition = 0;
int32_t lastPosition = 0;
float distance = 0;              // meters
float calories = 0;              // kcal
unsigned long sessionStart = 0;  
unsigned long sessionTime = 0;   // seconds
bool sessionActive = false;

// Calibration - ADJUST THESE AFTER TESTING
const float METERS_PER_ROTATION = 2.0;   // Start with 2m, calibrate later
const float CALORIES_PER_METER = 0.12;   // Conservative for elderly
const int ENCODER_RESOLUTION = 24;       // Pulses per rotation

// Timing
unsigned long lastUpdate = 0;
unsigned long lastActivity = 0;
const int UPDATE_INTERVAL = 100;
const int INACTIVITY_TIMEOUT = 30000;

// LED Speed Thresholds
int currentSpeed = 0;

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("Rowing Machine Starting...");
  
  // Initialize I2C
  Wire.begin(SDA_PIN, SCL_PIN);
  
  // Initialize Rotary Encoder
  if (!encoder.begin(0x36)) {
    Serial.println("ERROR: Encoder not found!");
    Serial.println("Check I2C wiring:");
    Serial.println("  SDA -> GPIO 21");
    Serial.println("  SCL -> GPIO 22");
    while(1) {
      delay(1000);
    }
  }
  Serial.println("✓ Encoder initialized");
  
  // Configure encoder button (optional for reset)
  encoder.pinMode(24, INPUT_PULLUP);
  encoder.enableEncoderInterrupt();
  
  // Initialize OLED Display
  if(!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("ERROR: Display not found!");
    while(1) delay(1000);
  }
  Serial.println("✓ Display initialized");
  
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.display();
  
  // Initialize NeoPixels
  pixels.begin();
  pixels.setBrightness(50);
  pixels.clear();
  pixels.show();
  Serial.println("✓ NeoPixels initialized");
  
  // Welcome Screen
  showWelcomeScreen();
  delay(3000);
  
  Serial.println("Ready! Start rowing...");
}

// ==================== MAIN LOOP ====================
void loop() {
  unsigned long currentMillis = millis();
  
  // Read encoder position
  int32_t newPosition = encoder.getEncoderPosition();
  
  // Detect movement
  if (newPosition != lastPosition) {
    // Start session if not active
    if (!sessionActive) {
      startSession();
    }
    
    // Calculate distance
    int32_t delta = newPosition - lastPosition;
    float rotations = (float)delta / ENCODER_RESOLUTION;
    float newDistance = rotations * METERS_PER_ROTATION;
    
    distance += abs(newDistance);
    calories = distance * CALORIES_PER_METER;
    
    // Update speed for LED feedback
    currentSpeed = abs(delta);
    
    lastPosition = newPosition;
    lastActivity = currentMillis;
    
    // Update LED feedback
    updateLEDFeedback(currentSpeed);
  } else {
    // Gradually dim LEDs when not rowing
    currentSpeed = max(0, currentSpeed - 1);
    updateLEDFeedback(currentSpeed);
  }
  
  // Update display periodically
  if (currentMillis - lastUpdate >= UPDATE_INTERVAL) {
    if (sessionActive) {
      sessionTime = (millis() - sessionStart) / 1000;
      updateDisplay();
    }
    lastUpdate = currentMillis;
  }
  
  // Check for session end (inactivity)
  if (sessionActive && (currentMillis - lastActivity > INACTIVITY_TIMEOUT)) {
    endSession();
  }
  
  // Check button press for manual end
  if (!encoder.digitalRead(24)) {
    delay(50);
    if (!encoder.digitalRead(24) && sessionActive) {
      endSession();
      while(!encoder.digitalRead(24)) delay(10); // Wait for release
    }
  }
}

// ==================== SESSION MANAGEMENT ====================
void startSession() {
  sessionActive = true;
  sessionStart = millis();
  lastActivity = millis();
  distance = 0;
  calories = 0;
  sessionTime = 0;
  lastPosition = encoder.getEncoderPosition();
  
  Serial.println("\n=== SESSION STARTED ===");
  
  // Flash LEDs green
  for(int i=0; i<NEOPIXEL_COUNT; i++) {
    pixels.setPixelColor(i, pixels.Color(0, 255, 0));
  }
  pixels.show();
  delay(200);
  pixels.clear();
  pixels.show();
}

void endSession() {
  sessionActive = false;
  
  Serial.println("\n=== SESSION ENDED ===");
  Serial.printf("Time: %d:%02d\n", sessionTime/60, sessionTime%60);
  Serial.printf("Distance: %.1f m\n", distance);
  Serial.printf("Calories: %.1f kcal\n", calories);
  
  // Show summary
  showSummaryScreen();
  celebrationLEDs();
  
  delay(10000);  // Show summary for 10 seconds
  showWelcomeScreen();
}

// ==================== DISPLAY FUNCTIONS ====================
void updateDisplay() {
  display.clearDisplay();
  
  // Title
  display.setTextSize(1);
  display.setCursor(25, 0);
  display.println("ROWING");
  
  // Time
  display.setTextSize(1);
  display.setCursor(0, 15);
  display.print("Time:  ");
  display.print(sessionTime / 60);
  display.print(":");
  if (sessionTime % 60 < 10) display.print("0");
  display.print(sessionTime % 60);
  
  // Distance
  display.setCursor(0, 30);
  display.print("Dist:  ");
  display.print(distance, 1);
  display.println(" m");
  
  // Calories
  display.setCursor(0, 45);
  display.print("Cal:   ");
  display.print(calories, 1);
  display.println(" kcal");
  
  display.display();
}

void showWelcomeScreen() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(10, 15);
  display.println("ROWING");
  display.setTextSize(1);
  display.setCursor(15, 40);
  display.println("Start Rowing!");
  display.display();
}

void showSummaryScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(10, 0);
  display.println("GREAT WORK!");
  
  display.setCursor(0, 18);
  display.print("Time: ");
  display.print(sessionTime / 60);
  display.print(":");
  if (sessionTime % 60 < 10) display.print("0");
  display.println(sessionTime % 60);
  
  display.setCursor(0, 31);
  display.print("Distance: ");
  display.print(distance, 1);
  display.println(" m");
  
  display.setCursor(0, 44);
  display.print("Calories: ");
  display.print(calories, 1);
  display.println(" kcal");
  
  display.display();
}

// ==================== LED FEEDBACK ====================
void updateLEDFeedback(int speed) {
  // Map speed to number of LEDs (0-12)
  int numLEDs = map(constrain(speed, 0, 50), 0, 50, 0, NEOPIXEL_COUNT);
  
  pixels.clear();
  
  // Choose color based on intensity
  uint32_t color;
  if (numLEDs < 4) {
    color = pixels.Color(0, 100, 255);    // Blue - slow/warmup
  } else if (numLEDs < 8) {
    color = pixels.Color(0, 255, 0);      // Green - moderate
  } else if (numLEDs < 11) {
    color = pixels.Color(255, 255, 0);    // Yellow - good pace
  } else {
    color = pixels.Color(255, 100, 0);    // Orange - excellent!
  }
  
  // Light up LEDs in a circle
  for (int i = 0; i < numLEDs; i++) {
    pixels.setPixelColor(i, color);
  }
  
  pixels.show();
}

void celebrationLEDs() {
  // Rainbow celebration
  for (int j = 0; j < 3; j++) {
    for (int i = 0; i < NEOPIXEL_COUNT; i++) {
      uint32_t color = pixels.ColorHSV((i * 65536L) / NEOPIXEL_COUNT);
      pixels.setPixelColor(i, color);
      pixels.show();
      delay(50);
    }
    delay(200);
  }
  pixels.clear();
  pixels.show();
}