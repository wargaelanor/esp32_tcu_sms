#pragma once
#include <Arduino.h>

// Board/pinout is selected by the build flag (-DBOARD_T_CALL=1 for TTGO T-Call).
// Default: custom ESP32-C3 + SIM800 board (logging via native USB-CDC).
#if defined(BOARD_T_CALL)
  // TTGO T-Call v1.3 / v1.4: classic ESP32 + SIM800L/H, modem UART GPIO26/27,
  // PWRKEY -> GPIO4 (a LOW pulse up to 1.2s powers the modem on),
  // RST -> GPIO5 (must be HIGH, otherwise the modem is held in reset),
  // POWER_ON -> GPIO23 (must be HIGH, otherwise the modem rail is unpowered).
  #define SIM800_RX_PIN    26     // ESP32 RX  -> SIM800 TX
  #define SIM800_TX_PIN    27     // ESP32 TX  -> SIM800 RX
  #define SIM800_PWR_PIN   4      // >=0: PWRKEY pulse pin
  #define SIM800_RST_PIN   5      // >=0: modem reset (drive HIGH)
  #define SIM800_POWER_PIN 23     // >=0: modem power rail enable (drive HIGH)
  #define SIM_SERIAL       Serial2
#else
  // ESP32-C3 + SIM800 (logging via native USB-CDC), modem on Serial1 GP4/GP5.
  #define SIM800_RX_PIN    4      // ESP32 RX  -> SIM800 TX
  #define SIM800_TX_PIN    5      // ESP32 TX  -> SIM800 RX
  #define SIM800_PWR_PIN  -1      // >=0: PWRKEY pulse pin
  #define SIM800_RST_PIN  -1      // >=0: modem reset (drive HIGH)
  #define SIM800_POWER_PIN -1     // >=0: modem power rail enable (drive HIGH)
  #define SIM_SERIAL       Serial1
#endif