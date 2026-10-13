/* The TV's volume from a controller while the streaming overlay is up. The why
 * is in the header. */

#include "overlay_volume.h"

#if defined(TARGET_WEBOS)

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SDL.h>
#include <pbnjson.h>
#include "lunasynccall.h"   /* the TV's audio service */

#include "app_input.h"
#include "ctm_bridge_gesture.h"   /* which of SDL's pads is bridged */
#include "ctmbridge/ctm_bridge_glue.h"   /* a bridged pad's right stick */

/* What happened, where it can be read over SSH (tv_cursor.c says why). */
#define OVERLAY_VOLUME_LOG "overlay-volume.log"
FILE *ctm_log_open(const char *name, const char *mode);

/* A push counts once the stick is past half way, and ends once it is back
 * inside a quarter; between the two it stays what it was, so a stick held near
 * half way does not step on and off. */
#define PUSH_PAST   16384
#define REST_INSIDE 8192
#define REPEAT_MS   200
#define MAX_STICKS  24

typedef struct {
    bool used;
    bool seen;      /* read this pass */
    bool armed;     /* back at rest since the overlay opened */
    bool click;     /* the click was down last pass */
    int dir;        /* 1 up, -1 down, 0 neither */
    Uint32 next_ms; /* when a held push steps again */
    char key[80];
} stick_t;

static stick_t s_sticks[MAX_STICKS];
static bool s_overlay_up;

static SDL_Thread *s_thread;
static SDL_sem *s_wake;
static SDL_atomic_t s_steps;   /* steps owed: up positive, down negative */
static SDL_atomic_t s_mutes;   /* clicks owed */
static SDL_atomic_t s_quit;

