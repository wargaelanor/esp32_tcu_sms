#pragma once
// Diagnostics: name of the step the firmware is currently in. The main loop
// prints it with the heartbeat so a watchdog reset can be traced back to the
// blocking call.
extern const char* g_step;
#define STEP(s) do { g_step = (s); } while (0)
