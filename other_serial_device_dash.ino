/* ====================================================================================
 *  SIM RACING GAUGE CLUSTER CONTROLLER (Arduino Nano)
 *  Takes comma-separated telemetry data from a PC (e.g., SimHub) and drives real 
 *  car dashboard gauges using PWM and Direct Digital Synthesis (DDS) via hardware interrupts.
 *  
 *  Data Format Expected (6 integers + 2 strings): 
 *  ShiftLight, EngineLight, Speed, RPM, Temp, Fuel, LCD_Line1, LCD_Line2\n
 * ==================================================================================== */

#include <LiquidCrystal_I2C.h> // Library for the I2C LCD display
#include <Wire.h>              // Standard Arduino I2C/TWI library
#include <TimerOne.h>          // Hardware timer library used to generate clean gauge frequencies

// ==========================================
// --- HARDWARE PIN CONFIGURATION ---
// ==========================================
// Note: Pins 3, 5, 6, 9, 10, 11 on the Nano support native hardware PWM (analogWrite).
#define shiftLight 3  // Shift Light (Uses Native PWM for smooth fading)
#define engineLight 4 // Check Engine Light (Basic ON/OFF digital pin)
#define TACH 2        // Tachometer/RPM gauge (Driven by Timer1 Interrupt for clean sound waves)
#define TEMP 5        // Coolant Temp gauge (Uses Native PWM to output variable voltage)
#define SP 11         // Speedometer gauge (Driven by Timer1 Interrupt to prevent thrashing)
#define FUEL 6        // Fuel Level gauge (Uses Native PWM to output variable voltage)
#define FIVEV 12      // Extra 5V output pin (Can be used to power small 5V accessories)

// ==========================================
// --- SERIAL PARSING VARIABLES ---
// ==========================================
const byte maxChars = 80;        // Maximum length of incoming data string
char receivedChars[maxChars];    // Array to hold incoming serial data
boolean newData = false;         // Flag to tell the loop when a full packet is ready

// Initialize the LCD (I2C address 0x27, 16 columns, 2 rows)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==========================================
// --- TIMING VARIABLES ---
// ==========================================
unsigned long lastProcessTime = 0;
// We run the hardware updates at 120Hz (120 times a second) for buttery smooth needles.
// 1,000,000 microseconds / 120 Hz = ~8333 microseconds per loop.
const unsigned long intervalMicros = 8333; 

// Variable to track the current brightness of the shift light during fades
int currentShiftPWM = 0; 

// Array to hold the parsed telemetry data: [Shift, EngLight, Speed, RPM, Temp, Fuel]
int values[6] = {0, 0, 0, 0, 0, 0}; 

// LCD screen buffers (Pre-filled with 16 spaces to clear the screen efficiently)
char lcdLine1[17] = "                "; 
char lcdLine2[17] = "                "; 
char lastLine1[17] = ""; // Remembers what was printed last so we only update when necessary
char lastLine2[17] = "";

// ==========================================
// --- ISR (INTERRUPT) FREQUENCY VARIABLES ---
// ==========================================
// These 'volatile' variables are shared between the main loop and the background interrupt.
// They track the on/off states of the pins generating square waves for the air core motors.
volatile unsigned int tachPeriodTicks = 0;
volatile unsigned int speedoPeriodTicks = 0;
volatile unsigned int tachTickCounter = 0;
volatile unsigned int speedoTickCounter = 0;
volatile bool tachState = false;
volatile bool speedoState = false;

/* 
 *  BACKGROUND TASK: toneISR()
 *  This runs automatically every 50 microseconds. It counts "ticks". 
 *  When enough ticks pass, it flips the pin state (HIGH to LOW or LOW to HIGH).
 *  This creates a perfectly stable square wave tone without blocking the main code.
 */
void toneISR() {
  // Process Tachometer Wave
  if (tachPeriodTicks > 0) {
    tachTickCounter++;
    if (tachTickCounter >= tachPeriodTicks) {
      tachTickCounter = 0;
      tachState = !tachState; // Flip the state
      digitalWrite(TACH, tachState);
    }
  }

  // Process Speedometer Wave
  if (speedoPeriodTicks > 0) {
    speedoTickCounter++;
    if (speedoTickCounter >= speedoPeriodTicks) {
      speedoTickCounter = 0;
      speedoState = !speedoState; // Flip the state
      digitalWrite(SP, speedoState);
    }
  }
}

