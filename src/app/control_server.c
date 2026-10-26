#include "control_server.h"

#if defined(AURORA_TERMINAL_CONTROL)

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "lvgl.h"
#include <SDL.h>

#include "app.h"
#include "logging.h"
#include "ctm_screenshot.h"
#include "crash_recorder.h"
#include "util/bus.h"
#include "util/user_event.h"
#include "backend/pcmanager.h"
#include "stream/session.h"
#include "stream/session_priv.h"   /* the stream's app, for status */
#include "stream/bridge_session.h"
#include "stream/video/session_video.h"
#include "ui/launcher/launcher.controller.h"
#include "ui/launcher/stream_actions.h"   /* an app's name, by id */
#include "ui/streaming/streaming.controller.h"
#include "ui/streaming/bridge_prompt.h"
#include "ui/streaming/ctm_panel.h"
#include "input/bridge_override.h"
#include "input/bridge_keyboard.h"
#include "input/tv_cursor.h"
#include "ui/streaming/cursor_align.h"

#if defined(TARGET_WEBOS)
#include "ctm_bridge_glue.h"
#include "input/auto_bridge.h"
#include "input/bridge_request.h"
#include "input/ctm_bridge_gesture.h"
#include "input/device_groups.h"
#endif

#define CONTROL_LINE_MAX 256
#define CONTROL_REPLY_MAX 16384
/* ⓘ How long a command may wait for the UI thread before the terminal is told
 * so. Generous, because a bridge request enumerates every device. */
#define CONTROL_WAIT_SECONDS 10
#define CONTROL_MAX_DEVICES 32

/* One command on its way through the UI thread and back.
 *
 * ⚠️ Owned by the server thread UNLESS it gave up waiting: then `abandoned` is
 * set and the UI side frees it once the command has run. */
typedef struct {
    char line[CONTROL_LINE_MAX];
    char reply[CONTROL_REPLY_MAX];
    size_t used;
    bool done;
    bool abandoned;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} control_job_t;

static app_t *s_app = NULL;
static int s_listen_fd = -1;

