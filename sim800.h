#pragma once
#include <Arduino.h>

// Driver for SIM800/900 GSM module on a dedicated UART.
class Sim800 {
 public:
  // pwrPin: if >= 0, used to pulse PWRKEY on begin(). autoBoot: skip warning checks.
  void begin(HardwareSerial& ser, long baud, int8_t rxPin, int8_t txPin, int8_t pwrPin = -1);

  // === basics ===
  bool at();                         // "AT" -> OK
  int  csq();                        // 0..31, -1 = no answer / invalid
  bool creg();                       // true when registered (CREG? 0,1 / 0,5)
  void setSmsPduMode();              // AT+CMGF=0 (PDU mode, default)
  bool sendPdu(const char* hex, int len);
  // diagnostic: raw AT round-trip returning accumulated modem text
  String rawCmd(const char* cmd, uint32_t timeoutMs);  // AT+CMGS=<len> ... hex ... 0x1A -> +CMGS: OK
  void powerPulse();

  // === GPRS / data ===
  // Brings up PDP context. Returns true and fills ip on success.
  bool gprsUp(const String& apn, const String& user, const String& pass, String& ip);
  // ICMP ping via AT+CIPPING
  bool pingHost(const String& host, int* sent, int* rcvd, int* rttMs);
  // Open TCP in normal mode then switch to transparent:
  bool tcpOpenPassthrough(const String& host, int port, const String& apn, const String& user, const String& pass);
  // +++ escape to AT command level (transparent mode)
  bool backToAT();
  // Re-enter transparent mode after backToAT()
  bool enterTransparent();
  bool closeTcp();                   // AT+CIPSHUT (from AT level)

  // === byte stream over passthrough ===
  int  tcpRead(uint8_t* out, size_t max, uint32_t timeoutMs); // n>0, 0=timeout, -1=closed
  size_t tcpWrite(const uint8_t* d, size_t n);

  // === low level ===
  void flushInput();
  void sendRaw(const char* s);       // no CR
  void sendCmd(const char* s, bool withCR = true);
  bool await(const char* expect, uint32_t timeoutMs, String* response = nullptr);
  bool passthroughActive = false;
  bool verbose = false;              // rawCmd prints modem response to Serial
  bool pdpClosed = false;            // closeTcp() снял PDP - нужен gprsUp перед следующей попыткой

 private:
  HardwareSerial* _ser = nullptr;
  int8_t _pwrPin = -1;
};

extern Sim800 g_sim;