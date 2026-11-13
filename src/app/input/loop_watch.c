/* A slow pass of the app's loop, named by its steps. The why is in the header. */

#include "loop_watch.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <SDL.h>

#define LOOP_WATCH_SLOW_MS 300
#define LOOP_WATCH_STEPS 32
#define LOOP_WATCH_LOG "loop-watch.log"

#if defined(TARGET_WEBOS)
FILE *ctm_log_open(const char *name, const char *mode);
#endif

static Uint64 s_freq;
static Uint64 s_begin;
static Uint64 s_last;
static int s_n;
static struct {
    const char *step;
    Uint64 ticks;
} s_steps[LOOP_WATCH_STEPS];

void loop_watch_begin(void) {
    if (s_freq == 0) {
        s_freq = SDL_GetPerformanceFrequency();
    }
    s_begin = s_last = SDL_GetPerformanceCounter();
    s_n = 0;
}

void loop_watch_mark(const char *step) {
    if (s_begin == 0) {
        return;
    }
    const Uint64 now = SDL_GetPerformanceCounter();
    if (s_n < LOOP_WATCH_STEPS) {
        s_steps[s_n].step = step;
        s_steps[s_n].ticks = now - s_last;
        s_n++;
    }
    s_last = now;
}

static unsigned long long ms_of(Uint64 ticks) {
    return s_freq != 0 ? (unsigned long long) (ticks * 1000 / s_freq) : 0;
}

void loop_watch_end(void) {
    if (s_begin == 0 || s_freq == 0) {
        return;
    }
    const Uint64 now = SDL_GetPerformanceCounter();
    const unsigned long long total = ms_of(now - s_begin);
    s_begin = 0;
    if (total < LOOP_WATCH_SLOW_MS) {
        return;
    }
    char line[400];
    int at = snprintf(line, sizeof(line), "a pass of the app's loop took %llu ms; slowest:", total);
    bool used[LOOP_WATCH_STEPS] = {false};
    for (int k = 0; k < 3 && at > 0 && at < (int) sizeof(line); ++k) {
        int best = -1;
        for (int i = 0; i < s_n; ++i) {
            if (!used[i] && (best < 0 || s_steps[i].ticks > s_steps[best].ticks)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        used[best] = true;
        at += snprintf(line + at, sizeof(line) - (size_t) at, "%s %s %llu ms", k > 0 ? "," : "", s_steps[best].step,
                       ms_of(s_steps[best].ticks));
    }
#if defined(TARGET_WEBOS)
    FILE *f = ctm_log_open(LOOP_WATCH_LOG, "a");
    if (f == NULL) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%lld.%03ld %s\n", (long long) ts.tv_sec, ts.tv_nsec / 1000000L, line);
    fclose(f);
#else
    fprintf(stderr, "%s\n", line);
#endif
}
