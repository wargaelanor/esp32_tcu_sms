#include "tls_link.h"

#include "mbedtls/error.h"

static TlsLink* bioCtx(void* ctx) { return (TlsLink*)ctx; }

int TlsLink::sendCb(void* ctx, const uint8_t* buf, size_t len) {
  TlsLink* t = bioCtx(ctx);
  size_t off = 0;
  uint32_t t0 = millis();
  while (off < len) {
    size_t w = t->_base->write(buf + off, len - off);
    if (w > 0) { off += w; t0 = millis(); continue; }
    if (millis() - t0 > 5000) return MBEDTLS_ERR_SSL_WANT_WRITE;
    delay(2);
  }
  return (int)len;
}

int TlsLink::recvCb(void* ctx, uint8_t* buf, size_t len, uint32_t timeoutMs) {
  TlsLink* t = bioCtx(ctx);
  int n = t->_base->read(buf, len, timeoutMs);
  if (n > 0)  return n;
  if (n == 0) return MBEDTLS_ERR_SSL_WANT_READ;   // timeout != EOF
  return 0;                                        // -1 = closed -> EOF
}

void TlsLink::configure(const String& host) { _sni = host; }

bool TlsLink::open() {
  if (_up) return _base->isConnected();
  if (!_base->open()) return false;
  if (!_c) _c = new Ctx();
  mbedtls_ssl_init(&_c->ssl);
  mbedtls_ssl_config_init(&_c->sc);
  mbedtls_entropy_init(&_c->ent);
  mbedtls_ctr_drbg_init(&_c->rng);

  const char* pers = "tcu_tls";
  int rc = mbedtls_ctr_drbg_seed(&_c->rng, mbedtls_entropy_func, &_c->ent,
                                 (const unsigned char*)pers, strlen(pers));
  if (rc) { Serial.printf("[tls] rng seed fail %d\n", rc); close(); return false; }

  rc = mbedtls_ssl_config_defaults(&_c->sc, MBEDTLS_SSL_IS_CLIENT,
                                   MBEDTLS_SSL_TRANSPORT_STREAM,
                                   MBEDTLS_SSL_PRESET_DEFAULT);
  if (rc) { Serial.printf("[tls] defaults fail %d\n", rc); close(); return false; }

  mbedtls_ssl_conf_authmode(&_c->sc, MBEDTLS_SSL_VERIFY_NONE);   // no cert pinning for now
  mbedtls_ssl_conf_rng(&_c->sc, mbedtls_ctr_drbg_random, &_c->rng);
  mbedtls_ssl_conf_read_timeout(&_c->sc, 3000);   // recvCb блокируется до 3с (GPRS RTT велик)

  mbedtls_ssl_conf_min_version(&_c->sc, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);

  rc = mbedtls_ssl_setup(&_c->ssl, &_c->sc);
  if (rc) { Serial.printf("[tls] setup fail %d\n", rc); close(); return false; }

  if (_sni.length()) mbedtls_ssl_set_hostname(&_c->ssl, _sni.c_str());
  mbedtls_ssl_set_bio(&_c->ssl, this, (mbedtls_ssl_send_t*)sendCb,
                      (mbedtls_ssl_recv_t*)recvCb, (mbedtls_ssl_recv_timeout_t*)recvCb);

  uint32_t t0 = millis();
  do {
    rc = mbedtls_ssl_handshake(&_c->ssl);
    if (rc == 0) { _up = true; return true; }
    if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
      char e[128];
      mbedtls_strerror(rc, e, sizeof(e));
      Serial.printf("[tls] handshake fail %d (%s)\n", rc, e);
      close();
      return false;
    }
  } while (millis() - t0 < 20000);
  Serial.println("[tls] handshake timeout");
  close();
  return false;
}

void TlsLink::close() {
  if (_c) {
    if (_up) mbedtls_ssl_close_notify(&_c->ssl);
    mbedtls_ssl_free(&_c->ssl);
    mbedtls_ssl_config_free(&_c->sc);
    mbedtls_ctr_drbg_free(&_c->rng);
    mbedtls_entropy_free(&_c->ent);
    _up = false;
  }
  _base->close();
}

bool TlsLink::isConnected() { return _up && _base->isConnected(); }

size_t TlsLink::write(const uint8_t* d, size_t n) {
  if (!_up) return 0;
  uint32_t t0 = millis();
  int ret = mbedtls_ssl_write(&_c->ssl, d, n);
  while ((ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) &&
         millis() - t0 < 5000) {
    ret = mbedtls_ssl_write(&_c->ssl, d, n);
  }
  return ret > 0 ? (size_t)ret : 0;
}

int TlsLink::read(uint8_t* out, size_t max, uint32_t timeoutMs) {
  if (!_up) return -1;
  uint32_t t0 = millis();
  for (;;) {
    int ret = mbedtls_ssl_read(&_c->ssl, out, max);
    if (ret > 0) return ret;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE ||
        ret == MBEDTLS_ERR_SSL_TIMEOUT) {
      if (millis() - t0 >= timeoutMs) return 0;
      delay(2);
      continue;
    }
    _up = false;                               // fatal
    return -1;
  }
}