// ==========================================
// --- GAUGE CALIBRATION TABLES ---
// ==========================================
// These arrays map incoming game data (e.g., MPH or RPM) to physical output values
// (PWM duty cycle 0-255, or Frequency in Hz). 
// You can add more points to make the gauge mapping non-linear if the physical needle isn't linear.

// Temperature Gauge Calibration (PWM 0-255)
const int TEMP_POINTS = 3;
const int tempSteps[] = {0, 80, 140};    // Incoming Game Data (e.g., Celsius)
const int tempPWMVal[] = {0, 170, 220};  // Output Voltage (0 = 0V, 255 = 5V)

// Fuel Gauge Calibration (PWM 0-255)
const int FUEL_POINTS = 4;
const int fuelSteps[] = {0, 15, 50, 100};   // Incoming Game Data (Fuel %)
const int fuelPWMVal[] = {0, 130, 161, 255};// Output Voltage

// Tachometer Calibration (Frequency in Hz)
const int TACH_POINTS = 14;
const int rpmSteps[] = {0,1000,2000,3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,13000}; // Game RPM
const int tachHz[] = {0,38,73,105,135,166,197,226,258,289,320,352,386,434};                   // Required Hz for the motor

// Speedometer Calibration (Frequency in Hz)
const int SPEED_POINTS = 8;
const int speedSteps[] = {0,16,40,80,100,120,140,180}; // Game Speed
const int speedHz[] = {0,31,88,178,222,265,300,384};   // Required Hz for the motor

/*
 * HELPER: calcHz (Linear Interpolation)
 * Takes a raw game value and looks it up in the calibration arrays above. 
 * If the value falls between two points, it does the math to find the exact output needed.
 */
int calcHz(int currentVal, const int stepsArray[], const int hzArray[], int totalPoints) {
  // Cap at absolute minimum or maximum
  if (currentVal <= stepsArray[0]) return hzArray[0];
  if (currentVal >= stepsArray[totalPoints - 1]) return hzArray[totalPoints - 1];

  // Find where the value falls in the array and interpolate
  for (int i = 0; i < totalPoints - 1; i++) {
    if (currentVal >= stepsArray[i] && currentVal <= stepsArray[i+1]) {
      // Temporarily cast to 'long' during the math step to prevent integer overflow
      long gapValue = (long)hzArray[i] + ((long)(currentVal - stepsArray[i]) * (hzArray[i+1] - hzArray[i])) / (stepsArray[i+1] - stepsArray[i]);
      return (int)gapValue;
    }
  }
  return 0;
}

/*
 * HELPER: recvWithEndMarker
 * Reads incoming serial data one character at a time without blocking the rest of the code.
 * It stops and sets the 'newData' flag when it sees the '\n' (newline) character.
 */
void recvWithEndMarker() {
    static byte ndx = 0;
    char endMarker = '\n';
    char rc;
   
    while (Serial.available() > 0 && newData == false) {
        rc = Serial.read();
        if (rc != endMarker) {
            if (rc != '\r' && ndx < maxChars - 1) { // Ignore carriage returns
                receivedChars[ndx] = rc;
                ndx++;
            }
        } else {
            receivedChars[ndx] = '\0'; // Terminate the string
            ndx = 0;
            newData = true;
        }
    }
}

/*
 * HELPER: extractAndPadString
 * Ensures LCD text is always exactly 16 characters long by padding empty space.
 * This prevents leftover characters from previous screens getting stuck on the display.
 */
void extractAndPadString(char* source, char* destination) {
    if (source != NULL) {
        int len = strlen(source);
        for (int i = 0; i < 16; i++) {
            if (i < len) destination[i] = source[i];
            else destination[i] = ' '; // Pad with space if source is shorter than 16
        }
        destination[16] = '\0';
    } else {
        // If no data, fill entirely with spaces
        for (int i = 0; i < 16; i++) destination[i] = ' ';
        destination[16] = '\0';
    }
}

/*
 * HELPER: parseCommaData
 * Chops up the incoming string separated by commas (e.g. "0,1,120,4500,...")
 * and assigns them to the values[] array.
 */