static void reply(control_job_t *job, const char *fmt, ...)
{
    if (job->used + 1 >= sizeof(job->reply)) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(job->reply + job->used, sizeof(job->reply) - job->used, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    job->used += (size_t) n;
    if (job->used >= sizeof(job->reply)) {
        job->used = sizeof(job->reply) - 1;   /* truncated: keep what fitted */
    }
}

/* Case-insensitive substring test, without relying on _GNU_SOURCE. */
static bool contains_ci(const char *haystack, const char *needle)
{
    const size_t n = strlen(needle);
    for (const char *p = haystack; *p != '\0'; ++p) {
        if (strncasecmp(p, needle, n) == 0) {
            return true;
        }
    }
    return n == 0;
}

static bool is_all_digits(const char *s)
{
    if (*s == '\0') {
        return false;
    }
    for (; *s != '\0'; ++s) {
        if (*s < '0' || *s > '9') {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------- commands */

static void cmd_help(control_job_t *job)
{
    reply(job, "OK commands\n"
               "status                  the build, the stream and the bridge\n"
               "hosts                   known hosts: uuid, state, address, name\n"
               "stream <host> <app id>  start a stream; host is a uuid or part of its name\n"
               "stop                    end the stream and leave the game running\n"
               "quit                    end the stream and quit the game on the host\n"
               "stats                   the stream's own numbers, one line, as the overlay draws them\n"
               "devices                 every part the core lists, with its SDL match and its group\n"
               "groups                  the devices those parts make up, as the panel's rows show them\n"
               "bridge <device>         bridge one part\n"
               "release <device>        release one part\n"
               "bridge-group <n>        bridge every part of a device, exactly as its panel row does\n"
               "release-group <n>       release every part of a device, exactly as its panel row does\n"
               "bridge-all, release-all every part, as the panel's buttons do\n"
               "open-config             ask the host for its settings window, as the overlay's DS5-USBIP button does\n"
               "set <name> <on|off>     a switch, without the remote: boost, light, rumble, tone, override, vmouse\n"
               "prompt [ok|dismiss]     the question a bridge raises: is it up, or answer it without a hand\n"
               "ui                      what is open on the screen: overlay, its keyboard, the panel, the align bar\n"
               "press <key>[:ms] ...    keys into aurora-tv's own screens, as a keyboard types them; not once a stream is up\n"
               "                        keys: up down left right enter esc home end backspace; :ms holds it (a long press)\n"
               "type <text>             characters into aurora-tv's own screens, a key each, as a keyboard types them\n"
               "                        (Shift for capitals, ':' and '_'); letters, digits and . - _ : [ ]; not once a stream is up\n"
               "host [<host>]           the launcher's host: which it shows, or pick one (a uuid or part of its name)\n"
               "input                   the stream's live input state: Bridge Override and what it switches\n"
               "keys                    key events counted since the app started: the TV's keyboard grab, and webOS\n"
               "set settle <ms>         how long to let the link settle before a DS4 handback tone\n"
               "signal refuse <n>       play a device's refusal signal; no plug has to fail\n"
               "cursor [<right> <down> [<thin>]]  Aurora's TV pointer: what stands, or a shift and a\n"
               "                        thinner outline to try live, in the TV's own pixels; not saved\n"
               "cursor on|off           the TV cursor on or off, and saved, as Ctrl+Alt+Shift+M does\n"
               "cursor align            open the bar that aligns the TV cursor, as Ctrl+Alt+Shift+A does\n"
               "cursor sharp|regular    the drop or webOS's own pointer, and saved, as Ctrl+Alt+Shift+C does\n"
               "shot [name]             the app's own screen (never the stream's picture) to logs/, as a .bmp\n"
               "crash                   the crash recorder: armed or not, and where its record goes\n"
               "crash test|thread       crash the app on purpose, on its own loop or on a thread, to prove it\n"
               "<device> is its number, its node, its vid:pid, or part of its name; <n> is from groups\n");
}

/* A switch, from here, without the remote.
 *
 * What this costs when it is missing, 2026-09-23: answering "do the light and
 * the pulse break the DS4 tone?" needed those switches toggled on a TV with no
 * keyboard. luna-send and kill are both denied to `prisoner` on the C3, and the
 * app rewrites moonlight.ini from memory on shutdown, so a file edit never
 * survived a restart. It took two throwaway builds to flip two booleans, and
 * the first silently did nothing because the stored value won.
 *
 * ⛔ NOT MEMORY ONLY (code review, 2026-10-05; this said it was). The switches
 * change the app's live settings, and those are written to moonlight.ini when
 * Settings is left and on every clean exit, so a test that changes one must
 * put it back before it ends. ⓘ `settle` alone is not saved: see below. */
static void cmd_set(control_job_t *job, const char *args)
{
    char name[32] = "";
    char value[16] = "";
    if (args == NULL || sscanf(args, "%31s %15s", name, value) != 2) {
        reply(job, "ERR usage: set <boost|light|rumble|tone|override|vmouse> <on|off>, or set settle <ms>\n");
        return;
    }
    /* ⛔⛔ BEFORE THE on/off CHECK, AND THAT IS THE WHOLE POINT.
     * ⚠️ It was added after it on 2026-09-23, so every numeric value was
     * answered "ERR <on|off>" and silently never applied -- `0` was the one
     * that got through, because it matches "off". A four-point sweep of the
     * tone gap was run and reported before the replies were read, and every
     * point of it was really the same setting. ➡️ A knob that rejects its own
     * values is worse than no knob: it reads as a result.
     * 🔗 control-a-probe-before-trusting-it. */
#if defined(TARGET_WEBOS)
    /* ⭐ A NUMBER RATHER THAN A SWITCH, and the only setting here that is not a
     * bool: how long to let a pad's link settle after a session ends, before the
     * handback tone goes out. ⓘ Sweeping it from here is what keeps a pad's
     * battery and a room's noise pinned across a comparison; a build per value
     * does not. Memory only: the gap is not a setting and is never saved,
     * unlike the switches. */
    if (strcasecmp(name, "settle") == 0) {
        char *end = NULL;
        const long ms = strtol(value, &end, 10);
        if (end == value || ms < 0 || ms > 3000) {
            reply(job, "ERR settle takes 0 to 3000 ms\n");
            return;
        }
        ctm_bridge_set_tone_gap((int) ms);
        reply(job, "OK settle=%ldms before a Bluetooth DS4 handback tone\n", ms);
        return;
    }
#endif
    const bool on  = (strcasecmp(value, "on") == 0 || strcmp(value, "1") == 0);
    const bool off = (strcasecmp(value, "off") == 0 || strcmp(value, "0") == 0);
    if (!on && !off) { reply(job, "ERR <on|off>\n"); return; }
    if (app_configuration == NULL) { reply(job, "ERR no settings loaded\n"); return; }

    if (strcasecmp(name, "boost") == 0) {
        app_configuration->stream_priority = on;
        reply(job, "OK boost=%s -- applied at STREAM START, so restart the stream\n",
              on ? "on" : "off");
        return;
    }
    /* ⭐ Virtual Mouse through the event its overlay button's twin on the
     * remote raises, so it ends Bridge Override exactly as a press does. */
    if (strcasecmp(name, "vmouse") == 0) {
        if (s_app->session == NULL) {
            reply(job, "ERR no stream is running\n");
            return;
        }
        const bool active = bridge_session_vmouse_active(s_app->session);
        if (on != active) {
            bus_pushevent(USER_TOGGLE_VMOUSE, NULL, NULL);
        }
        reply(job, "OK virtual mouse %s%s\n", on ? "on" : "off",
              on != active ? ": asked for, as its button does" : " already");
        return;
    }
    /* ⭐ Bridge Override by name, which a person never sees: the question a
     * bridge raises switches it on and Virtual Mouse switches it off. Here
     * it is set directly. Nothing is shown on the TV, as with either of those. */
    if (strcasecmp(name, "override") == 0) {
        bridge_override_set(s_app->session, on);
        char state[256];
        bridge_override_describe(s_app->session != NULL ? session_get_input(s_app->session) : NULL,
                                 state, sizeof state);
        reply(job, "OK %s%s\n", state,
              on && !bridge_override_active() ? " -- Enable Device Bridging is off, so it does not act" : "");
        return;
    }
#if defined(TARGET_WEBOS)
    if (strcasecmp(name, "light") == 0 || strcasecmp(name, "rumble") == 0 ||
        strcasecmp(name, "tone") == 0) {
        if (strcasecmp(name, "light") == 0)  app_configuration->bridge.signal_light = on;
        if (strcasecmp(name, "rumble") == 0) app_configuration->bridge.signal_rumble = on;
        if (strcasecmp(name, "tone") == 0)   app_configuration->bridge.signal_tone = on;
        ctm_bridge_set_signals(app_configuration->bridge.signal_light,
                               app_configuration->bridge.signal_rumble,
                               app_configuration->bridge.signal_tone);
        reply(job, "OK light=%s rumble=%s tone=%s\n",
              app_configuration->bridge.signal_light ? "on" : "off",
              app_configuration->bridge.signal_rumble ? "on" : "off",
              app_configuration->bridge.signal_tone ? "on" : "off");
        return;
    }
#endif
    reply(job, "ERR try boost, light, rumble, tone, override, vmouse or settle\n");
}

/* The question a bridge raises (ui/streaming/bridge_prompt.h): whether it is
 * up and how long it has left, and its two answers without a hand. */
static void cmd_prompt(control_job_t *job, const char *args)
{
    if (args != NULL && strcasecmp(args, "ok") == 0) {
        if (bridge_prompt_accept()) {
            reply(job, "OK pressed: the TV's mouse controls are off\n");
        } else {
            reply(job, "ERR the question is not up\n");
        }
        return;
    }
    if (args != NULL && strcasecmp(args, "dismiss") == 0) {
        const bool up = bridge_prompt_shown();
        bridge_prompt_dismiss();
        reply(job, up ? "OK dismissed: nothing changed\n" : "ERR the question is not up\n");
        return;
    }
    const int left = bridge_prompt_seconds_left();
    if (left > 0) {
        /* ⓘ Its shape as well as its count: `at` is its top left corner,
         * `bottom` its lower edge, `buttons_top` the top of the overlay's row
         * of buttons that it has to stay above, and `lines` how many the
         * warning takes. So "one line, in from the corner, clear of the
         * buttons" is read here rather than off a television. */
        bridge_prompt_measure_t m;
        if (bridge_prompt_measure(&m)) {
            reply(job, "OK prompt=up seconds=%d size=%dx%d at=%d,%d bottom=%d buttons_top=%d lines=%d row=%d "
                       "selected=%d highlighted=%s screen=%dx%d\n",
                  left, m.width, m.height, m.x, m.y, m.y + m.height, m.buttons_top, m.text_lines, m.row_width,
                  m.selected, m.highlighted ? "yes" : "no", m.screen_width, m.screen_height);
        } else {
            reply(job, "OK prompt=up seconds=%d\n", left);
        }
    } else {
        /* The message its button leaves, in the same place, for a few seconds. */
        bridge_prompt_measure_t m;
        const int note_ms = bridge_prompt_note_measure(&m);
        if (note_ms >= 0) {
            reply(job, "OK prompt=down note=up ms_left=%d size=%dx%d at=%d,%d bottom=%d buttons_top=%d lines=%d\n",
                  note_ms, m.width, m.height, m.x, m.y, m.y + m.height, m.buttons_top, m.text_lines);
        } else {
            reply(job, "OK prompt=down note=down\n");
        }
    }
}

/* What is open on the screen, read only: the overlay, its keyboard and its
 * numbers, the USB Bridge panel, the question a bridge raises, the TV cursor's
 * align bar and the TV cursor itself. Made so a test that drives the TV with
 * nobody at it can see what a keyboard chord or a gesture opened or closed
 * (2026-10-17), where until then only a person looking at the TV could. */
static void cmd_ui(control_job_t *job)
{
    reply(job, "OK overlay=%s kbd=%s stats=%s panel=%s prompt=%s align=%s cursor=%s stream=%s\n",
          streaming_overlay_shown() ? "open" : "closed",
          streaming_soft_keyboard_shown() ? "open" : "closed",
          streaming_stats_shown() ? "shown" : "hidden",
          ctm_panel_shown() ? "open" : "closed",
          bridge_prompt_shown() ? "up" : "down",
          cursor_align_shown() ? "open" : "closed",
          tv_cursor_drawn() ? "drawn" : "hidden",
          s_app->session != NULL ? "active" : "idle");
}

static void cmd_status(control_job_t *job)
{
    reply(job, "OK build=\"%s\" stream=%s", APP_VERSION, s_app->session != NULL ? "active" : "idle");
    /* ⓘ What the stream is: the app this TV started or joined, or Remote
     * Input or Remote Monitor, by the name its tile had. For the TV/VM
     * website (rhoquinn8217, 2026-10-20: "show which VM and app/remote
     * input/remote monitor it's currently streaming"). */
    if (s_app->session != NULL && s_app->session->app_name != NULL) {
        char name[96];
        snprintf(name, sizeof(name), "%s", s_app->session->app_name);
        for (char *c = name; *c != '\0'; c++) {
            if (*c == '"') {
                *c = '\'';
            }
        }
        reply(job, " app=\"%s\"", name);
    }
#if defined(TARGET_WEBOS)
    const char *listener = !ctm_bridge_agent_probed() ? "unknown"
                         : ctm_bridge_agent_online() ? "online" : "offline";
    char agent[128] = "";
    ctm_bridge_agent(agent, sizeof agent);
    /* ⓘ Whether this app and the listener can work together. */
    const ctm_bridge_compat_t compat = ctm_bridge_agent_compat();
    const char *protocol = compat == CTM_BRIDGE_COMPAT_OK          ? "ok"
                         : compat == CTM_BRIDGE_COMPAT_UPDATE_PC   ? "update-pc"
                         : compat == CTM_BRIDGE_COMPAT_UPDATE_TV   ? "update-tv"
                                                                   : "unknown";
    reply(job, " bridge=%s listener=%s protocol=%s agent=\"%s\" core=%s",
          ctm_bridge_active() ? "running" : "stopped", listener, protocol, agent, bridge_core_commit());
#endif
    reply(job, " override=%s\n", bridge_override_active() ? "on" : "off");
}

/* The stream's LIVE input state, which is what Bridge Override changes. The
 * saved settings are not it: the override never writes them. */
static void cmd_input(control_job_t *job)
{
    if (s_app->session == NULL) {
        reply(job, "ERR no stream is running\n");
        return;
    }
    char state[256];
    bridge_override_describe(session_get_input(s_app->session), state, sizeof state);
    reply(job, "OK %s\n", state);
}

/* Aurora's TV pointer (input/tv_cursor.h): what stands now, or a shift and a
 * thinning to try live, not saved (rhoquinn8217, 2026-10-11: "move it
 * slightly down and to the right"), so a test needs no build per try. And
 * the TV cursor on or off, saved, as Ctrl+Alt+Shift+M switches it; the align
 * bar, as Ctrl+Alt+Shift+A opens it; and which cursor, saved, as
 * Ctrl+Alt+Shift+C switches it. */
static void cmd_cursor(control_job_t *job, const char *args)
{
    if (strcasecmp(args, "align") == 0) {
        cursor_align_request();
        reply(job, "OK the align bar is asked for: it opens on the interface's next pass, or says why not\n");
        return;
    }
    if (strcasecmp(args, "sharp") == 0 || strcasecmp(args, "regular") == 0) {
        if (!tv_cursor_set_look(strcasecmp(args, "regular") == 0)) {
            reply(job, "ERR that cursor could not be made: see logs/tv-cursor.log\n");
            return;
        }
    } else if (strcasecmp(args, "on") == 0 || strcasecmp(args, "off") == 0) {
        if (!tv_cursor_set_mode(strcasecmp(args, "on") == 0)) {
            reply(job, "%s", tv_cursor_mode_switchable()
                                 ? "ERR the TV cursor stays off while a mouse is bridged, as the TV now says\n"
                                 : "ERR the TV cursor cannot be switched while \"Use mouse hardware\" is on\n");
            return;
        }
    } else if (*args != '\0') {
        int right = 0, down = 0, thin = -1;
        const int got = sscanf(args, "%d %d %d", &right, &down, &thin);
        if (got < 2) {
            reply(job, "ERR usage: cursor [<right> <down> [<thin>]], in the TV's own pixels\n");
            return;
        }
        if (!tv_cursor_adjust(right, down, got >= 3 ? thin : -1)) {
            reply(job, "ERR Aurora has no pointer of its own to change: see logs/tv-cursor.log\n");
            return;
        }
    }
    char line[224];
    tv_cursor_describe(line, sizeof line);
    reply(job, "OK %s\n", line);
}

static void cmd_shot(control_job_t *job, const char *args)
{
    /* Runs on the interface thread, which is the thread that owns the
     * renderer -- every command here does, which is why this can read it. */
    char detail[320];
    if (ctm_screenshot_write(args, detail, sizeof detail)) {
        reply(job, "OK wrote %s\n", detail);
    } else {
        reply(job, "ERR %s\n", detail);
    }
}

/* press: keys into aurora-tv's own screens, from here.
 *
 * rhoquinn8217, 2026-10-17, asked how much of a test of the launcher could be
 * run without them, and said yes to this: `shot` already shows what is on the
 * screen, and nothing could press a key on a TV without root (the lab's virtual
 * keyboard needs it). So a test of a menu or a window needed their hands.
 *
 * The keys go in as SDL key events, where a keyboard's do, and the interface
 * reads them from there the same way (lvgl/input/lv_drv_sdl_key.c). One at a
 * time, PRESS_GAP_MS apart: that file drops the same key twice within 50 ms as
 * the Magic Remote's double report. `enter:600` holds it 600 ms, a long press.
 *
 * ⛔ Refused while a stream runs: there the keys would go to the host, into
 * whatever is in front on the PC. The overlay is not a target.
 * ⓘ But not while a stream is still connecting (2026-10-22): the host takes
 * no keys yet, and the "Connecting..." box and its Cancel are on the screen. */
#define PRESS_MAX 40
#define PRESS_GAP_MS 150
#define PRESS_TAP_MS 40

/* Whether a stream would take the keys: one is up, not merely connecting. */
static bool stream_takes_keys(void)
{
    session_t *session = s_app->session;
    if (session == NULL) {
        return false;
    }
    SDL_LockMutex(session->mutex);
    const bool connecting = session->state == STREAMING_CONNECTING;
    SDL_UnlockMutex(session->mutex);
    return !connecting;
}

typedef struct press_step_t {
    SDL_Keycode sym;
    SDL_Scancode scan;
    SDL_Keymod mod;
    uint32_t hold_ms;
} press_step_t;

static press_step_t s_press[PRESS_MAX];
static int s_press_count;
static int s_press_next;
static bool s_press_down;
static lv_timer_t *s_press_timer;

static bool press_key_named(const char *name, press_step_t *step)
{
    static const struct {
        const char *name;
        SDL_Keycode sym;
        SDL_Scancode scan;
    } keys[] = {
            {"up",    SDLK_UP,     SDL_SCANCODE_UP},
            {"down",  SDLK_DOWN,   SDL_SCANCODE_DOWN},
            {"left",  SDLK_LEFT,   SDL_SCANCODE_LEFT},
            {"right", SDLK_RIGHT,  SDL_SCANCODE_RIGHT},
            {"enter", SDLK_RETURN, SDL_SCANCODE_RETURN},
            {"ok",    SDLK_RETURN, SDL_SCANCODE_RETURN},
            {"esc",   SDLK_ESCAPE, SDL_SCANCODE_ESCAPE},
            {"back",  SDLK_ESCAPE, SDL_SCANCODE_ESCAPE},
            {"home",  SDLK_HOME,   SDL_SCANCODE_HOME},
            {"end",   SDLK_END,    SDL_SCANCODE_END},
            {"backspace", SDLK_BACKSPACE, SDL_SCANCODE_BACKSPACE},
    };
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        if (strcasecmp(name, keys[i].name) == 0) {
            step->sym = keys[i].sym;
            step->scan = keys[i].scan;
            step->mod = KMOD_NONE;
            return true;
        }
    }
    return false;
}

/* type: the key a keyboard types `c` with (2026-10-22). A box takes a
 * keyboard's keys as SDL key events while its text input is off
 * (ui/common/type_into_box.h), and no keyboard the lab makes reaches the app
 * outside a stream: webOS's compositor reads only keyboards it opened itself,
 * which neither ctm-vdev's nor one made through uinput is. These go in where a
 * keyboard's keys do. */
static bool type_key_for(char c, press_step_t *step)
{
    step->mod = KMOD_NONE;
    if (c >= 'a' && c <= 'z') {
        step->sym = (SDL_Keycode) c;
    } else if (c >= 'A' && c <= 'Z') {
        step->sym = (SDL_Keycode) (c - 'A' + 'a');
        step->mod = KMOD_LSHIFT;
    } else if (c >= '0' && c <= '9') {
        step->sym = (SDL_Keycode) c;
    } else if (c == '.') {
        step->sym = SDLK_PERIOD;
    } else if (c == '-' || c == '_') {
        step->sym = SDLK_MINUS;
        step->mod = c == '_' ? KMOD_LSHIFT : KMOD_NONE;
    } else if (c == ':') {
        step->sym = SDLK_SEMICOLON;
        step->mod = KMOD_LSHIFT;
    } else if (c == '[' || c == ']') {
        step->sym = c == '[' ? SDLK_LEFTBRACKET : SDLK_RIGHTBRACKET;
    } else {
        return false;
    }
    step->scan = SDL_GetScancodeFromKey(step->sym);
    step->hold_ms = PRESS_TAP_MS;
    return true;
}

static void press_push(const press_step_t *step, bool down)
{
    SDL_Event event;
    SDL_zero(event);
    event.type = down ? SDL_KEYDOWN : SDL_KEYUP;
    event.key.timestamp = SDL_GetTicks();
    event.key.state = down ? SDL_PRESSED : SDL_RELEASED;
    event.key.keysym.sym = step->sym;
    event.key.keysym.scancode = step->scan;
    event.key.keysym.mod = step->mod;
    SDL_PushEvent(&event);
}

static void press_tick(lv_timer_t *timer)
{
    if (s_press_next >= s_press_count) {
        lv_timer_del(timer);
        s_press_timer = NULL;
        return;
    }
    const press_step_t *step = &s_press[s_press_next];
    if (!s_press_down) {
        press_push(step, true);
        s_press_down = true;
        lv_timer_set_period(timer, step->hold_ms);
    } else {
        press_push(step, false);
        s_press_down = false;
        s_press_next++;
        lv_timer_set_period(timer, PRESS_GAP_MS);
    }
}

/* The keys of press or type go in on the interface's own timer, one at a time. */
static void press_start(control_job_t *job, const press_step_t *steps, int count, uint32_t total_ms)
{
    memcpy(s_press, steps, sizeof(press_step_t) * count);
    s_press_count = count;
    s_press_next = 0;
    s_press_down = false;
    s_press_timer = lv_timer_create(press_tick, 1, NULL);
    reply(job, "OK pressing %d key(s), done in about %u ms\n", count, (unsigned) total_ms);
}

static void cmd_press(control_job_t *job, const char *args)
{
    if (stream_takes_keys()) {
        reply(job, "ERR a stream is running: press drives aurora-tv's own screens only\n");
        return;
    }
    if (s_press_timer != NULL) {
        reply(job, "ERR still pressing the last keys\n");
        return;
    }
    press_step_t steps[PRESS_MAX];
    int count = 0;
    uint32_t total_ms = 0;
    char copy[256];
    snprintf(copy, sizeof copy, "%s", args != NULL ? args : "");
    char *save = NULL;
    for (char *word = strtok_r(copy, " \t", &save); word != NULL; word = strtok_r(NULL, " \t", &save)) {
        if (count == PRESS_MAX) {
            reply(job, "ERR at most %d keys at a time\n", PRESS_MAX);
            return;
        }
        char *colon = strchr(word, ':');
        long hold = PRESS_TAP_MS;
        if (colon != NULL) {
            *colon = '\0';
            hold = strtol(colon + 1, NULL, 10);
            if (hold < PRESS_TAP_MS || hold > 5000) {
                reply(job, "ERR a hold is %d to 5000 ms\n", PRESS_TAP_MS);
                return;
            }
        }
        if (!press_key_named(word, &steps[count])) {
            reply(job, "ERR no key '%s': up down left right enter ok esc back home end backspace\n", word);
            return;
        }
        steps[count].hold_ms = (uint32_t) hold;
        total_ms += (uint32_t) hold + PRESS_GAP_MS;
        count++;
    }
    if (count == 0) {
        reply(job, "ERR usage: press <key>[:ms] ...  keys: up down left right enter ok esc back home end backspace\n");
        return;
    }
    press_start(job, steps, count, total_ms);
}

static void cmd_type(control_job_t *job, const char *args)
{
    if (stream_takes_keys()) {
        reply(job, "ERR a stream is running: type drives aurora-tv's own screens only\n");
        return;
    }
    if (s_press_timer != NULL) {
        reply(job, "ERR still pressing the last keys\n");
        return;
    }
    press_step_t steps[PRESS_MAX];
    int count = 0;
    const char *text = args != NULL ? args : "";
    /* ⓘ One word: a space types nothing (no key for it), and stopping at it
     * while answering OK hid that the rest was never typed (2026-10-23). */
    if (strpbrk(text, " \t") != NULL) {
        reply(job, "ERR type takes one word: no key types a space\n");
        return;
    }
    for (const char *c = text; *c != '\0' && *c != '\r'; c++) {
        if (count == PRESS_MAX) {
            reply(job, "ERR at most %d characters at a time\n", PRESS_MAX);
            return;
        }
        if (!type_key_for(*c, &steps[count])) {
            reply(job, "ERR no key types '%c': letters, digits and . - _ : [ ]\n", *c);
            return;
        }
        count++;
    }
    if (count == 0) {
        reply(job, "ERR usage: type <text>  letters, digits and . - _ : [ ]\n");
        return;
    }
    press_start(job, steps, count, (uint32_t) count * (PRESS_TAP_MS + PRESS_GAP_MS));
}

/* Where key events have come from since the app started: the TV's keyboard
 * grab, and webOS through SDL. Works with no stream too, which is how to see
 * whether webOS hands a keyboard's keys to the app at all. */
static void cmd_keys(control_job_t *job)
{
    char counts[160];
    bridge_keyboard_counts(counts, sizeof counts);
    reply(job, "OK %s\n", counts);
}

static const char *host_state_name(SERVER_STATE_ENUM code)
{
    switch (code) {
        case SERVER_STATE_AVAILABLE:
            return "available";
        case SERVER_STATE_NOT_PAIRED:
            return "not-paired";
        case SERVER_STATE_ONLINE:
            return "online";
        case SERVER_STATE_QUERYING:
            return "querying";
        case SERVER_STATE_OFFLINE:
            return "offline";
        case SERVER_STATE_ERROR:
            return "error";
        default:
            return "unknown";
    }
}

static const char *host_name(const pclist_t *node)
{
    return node->server != NULL && node->server->hostname != NULL ? node->server->hostname : "";
}

static void cmd_hosts(control_job_t *job)
{
    int count = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        ++count;
    }
    reply(job, "OK %d host(s)\n", count);
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        const char *address = cur->server != NULL && cur->server->serverInfo.address != NULL
                              ? cur->server->serverInfo.address : "-";
        reply(job, "%s state=%s address=%s name=\"%s\"\n", cur->id.data,
              host_state_name(cur->state.code), address, host_name(cur));
    }
}

/* host: which host the launcher shows, or pick one, as "Select server" does.
 *
 * The lab's monitor (2026-10-17) drives the launcher with press, and its
 * launcher showed CAELUM, the PC rhoquinn8217 uses: an Enter on a tile there
 * starts something on their PC. Asked for by the lab, yes'd by them, so its
 * cases can point the launcher at its own VM first. Refused in a stream, and
 * for a host not paired: picking one opens the pairing dialog, which waits
 * for a PIN nobody is there to type. */
static void cmd_host(control_job_t *job, const char *args)
{
    char want[128] = "";
    if (args != NULL) {
        sscanf(args, "%127s", want);
    }
    if (want[0] == '\0') {
        for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
            if (cur->selected) {
                reply(job, "OK selected %s name=\"%s\"\n", cur->id.data, host_name(cur));
                return;
            }
        }
        reply(job, "OK selected none\n");
        return;
    }
    if (s_app->session != NULL) {
        reply(job, "ERR a stream is running: stop it first\n");
        return;
    }
    launcher_fragment_t *launcher = launcher_instance();
    if (launcher == NULL) {
        reply(job, "ERR the launcher is not open\n");
        return;
    }
    const pclist_t *match = NULL;
    int matches = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (uuidstr_t_equals_s(&cur->id, want)) {
            match = cur;
            matches = 1;
            break;
        }
        if (host_name(cur)[0] != '\0' && contains_ci(host_name(cur), want)) {
            match = cur;
            ++matches;
        }
    }
    if (matches == 0) {
        reply(job, "ERR no host '%s'; see hosts\n", want);
        return;
    }
    if (matches > 1) {
        reply(job, "ERR '%s' matches %d hosts; use the uuid from hosts\n", want, matches);
        return;
    }
    if (match->state.code == SERVER_STATE_NOT_PAIRED) {
        reply(job, "ERR %s is not paired with this TV\n", host_name(match));
        return;
    }
    uuidstr_t id = match->id;
    launcher_select_server(launcher, &id);
    reply(job, "OK the launcher shows %s (%s)\n", host_name(match), id.data);
}

