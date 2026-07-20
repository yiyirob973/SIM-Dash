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
const byte maxChars = 80;        
char receivedChars[maxChars];    
char tempChars[maxChars];        // OPTIMIZATION: Temporary buffer for safe parsing
boolean newData = false;         

// ==========================================
// --- TIMEOUT FAILSAFE VARIABLES ---
// ==========================================
unsigned long lastValidDataTime = 0;
const unsigned long timeoutMillis = 2000; // 2 seconds before gauges zero out
bool isHardwareStandby = false;

// Initialize the LCD (I2C address 0x27, 16 columns, 2 rows)
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ==========================================
// --- TIMING VARIABLES ---
// ==========================================
unsigned long lastProcessTime = 0;
const unsigned long intervalMicros = 8333; // 120Hz refresh rate

int currentShiftPWM = 0; 

// Array to hold the parsed telemetry data: [Shift, EngLight, Speed, RPM, Temp, Fuel]
int values[6] = {0, 0, 0, 0, 0, 0}; 

// LCD screen buffers
char lcdLine1[17] = "                "; 
char lcdLine2[17] = "                "; 
char lastLine1[17] = ""; 
char lastLine2[17] = "";

// ==========================================
// --- ISR (INTERRUPT) FREQUENCY VARIABLES ---
// ==========================================
volatile unsigned int tachPeriodTicks = 0;
volatile unsigned int speedoPeriodTicks = 0;
volatile unsigned int tachTickCounter = 0;
volatile unsigned int speedoTickCounter = 0;
volatile bool tachState = false;
volatile bool speedoState = false;

// OPTIMIZATION: Direct Port Manipulation for maximum CPU efficiency
void toneISR() {
  if (tachPeriodTicks > 0) {
    tachTickCounter++;
    if (tachTickCounter >= tachPeriodTicks) {
      tachTickCounter = 0;
      tachState = !tachState; 
      
      // Direct Port Manipulation for Pin 2 (TACH) on Port D, Bit 2
      if (tachState) { PORTD |= B00000100; }  
      else           { PORTD &= ~B00000100; } 
    }
  }

  if (speedoPeriodTicks > 0) {
    speedoTickCounter++;
    if (speedoTickCounter >= speedoPeriodTicks) {
      speedoTickCounter = 0;
      speedoState = !speedoState; 
      
      // Direct Port Manipulation for Pin 11 (SP) on Port B, Bit 3
      if (speedoState) { PORTB |= B00001000; }  
      else             { PORTB &= ~B00001000; } 
    }
  }
}

// ==========================================
// --- GAUGE CALIBRATION TABLES ---
// ==========================================
const int TEMP_POINTS = 3;
const int tempSteps[] = {0, 80, 140};    
const int tempPWMVal[] = {0, 170, 220};  

const int FUEL_POINTS = 4;
const int fuelSteps[] = {0, 15, 50, 100};   
const int fuelPWMVal[] = {0, 130, 161, 255};

const int TACH_POINTS = 14;
const int rpmSteps[] = {0,1000,2000,3000,4000,5000,6000,7000,8000,9000,10000,11000,12000,13000}; 
const int tachHz[] = {0,38,73,105,135,166,197,226,258,289,320,352,386,434};                   

const int SPEED_POINTS = 8;
const int speedSteps[] = {0,16,40,80,100,120,140,180}; 
const int speedHz[] = {0,31,88,178,222,265,300,384};   

int calcHz(int currentVal, const int stepsArray[], const int hzArray[], int totalPoints) {
  if (currentVal <= stepsArray[0]) return hzArray[0];
  if (currentVal >= stepsArray[totalPoints - 1]) return hzArray[totalPoints - 1];

  for (int i = 0; i < totalPoints - 1; i++) {
    if (currentVal >= stepsArray[i] && currentVal <= stepsArray[i+1]) {
      long gapValue = (long)hzArray[i] + ((long)(currentVal - stepsArray[i]) * (hzArray[i+1] - hzArray[i])) / (stepsArray[i+1] - stepsArray[i]);
      return (int)gapValue;
    }
  }
  return 0;
}

// ==========================================
// --- NON-BLOCKING SERIAL READ ---
// ==========================================
void recvWithEndMarker() {
    static byte ndx = 0;
    char endMarker = '\n';
    char rc;
   
    while (Serial.available() > 0 && newData == false) {
        rc = Serial.read();
        if (rc != endMarker) {
            if (rc != '\r' && ndx < maxChars - 1) { 
                receivedChars[ndx] = rc;
                ndx++;
            }
        } else {
            receivedChars[ndx] = '\0'; 
            ndx = 0;
            newData = true;
        }
    }
}

void extractAndPadString(char* source, char* destination) {
    if (source != NULL) {
        int len = strlen(source);
        for (int i = 0; i < 16; i++) {
            if (i < len) destination[i] = source[i];
            else destination[i] = ' '; 
        }
        destination[16] = '\0';
    } else {
        for (int i = 0; i < 16; i++) destination[i] = ' ';
        destination[16] = '\0';
    }
}

