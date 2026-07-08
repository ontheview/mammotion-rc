#pragma once

// Per-core CPU load + task dump for diagnosing webpage-load stalls.
//
// Two-part diagnostic:
//   1) idle-hook tick counter on each core -> per-second idle ticks/sec ratio
//      relative to a baseline captured at boot when the system is quiet.
//   2) one-shot task list dump showing every FreeRTOS task, its state, prio,
//      stack high-water, and (if FreeRTOS was built with the option) the core
//      it's pinned to.

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

// Register idle hooks on both cores and capture a 200 ms baseline.
// Call once early in setup() (after Serial.begin so we can log).
void cpu_stats_init();

// Read+reset the per-core idle tick counters.  Returns the ratio of observed
// ticks to baseline ticks scaled to 100, i.e. idle_pct_cpu0=100 means the
// core was as idle as it was at baseline; 0 means fully saturated.
//
// Reads are racy with the idle hooks but that's fine for diagnostics.
void cpu_stats_dump_line();

// Dump the full FreeRTOS task table.  Heavy; call manually on stall, not
// every second.  Falls back to a stub message if vTaskList isn't compiled in.
void cpu_stats_task_dump();

#ifdef __cplusplus
}
#endif