static void cmd_stream(control_job_t *job, const char *args)
{
    char host[128] = "";
    char app_arg[32] = "";
    if (sscanf(args, "%127s %31s", host, app_arg) != 2 || !is_all_digits(app_arg)) {
        reply(job, "ERR usage: stream <host uuid, or part of its name> <app id>\n");
        return;
    }
    if (s_app->session != NULL) {
        reply(job, "ERR a stream is already running; stop it first\n");
        return;
    }
    /* ⓘ A uuid, or part of the name. Host names carry spaces -- "CAELUM
     * (Apollo)" -- and the argument cannot, so a unique part of one is enough.
     * ⛔ Two hosts matching is refused rather than guessed between. */
    const pclist_t *match = NULL;
    int matches = 0;
    for (const pclist_t *cur = pcmanager_servers(pcmanager); cur != NULL; cur = cur->next) {
        if (uuidstr_t_equals_s(&cur->id, host)) {
            match = cur;
            matches = 1;
            break;
        }
        if (host_name(cur)[0] != '\0' && contains_ci(host_name(cur), host)) {
            match = cur;
            ++matches;
        }
    }
    if (matches == 0) {
        reply(job, "ERR no host '%s'; see hosts\n", host);
        return;
    }
    if (matches > 1) {
        reply(job, "ERR '%s' matches %d hosts; use the uuid from hosts\n", host, matches);
        return;
    }
    /* ⭐ EXACTLY WHAT PRESSING THE APP'S TILE DOES (launcher_launch_game in
     * apps.controller.c): push the streaming screen, whose constructor starts
     * the session. ⓘ The arguments are only read during that constructor, and
     * the session copies the name, so a stack copy is safe here as it is there.
     * The name is what the stream shows; the app id is what launches.
     * ⭐ The app's own name, from the host's list as it last loaded
     * (rhoquinn8217, 2026-10-20: "fix the terminal name": the TV dashboard's
     * chip read "Streaming terminal"). "terminal" only for a list not loaded. */
    const int app_id = atoi(app_arg);
    const char *app_name = stream_actions_app_name(&match->id, app_id);
    streaming_scene_arg_t scene = {
            .global = s_app,
            .uuid = match->id,
            .app = {.name = (char *) (app_name != NULL ? app_name : "terminal"), .id = app_id},
    };
    lv_fragment_t *fragment = lv_fragment_create(&streaming_controller_class, &scene);
    if (fragment == NULL) {
        reply(job, "ERR the streaming screen could not be created\n");
        return;
    }
    lv_obj_t *const *container = lv_fragment_get_container(lv_fragment_manager_get_top(s_app->ui.fm));
    lv_fragment_manager_push(s_app->ui.fm, fragment, container);
    reply(job, "OK starting app %s on %s; status says stream=active once it is up\n",
          app_arg, host_name(match)[0] != '\0' ? host_name(match) : host);
}