void parseCommaData() {
    char * strtokIndx; 
    byte index = 0;
    int tempValues[6]; // Hold data temporarily to ensure the packet isn't broken

    strtokIndx = strtok(receivedChars, ","); 
    while (strtokIndx != NULL && index < 6) {
        tempValues[index] = atoi(strtokIndx); // Convert text to integer
        index++;
        strtokIndx = strtok(NULL, ","); 
    }

    // Only update the main array if we got exactly 6 numbers (protects against cut-off packets)
    if (index == 6) {
        for (int i = 0; i < 6; i++) {
            values[i] = tempValues[i];
        }
        
        // Extract LCD text lines
        extractAndPadString(strtokIndx, lcdLine1);
        if (strtokIndx != NULL) strtokIndx = strtok(NULL, ","); 
        
        extractAndPadString(strtokIndx, lcdLine2);
    }
}

// ==========================================
// --- SYSTEM SETUP (Runs once on boot) ---
// ==========================================
void setup() {
  Serial.begin(115200); // Must match the baud rate set in your PC software (e.g., SimHub)
  
  // Configure output pins
  pinMode(FIVEV, OUTPUT);
  digitalWrite(FIVEV, HIGH); // Turn on the extra 5V pin permanently
  
  pinMode(shiftLight, OUTPUT);
  pinMode(engineLight, OUTPUT);
  digitalWrite(shiftLight, LOW);
  digitalWrite(engineLight, LOW);

  pinMode(TACH, OUTPUT);
  pinMode(SP, OUTPUT);
  digitalWrite(TACH, LOW);
  digitalWrite(SP, LOW);
  
  pinMode(TEMP, OUTPUT);
  pinMode(FUEL, OUTPUT);
  digitalWrite(TEMP, LOW);
  digitalWrite(FUEL, LOW);
    
  // Initialize the Hardware Interrupt for the Tone Generator
  // 50 microseconds provides a very high resolution 20kHz base clock for our tones
  Timer1.initialize(50);
  Timer1.attachInterrupt(toneISR); 

  // Initialize I2C display and boost speed
  Wire.begin();
  Wire.setClock(400000); // Overclock I2C to 400kHz (Stops the LCD from lagging the gauges)
  
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0,0);
  lcd.print("System Loading..");
}

