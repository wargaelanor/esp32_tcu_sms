#pragma once
#include <Arduino.h>
#include <WiFiUdp.h>

// Minimal captive-portal DNS responder for the ESP32 softAP:
// answers every A query on UDP:53 with the AP's own IP (192.168.4.1).
// OS captive-portal probes (Apple/Android/Windows) then land on the ESP,
// whose web server 302-redirects unknown Host headers to the status page.
void portal_setup();
void portal_tick();