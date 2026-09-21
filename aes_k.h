#pragma once
#include <Arduino.h>
#include <stdint.h>

// Decrypt an AES-CBC parcel as produced by the server:
//   parcel = nonce(16) || AES-CBC(enc_key, plaintext padded to 16B, pad len repeated)
// Server always sends nonce=16 bytes. Key length must be 16/24/32 bytes.
// Returns true and writes unpadded plaintext into `out` (owned by caller, size >= inLen).
bool aes_cbc_decrypt_parcel(const uint8_t* key, size_t keyLen,
                            const uint8_t* in, size_t inLen,
                            uint8_t* out, size_t& outLen);

// "E4BA DE01 ..." -> raw bytes. Skips spaces/dashes. Returns count or 0.
size_t hex_key_parse(const String& hex, uint8_t* raw, size_t maxLen);

// Hex string -> raw bytes (even length required).
size_t hex_decode(const char* hex, size_t hexLen, uint8_t* raw, size_t maxLen);

// Raw bytes -> lowercase hex (no spaces).
size_t hex_encode(const uint8_t* raw, size_t len, char* out, size_t maxOut);

void base64_encode(const uint8_t* data, size_t len, char* out, size_t maxOut);