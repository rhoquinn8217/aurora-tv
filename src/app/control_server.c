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

#include "app.h"
#include "app_version.h"
#include "logging.h"
#include "util/bus.h"
#include "backend/pcmanager.h"
#include "stream/session.h"
#include "stream/video/session_video.h"
#include "ui/streaming/streaming.controller.h"

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
               "set <name> <on|off>     a switch, without the remote: boost, light, rumble, tone\n"
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
 * In memory only: it does NOT write moonlight.ini, so a restart puts the
 * settings back and no test can leave a set permanently altered. */
static void cmd_set(control_job_t *job, const char *args)
{
    char name[32] = "";
    char value[16] = "";
    if (args == NULL || sscanf(args, "%31s %15s", name, value) != 2) {
        reply(job, "ERR usage: set <boost|light|rumble|tone> <on|off>\n");
        return;
    }
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
#if defined(TARGET_WEBOS)
    if (strcasecmp(name, "light") == 0 || strcasecmp(name, "rumble") == 0 ||
        strcasecmp(name, "tone") == 0) {
        if (strcasecmp(name, "light") == 0)  app_configuration->bridge_signal_light = on;
        if (strcasecmp(name, "rumble") == 0) app_configuration->bridge_signal_rumble = on;
        if (strcasecmp(name, "tone") == 0)   app_configuration->bridge_signal_tone = on;
        ctm_bridge_set_signals(app_configuration->bridge_signal_light,
                               app_configuration->bridge_signal_rumble,
                               app_configuration->bridge_signal_tone);
        reply(job, "OK light=%s rumble=%s tone=%s\n",
              app_configuration->bridge_signal_light ? "on" : "off",
              app_configuration->bridge_signal_rumble ? "on" : "off",
              app_configuration->bridge_signal_tone ? "on" : "off");
        return;
    }
#endif
    reply(job, "ERR try boost, light, rumble or tone\n");
}

static void cmd_status(control_job_t *job)
{
    reply(job, "OK build=\"%s\" stream=%s", APP_VERSION, s_app->session != NULL ? "active" : "idle");
#if defined(TARGET_WEBOS)
    const char *listener = !ctm_bridge_agent_probed() ? "unknown"
                         : ctm_bridge_agent_online() ? "online" : "offline";
    char agent[128] = "";
    ctm_bridge_agent(agent, sizeof agent);
    reply(job, " bridge=%s listener=%s agent=\"%s\"",
          ctm_bridge_active() ? "running" : "stopped", listener, agent);
#endif
    reply(job, "\n");
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
     * The name is what the stream shows; the app id is what launches. */
    streaming_scene_arg_t scene = {
            .global = s_app,
            .uuid = match->id,
            .app = {.name = (char *) "terminal", .id = atoi(app_arg)},
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
        const char *marked = g >= 0 && auto_bridge_marked(app_configuration->bridge_auto_macs, devs,
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
        const bool marked = auto_bridge_marked(app_configuration->bridge_auto_macs, devs, g);
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
        reply(job, "ERR the listener is offline\n");
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
    reply(job, "OK released: %d (%s)\n", devs[i].index, devs[i].name);
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
    reply(job, "OK released %d device(s)\n", released);
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
    reply(job, "OK released %d part(s) of device %d (%s)\n", released, k, g->name);
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
#if defined(TARGET_WEBOS)
    } else if (strcasecmp(verb, "devices") == 0) {
        cmd_devices(job);
    } else if (strcasecmp(verb, "groups") == 0) {
        cmd_groups(job);
    } else if (strcasecmp(verb, "bridge-group") == 0) {
        cmd_bridge_group(job, args);
    } else if (strcasecmp(verb, "release-group") == 0) {
        cmd_release_group(job, args);
    } else if (strcasecmp(verb, "bridge") == 0) {
        cmd_bridge(job, args);
    } else if (strcasecmp(verb, "release") == 0) {
        cmd_release(job, args);
    } else if (strcasecmp(verb, "bridge-all") == 0) {
        cmd_bridge_all(job);
    } else if (strcasecmp(verb, "release-all") == 0) {
        cmd_release_all(job);
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