/* ⭐ The crash recorder (crash_recorder.h), and a crash on purpose to prove it:
 * a recorder nobody has seen record anything cannot be trusted with the crash
 * it is for. ⛔ `test` and `thread` end the app, and the stream with it. */
static void cmd_crash(control_job_t *job, const char *args)
{
    if (args == NULL || args[0] == '\0') {
        char state[512];
        crash_recorder_describe(state, sizeof state);
        reply(job, "OK crash recorder %s\n", state);
    } else if (strcasecmp(args, "test") == 0 || strcasecmp(args, "thread") == 0) {
        const bool on_thread = strcasecmp(args, "thread") == 0;
        crash_recorder_test(on_thread);
        reply(job, "OK the app crashes on purpose in half a second, %s; logs/crash.log has the record\n",
              on_thread ? "on a thread of its own" : "on its own loop");
    } else {
        reply(job, "ERR usage: crash [test|thread]\n");
    }
}

static void cmd_stop(control_job_t *job, bool quit_game)
{
    if (s_app->session == NULL) {
        reply(job, "ERR no stream is running\n");
        return;
    }
    /* The same call the overlay's own buttons make. */
    session_interrupt(s_app->session, quit_game, STREAMING_INTERRUPT_USER);
    reply(job, quit_game ? "OK ending the stream and quitting the game\n"
                         : "OK ending the stream\n");
}

