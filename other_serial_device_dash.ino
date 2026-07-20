/* ====================================================================================
 *  SIM RACING GAUGE CLUSTER CONTROLLER (Arduino Nano)
 *  
 *  Description: 
 *  Receives telemetry data from SimHub via Serial and drives physical hardware:
 *  - High-frequency signals for Tachometer and Speedometer (via Timer1 Interrupts)
 *  - PWM signals for Analog Gauges (Temperature, Fuel, Boost)
 *  - PWM/Digital signals for Warning Lights (Shift, Check Engine)
 *  - I2C Communication for a 16x2 Character LCD
 *
 *  Telemetry Format (Semicolon-delimited Key-Value Pairs):
 *  "S=1;E=0;V=60;R=4500;T=90;F=100;B=12;L1=BOOST ACTIVE;L2=CRUISING;\n"
 * ==================================================================================== */

// ------------------------------------------------------------------------------------
// 1. LIBRARIES & OBJECT INITIALIZATION
// ------------------------------------------------------------------------------------
#include <LiquidCrystal_I2C.h> // Drives the 16x2 LCD display
#include <Wire.h>              // Required for I2C communication
#include <TimerOne.h>          // Hardware timer for precise frequency generation
#include <avr/pgmspace.h>      // Required for PROGMEM string handling

// Initialize LCD (Address: 0x27, Columns: 16, Rows: 2)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ------------------------------------------------------------------------------------
// 2. HARDWARE PIN DEFINITIONS
// ------------------------------------------------------------------------------------
#define TACH_PIN       2  // Tachometer frequency output (PORTD Bit 2)
#define SHIFT_PIN      3  // Shift light PWM output
#define ENGINE_PIN     4  // Check engine light digital output
#define TEMP_PIN       5  // Coolant temperature gauge PWM output
#define FUEL_PIN       6  // Fuel gauge PWM output
#define BOOST_PIN      9  // Boost pressure gauge PWM output
#define SPEEDO_PIN     11 // Speedometer frequency output (PORTB Bit 3)
#define AUX_5V_PIN     12 // Auxiliary 5V output for external logic/relays

// ------------------------------------------------------------------------------------
// 3. GLOBAL VARIABLES & STATE TRACKING
// ------------------------------------------------------------------------------------
// Serial Parsing Variables (Optimized size to save SRAM)
const byte maxChars = 100;      
char receivedChars[maxChars];    
char tempChars[maxChars];        
boolean newData = false;         

// Safety & Timing Variables
unsigned long lastValidDataTime = 0;
const unsigned long timeoutMillis = 2000; // Time before failsafe triggers
bool isHardwareStandby = false;
unsigned long lastProcessTime = 0;
const unsigned long intervalMicros = 8333; // Main loop runs at ~120Hz

// Telemetry Value Storage Array
// Index: 0=Shift, 1=Engine, 2=Speed, 3=RPM, 4=Temp, 5=Fuel, 6=Boost
int values[7] = {0, 0, 0, 0, 0, 0, 0}; 
int currentShiftPWM = 0; // Tracks current brightness for smooth fading

// LCD State Buffers
char lcdLine1[17] = "                "; 
char lcdLine2[17] = "                "; 
char lastLine1[17] = ""; 
char lastLine2[17] = "";

// Interrupt (ISR) State Variables (Must be volatile as they change outside the main loop)
volatile unsigned int tachPeriodTicks = 0;
volatile unsigned int speedoPeriodTicks = 0;
volatile unsigned int tachTickCounter = 0;
volatile unsigned int speedoTickCounter = 0;
volatile bool tachState = false;
volatile bool speedoState = false;

// ------------------------------------------------------------------------------------
// 4. GAUGE CALIBRATION TABLES
// ------------------------------------------------------------------------------------
// These tables map incoming telemetry values (Steps) to hardware outputs (PWM or Hz).
// Adjust these arrays to calibrate your physical gauge needles.

// Temperature (Values -> PWM)
const int TEMP_POINTS = 3;
const int tempSteps[] = {0, 80, 140};    
const int tempPWMVal[] = {0, 170, 220};  

// Fuel (Values -> PWM)
const int FUEL_POINTS = 4;
const int fuelSteps[] = {0, 15, 50, 100};   
const int fuelPWMVal[] = {0, 130, 161, 255};

