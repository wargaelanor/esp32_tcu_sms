#pragma once
#include <Arduino.h>
#include "transport.h"

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"

// TLS 1.2 client stream running on top of any Link (e.g. SIM800 transparent TCP).
// Implemented with mbedTLS from the ESP32 SDK; handshake is done in open().
struct TlsLink : Link {
  explicit TlsLink(Link* base) : _base(base) { name = "gprs+tls"; }
  void configure(const String& host);        // SNI / validation name
  bool open() override;
  void close() override;
  bool isConnected() override;
  size_t write(const uint8_t* d, size_t n) override;
  int read(uint8_t* out, size_t max, uint32_t timeoutMs) override;

 private:
  Link*    _base;
  String   _sni;
  struct Ctx {                              // mbedTLS contexts
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  sc;
    mbedtls_entropy_context ent;
    mbedtls_ctr_drbg_context rng;
  };
  Ctx*     _c = nullptr;
  bool     _up = false;

  static int sendCb(void* ctx, const uint8_t* buf, size_t len);
  static int recvCb(void* ctx, uint8_t* buf, size_t len, uint32_t timeoutMs);
};