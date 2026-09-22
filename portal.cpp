#include "portal.h"
#include <WiFi.h>

static WiFiUDP g_dns;
static bool    g_dnsOk = false;

void portal_setup() {
  if (g_dns.begin(53)) {
    g_dnsOk = true;
    Serial.println("[portal] DNS responder on udp:53");
  } else {
    g_dnsOk = false;
    Serial.println("[portal] DNS bind failed");
  }
}

void portal_tick() {
  if (!g_dnsOk) return;
  // Only serve clients on the softAP network.
  if (!(WiFi.getMode() & WIFI_AP)) return;

  int n = g_dns.parsePacket();
  if (n <= 0) return;

  uint8_t b[512];
  int len = g_dns.read(b, sizeof(b));
  if (len < 12) return;
  if ((b[2] & 0x80) != 0) return;          // ignore responses
  int qd = (b[4] << 8) | b[5];
  if (qd == 0) return;

  // Walk the QNAME (labels) in the question section.
  int p = 12;
  while (p < len && b[p] != 0) {
    int l = b[p] & 0x3f;
    if (l == 0 || p + l + 1 >= len) return;
    p += l + 1;
  }
  if (p >= len || b[p] != 0) return;
  p += 1;
  if (p + 4 > len) return;
  uint16_t qtype = (b[p] << 8) | b[p + 1];
  p += 4;

  if (qtype == 1) {                        // only answer A records
    // Recover the queried name for the debug log.
    String qn = "";
    for (int i = 12; i < len; i++) {
      uint8_t c = b[i];
      if (c == 0) break;
      if (c > 0x3f) break;
      if (!qn.isEmpty()) qn += '.';
      for (int j = 0; j < c && i + 1 + j < len; j++) qn += (char)b[i + 1 + j];
      i += c;
    }
    Serial.printf("[portal] DNS A %s -> %s\n", qn.c_str(), WiFi.softAPIP().toString().c_str());
  }

  // Build response (skip query answer section headers).
  uint8_t r[512];
  uint32_t rp = 0;
  memcpy(r, b, 12);                        // echo header
  r[2] = 0x81; r[3] = 0x80;                // response, recursion available
  r[6] = 0; r[7] = (qtype == 1) ? 1 : 0;   // ANCOUNT
  rp = 12;
  memcpy(r + rp, b + 12, p - 12);          // echo question
  rp += (uint32_t)(p - 12);

  if (qtype == 1) {                        // only answer A records
    IPAddress ip = WiFi.softAPIP();
    r[rp++] = 0xC0; r[rp++] = 0x0C;        // pointer to QNAME at offset 12
    r[rp++] = 0x00; r[rp++] = 0x01;        // type A
    r[rp++] = 0x00; r[rp++] = 0x01;        // class IN
    r[rp++] = 0x00; r[rp++] = 0x00; r[rp++] = 0x00; r[rp++] = 0x3C; // TTL 60
    r[rp++] = 0x00; r[rp++] = 0x04;        // rdlength
    r[rp++] = ip[0]; r[rp++] = ip[1]; r[rp++] = ip[2]; r[rp++] = ip[3];
  }

  g_dns.beginPacket(g_dns.remoteIP(), g_dns.remotePort());
  g_dns.write(r, rp);
  g_dns.endPacket();
}