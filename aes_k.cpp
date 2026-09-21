#include "aes_k.h"
#include "mbedtls/aes.h"

static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t hex_decode(const char* hex, size_t hexLen, uint8_t* raw, size_t maxLen) {
  size_t n = 0;
  for (size_t i = 0; i + 1 < hexLen && n < maxLen; i += 2) {
    int hi = hexval(hex[i]);
    int lo = hexval(hex[i+1]);
    if (hi < 0 || lo < 0) return n == 0 ? 0 : n;  // tolerate trailing garbage
    raw[n++] = (uint8_t)((hi << 4) | lo);
  }
  return n;
}

size_t hex_encode(const uint8_t* raw, size_t len, char* out, size_t maxOut) {
  static const char* hx = "0123456789abcdef";
  size_t n = 0;
  for (size_t i = 0; i < len && n + 2 < maxOut; i++) {
    out[n++] = hx[(raw[i] >> 4) & 0xF];
    out[n++] = hx[raw[i] & 0xF];
  }
  out[n] = 0;
  return n;
}

size_t hex_key_parse(const String& hex, uint8_t* raw, size_t maxLen) {
  size_t n = 0;
  for (size_t i = 0; i < hex.length() && n < maxLen; i++) {
    char c = hex[i];
    if (c == ' ' || c == '-' || c == '\t' || c == ':') continue;
    int v = hexval(c);
    if (v < 0) break;
    if ((n & 1) == 0) raw[n >> 1] = (uint8_t)(v << 4);
    else raw[n >> 1] |= (uint8_t)v;
    n++;
  }
  return n >> 1;
}

bool aes_cbc_decrypt_parcel(const uint8_t* key, size_t keyLen,
                            const uint8_t* in, size_t inLen,
                            uint8_t* out, size_t& outLen) {
  if (!key || !in || !out) return false;
  if (keyLen != 16 && keyLen != 24 && keyLen != 32) return false;
  if (inLen <= 16) return false;                  // need nonce + at least one block
  size_t ctLen = inLen - 16;
  if ((ctLen & 0xF) != 0) return false;           // CBC block aligned
  const uint8_t* nonce = in;
  const uint8_t* ct = in + 16;

  unsigned char iv[16];
  memcpy(iv, nonce, 16);

  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  int rc = mbedtls_aes_setkey_dec(&ctx, key, (unsigned)keyLen * 8);
  if (rc != 0) { mbedtls_aes_free(&ctx); return false; }
  rc = mbedtls_aes_crypt_cbc(&ctx, MBEDTLS_AES_DECRYPT, ctLen, iv, ct, out);
  mbedtls_aes_free(&ctx);
  if (rc != 0) return false;

  // PKCS#7-style unpad (server: pad_len bytes of pad_len, 1..16)
  uint8_t pad = out[ctLen - 1];
  if (pad < 1 || pad > 16 || pad > ctLen) return false;
  for (uint8_t i = 0; i < pad; i++) {
    if (out[ctLen - 1 - i] != pad) return false;
  }
  outLen = ctLen - pad;
  return true;
}

static const char b64t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

void base64_encode(const uint8_t* data, size_t len, char* out, size_t maxOut) {
  size_t o = 0;
  for (size_t i = 0; i < len && o + 4 <= maxOut; i += 3) {
    uint32_t v = (uint32_t)data[i] << 16;
    if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
    if (i + 2 < len) v |= data[i+2];
    out[o++] = b64t[(v >> 18) & 0x3F];
    out[o++] = b64t[(v >> 12) & 0x3F];
    out[o++] = (i + 1 < len) ? b64t[(v >> 6) & 0x3F] : '=';
    out[o++] = (i + 2 < len) ? b64t[v & 0x3F] : '=';
  }
  if (o < maxOut) out[o] = 0;
}