static void volume_log(const char *fmt, ...) {
    FILE *f = ctm_log_open(OVERLAY_VOLUME_LOG, "a");
    if (f == NULL) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    fprintf(f, "%lld.%03ld ", (long long) ts.tv_sec, ts.tv_nsec / 1000000L);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* ---------------------------------------------------------------------------
 * The TV's calls, on their own thread.
 * ------------------------------------------------------------------------- */

static jvalue_ref parse_reply(const char *reply, jdomparser_ref *parser) {
    *parser = NULL;
    if (reply == NULL) {
        return NULL;
    }
    JSchemaInfo schema;
    jschema_info_init(&schema, jschema_all(), NULL, NULL);
    *parser = jdomparser_create(&schema, 0);
    if (*parser == NULL || !jdomparser_feed(*parser, reply, (int) strlen(reply)) || !jdomparser_end(*parser)) {
        return NULL;
    }
    return jdomparser_get_result(*parser);
}

/* A boolean member, or -1 when there is none. */
static int bool_member(jvalue_ref object, const char *name) {
    if (!jis_object(object)) {
        return -1;
    }
    jvalue_ref v = jobject_get(object, j_cstr_to_buffer(name));
    bool b = false;
    if (v == NULL || !jis_boolean(v) || jboolean_get(v, &b) != CONV_OK) {
        return -1;
    }
    return b ? 1 : 0;
}

/* Call, and say whether the TV answered returnValue true. */
static bool call_ok(const char *uri, const char *payload, char **reply_out) {
    char *reply = NULL;
    const bool called = HLunaServiceCallSync(uri, payload, true, &reply);
    jdomparser_ref parser = NULL;
    const bool ok = called && bool_member(parse_reply(reply, &parser), "returnValue") == 1;
    if (parser != NULL) {
        jdomparser_release(&parser);
    }
    if (reply_out != NULL) {
        *reply_out = reply;
    } else {
        free(reply);
    }
    return ok;
}

/* The volume one step up (dir 1) or down (-1): the older service first, which
 * both sets checked answer, then the newer one. The one that answered is tried
 * first from then on. */
static void step_volume(int dir) {
    static const char *const up[2] = {
        "luna://com.webos.audio/volumeUp", "luna://com.webos.service.audio/master/volumeUp"};
    static const char *const down[2] = {
        "luna://com.webos.audio/volumeDown", "luna://com.webos.service.audio/master/volumeDown"};
    static int s_works[2] = {-1, -1};   /* [0] down, [1] up */
    const char *const *uris = dir > 0 ? up : down;
    int *works = &s_works[dir > 0 ? 1 : 0];
    for (int k = 0; k < 2; ++k) {
        const int i = *works >= 0 ? (*works + k) % 2 : k;
        char *reply = NULL;
        if (call_ok(uris[i], "{}", &reply)) {
            if (*works != i) {
                volume_log("%s goes through %s", dir > 0 ? "up" : "down", uris[i]);
                *works = i;
            }
            free(reply);
            return;
        }
        volume_log("%s: %s answered %s", dir > 0 ? "up" : "down", uris[i], reply != NULL ? reply : "nothing");
        free(reply);
    }
}

/* Whether the TV is muted: 1, 0, or -1 when it does not say. */
static int tv_muted(void) {
    char *reply = NULL;
    if (!call_ok("luna://com.webos.service.audio/master/getVolume", "{}", &reply)) {
        volume_log("mute: getVolume answered %s", reply != NULL ? reply : "nothing");
        free(reply);
        return -1;
    }
    jdomparser_ref parser = NULL;
    jvalue_ref root = parse_reply(reply, &parser);
    int muted = -1;
    if (jis_object(root)) {
        muted = bool_member(jobject_get(root, J_CSTR_TO_BUF("volumeStatus")), "muteStatus");
        if (muted < 0) {
            muted = bool_member(root, "muteStatus");
        }
    }
    if (parser != NULL) {
        jdomparser_release(&parser);
    }
    free(reply);
    return muted;
}

/* Mute the TV, or unmute it. ⓘ When the TV does not say which it is, the
 * opposite of what this last set. */
static void toggle_mute(void) {
    static bool s_last_set = false;
    const int muted = tv_muted();
    const bool want = muted < 0 ? !s_last_set : muted == 0;
    char *reply = NULL;
    if (call_ok("luna://com.webos.audio/setMuted", want ? "{\"muted\":true}" : "{\"muted\":false}", &reply)) {
        volume_log("%s", want ? "muted" : "unmuted");
        s_last_set = want;
    } else {
        volume_log("mute: setMuted answered %s", reply != NULL ? reply : "nothing");
    }
    free(reply);
}

static int volume_thread(void *arg) {
    (void) arg;
    while (!SDL_AtomicGet(&s_quit)) {
        SDL_SemWaitTimeout(s_wake, 500);
        int steps = SDL_AtomicSet(&s_steps, 0);
        for (; steps > 0 && !SDL_AtomicGet(&s_quit); --steps) {
            step_volume(1);
        }
        for (; steps < 0 && !SDL_AtomicGet(&s_quit); ++steps) {
            step_volume(-1);
        }
        /* ⓘ Two clicks before the thread got to them leave the TV as it was. */
        if (SDL_AtomicSet(&s_mutes, 0) % 2 != 0) {
            toggle_mute();
        }
    }
    return 0;
}

void overlay_volume_init(void) {
    if (s_thread != NULL) {
        return;
    }
    SDL_AtomicSet(&s_quit, 0);
    SDL_AtomicSet(&s_steps, 0);
    SDL_AtomicSet(&s_mutes, 0);
    s_wake = SDL_CreateSemaphore(0);
    s_thread = s_wake != NULL ? SDL_CreateThread(volume_thread, "overlay-volume", NULL) : NULL;
    if (s_thread == NULL) {
        volume_log("could not start: %s", SDL_GetError());
    }
}

void overlay_volume_deinit(void) {
    if (s_thread == NULL) {
        return;
    }
    SDL_AtomicSet(&s_quit, 1);
    SDL_SemPost(s_wake);
    /* ⓘ Let go rather than waited for: a call the TV never answers would hold
     * the app's close. The semaphore stays for the same reason. */
    SDL_DetachThread(s_thread);
    s_thread = NULL;
}

/* ---------------------------------------------------------------------------
 * The sticks, on the event loop.
 * ------------------------------------------------------------------------- */

static void ask_step(int dir, const char *key) {
    SDL_AtomicAdd(&s_steps, dir);
    SDL_SemPost(s_wake);
    volume_log("%s from %s", dir > 0 ? "up" : "down", key);
}

static void ask_mute(const char *key) {
    SDL_AtomicAdd(&s_mutes, 1);
    SDL_SemPost(s_wake);
    volume_log("mute or unmute from %s", key);
}

static stick_t *stick_for(const char *key) {
    stick_t *free_slot = NULL;
    for (int i = 0; i < MAX_STICKS; ++i) {
        if (s_sticks[i].used && strcmp(s_sticks[i].key, key) == 0) {
            return &s_sticks[i];
        }
        if (!s_sticks[i].used && free_slot == NULL) {
            free_slot = &s_sticks[i];
        }
    }
    if (free_slot != NULL) {
        memset(free_slot, 0, sizeof(*free_slot));
        free_slot->used = true;
        snprintf(free_slot->key, sizeof(free_slot->key), "%s", key);
    }
    return free_slot;
}

/* One controller's right stick (`y` from -32768 up to 32767 down) and click. */
static void feed(const char *key, int y, bool click, Uint32 now) {
    stick_t *s = stick_for(key);
    if (s == NULL) {
        return;
    }
    s->seen = true;
    const int far = y < 0 ? -y : y;
    if (!s->armed) {
        if (far < REST_INSIDE && !click) {
            s->armed = true;
            s->dir = 0;
            s->click = false;
        }
        return;
    }
    int dir = s->dir;
    if (dir == 0) {
        dir = y <= -PUSH_PAST ? 1 : (y >= PUSH_PAST ? -1 : 0);
    } else if (far < REST_INSIDE) {
        dir = 0;
    } else if (dir > 0 && y >= PUSH_PAST) {
        dir = -1;   /* straight from up to down */
    } else if (dir < 0 && y <= -PUSH_PAST) {
        dir = 1;
    }
    if (dir != 0 && (dir != s->dir || (Sint32) (now - s->next_ms) >= 0)) {
        ask_step(dir, key);
        s->next_ms = now + REPEAT_MS;
    }
    s->dir = dir;
    if (click && !s->click) {
        ask_mute(key);
    }
    s->click = click;
}

void overlay_volume_tick(struct app_input_t *input, bool overlay_up) {
    if (!overlay_up) {
        if (s_overlay_up) {
            memset(s_sticks, 0, sizeof(s_sticks));
            s_overlay_up = false;
        }
        return;
    }
    if (!s_overlay_up) {
        s_overlay_up = true;
        volume_log("overlay up: the right stick is the TV's volume");
    }
    const Uint32 now = SDL_GetTicks();
    for (int i = 0; i < MAX_STICKS; ++i) {
        s_sticks[i].seen = false;
    }
    char key[80];
    ctm_bridge_stick_t bridged[8];
    const int n = ctm_bridge_held_sticks(bridged, 8);
    if (input != NULL) {
        const size_t slots = input->max_num_gamepads < 16 ? input->max_num_gamepads : 16;
        /* ⛔⛔ A BRIDGED PAD IS READ FROM THE CORE ONLY. SDL can still read
         * one: the Edge on its DS5Dongle, bridged on the C1, came through both,
         * and every push stepped the volume twice and every click muted and
         * unmuted at once (2026-10-12, build 532). So a pad of SDL's that is
         * at a bridged node is left out. ⓘ Matched every quarter second, not
         * every pass: the match can look in /sys. */
        static bool s_bridged_slot[16];
        static Uint32 s_matched_at;
        static int s_matched_n = -1;
        if (n != s_matched_n || (Sint32) (now - s_matched_at) >= 250) {
            s_matched_n = n;
            s_matched_at = now;
            for (size_t i = 0; i < slots; ++i) {
                app_gamepad_state_t *pad = &input->gamepads[i];
                s_bridged_slot[i] = false;
                if (pad->instance_id == -1 || pad->controller == NULL) {
                    continue;
                }
                const char *path = SDL_GameControllerPath(pad->controller);
                for (int b = 0; b < n && !s_bridged_slot[i]; ++b) {
                    s_bridged_slot[i] = ctm_bridge_gesture_path_is_node(path, bridged[b].node);
                }
            }
        }
        for (size_t i = 0; i < slots; ++i) {
            app_gamepad_state_t *pad = &input->gamepads[i];
            if (pad->instance_id == -1 || pad->controller == NULL || s_bridged_slot[i]) {
                continue;
            }
            snprintf(key, sizeof(key), "the TV's pad %d", (int) pad->instance_id);
            feed(key, SDL_GameControllerGetAxis(pad->controller, SDL_CONTROLLER_AXIS_RIGHTY),
                 SDL_GameControllerGetButton(pad->controller, SDL_CONTROLLER_BUTTON_RIGHTSTICK) != 0, now);
        }
    }
    for (int i = 0; i < n; ++i) {
        if (!bridged[i].reading) {
            continue;
        }
        snprintf(key, sizeof(key), "bridged %s", bridged[i].node);
        feed(key, bridged[i].y, bridged[i].click, now);
    }
    for (int i = 0; i < MAX_STICKS; ++i) {
        if (s_sticks[i].used && !s_sticks[i].seen) {
            memset(&s_sticks[i], 0, sizeof(s_sticks[i]));
        }
    }
}

#else

void overlay_volume_init(void) {
}

void overlay_volume_deinit(void) {
}

void overlay_volume_tick(struct app_input_t *input, bool overlay_up) {
    (void) input;
    (void) overlay_up;
}

#endif