/* ⭐ WHY THIS EXISTS: the overlay's numbers can be read only by someone sitting
 * in front of the set, and the fault they are wanted for -- decode latency that
 * climbs, stays up for hours and then clears on its own -- turns up while nobody
 * is watching. One line a second into a file can be lined up afterwards against
 * the host's encoder trace and the set's own lag trace, which share its clock.
 *
 * ⓘ Every figure is derived exactly as `streaming_refresh_stats` derives it for
 * the overlay, so the two cannot disagree. Units are ms, except `fps`/`recvfps`,
 * `loss` (percent of frames), `bitrate` (Mbps) and the plain counts.
 *
 * ⚠️ `window` says how often the numbers underneath are recomputed: 2000 ms
 * normally, 1000 ms while the overlay is shown. Sampling faster than that
 * repeats a reading rather than refining it. */
static void cmd_stats(control_job_t *job)
{
    /* ⛔ session_is_streaming, not merely a session: LiGetEstimatedRttInfo below
     * may only be called between LiStartConnection and LiStopConnection, and a
     * session exists while it is still connecting and while it is tearing down. */
    if (s_app->session == NULL || !session_is_streaming(s_app->session)) {
        reply(job, "ERR no stream is running\n");
        return;
    }

    struct VIDEO_STATS st;
    vdec_stats_snapshot(&st);
    const struct VIDEO_INFO *info = &vdec_stream_info;

    /* ⛔ THE SNAPSHOT'S RTT IS STALE WHILE THE OVERLAY IS OFF -- vdec_stat_submit
     * asks ENet for it only when the overlay is shown. Ask for it here instead.
     * It is a deliberately lock-free read of the peer's own metrics, and it
     * leaves both values untouched when it fails. */
    uint32_t rtt = st.rtt, rtt_var = st.rttVariance;
    LiGetEstimatedRttInfo(&rtt, &rtt_var);

    float host_ms = 0.0f, render_ms = 0.0f, decode_ms = 0.0f, reasm_ms = 0.0f;
    bool have_host = false, have_render = false, have_decode = false;
    if (st.submittedFrames > 0) {
        render_ms = (float) st.totalSubmitTime / (float) st.submittedFrames;
        have_render = true;
        if (info->has_host_latency) {
            /* ⓘ The host reports capture-to-encode in TENTHS of a millisecond. */
            host_ms = (float) st.totalCaptureLatency / (float) st.submittedFrames / 10.0f;
            have_host = true;
        }
        if (info->has_decoder_latency) {
            decode_ms = st.avgDecoderLatency;
            have_decode = true;
        }
    }
    if (st.receivedFrames > 0) {
        /* Not on the overlay: how long a frame spent being put back together from
         * its packets, which tells a late network from a slow decoder. */
        reasm_ms = (float) st.totalReassemblyTime / (float) st.receivedFrames;
    }
    const float total_ms = (float) rtt + host_ms + render_ms + decode_ms;
    const float loss_pct = st.totalFrames > 0
                           ? (float) st.networkDroppedFrames / (float) st.totalFrames * 100.0f
                           : 0.0f;
    /* ⚠️ currentBitrateKbps is bits per second, whatever its name says. */
    const float bitrate_mbps = (float) st.currentBitrateKbps / 1000000.0f;

    char host_s[16], decode_s[16], render_s[16], queue_s[16];
    if (have_host) {
        snprintf(host_s, sizeof host_s, "%.2f", host_ms);
    } else {
        snprintf(host_s, sizeof host_s, "-");
    }
    if (have_decode) {
        snprintf(decode_s, sizeof decode_s, "%.2f", decode_ms);
    } else {
        snprintf(decode_s, sizeof decode_s, "-");
    }
    if (have_render) {
        snprintf(render_s, sizeof render_s, "%.2f", render_ms);
    } else {
        snprintf(render_s, sizeof render_s, "-");
    }
    if (info->has_render_queue && st.videoRenderQueue >= 0) {
        snprintf(queue_s, sizeof queue_s, "%d", st.videoRenderQueue);
    } else {
        snprintf(queue_s, sizeof queue_s, "-");
    }
    const char *audio_ch = audio_stream_info.channels;

    /* ⭐ ONE LINE, key=value, because what reads it is usually a log. */
    reply(job, "OK res=%dx%d codec=\"%s\" hdr=%d window=%u"
               " fps=%.1f recvfps=%.1f frames=%u netdrop=%u loss=%.2f bitrate=%.1f"
               " rtt=%u rttvar=%u host=%s decode=%s render=%s reasm=%.2f queue=%s"
               " total=%.2f audio=\"%s\" af=%u\n",
          info->width, info->height,
          info->format != NULL && info->format[0] != '\0' ? info->format : "-",
          app_configuration->hdr ? 1 : 0,
          streaming_stats_shown() ? 1000u : 2000u,
          st.decodedFps, st.receivedFps,
          (unsigned) st.totalFrames, (unsigned) st.networkDroppedFrames,
          loss_pct, bitrate_mbps,
          (unsigned) rtt, (unsigned) rtt_var,
          host_s, decode_s, render_s, reasm_ms, queue_s, total_ms,
          audio_ch != NULL && audio_ch[0] != '\0' ? audio_ch : "-",
          (unsigned) audio_stream_info.feedFailures);
}

