#pragma once
#include <Arduino.h>

// Плата/распайка выбирается флагом сборки (-DBOARD_T_CALL=1 для TTGO T-Call).
// По умолчанию: кастомная плата ESP32-C3 + SIM800 (лог через нативный USB-CDC).
#if defined(BOARD_T_CALL)
  // TTGO T-Call v1.3 / v1.4: классический ESP32 + SIM800L/H, UART модема GPIO26/27,
  // PWRKEY -> GPIO4 (импульс LOW до 1.2c включает модем).
  #define SIM800_RX_PIN   26     // ESP32 RX  -> SIM800 TX
  #define SIM800_TX_PIN   27     // ESP32 TX  -> SIM800 RX
  #define SIM800_PWR_PIN  4      // >=0: пин импульса PWRKEY
  #define SIM_SERIAL      Serial2
#else
  // ESP32-C3 + SIM800 (лог через нативный USB-CDC), модем на Serial1 GP4/GP5.
  #define SIM800_RX_PIN   4      // ESP32 RX  -> SIM800 TX
  #define SIM800_TX_PIN   5      // ESP32 TX  -> SIM800 RX
  #define SIM800_PWR_PIN -1      // >=0: пин импульса PWRKEY
  #define SIM_SERIAL      Serial1
#endif