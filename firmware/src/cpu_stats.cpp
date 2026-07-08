#include "cpu_stats.h"

#include <Arduino.h>
#include <esp_freertos_hooks.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Per-core idle tick counters.  Incremented every time the FreeRTOS idle
// task on the respective core gets a tick of CPU.  Higher == more idle.
//
// Marked volatile because the dump reader runs on a different task than
// either hook.  No mutex: monotonic-increment + single-reader is racy-safe
// at the precision we care about (diagnostic, not accounting).
static volatile uint32_t s_idle_ticks[2] = {0, 0};

// Baseline ticks/sec measured at boot when system is quiet.  Reading scales
// against this so "idle_pct=100" means "as idle as boot".
static uint32_t s_baseline[2] = {0, 0};

// Last sample time so we can compute ticks/sec over actual elapsed wall time.
static uint32_t s_last_dump_ms = 0;

static bool IRAM_ATTR idle_cb_cpu0() { s_idle_ticks[0]++; return true; }
static bool IRAM_ATTR idle_cb_cpu1() { s_idle_ticks[1]++; return true; }

void cpu_stats_init() {
    esp_err_t e0 = esp_register_freertos_idle_hook_for_cpu(idle_cb_cpu0, 0);
    esp_err_t e1 = esp_register_freertos_idle_hook_for_cpu(idle_cb_cpu1, 1);
    Serial.printf("[cpu] idle hooks registered: cpu0=%d cpu1=%d\n",
                  (int)e0, (int)e1);
    Serial.flush();

    // Capture a 200 ms baseline.  Anything else running during boot is fine —
    // we just want a ballpark for "system at rest" to normalise later samples
    // against.  The baseline is recorded as ticks-per-second so it's directly
    // comparable to later per-second samples.
    s_idle_ticks[0] = 0;
    s_idle_ticks[1] = 0;
    uint32_t t0 = millis();
    delay(200);
    uint32_t dt = millis() - t0;
    if (dt == 0) dt = 1;
    s_baseline[0] = (s_idle_ticks[0] * 1000U) / dt;
    s_baseline[1] = (s_idle_ticks[1] * 1000U) / dt;
    s_idle_ticks[0] = 0;
    s_idle_ticks[1] = 0;
    s_last_dump_ms = millis();
    Serial.printf("[cpu] idle baseline (ticks/sec): cpu0=%u cpu1=%u\n",
                  (unsigned)s_baseline[0], (unsigned)s_baseline[1]);
    Serial.flush();
}

void cpu_stats_dump_line() {
    uint32_t now = millis();
    uint32_t dt  = now - s_last_dump_ms;
    if (dt < 100) return;     // ignore back-to-back calls
    s_last_dump_ms = now;

    uint32_t i0 = s_idle_ticks[0];
    uint32_t i1 = s_idle_ticks[1];
    s_idle_ticks[0] = 0;
    s_idle_ticks[1] = 0;

    // ticks/sec over the measured interval
    uint32_t r0 = (i0 * 1000U) / dt;
    uint32_t r1 = (i1 * 1000U) / dt;

    // idle % relative to baseline, capped at 100
    uint32_t p0 = s_baseline[0] ? (r0 * 100U) / s_baseline[0] : 0;
    uint32_t p1 = s_baseline[1] ? (r1 * 100U) / s_baseline[1] : 0;
    if (p0 > 100) p0 = 100;
    if (p1 > 100) p1 = 100;

    Serial.printf("[cpu] idle cpu0=%u%% (%u/s)  cpu1=%u%% (%u/s)  dt=%ums\n",
                  (unsigned)p0, (unsigned)r0,
                  (unsigned)p1, (unsigned)r1,
                  (unsigned)dt);
    Serial.flush();
}

void cpu_stats_task_dump() {
#if (configUSE_TRACE_FACILITY == 1)
    // vTaskList writes a human-readable table.  Format columns:
    //   Name  State  Prio  StackHighWater  Num  [CoreID]
    // CoreID column only appears if FreeRTOS was built with
    // configTASKLIST_INCLUDE_COREID (CONFIG_FREERTOS_VTASKLIST_INCLUDE_COREID).
    // Buffer needs ~40 bytes per task; 4 KB safely covers 30+ tasks.
    static char buf[4096];
    Serial.println("[cpu] === task table ===");
    Serial.println("Name             State  Prio  Stack  Num  Core");
    vTaskList(buf);
    Serial.print(buf);
    Serial.println("[cpu] === end task table ===");
    Serial.flush();
#else
    Serial.println("[cpu] vTaskList unavailable (configUSE_TRACE_FACILITY=0)");
    Serial.flush();
#endif
}