#if defined(TARGET_WEBOS)

static int list_devices(ctm_bridge_dev_t *devs)
{
    /* ⛔ The quiet list outside a stream. The full one wakes the bridge core and
     * broadcasts for a listener that cannot exist yet, which is why the settings
     * pane uses the quiet one too. */
    return ctm_bridge_active() ? ctm_bridge_list(devs, CONTROL_MAX_DEVICES)
                               : ctm_bridge_list_quiet(devs, CONTROL_MAX_DEVICES);
}

/* The devices the parts make up, in the order the panel's rows and the Auto
 * Bridge window's cards show them. ⓘ Static, because commands run one at a
 * time on the UI thread. */
static device_group_t s_groups[DEVICE_GROUPS_MAX];

static int list_groups(const ctm_bridge_dev_t *devs, int n)
{
    return device_groups_build(devs, n, s_groups, DEVICE_GROUPS_MAX);
}

/* The number of the group a part is in, or -1. */
static int group_of_part(int count, int part)
{
    for (int k = 0; k < count; ++k) {
        for (int p = 0; p < s_groups[k].part_count; ++p) {
            if (s_groups[k].part[p] == part) {
                return k;
            }
        }
    }
    return -1;
}

static void cmd_devices(control_job_t *job)
{
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    const int count = list_groups(devs, n);
    reply(job, "OK %d device(s)\n", n);
    for (int i = 0; i < n; ++i) {
        const ctm_bridge_dev_t *d = &devs[i];
        /* ⭐ THE SDL MATCH IS THE POINT OF THESE TWO FIELDS. player and sdl_mac
         * are what the handover can see: -1 and "-" mean no SDL controller was
         * matched to this node, so a bridge takes the direct plug and retires
         * nothing. ⓘ uniq is the core's own field, which is NOT a MAC on a
         * cable or through a dongle. */
        char sdl_mac[64] = "-";
        if (!ctm_bridge_gesture_mac_for_node(d->node, sdl_mac, sizeof sdl_mac)) {
            snprintf(sdl_mac, sizeof sdl_mac, "-");
        }
        /* ⭐ serial is the core's own (uniq, or the USB serial number where no
         * driver filled uniq), and mark is the identity half of what auto bridge
         * keys on: a DualSense's MAC, any other device's serial, "-" when it has
         * none. The name is the other half. */
        char mark[64] = "-";
        if (!auto_bridge_identity(d, mark, sizeof mark)) {
            snprintf(mark, sizeof mark, "-");
        }
        /* ⓘ marked says whether auto bridge would take it at the next stream
         * start, which since 2026-09-14 is decided for its whole device: group
         * is that device's number in groups, and iface the part's interface. */
        const int g = group_of_part(count, i);
        const char *marked = g >= 0 && auto_bridge_ticked(&app_configuration->bridge, devs,
                                                          &s_groups[g]) ? "yes" : "no";
        reply(job, "%d bridged=%s kind=%s id=%s:%s bus=%s node=%s player=%d sdl_mac=%s uniq=%s "
                   "serial=%s controller=%s type=%s mark=%s marked=%s group=%d iface=%d name=\"%s\"\n",
              d->index, d->plugged ? "yes" : "no", d->kind, d->vid, d->pid, d->bus,
              d->node[0] != '\0' ? d->node : "-", ctm_bridge_gesture_player_for_node(d->node),
              sdl_mac, d->mac[0] != '\0' ? d->mac : "-",
              d->serial[0] != '\0' ? d->serial : "-", d->controller ? "yes" : "no",
              d->type[0] != '\0' ? d->type : "-", mark, marked, g, d->iface, d->name);
    }
}

/* ⭐ The devices as the panel's rows show them: the name and the line under it,
 * the state its badge shows, what a mark for it is stored as, and its parts by
 * their numbers in devices. */
static void cmd_groups(control_job_t *job)
{
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    const int count = list_groups(devs, n);
    reply(job, "OK %d device group(s)\n", count);
    for (int k = 0; k < count; ++k) {
        const device_group_t *g = &s_groups[k];
        const char *state = g->plugged == 0 ? "BASIC" : g->plugged == g->part_count ? "FULL" : "PARTIAL";
        const bool marked = auto_bridge_ticked(&app_configuration->bridge, devs, g);
        char key[256];
        auto_bridge_mark_key(g, key, sizeof key);
        reply(job, "%d state=%s bridged=%d/%d identity=%s marked=%s key=\"%s\" shown=\"%s\" name=\"%s\" parts=",
              k, state, g->plugged, g->part_count, g->identity[0] != '\0' ? g->identity : "-",
              marked ? "yes" : "no", key, g->shown, g->name);
        for (int p = 0; p < g->part_count; ++p) {
            reply(job, "%s%d", p > 0 ? "," : "", devs[g->part[p]].index);
        }
        /* ⭐⭐ THE LABEL THE PANEL DRAWS, verbatim (T-226, 2026-09-19).
         *
         * ⛔ THE GAP: this command's own help says it shows a device "as the
         * panel's rows show them", and it showed the name without the type --
         * so the one part of a row that is COMPUTED could not be read from here
         * at all. A change to it could only be checked by looking at a
         * television, which is what this port exists to avoid.
         * ⓘ The same call auto_bridge_window.c makes, so the two cannot
         * disagree: if this prints CONTROLLER (BT), that is what is on screen.
         * ⓘ Per PART, because a device of several parts draws one label each
         * and they differ -- a dongle's keyboard half against its mouse half. */
        reply(job, " labels=");
        for (int p = 0; p < g->part_count; ++p) {
            char label[24];
            device_part_type(&devs[g->part[p]], label, sizeof label);
            reply(job, "%s" "\"" "%s" "\"", p > 0 ? "," : "", label);
        }
        reply(job, "\n");
    }
}

/* Does `sel` name this device? A number is its index, a path is its node,
 * nnnn:nnnn is its vid:pid, and anything else is part of its name. */
static bool device_matches(const ctm_bridge_dev_t *d, const char *sel)
{
    if (is_all_digits(sel)) {
        return d->index == atoi(sel);
    }
    if (strncmp(sel, "/dev/", 5) == 0) {
        return strcmp(d->node, sel) == 0;
    }
    if (strlen(sel) == 9 && sel[4] == ':') {
        return strncasecmp(d->vid, sel, 4) == 0 && strncasecmp(d->pid, sel + 5, 4) == 0;
    }
    return contains_ci(d->name, sel);
}

/* Finds exactly one device. Returns its position in devs, or -1 with the
 * reason already written to the reply. ⛔ Two matches are refused rather than
 * guessed between: bridging the wrong device is the one outcome a test
 * cannot afford to have happen silently. */
static int select_device(control_job_t *job, const ctm_bridge_dev_t *devs, int n, const char *sel)
{
    if (sel == NULL || sel[0] == '\0') {
        reply(job, "ERR name a device: its number, node, vid:pid, or part of its name\n");
        return -1;
    }
    int found = -1;
    int count = 0;
    for (int i = 0; i < n; ++i) {
        if (device_matches(&devs[i], sel)) {
            found = i;
            ++count;
        }
    }
    if (count == 0) {
        reply(job, "ERR no device matches '%s'; see devices\n", sel);
        return -1;
    }
    if (count > 1) {
        reply(job, "ERR '%s' matches %d devices; use a number:", sel, count);
        for (int i = 0; i < n; ++i) {
            if (device_matches(&devs[i], sel)) {
                reply(job, " %d", devs[i].index);
            }
        }
        reply(job, "\n");
        return -1;
    }
    return found;
}