// Boost (Values -> PWM)
const int BOOST_POINTS = 3;
const int boostSteps[] = {0, 15, 30};    
const int boostPWMVal[] = {0, 127, 255};  

// Tachometer (RPM -> Frequency in Hz)
const int TACH_POINTS = 14;
const int rpmSteps[] = {0,1000,2000,3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,13000}; 
const int tachHz[]   = {0,38,73,105,135,166,197,226,258,289,320,352,386,434};                   

// Speedometer (MPH -> Frequency in Hz)
const int SPEED_POINTS = 8;
const int speedSteps[] = {0,16,40,80,100,120,140,180}; 
const int speedHz[]    = {0,31,88,178,222,265,300,384};   

// ------------------------------------------------------------------------------------
// 5. INTERRUPT SERVICE ROUTINE (ISR)
// ------------------------------------------------------------------------------------
// This function is called every 50 microseconds by Timer1.
// It uses direct port manipulation for extremely fast execution to generate clean frequencies.
void toneISR() {
  // Process Tachometer Pulse
  if (tachPeriodTicks > 0) {
    tachTickCounter++;
    if (tachTickCounter >= tachPeriodTicks) {
      tachTickCounter = 0;
      tachState = !tachState; 
      // Toggle Pin 2 (PORTD Bit 2)
      if (tachState) { PORTD |= B00000100; } else { PORTD &= ~B00000100; } 
    }
  }

  // Process Speedometer Pulse
  if (speedoPeriodTicks > 0) {
    speedoTickCounter++;
    if (speedoTickCounter >= speedoPeriodTicks) {
      speedoTickCounter = 0;
      speedoState = !speedoState; 
      // Toggle Pin 11 (PORTB Bit 3)
      if (speedoState) { PORTB |= B00001000; } else { PORTB &= ~B00001000; } 
    }
  }
}

// ------------------------------------------------------------------------------------
// 6. HELPER FUNCTIONS
// ------------------------------------------------------------------------------------

/**
 * Calculates the exact output (PWM or Hz) based on the current telemetry value 
 * by interpolating between the points defined in the calibration tables.
 */
int calculateOutput(int currentVal, const int stepsArray[], const int outArray[], int totalPoints) {
  if (currentVal <= stepsArray[0]) return outArray[0];
  if (currentVal >= stepsArray[totalPoints - 1]) return outArray[totalPoints - 1];
  
  for (int i = 0; i < totalPoints - 1; i++) {
    if (currentVal >= stepsArray[i] && currentVal <= stepsArray[i+1]) {
      // Linear interpolation formula
      return (int)((long)outArray[i] + ((long)(currentVal - stepsArray[i]) * (outArray[i+1] - outArray[i])) / (stepsArray[i+1] - stepsArray[i]));
    }
  }
  return 0;
}

/**
 * Reads incoming serial data character by character until a newline ('\n') is detected.
 */
void receiveSerialData() {
    static byte ndx = 0;
    char rc;
    while (Serial.available() > 0 && newData == false) {
        rc = Serial.read();
        if (rc != '\n') {
            // Ignore carriage returns, store valid characters
            if (rc != '\r' && ndx < maxChars - 1) { 
                receivedChars[ndx++] = rc;
            }
        } else {
            receivedChars[ndx] = '\0'; // Terminate string
            ndx = 0;
            newData = true;
        }
    }
}

/**
 * Ensures LCD text is exactly 16 characters long by padding with spaces.
 */
void extractAndPadString(char* source, char* destination) {
    if (source != NULL) {
        int len = strlen(source);
        for (int i = 0; i < 16; i++) {
            if (i < len) destination[i] = source[i];
            else destination[i] = ' '; 
        }
        destination[16] = '\0'; // Null terminate
    } else {
        for (int i = 0; i < 16; i++) destination[i] = ' ';
        destination[16] = '\0';
    }
}

/**
 * Tokenizes the incoming string (split by ';') and assigns values to the respective variables.
 */
