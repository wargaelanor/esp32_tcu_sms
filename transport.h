#pragma once
#include <Arduino.h>
#include <WiFiClient.h>

// Abstract byte-stream that the WebSocket client runs over.
// Implementations: WifiLink (ESP32 WiFiClient) and GprsLink (SIM800 transparent TCP).
struct Link {
  virtual bool open() = 0;
  virtual void close() = 0;
  virtual bool isConnected() = 0;
  virtual size_t write(const uint8_t* d, size_t n) = 0;
  virtual int read(uint8_t* out, size_t max, uint32_t timeoutMs) = 0; // n>0 | 0=timeout | -1=closed
  const char* name = "link";
};

struct WifiLink : Link {
  WifiLink() { name = "wifi"; }
  void configure(const String& host, int port, uint32_t timeoutMs);
  bool open() override;
  void close() override;
  bool isConnected() override;
  size_t write(const uint8_t* d, size_t n) override;
  int read(uint8_t* out, size_t max, uint32_t timeoutMs) override;

 private:
  String    _host;
  int       _port = 80;
  uint32_t  _timeout = 3000;
  WiFiClient _c;
};

struct GprsLink : Link {
  GprsLink() { name = "gprs"; }
  void configure(const String& host, int port, const String& apn, const String& user, const String& pass);
  bool open() override;
  void close() override;
  bool isConnected() override;
  size_t write(const uint8_t* d, size_t n) override;
  int read(uint8_t* out, size_t max, uint32_t timeoutMs) override;

 private:
  String _host;
  int    _port = 80;
  String _apn, _apnUser, _apnPass;
};