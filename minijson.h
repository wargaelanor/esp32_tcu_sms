#pragma once
#include <Arduino.h>

// Minimal JSON field extractors (no external dependency).
// Matches: "key": vsllue  (string)  and  "key": 123 (int)

// Returns value of the next occurrence of "key": "..." (string) after position `from` (or 0).
inline bool json_get_string(const char* buf, size_t len, const char* key, String& out, size_t from = 0) {
  String mk = "\"";
  mk += key;
  mk += "\":";
  // bounded search
  size_t i = from;
  size_t klen = mk.length();
  size_t searchEnd = (len > 512) ? 512 : len;  // values sit near start of our small JSON
  bool found = false;
  while (i + klen <= searchEnd) {
    if (memcmp(buf + i, mk.c_str(), klen) == 0) { found = true; break; }
    i++;
  }
  if (!found) return false;
  size_t j = i + klen;
  while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;
  if (j >= len || buf[j] != '"') return false;
  j++;
  out = "";
  while (j < len && buf[j] != '"') {
    if (buf[j] == '\\' && j + 1 < len) { j++; out += (char)buf[j]; }
    else out += (char)buf[j];
    j++;
  }
  return true;
}

inline bool json_get_int(const char* buf, size_t len, const char* key, long& out, size_t from = 0) {
  String mk = "\"";
  mk += key;
  mk += "\":";
  size_t i = from;
  size_t klen = mk.length();
  size_t searchEnd = (len > 512) ? 512 : len;
  bool found = false;
  while (i + klen <= searchEnd) {
    if (memcmp(buf + i, mk.c_str(), klen) == 0) { found = true; break; }
    i++;
  }
  if (!found) return false;
  size_t j = i + klen;
  while (j < len && (buf[j] == ' ' || buf[j] == '\t')) j++;
  if (j >= len) return false;
  bool neg = false;
  if (buf[j] == '-') { neg = true; j++; }
  long v = 0;
  bool any = false;
  while (j < len && isdigit((unsigned char)buf[j])) { v = v * 10 + (buf[j] - '0'); j++; any = true; }
  if (!any) return false;
  out = neg ? -v : v;
  return true;
}