/* A bridge needs a stream, because the listener is the host the stream is
 * from, and a listener known to be down refuses exactly as the panel's row
 * does. Returns false with the reason written. */
static bool bridge_possible(control_job_t *job)
{
    if (!ctm_bridge_active()) {
        reply(job, "ERR no stream is running, and a bridge needs one\n");
        return false;
    }
    if (ctm_bridge_agent_probed() && !ctm_bridge_agent_online()) {
        /* ⓘ The reading can be ten seconds old; ask again, so a retry a second
         * later meets a listener that has just started. */
        ctm_bridge_agent_recheck();
        reply(job, "ERR the listener is offline (checking again now)\n");
        return false;
    }
    /* ⓘ And as the panel does for a side that needs an update. */
    const char *advice = ctm_bridge_agent_update_advice();
    if (advice[0] != '\0') {
        reply(job, "ERR %s\n", advice);
        return false;
    }
    return true;
}

static void report_request(control_job_t *job, const ctm_bridge_dev_t *d, bridge_request_result_t r)
{
    switch (r) {
        case BRIDGE_REQUEST_ASKED:
            reply(job, "OK asked: the handover takes %d (%s) and bridges it shortly\n", d->index, d->name);
            break;
        case BRIDGE_REQUEST_PLUGGED:
            reply(job, "OK plugged directly: %d (%s) -- no SDL controller was matched, so nothing was retired\n",
                  d->index, d->name);
            break;
        case BRIDGE_REQUEST_FAILED:
        default:
            /* ⓘ The core writes why -- a refusal names the node and the errno --
             * to logs/ctm-gesture.log. */
            reply(job, "ERR the plug failed for %d (%s) -- the reason is in logs/ctm-gesture.log\n",
                  d->index, d->name);
            break;
    }
}

static void cmd_bridge(control_job_t *job, const char *sel)
{
    if (!bridge_possible(job)) {
        return;
    }
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    const int i = select_device(job, devs, n, sel);
    if (i < 0) {
        return;
    }
    if (devs[i].plugged) {
        reply(job, "OK already bridged: %d (%s)\n", devs[i].index, devs[i].name);
        return;
    }
    report_request(job, &devs[i], bridge_request_device(&devs[i]));
}

static void cmd_release(control_job_t *job, const char *sel)
{
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    const int i = select_device(job, devs, n, sel);
    if (i < 0) {
        return;
    }
    if (!devs[i].plugged) {
        reply(job, "OK not bridged: %d (%s)\n", devs[i].index, devs[i].name);
        return;
    }
    bridge_release_device(&devs[i]);
    /* ⓘ "releasing": a release finishes on its own thread, after this reply
     * (code review, 2026-10-05); `groups` says when it has. */
    reply(job, "OK releasing: %d (%s)\n", devs[i].index, devs[i].name);
}

static void cmd_bridge_all(control_job_t *job)
{
    if (!bridge_possible(job)) {
        return;
    }
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    int asked = 0;
    for (int i = 0; i < n; ++i) {
        if (!devs[i].plugged) {
            ++asked;
        }
    }
    reply(job, "OK bridging %d device(s)\n", asked);
    for (int i = 0; i < n; ++i) {
        if (!devs[i].plugged) {
            report_request(job, &devs[i], bridge_request_device(&devs[i]));
        }
    }
}

static void cmd_release_all(control_job_t *job)
{
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    int released = 0;
    for (int i = 0; i < n; ++i) {
        if (devs[i].plugged) {
            bridge_release_device(&devs[i]);
            ++released;
        }
    }
    reply(job, "OK releasing %d device(s)\n", released);
}

/* The overlay's DS5-USBIP button, without a hand on it: the same call, so a
 * check from a terminal exercises what a press does. */
static void cmd_open_config(control_job_t *job)
{
    char name[128];
    if (ctm_bridge_open_config(name, sizeof name)) {
        reply(job, "OK asked the host to open its settings window on %s\n", name);
    } else {
        reply(job, "ERR nothing is bridged, so there is no device to open it for\n");
    }
}

/* A device by its number in groups. Returns its position, or -1 with the
 * reason already written. */
static int select_group(control_job_t *job, int count, const char *sel)
{
    if (sel == NULL || !is_all_digits(sel)) {
        reply(job, "ERR name a device by its number in groups\n");
        return -1;
    }
    const int k = atoi(sel);
    if (k < 0 || k >= count) {
        reply(job, "ERR there is no device %s in groups\n", sel);
        return -1;
    }
    return k;
}

static void cmd_bridge_group(control_job_t *job, const char *sel)
{
    if (!bridge_possible(job)) {
        return;
    }
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    const int k = select_group(job, list_groups(devs, n), sel);
    if (k < 0) {
        return;
    }
    const device_group_t *g = &s_groups[k];
    if (g->plugged == g->part_count) {
        reply(job, "OK already bridged: device %d (%s)\n", k, g->name);
        return;
    }
    reply(job, "OK bridging %d of %d part(s) of device %d (%s)\n", g->part_count - g->plugged, g->part_count,
          k, g->name);
    for (int p = 0; p < g->part_count; ++p) {
        const ctm_bridge_dev_t *d = &devs[g->part[p]];
        if (!d->plugged) {
            report_request(job, d, bridge_request_device(d));
        }
    }
}

/* ⭐ THE REFUSAL SIGNAL, WITHOUT A FAILING PLUG (2026-09-23).
 *
 * ⛔ A refusal cannot be reached from `bridge-group`: bridge_possible() replies
 * "the listener is offline" and returns BEFORE any plug is attempted, so the
 * one path that plays it -- the gesture worker, when its plug comes back false
 * -- never runs. Forcing a real failure means breaking the stream, which is the
 * thing under test.
 *
 * ⓘ This calls exactly what the gesture worker calls, on the same node, so
 * what it plays is the refusal itself rather than an imitation of it. On
 * Bluetooth that is the whole signal: the core carries the light, the pulse and
 * the tone on one report. On a cable the flashes come from the gesture worker
 * instead, so what is heard here is the sound alone -- say so rather than
 * reading a missing light as a fault. */
static void cmd_signal(control_job_t *job, const char *args)
{
    char what[16] = "";
    char sel[16] = "";
    if (args == NULL || sscanf(args, "%15s %15s", what, sel) != 2) {
        reply(job, "ERR usage: signal refuse <n>\n");
        return;
    }
    if (strcasecmp(what, "refuse") != 0 && strcasecmp(what, "refused") != 0) {
        reply(job, "ERR only refuse: bridge and release are reached by bridging\n");
        return;
    }
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    const int k = select_group(job, list_groups(devs, n), sel);
    if (k < 0) {
        return;
    }
    const device_group_t *g = &s_groups[k];
    if (g->part_count <= 0) {
        reply(job, "ERR device %d (%s) has no parts\n", k, g->name);
        return;
    }
    const ctm_bridge_dev_t *d = &devs[g->part[0]];
    const bool played = ctm_bridge_signal_refused(d->node);
    reply(job, "OK refusal signal on %s (%s): %s\n", d->node, g->name,
          played ? "played" : "declined -- signals off, or no signal for this kind");
}

static void cmd_release_group(control_job_t *job, const char *sel)
{
    ctm_bridge_dev_t devs[CONTROL_MAX_DEVICES];
    const int n = list_devices(devs);
    const int k = select_group(job, list_groups(devs, n), sel);
    if (k < 0) {
        return;
    }
    const device_group_t *g = &s_groups[k];
    if (g->plugged == 0) {
        reply(job, "OK not bridged: device %d (%s)\n", k, g->name);
        return;
    }
    int released = 0;
    for (int p = 0; p < g->part_count; ++p) {
        const ctm_bridge_dev_t *d = &devs[g->part[p]];
        if (d->plugged) {
            bridge_release_device(d);
            ++released;
        }
    }
    reply(job, "OK releasing %d part(s) of device %d (%s)\n", released, k, g->name);
}