// ==========================================
// --- OPTIMIZED NULL-SAFE PARSER ---
// ==========================================
bool parseCommaData() {
    char * strtokIndx; 
    int tempValues[6];

    // Read the 6 integers first
    strtokIndx = strtok(tempChars, ","); 
    for (int i = 0; i < 6; i++) {
        if (strtokIndx == NULL) return false; // Packet cut off prematurely
        tempValues[i] = atoi(strtokIndx); 
        strtokIndx = strtok(NULL, ","); 
    }

    // Read LCD Line 1
    if (strtokIndx == NULL) return false;
    extractAndPadString(strtokIndx, lcdLine1);
    
    // Read LCD Line 2 (stops at newline)
    strtokIndx = strtok(NULL, "\n"); 
    if (strtokIndx == NULL) return false;
    extractAndPadString(strtokIndx, lcdLine2);

    // If we reach here, the packet is 100% valid. Commit to main arrays.
    for (int i = 0; i < 6; i++) {
        values[i] = tempValues[i];
    }
    
    return true;
}

// ==========================================
// --- FAILSAFE TRIGGER ---
// ==========================================
void triggerFailsafe() {
    for (int i = 0; i < 6; i++) {
        values[i] = 0; // Drop all needles and lights to 0
    }
    strcpy(lcdLine1, "  SIMHUB READY  ");
    strcpy(lcdLine2, " WAITING FOR PC ");
}

// ==========================================
// --- SYSTEM SETUP ---
// ==========================================
void setup() {
  Serial.begin(115200); 
  
  pinMode(FIVEV, OUTPUT);
  digitalWrite(FIVEV, HIGH); 
  
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
    
  Timer1.initialize(50);
  Timer1.attachInterrupt(toneISR); 

  Wire.begin();
  
  lcd.init();
  Wire.setClock(400000); // FIXED: Overclock AFTER init so it isn't overwritten
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
    strcpy(tempChars, receivedChars); // Copy to temp buffer to protect original
    
    // Only update timers and flags if parsing was successful
    if (parseCommaData()) {
        lastValidDataTime = millis();
        isHardwareStandby = false;
    }
    newData = false;
  }

  // 2. TIMEOUT FAILSAFE CHECK
  if (!isHardwareStandby && (millis() - lastValidDataTime > timeoutMillis)) {
      triggerFailsafe();
      isHardwareStandby = true;
  }

  // 3. HARDWARE UPDATE TICK
  unsigned long nowMicros = micros();
  if (nowMicros - lastProcessTime >= intervalMicros) {
    lastProcessTime = nowMicros; 
                
    // ----------------------------------------------------
    // SPEEDOMETER LOGIC (With Glitch & Laziness Protection)
    // ----------------------------------------------------
    int rawSpeedHz = calcHz(values[2], speedSteps, speedHz, SPEED_POINTS);
    static int currentSpeedHz = 0;
    static int zeroFrameCounter = 0;
    
    if (rawSpeedHz == 0 && currentSpeedHz > 15) {
      zeroFrameCounter++;
      if (zeroFrameCounter < 12) { 
        rawSpeedHz = currentSpeedHz; 
      }
    } else {
      zeroFrameCounter = 0; 
    }

    const int SPEED_ACCEL_RATE = 5; 
    const int SPEED_DECEL_RATE = 3; 

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

    if (targetFuelPWM > 0) { analogWrite(FUEL, targetFuelPWM); } else { analogWrite(FUEL, 0); }
    if (targetTempPWM > 0) { analogWrite(TEMP, targetTempPWM); } else { analogWrite(TEMP, 0); }
    
    // ----------------------------------------------------
    // PUSH FREQUENCIES TO THE BACKGROUND INTERRUPT (ISR)
    // ----------------------------------------------------
    noInterrupts(); 

    if (targetTachHz > 15) {
      tachPeriodTicks = 10000 / targetTachHz; 
    } else {
      tachPeriodTicks = 0; 
      tachState = false;
      digitalWrite(TACH, LOW);
    }
    
    if (currentSpeedHz > 15) {
      speedoPeriodTicks = 10000 / currentSpeedHz;
    } else {
      speedoPeriodTicks = 0; 
      speedoState = false;
      digitalWrite(SP, LOW);
    }
    interrupts(); 

    // ----------------------------------------------------
    // INDICATOR LIGHTS LOGIC
    // ----------------------------------------------------
    if ( values[1] == 1) { digitalWrite(engineLight,HIGH); } // FIXED: Engine Light active state corrected
    else { digitalWrite(engineLight,LOW); }

    int targetShiftPWM = 0;
    if ( values[0] == 1) { targetShiftPWM = 20; }       
    else if (values[0] == 2) { targetShiftPWM = 255; }  
    else { targetShiftPWM = 0; }                        

    const int SHIFT_FADE_SPEED = 2; 

    if (currentShiftPWM < targetShiftPWM) {
        currentShiftPWM += SHIFT_FADE_SPEED;
        if (currentShiftPWM > targetShiftPWM) currentShiftPWM = targetShiftPWM; 
    } else if (currentShiftPWM > targetShiftPWM) {
        currentShiftPWM -= (SHIFT_FADE_SPEED * 3); 
        if (currentShiftPWM < targetShiftPWM) currentShiftPWM = targetShiftPWM; 
    }
    
    analogWrite(shiftLight, currentShiftPWM);

    // ----------------------------------------------------
    // LCD SCREEN UPDATES
    // ----------------------------------------------------
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