// ==========================================
// --- MAIN PROCESSING LOOP ---
// ==========================================
void loop() {
  
  // 1. CONSTANTLY LISTEN TO SERIAL PORT
  recvWithEndMarker();
  if (newData == true) {
    parseCommaData();
    newData = false;
  }

  // 2. HARDWARE UPDATE TICK (Runs exactly 120 times per second)
  unsigned long nowMicros = micros();
  if (nowMicros - lastProcessTime >= intervalMicros) {
    lastProcessTime = nowMicros; 
                
    // ----------------------------------------------------
    // SPEEDOMETER LOGIC (With Glitch & Laziness Protection)
    // ----------------------------------------------------
    int rawSpeedHz = calcHz(values[2], speedSteps, speedHz, SPEED_POINTS);
    static int currentSpeedHz = 0;
    static int zeroFrameCounter = 0;
    
    // TELEMETRY GLITCH SHIELD: 
    // If the game suddenly sends a '0' speed while we are moving, freeze the needle.
    // If it stays '0' for more than 12 frames (~100ms), assume we actually crashed/stopped and let it drop.
    if (rawSpeedHz == 0 && currentSpeedHz > 15) {
      zeroFrameCounter++;
      if (zeroFrameCounter < 12) { 
        rawSpeedHz = currentSpeedHz; // Ignore the zero, use previous value
      }
    } else {
      zeroFrameCounter = 0; 
    }

    // TUNE THIS: ASYMMETRICAL SLEW RATES (Needle inertia)
    const int SPEED_ACCEL_RATE = 5; // How fast the needle swings UP (Higher = Faster)
    const int SPEED_DECEL_RATE = 3; // How fast the needle falls DOWN (Lower = Lazier)

    // Apply the slew rates to smoothly move the physical needle toward the target speed
    if (rawSpeedHz > currentSpeedHz + SPEED_ACCEL_RATE) {
      currentSpeedHz += SPEED_ACCEL_RATE; 
    } else if (rawSpeedHz < currentSpeedHz - SPEED_DECEL_RATE) {
      currentSpeedHz -= SPEED_DECEL_RATE; 
    } else {
      currentSpeedHz = rawSpeedHz; 
    }

    // ----------------------------------------------------
    // TACHOMETER LOGIC (Instant response)
    // ----------------------------------------------------
    int targetTachHz = calcHz(values[3], rpmSteps, tachHz, TACH_POINTS);

    // ----------------------------------------------------
    // ANALOG GAUGES LOGIC (Temp & Fuel)
    // ----------------------------------------------------
    int targetTempPWM = calcHz(values[4], tempSteps, tempPWMVal, TEMP_POINTS);
    int targetFuelPWM = calcHz(values[5], fuelSteps, fuelPWMVal, FUEL_POINTS);

    // Output mapped values as physical voltages via native PWM
    if (targetFuelPWM > 0) { analogWrite(FUEL, targetFuelPWM); } else { analogWrite(FUEL, 0); }
    if (targetTempPWM > 0) { analogWrite(TEMP, targetTempPWM); } else { analogWrite(TEMP, 0); }
    
    // ----------------------------------------------------
    // PUSH FREQUENCIES TO THE BACKGROUND INTERRUPT (ISR)
    // ----------------------------------------------------
    noInterrupts(); // Briefly pause the background task while we update its variables safely

    // Math: (1,000,000 microseconds / 2 for half-cycle) / 50us ISR tick interval = 10,000 magic number
    if (targetTachHz > 15) {
      tachPeriodTicks = 10000 / targetTachHz; 
    } else {
      tachPeriodTicks = 0; // Turn off tone
      tachState = false;
      digitalWrite(TACH, LOW);
    }
    
    if (currentSpeedHz > 15) {
      speedoPeriodTicks = 10000 / currentSpeedHz;
    } else {
      speedoPeriodTicks = 0; // Turn off tone
      speedoState = false;
      digitalWrite(SP, LOW);
    }
    interrupts(); // Resume background task

    // ----------------------------------------------------
    // INDICATOR LIGHTS LOGIC
    // ----------------------------------------------------
    // Engine Light (Basic ON/OFF)
    if ( values[1] == 0) { digitalWrite(engineLight,HIGH); } 
    else { digitalWrite(engineLight,LOW); }

    // Shift Light (3-stage Fading System)
    int targetShiftPWM = 0;
    if ( values[0] == 1) { targetShiftPWM = 20; }       // Stage 1: Dim
    else if (values[0] == 2) { targetShiftPWM = 255; }  // Stage 2: Max Brightness
    else { targetShiftPWM = 0; }                        // Stage 0: Off

    // TUNE THIS: SHIFT LIGHT FADE SPEED
    const int SHIFT_FADE_SPEED = 2; // How fast the light transitions (Higher = snappier, Lower = lazier)

    // Smoothly walk the current brightness up or down toward the target
    if (currentShiftPWM < targetShiftPWM) {
        currentShiftPWM += SHIFT_FADE_SPEED;
        if (currentShiftPWM > targetShiftPWM) currentShiftPWM = targetShiftPWM; // Don't overshoot
    } else if (currentShiftPWM > targetShiftPWM) {
        currentShiftPWM -= (SHIFT_FADE_SPEED * 3); // Light turns OFF 3x faster than it turns ON
        if (currentShiftPWM < targetShiftPWM) currentShiftPWM = targetShiftPWM; // Don't overshoot
    }
    
    // Output the smoothed brightness to the LED
    analogWrite(shiftLight, currentShiftPWM);

    // ----------------------------------------------------
    // LCD SCREEN UPDATES
    // ----------------------------------------------------
    // Only send data to the screen if the string has actually changed.
    // (Writing to screens is slow, so we avoid doing it if we don't have to).
    if (strcmp(lcdLine1, lastLine1) != 0) {
      lcd.setCursor(0, 0); // Column 0, Row 0
      lcd.print(lcdLine1); 
      strcpy(lastLine1, lcdLine1); // Remember what we just printed
    }

    if (strcmp(lcdLine2, lastLine2) != 0) {
      lcd.setCursor(0, 1); // Column 0, Row 1
      lcd.print(lcdLine2); 
      strcpy(lastLine2, lcdLine2); // Remember what we just printed
    } 
  }
}