bool parseTelemetryData() {
    char * token = strtok(tempChars, ";");
    while (token != NULL) {
        if (strncmp(token, "S=", 2) == 0) values[0] = atoi(token + 2); 
        else if (strncmp(token, "E=", 2) == 0) values[1] = atoi(token + 2); 
        else if (strncmp(token, "V=", 2) == 0) values[2] = atoi(token + 2); 
        else if (strncmp(token, "R=", 2) == 0) values[3] = atoi(token + 2); 
        else if (strncmp(token, "T=", 2) == 0) values[4] = atoi(token + 2); 
        else if (strncmp(token, "F=", 2) == 0) values[5] = atoi(token + 2); 
        else if (strncmp(token, "B=", 2) == 0) values[6] = atoi(token + 2); 
        else if (strncmp(token, "L1=", 3) == 0) extractAndPadString(token + 3, lcdLine1); 
        else if (strncmp(token, "L2=", 3) == 0) extractAndPadString(token + 3, lcdLine2); 
        
        token = strtok(NULL, ";");
    }
    return true; 
}

/**
 * Zeroes out all gauges and updates the LCD if the connection to the PC is lost.
 */
void triggerFailsafe() {
    for (int i = 0; i < 7; i++) values[i] = 0; 
    strcpy_P(lcdLine1, PSTR("  SIMHUB READY  "));
    strcpy_P(lcdLine2, PSTR(" WAITING FOR PC "));
}

// ------------------------------------------------------------------------------------
// 7. ARDUINO SETUP
// ------------------------------------------------------------------------------------
void setup() {
  Serial.begin(115200); 
  
  // Configure Auxiliary 5V Pin
  pinMode(AUX_5V_PIN, OUTPUT); 
  digitalWrite(AUX_5V_PIN, HIGH); 
  
  // Configure Output Pins & Ensure they start LOW
  pinMode(SHIFT_PIN, OUTPUT); 
  pinMode(ENGINE_PIN, OUTPUT);
  digitalWrite(SHIFT_PIN, LOW); 
  digitalWrite(ENGINE_PIN, LOW);

  pinMode(TACH_PIN, OUTPUT); 
  pinMode(SPEEDO_PIN, OUTPUT);
  digitalWrite(TACH_PIN, LOW); 
  digitalWrite(SPEEDO_PIN, LOW);
  
  pinMode(TEMP_PIN, OUTPUT); 
  pinMode(FUEL_PIN, OUTPUT); 
  pinMode(BOOST_PIN, OUTPUT);
  digitalWrite(TEMP_PIN, LOW); 
  digitalWrite(FUEL_PIN, LOW); 
  digitalWrite(BOOST_PIN, LOW);
    
  // Initialize Hardware Timer for 50 microseconds
  Timer1.initialize(50);
  Timer1.attachInterrupt(toneISR); 

  // Initialize I2C LCD Display
  Wire.begin();
  lcd.init();
  Wire.setClock(400000); // Maximize I2C bus speed to prevent loop blocking
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print(F("System Loading.."));
}