#endif /* TARGET_WEBOS */

static void run_command(control_job_t *job)
{
    const char *p = job->line;
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    char verb[32] = "";
    size_t v = 0;
    while (*p != '\0' && *p != ' ' && *p != '\t' && v + 1 < sizeof verb) {
        verb[v++] = *p++;
    }
    verb[v] = '\0';
    while (*p == ' ' || *p == '\t') {
        ++p;
    }
    const char *args = p;

    if (strcasecmp(verb, "help") == 0) {
        cmd_help(job);
    } else if (strcasecmp(verb, "status") == 0) {
        cmd_status(job);
    } else if (strcasecmp(verb, "hosts") == 0) {
        cmd_hosts(job);
    } else if (strcasecmp(verb, "stream") == 0) {
        cmd_stream(job, args);
    } else if (strcasecmp(verb, "stop") == 0) {
        cmd_stop(job, false);
    } else if (strcasecmp(verb, "quit") == 0) {
        cmd_stop(job, true);
    } else if (strcasecmp(verb, "stats") == 0) {
        cmd_stats(job);
    } else if (strcasecmp(verb, "set") == 0) {
        cmd_set(job, args);
    } else if (strcasecmp(verb, "input") == 0) {
        cmd_input(job);
    } else if (strcasecmp(verb, "keys") == 0) {
        cmd_keys(job);
    } else if (strcasecmp(verb, "shot") == 0) {
        cmd_shot(job, args);
    } else if (strcasecmp(verb, "cursor") == 0) {
        cmd_cursor(job, args);
    } else if (strcasecmp(verb, "prompt") == 0) {
        cmd_prompt(job, args);
    } else if (strcasecmp(verb, "ui") == 0) {
        cmd_ui(job);
    } else if (strcasecmp(verb, "press") == 0) {
        cmd_press(job, args);
    } else if (strcasecmp(verb, "type") == 0) {
        cmd_type(job, args);
    } else if (strcasecmp(verb, "host") == 0) {
        cmd_host(job, args);
    } else if (strcasecmp(verb, "crash") == 0) {
        cmd_crash(job, args);
#if defined(TARGET_WEBOS)
    } else if (strcasecmp(verb, "devices") == 0) {
        cmd_devices(job);
    } else if (strcasecmp(verb, "groups") == 0) {
        cmd_groups(job);
    } else if (strcasecmp(verb, "bridge-group") == 0) {
        cmd_bridge_group(job, args);
    } else if (strcasecmp(verb, "release-group") == 0) {
        cmd_release_group(job, args);
    } else if (strcasecmp(verb, "signal") == 0) {
        cmd_signal(job, args);
    } else if (strcasecmp(verb, "bridge") == 0) {
        cmd_bridge(job, args);
    } else if (strcasecmp(verb, "release") == 0) {
        cmd_release(job, args);
    } else if (strcasecmp(verb, "bridge-all") == 0) {
        cmd_bridge_all(job);
    } else if (strcasecmp(verb, "release-all") == 0) {
        cmd_release_all(job);
    } else if (strcasecmp(verb, "open-config") == 0) {
        cmd_open_config(job);
#endif
    } else {
        reply(job, "ERR unknown command '%s'; try help\n", verb);
    }

    /* ⭐ Every command and the first line of its answer reach the app log, so a
     * terminal-driven run can be read back afterwards like any other. */
    const char *eol = strchr(job->reply, '\n');
    const int first = eol != NULL ? (int) (eol - job->reply) : (int) job->used;
    commons_log_info("Control", "%s -> %.*s", job->line, first, job->reply);
}

/* ------------------------------------------------------- threads and socket */

static void job_free(control_job_t *job)
{
    pthread_mutex_destroy(&job->mutex);
    pthread_cond_destroy(&job->cond);
    free(job);
}

/* On the UI thread, from an LVGL async call: where the panel's own work runs. */
static void job_run(void *data)
{
    control_job_t *job = data;
    run_command(job);
    pthread_mutex_lock(&job->mutex);
    job->done = true;
    const bool abandoned = job->abandoned;
    pthread_cond_signal(&job->cond);
    pthread_mutex_unlock(&job->mutex);
    if (abandoned) {
        job_free(job);
    }
}

/* ⛔ Runs inside the bus dispatch, which holds SDL's event queue: anything slow
 * here stalls every thread that pushes an event. A bridge request enumerates
 * devices and can wait on the listener, so the command itself is handed on to
 * LVGL rather than run here. */
static void job_post_to_ui(void *data)
{
    if (lv_async_call(job_run, data) != LV_RES_OK) {
        job_run(data);
    }
}

static void send_all(int fd, const char *buf, size_t len)
{
    while (len > 0) {
        const ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n <= 0) {
            return;
        }
        buf += n;
        len -= (size_t) n;
    }
}

static void send_text(int fd, const char *text)
{
    send_all(fd, text, strlen(text));
}

static void handle_client(int fd)
{
    const struct timeval timeout = {.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);

    char line[CONTROL_LINE_MAX];
    size_t len = 0;
    while (len + 1 < sizeof line) {
        const ssize_t n = recv(fd, line + len, 1, 0);
        if (n <= 0 || line[len] == '\n') {
            break;
        }
        ++len;
    }
    line[len] = '\0';
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        send_text(fd, "ERR empty command; try help\n");
        return;
    }

    control_job_t *job = calloc(1, sizeof *job);
    if (job == NULL) {
        send_text(fd, "ERR out of memory\n");
        return;
    }
    snprintf(job->line, sizeof job->line, "%s", line);
    pthread_mutex_init(&job->mutex, NULL);
    pthread_cond_init(&job->cond, NULL);

    if (!app_bus_post(s_app, job_post_to_ui, job)) {
        job_free(job);
        send_text(fd, "ERR the app did not accept the command\n");
        return;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += CONTROL_WAIT_SECONDS;
    pthread_mutex_lock(&job->mutex);
    int rc = 0;
    while (!job->done && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&job->cond, &job->mutex, &deadline);
    }
    if (!job->done) {
        /* ⚠️ The command is still queued and will run; it frees itself. */
        job->abandoned = true;
        pthread_mutex_unlock(&job->mutex);
        send_text(fd, "ERR the app did not answer in time; the command may still run\n");
        return;
    }
    pthread_mutex_unlock(&job->mutex);
    send_all(fd, job->reply, job->used);
    job_free(job);
}

static void *server_main(void *arg)
{
    (void) arg;
    for (;;) {
        const int fd = accept(s_listen_fd, NULL, NULL);
        if (fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;   /* the listening socket was closed: the app is going away */
        }
        handle_client(fd);
        close(fd);
    }
    return NULL;
}

void control_server_start(app_t *app)
{
    if (s_listen_fd >= 0 || app == NULL) {
        return;
    }
    s_app = app;
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        commons_log_warn("Control", "terminal control not started: socket: %s", strerror(errno));
        return;
    }
    const int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONTROL_SERVER_PORT);
    /* ⛔ LOOPBACK, NEVER INADDR_ANY: see control_server.h. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *) &addr, sizeof addr) != 0 || listen(fd, 4) != 0) {
        commons_log_warn("Control", "terminal control not started on 127.0.0.1:%d: %s",
                         CONTROL_SERVER_PORT, strerror(errno));
        close(fd);
        return;
    }
    s_listen_fd = fd;
    pthread_t thread;
    if (pthread_create(&thread, NULL, server_main, NULL) != 0) {
        commons_log_warn("Control", "terminal control not started: no thread");
        close(fd);
        s_listen_fd = -1;
        return;
    }
    pthread_detach(thread);
    commons_log_info("Control", "terminal control listening on 127.0.0.1:%d", CONTROL_SERVER_PORT);
}

void control_server_stop(void)
{
    if (s_listen_fd < 0) {
        return;
    }
    /* ⓘ shutdown() wakes the blocked accept(); close() alone may not. */
    shutdown(s_listen_fd, SHUT_RDWR);
    close(s_listen_fd);
    s_listen_fd = -1;
}

#endif /* AURORA_TERMINAL_CONTROL */
