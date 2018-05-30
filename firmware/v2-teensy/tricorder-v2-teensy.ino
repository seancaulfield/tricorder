/*
 * tricorder-v2-teensy.ino
 *
 * Teensy 3.2 based sensor thingy. Version 2: better, strong, now with more
 * calcium!
 *
 * Author: Sean Caulfield <sean@yak.net>
 * License: GPL v2.0
 * Hardware Revsion: Mk 1.7 Featherwing with Adafruit Teensy adapter
 *
 */

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <ssd1351.h>
#include <Adafruit_MLX90614.h>
#include <ClosedCube_HDC1080.h>
#include <VEML6075.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// Pin config
// Ugh. To use the optimized ssd1351 code, the DC and CS pins need to be
// hardware CS pins on separate masks. Thus my use of the analog pins here
// won't fly. Damnit.

// Alternate options, from reading SPI.pinIsChipSelect(uint8_t, uint8_t):
//
// mask cs1 cs2 cs3
// 0x01 10  2   26
// 0x02 9   6
// 0x04 20  23
// 0x08 21  22
// 0x10 15
// 0x20 45

#define DISP_DC      (15)
#define DISP_RST     (14)
#define DISP_CS      (10)
#define SD_CS        (16)
#define SD_CARDSW    (17)
#define BATT_DIV     (21)

// 16-bit color is weird: 5 bits for R & B, but 6 for G? I guess humans are
// more sensitive to it...

// How many bits of color ya want?
//typedef ssd1351::HighColor Color;    // 18 (6/chan)
typedef ssd1351::LowColor Color;     // 16 (5/6/5)
//typedef ssd1351::IndexedColor Color; // 8

// Enable or disable double buffer
//typedef ssd1351::NoBuffer Buffer;
typedef ssd1351::SingleBuffer Buffer;

// Display dimensions
#define DISP_H (128)
#define DISP_W (128)

// Define some color consts

#define COLOR_RED   (0xF800) // 0b1111100000000000 (5)
#define COLOR_GREEN (0x07E0) // 0b0000011111100000 (6)
#define COLOR_BLUE  (0x001F) // 0b0000000000011111 (5)

// Composite colors

#define COLOR_BLACK    (0x0000) // 0b0000000000000000 (  0%,   0%,   0%)
#define COLOR_BROWN    (0xA145) // 0b1010000101000101 ( 64%,  16%,  16%)
#define COLOR_MAGENTA  (0xF81F) // 0b1111100000011111 (100%,   0%, 100%)
#define COLOR_ORANGE   (0xFD20) // 0b1111110100100000 (100%,  64%,   0%)
#define COLOR_YELLOW   (0xFFE0) // 0b1111111111100000 (100%, 100%,   0%)
#define COLOR_CYAN     (0x07FF) // 0b0000011111111111 (  0%, 100%, 100%)
#define COLOR_PURPLE   (0xA11E) // 0b1010000100011110 ( 62%,  12%,  94%)
#define COLOR_LAVENDER (0XE73F) // 0b1110011100111111 ( 90%,  90%,  98%)
#define COLOR_WHITE    (0xFFFF) // 0b1111111111111111 (100%, 100%, 100%)

// "Special" characters
//#define STR_DEGREE  "\367"
//#define STR_SQUARED "\374"
#define DEG  "\370" // 248 or 0xF8
#define STR_SQUARED "\375" // 253 or 0xFD

// Misc
#define SD_FILENAME ("tricorder.log")
#define ANALOG_RES (10)
#define I2C_ADDR_HDC1080 0x40

// MicroSD card log file
File logfile;
bool card_present = false;

// Display controller
auto display = ssd1351::SSD1351<Color,Buffer,DISP_H,DISP_W>(DISP_CS, DISP_DC, DISP_RST);

// Sensors
VEML6075           veml6075  = VEML6075();
Adafruit_MLX90614  mlx90614  = Adafruit_MLX90614();
ClosedCube_HDC1080 hdc1080   = ClosedCube_HDC1080();

// Sensor values, current and last different value. The later is used to forego
// screen updates (which are unfortunately slow) if the value has not changed.