// ------------------------------------------------------------------------------------
// 8. MAIN ARDUINO LOOP
// ------------------------------------------------------------------------------------
void loop() {
  
  // STEP A: Fetch and parse incoming serial data
  receiveSerialData();
  if (newData == true) {
    strcpy(tempChars, receivedChars); 
    if (parseTelemetryData()) {
        lastValidDataTime = millis();
        isHardwareStandby = false;
    }
    newData = false;
  }

  // STEP B: Check for communication timeouts
  if (!isHardwareStandby && (millis() - lastValidDataTime > timeoutMillis)) {
      triggerFailsafe();
      isHardwareStandby = true;
  }

  // STEP C: Hardware Execution Loop (Runs at ~120Hz for stability)
  unsigned long nowMicros = micros();
  if (nowMicros - lastProcessTime >= intervalMicros) {
    lastProcessTime = nowMicros; 
                
    // -- SPEEDOMETER LOGIC --
    // Calculates target Hz and applies smoothing to prevent erratic needle jumping
    int rawSpeedHz = calculateOutput(values[2], speedSteps, speedHz, SPEED_POINTS);
    static int currentSpeedHz = 0;
    static int zeroFrameCounter = 0;
    
    // Prevent immediate zeroing to smooth out simulation stutters
    if (rawSpeedHz == 0 && currentSpeedHz > 15) {
      zeroFrameCounter++;
      if (zeroFrameCounter < 12) { rawSpeedHz = currentSpeedHz; }
    } else { zeroFrameCounter = 0; }

    // Apply acceleration/deceleration limits to the needle
    const int SPEED_ACCEL_RATE = 5; 
    const int SPEED_DECEL_RATE = 3; 
    if (rawSpeedHz > currentSpeedHz + SPEED_ACCEL_RATE) currentSpeedHz += SPEED_ACCEL_RATE; 
    else if (rawSpeedHz < currentSpeedHz - SPEED_DECEL_RATE) currentSpeedHz -= SPEED_DECEL_RATE; 
    else currentSpeedHz = rawSpeedHz; 

    // -- TACHOMETER LOGIC --
    int targetTachHz = calculateOutput(values[3], rpmSteps, tachHz, TACH_POINTS);

    // -- ANALOG GAUGES LOGIC (Temp, Fuel, Boost) --
    int targetTempPWM = calculateOutput(values[4], tempSteps, tempPWMVal, TEMP_POINTS);
    int targetFuelPWM = calculateOutput(values[5], fuelSteps, fuelPWMVal, FUEL_POINTS);
    int targetBoostPWM = calculateOutput(values[6], boostSteps, boostPWMVal, BOOST_POINTS);

    // Write PWM to analog gauge pins (Safely turning them off if 0)
    if (targetFuelPWM > 0) analogWrite(FUEL_PIN, targetFuelPWM); else analogWrite(FUEL_PIN, 0); 
    if (targetTempPWM > 0) analogWrite(TEMP_PIN, targetTempPWM); else analogWrite(TEMP_PIN, 0); 
    if (targetBoostPWM > 0) analogWrite(BOOST_PIN, targetBoostPWM); else analogWrite(BOOST_PIN, 0); 
    
    // -- FREQUENCY DELIVERY TO ISR (OPTIMIZED NON-BLOCKING MATH) --
    // Perform division math outside the critical section to prevent missing 50µs interrupts
    unsigned int newTachTicks = (targetTachHz > 15) ? (10000 / targetTachHz) : 0;
    unsigned int newSpeedoTicks = (currentSpeedHz > 15) ? (10000 / currentSpeedHz) : 0;

    // Briefly pause interrupts only for safe register/variable assignments
    noInterrupts(); 
    tachPeriodTicks = newTachTicks;
    if (newTachTicks == 0) { tachState = false; digitalWrite(TACH_PIN, LOW); }
    
    speedoPeriodTicks = newSpeedoTicks;
    if (newSpeedoTicks == 0) { speedoState = false; digitalWrite(SPEEDO_PIN, LOW); }
    interrupts(); 

    // -- WARNING LIGHTS LOGIC --
    // Check Engine Light (Basic On/Off)
    if (values[1] == 1) digitalWrite(ENGINE_PIN, HIGH); 
    else digitalWrite(ENGINE_PIN, LOW); 

    // Shift Light (Fading PWM based on status 0, 1, or 2)
    int targetShiftPWM = 0;
    if (values[0] == 1) targetShiftPWM = 20;       // Dim warning
    else if (values[0] == 2) targetShiftPWM = 255; // Full brightness flash

    const int SHIFT_FADE_SPEED = 2; 
    if (currentShiftPWM < targetShiftPWM) {
        currentShiftPWM += SHIFT_FADE_SPEED;
        if (currentShiftPWM > targetShiftPWM) currentShiftPWM = targetShiftPWM; 
    } else if (currentShiftPWM > targetShiftPWM) {
        currentShiftPWM -= (SHIFT_FADE_SPEED * 3); // Fades out faster than it fades in
        if (currentShiftPWM < targetShiftPWM) currentShiftPWM = targetShiftPWM; 
    }
    analogWrite(SHIFT_PIN, currentShiftPWM);

    // -- LCD SCREEN UPDATES --
    // Only update the screen if the string has actually changed to prevent flickering
    if (strcmp(lcdLine1, lastLine1) != 0) {
      lcd.setCursor(0, 0); 
      lcd.print(lcdLine1); 
      strcpy(lastLine1, lcdLine1); 
    }
    if (strcmp(lcdLine2, lastLine2) != 0) {
      lcd.setCursor(0, 1); 
      lcd.print(lcdLine2); 
      strcpy(lastLine2, lcdLine2); 
    } 
  }
}
