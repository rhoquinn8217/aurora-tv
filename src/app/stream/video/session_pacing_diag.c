#include "session_pacing_diag.h"

#include "logging.h"

#include <SDL.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PACING_WARN_MS    10.0
#define PACING_STUTTER_MS 12.5
#define PACING_HITCH_MS    16.0
#define PACING_SUMMARY_MS  1000

static int diag_enabled = -1; /* -1 unknown, 0 off, 1 on */
static uint64_t last_present_us;
static uint64_t last_recv_us;
static uint64_t window_start_ms;
static unsigned recv_n, feed_n, warn_n, stutter_n, hitch_n;
static double present_sum_ms;

static uint64_t pacing_now_us(void) {
    const uint64_t f = SDL_GetPerformanceFrequency();
    if (f == 0) {
        return (uint64_t) SDL_GetTicks() * 1000ull;
    }
    return SDL_GetPerformanceCounter() * 1000000ull / f;
}

static int diag_is_on(void) {
    if (diag_enabled >= 0) {
        return diag_enabled;
    }
    const char *env = getenv("AURORA_FRAME_DIAG");
    if (env != NULL && env[0] == '1') {
        diag_enabled = 1;
    } else {
        FILE *fp = fopen("/tmp/aurora_frame_diag.enable", "r");
        if (fp != NULL) {
            fclose(fp);
            diag_enabled = 1;
        } else {
            diag_enabled = 0;
        }
    }
    if (diag_enabled) {
        commons_log_info("Pacing", "Present-to-present diag ON. "
                         "Expected ~8.33 ms @ 120 Hz. Anomalies: >10 WARNING, >12.5 STUTTER, >=16 HITCH");
    }
    return diag_enabled;
}

void session_pacing_diag_reset(void) {
    last_present_us = 0;
    last_recv_us = 0;
    window_start_ms = 0;
    recv_n = feed_n = warn_n = stutter_n = hitch_n = 0;
    present_sum_ms = 0;
    diag_enabled = -1;
}

void session_pacing_diag_on_present(PDECODE_UNIT decodeUnit, SS4S_Player *player, int feed_ok) {
    if (!diag_is_on() || decodeUnit == NULL) {
        return;
    }
    const uint64_t t_us = pacing_now_us();
    const uint64_t t_ms = t_us / 1000ull;
    if (window_start_ms == 0) {
        window_start_ms = t_ms;
    }

    recv_n++;
    if (feed_ok) {
        feed_n++;
    }

    double present_delta_ms = 0;
    if (last_present_us != 0 && feed_ok) {
        present_delta_ms = (double) (t_us - last_present_us) / 1000.0;
        present_sum_ms += present_delta_ms;
    }
    if (feed_ok) {
        last_present_us = t_us;
    }

    const double assemble_ms = (decodeUnit->enqueueTimeUs > decodeUnit->receiveTimeUs)
            ? (double) (decodeUnit->enqueueTimeUs - decodeUnit->receiveTimeUs) / 1000.0
            : 0;
    const double queue_ms = (t_us > decodeUnit->enqueueTimeUs)
            ? (double) (t_us - decodeUnit->enqueueTimeUs) / 1000.0
            : 0;
    double recv_delta_ms = 0;
    if (last_recv_us != 0 && decodeUnit->receiveTimeUs > last_recv_us) {
        recv_delta_ms = (double) (decodeUnit->receiveTimeUs - last_recv_us) / 1000.0;
    }
    last_recv_us = decodeUnit->receiveTimeUs;

    int rq = -1;
    int latency_us = 0;
    if (player != NULL) {
        SS4S_PlayerGetVideoRenderQueueLength(player, &rq);
        SS4S_PlayerGetVideoLatency(player, 0, &latency_us);
    }
    const double decode_ms = (double) latency_us / 1000.0;

    const char *sev = NULL;
    if (feed_ok && present_delta_ms >= PACING_HITCH_MS) {
        hitch_n++;
        sev = "HITCH";
    } else if (feed_ok && present_delta_ms > PACING_STUTTER_MS) {
        stutter_n++;
        sev = "STUTTER";
    } else if (feed_ok && present_delta_ms > PACING_WARN_MS) {
        warn_n++;
        sev = "WARNING";
    }

    if (sev != NULL) {
        commons_log_warn("Pacing",
                         "%s present_delta=%.2fms (exp ~8.33) frame=%d "
                         "[RECV] %.2fms -> [DECODE] assemble=%.2fms decode=%.2fms -> [QUEUE] wait=%.2fms rq=%d -> [PRESENT] %.2fms",
                         sev, present_delta_ms, decodeUnit->frameNumber,
                         recv_delta_ms, assemble_ms, decode_ms, queue_ms, rq, present_delta_ms);
    }

    if (t_ms - window_start_ms >= PACING_SUMMARY_MS) {
        const double elapsed_s = (double) (t_ms - window_start_ms) / 1000.0;
        const double fps_recv = elapsed_s > 0 ? (double) recv_n / elapsed_s : 0;
        const double fps_feed = elapsed_s > 0 ? (double) feed_n / elapsed_s : 0;
        const unsigned deltas = feed_n > 1 ? feed_n - 1 : 0;
        const double avg = deltas > 0 ? present_sum_ms / (double) deltas : 0;
        commons_log_info("Pacing",
                         "1s summary: fps_recv=%.1f fps_feed=%.1f present_avg=%.2fms "
                         "warn=%u stutter=%u hitch=%u rq=%d decode=%.2fms "
                         "[RECV]->[DECODE]->[QUEUE]->[PRESENT] (NDL Feed=present)",
                         fps_recv, fps_feed, avg, warn_n, stutter_n, hitch_n, rq, decode_ms);
        window_start_ms = t_ms;
        recv_n = feed_n = warn_n = stutter_n = hitch_n = 0;
        present_sum_ms = 0;
    }
}