typedef struct sensval {
  float ir = -1.0;
  float vis = -1.0;
  float uva = -1.0;
  float uvb = -1.0;
  float uvi = -1.0;
  uint16_t raw_uva = 0;
  uint16_t raw_uvb = 0;
  uint16_t raw_dark = 0;
  uint16_t raw_vis_comp = 0;
  uint16_t raw_ir_comp = 0;
  float temp = -1.0;
  float temp_f = -1.0;
  float rh = -1.0;
  float color_temp = -1.0;
  float batt_v = -1.0;
  float press_abs = -1.0;
  float press_rel = -1.0;
  float alt = -1.0;
  float irtemp = -1.0;
  float irtemp_f = -1.0;
  float color_r = -1.0;
  float color_g = -1.0;
  float color_b = -1.0;
  float acc_x = -1.0;
  float acc_y = -1.0;
  float acc_z = -1.0;
  float mag_x = -1.0;
  float mag_y = -1.0;
  float mag_z = -1.0;
  float gyro_x = -1.0;
  float gyro_y = -1.0;
  float gyro_z = -1.0;

} sensval_t;

sensval_t curr, last;

//
// Helper macros
//

#define USEC_DIFF(x, y) \
  (((x) > (y)) ? ((x) - (y)) : ((x) + (ULONG_MAX - (y))))

#define DISPLAY_LABEL(color, x, y, val) \
  do { \
    display.setTextColor(color); \
    display.setCursor(x, y); \
    display.print(val); \
  } while (0)

#define DISPLAY_READING(color, x, y, prec, width, name) \
  do { \
    /*if (last.name != curr.name) { */ \
      last.name = curr.name; \
      display.setTextColor((color), COLOR_BLACK); \
      display.setCursor((x), (y)); \
      display.print(curr.name, prec); \
    /*}*/ \
  } while (0);

#define C_TO_F(x) ((x) * 9.0/5.0 + 32.0)

////////////////////////////////////////////////////////////////////////
// HALPING
////////////////////////////////////////////////////////////////////////

float getBattVoltage() {
  //float measured = analogRead(BATT_DIV);
  //return measured / ((float)(1 << ANALOG_RES)) * 2.0 * 3.3;
  return 0.0;
}

void displayLabels() {
  ////////////////////////////////////////////////////////////////////////////
  //           |color      | x|   y| label                             
  ////////////////////////////////////////////////////////////////////////////
  //DISPLAY_LABEL(COLOR_WHITE, 0,   0, F("Tricorder  Batt     V"));
  //DISPLAY_LABEL(COLOR_WHITE, 0,   8, F("   Tamb  Trem   R    "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  16, F(DEG "C              G    "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  24, F(DEG "F              B    "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  32, F("IR         UVA       "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  40, F("Vis        UVB       "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  48, F("lux        UVI       "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  56, F("Prel        kPa RH   "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  64, F("Pabs        kPa    % "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  72, F("    acc   mag  gyro  "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  80, F("x                    "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  88, F("y                    "));
  //DISPLAY_LABEL(COLOR_WHITE, 0,  96, F("z                    "));
  //DISPLAY_LABEL(COLOR_WHITE, 0, 104, F("lat         " DEG "     alt"));
  //DISPLAY_LABEL(COLOR_WHITE, 0, 112, F("lon         " DEG "       m"));
  DISPLAY_LABEL(COLOR_WHITE, 0,   8, F("Tricorder  Batt     V"));
  DISPLAY_LABEL(COLOR_WHITE, 0,  16, F("   Tamb  Trem   R    "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  24, F(DEG "C              G    "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  32, F(DEG "F              B    "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  40, F("IR         UVA       "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  48, F("Vis        UVB       "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  56, F("lux        UVI       "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  64, F("Prel        kPa RH   "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  72, F("Pabs        kPa    % "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  80, F("    acc   mag  gyro  "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  88, F("x                    "));
  DISPLAY_LABEL(COLOR_WHITE, 0,  96, F("y                    "));
  DISPLAY_LABEL(COLOR_WHITE, 0, 104, F("z                    "));
  DISPLAY_LABEL(COLOR_WHITE, 0, 112, F("lat         " DEG "     alt"));
  DISPLAY_LABEL(COLOR_WHITE, 0, 120, F("lon         " DEG "       m"));
}

void displayValues_veml6075() {
  // color, x, y, precision, value
  //DISPLAY_READING(COLOR_WHITE, 90, 32, 0, 5, uva);
  //DISPLAY_READING(COLOR_WHITE, 90, 40, 0, 5, uvb);
  //DISPLAY_READING(COLOR_WHITE, 90, 48, 5, 5, uvi);
  DISPLAY_READING(COLOR_WHITE, 90, 40, 0, 5, raw_uva);
  DISPLAY_READING(COLOR_WHITE, 90, 48, 0, 5, raw_uvb);
  DISPLAY_READING(COLOR_WHITE, 90, 56, 0, 5, raw_dark);
  DISPLAY_READING(COLOR_WHITE, 24, 40, 0, 5, raw_ir_comp);
  DISPLAY_READING(COLOR_WHITE, 24, 48, 0, 5, raw_vis_comp);
}

/*
void displayLabels() {
  DISPLAY_LABEL(COLOR_BROWN,      0,  16, F("  IR"));
  //DISPLAY_LABEL(COLOR_GREEN,     0,  24, F(" Vis"));
  DISPLAY_LABEL(COLOR_PURPLE,     0,  24, F(" UVA"));
  DISPLAY_LABEL(COLOR_PURPLE,     0,  32, F(" UVB"));
  DISPLAY_LABEL(COLOR_PURPLE,     0,  40, F(" UVI"));
  DISPLAY_LABEL(COLOR_WHITE,      0,  48, F("Temperature"));
  //DISPLAY_LABEL(COLOR_WHITE,      0,  48, F("Fa"));
  //DISPLAY_LABEL(COLOR_WHITE,      0,  56, F(" Amb"));
  DISPLAY_LABEL(COLOR_WHITE,      0,  64, F(" RH%"));
  //DISPLAY_LABEL(COLOR_WHITE,     0,  72, F("  CT"));
  DISPLAY_LABEL(COLOR_WHITE,      0,  80, F("Batt"));
  DISPLAY_LABEL(COLOR_WHITE,      0,  88, F("Pres"));
  DISPLAY_LABEL(COLOR_WHITE,      0,  96, F(" Alt"));
  DISPLAY_LABEL(COLOR_WHITE,      0, 104, F("Thrm"));

  DISPLAY_LABEL(COLOR_WHITE,     70,  16, F("Visible"));
  DISPLAY_LABEL(COLOR_WHITE,     70,  24, F("R"));
  DISPLAY_LABEL(COLOR_WHITE,     70,  32, F("G"));
  DISPLAY_LABEL(COLOR_WHITE,     70,  40, F("B"));
  //DISPLAY_LABEL(COLOR_WHITE,    70,  48, F("  CT"));
  //DISPLAY_LABEL(COLOR_WHITE,    70,  56, F(" Vis"));
}

void displayUnits() {
  //DISPLAY_LABEL(COLOR_WHITE, 60,  16, F("W/m" STR_SQUARED));
  //DISPLAY_LABEL(COLOR_WHITE, 60,  24, F("lux"));
  //DISPLAY_LABEL(COLOR_WHITE, 60,  32, F("W/m" STR_SQUARED));
  //DISPLAY_LABEL(COLOR_WHITE, 60,  40, F("W/m" STR_SQUARED));
  DISPLAY_LABEL(COLOR_WHITE, 60,  48, F(DEG "C IR"));
  DISPLAY_LABEL(COLOR_WHITE, 60,  56, F(DEG "C Amb"));
  //DISPLAY_LABEL(COLOR_WHITE, 60,  64, F("%"));
  DISPLAY_LABEL(COLOR_WHITE, 60,  72, F("K"));
  DISPLAY_LABEL(COLOR_WHITE, 60,  80, F("V"));
  DISPLAY_LABEL(COLOR_WHITE, 60,  88, F("kPa"));
  DISPLAY_LABEL(COLOR_WHITE, 60,  96, F("m"));
}
*/

void displayValues_mlx90614() {
  // color, x, y, precision, value
  DISPLAY_READING(COLOR_WHITE, 54, 16, 1, 6, irtemp);
  DISPLAY_READING(COLOR_WHITE, 54, 24, 1, 6, irtemp_f);
}

void displayValues_hdc1080() {
  // color, x, y, precision, value
  DISPLAY_READING(COLOR_WHITE, 18, 24, 1, 5, temp);
  DISPLAY_READING(COLOR_WHITE, 18, 32, 1, 5, temp_f);
  DISPLAY_READING(COLOR_WHITE, 96, 72, 0, 3, rh);
}

void displayValues_ms5611() {
  // color, x, y, precision, value
  DISPLAY_READING(COLOR_WHITE, 30, 64, 2, 6, press_rel);
  DISPLAY_READING(COLOR_WHITE, 30, 72, 2, 6, press_abs);
  DISPLAY_READING(COLOR_WHITE, 84, 120, 0, 5, alt);
}

void displayValues_lsm9ds0() {
  // color, x, y, precision, value
  DISPLAY_READING(COLOR_WHITE, 12,  88, 0, 6, acc_x);
  DISPLAY_READING(COLOR_WHITE, 12,  96, 0, 6, acc_y);
  DISPLAY_READING(COLOR_WHITE, 12, 104, 0, 6, acc_z);

  DISPLAY_READING(COLOR_WHITE, 48,  88, 0, 6, mag_x);
  DISPLAY_READING(COLOR_WHITE, 48,  96, 0, 6, mag_y);
  DISPLAY_READING(COLOR_WHITE, 48, 104, 0, 6, mag_z);

  DISPLAY_READING(COLOR_WHITE, 84,  88, 0, 6, gyro_x);
  DISPLAY_READING(COLOR_WHITE, 84,  96, 0, 6, gyro_y);
  DISPLAY_READING(COLOR_WHITE, 84, 104, 0, 6, gyro_z);
}

void displayValues_gps() {
  // color, x, y, precision, value
  //DISPLAY_READING(COLOR_WHITE, 96, 8, 2, 4, batt_v);
}

void displayValues_batt() {
  // color, x, y, precision, value
  //DISPLAY_READING(COLOR_WHITE, 96, 8, 2, 4, batt_v);
}

////////////////////////////////////////////////////////////////////////
// SETUP
////////////////////////////////////////////////////////////////////////

void setup() {

  delay(500);
  Serial.begin(115200);
  Serial.println(F("Tricorder v2.0-teensy (Built " __DATE__ " " __TIME__")"));

  // Setup ADC
  analogReadResolution(ANALOG_RES);

  // Init display
  delay(100);
  display.begin();
  display.sleep(false);
  display.fillScreen(COLOR_BLACK);
  //display.updateScreen();
  display.setTextSize(1);
  //display.cp437();
  //display.setFont(OGFont);
  //display.setFont(Picopixel);
  //display.setFont(NULL);
  displayLabels();
  display.updateScreen();
  Serial.println("display init complete");

  // Check if SD card is present (shorts to ground if not present)
  pinMode(SD_CARDSW, INPUT_PULLUP);
  if (digitalRead(SD_CARDSW) == HIGH) {
    if (SD.begin(SD_CS)) {
      logfile = SD.open(SD_FILENAME, FILE_WRITE);
      if (logfile) {
        card_present = true;
      }
    }
  }
  Serial.println("SD init complete");

  // Init i2C
  Wire.begin();

  // Init sensors
  veml6075.begin();
  mlx90614.begin(); 
  hdc1080.begin(I2C_ADDR_HDC1080);
  Serial.println("i2c init complete");

}

////////////////////////////////////////////////////////////////////////
// MAIN LOOP
////////////////////////////////////////////////////////////////////////

void loop() {

  //
  // Poll RTC for time (UTC)
  //
  //static DateTime ts;
  //static String now;
  //ts = rtc.now();
  //ts.iso8601(now);
  
  //
  // Sensor polling
  //

  /// veml6075.poll();
  /// curr.uva = veml6075.getUVA();
  /// curr.uvb = veml6075.getUVB();
  /// curr.uvi = veml6075.getUVIndex();
  /// curr.temp = hdc1080.readTemperature();
  /// curr.temp_f = C_TO_F(curr.temp);
  /// curr.rh = hdc1080.readHumidity();
  /// curr.irtemp = mlx90614.readObjectTempC();
  /// curr.irtemp_f = C_TO_F(curr.irtemp);
  /// curr.batt_v = getBattVoltage();

  /// curr.ir = 0.0;
  /// curr.vis = 0.0;
  /// curr.color_r = 0.0;
  /// curr.color_g = 0.0;
  /// curr.color_b = 0.0;
  /// curr.color_temp = 0.0;

  /// curr.press_abs = 0.0;
  /// curr.press_rel = 0.0;
  /// curr.alt = 0.0;

  /// curr.acc_x = 0.0;
  /// curr.acc_y = 0.0;
  /// curr.acc_z = 0.0;
  /// curr.mag_x = 0.0;
  /// curr.mag_y = 0.0;
  /// curr.mag_z = 0.0;
  /// curr.gyro_x = 0.0;
  /// curr.gyro_y = 0.0;
  /// curr.gyro_z = 0.0;

  /// // DEBUG
  /// curr.raw_uva = veml6075.getRawUVA();
  /// curr.raw_uvb = veml6075.getRawUVB();
  /// curr.raw_dark = veml6075.getRawDark();
  /// curr.raw_ir_comp = veml6075.getRawIRComp();
  /// curr.raw_vis_comp = veml6075.getRawVisComp();

  /// //
  /// // Display readings refresh
  /// //

  display.fillScreen(COLOR_BLACK);
  displayLabels();
  displayValues_batt();
  displayValues_veml6075();
  displayValues_mlx90614();
  displayValues_hdc1080(); 
  displayValues_ms5611();
  displayValues_lsm9ds0();
  displayValues_gps();
  display.updateScreen();

  // Delay between data polls
  delay(100);

}
