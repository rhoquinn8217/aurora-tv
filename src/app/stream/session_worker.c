#include "session_worker.h"
#include "session_priv.h"
#include "app.h"
#include "util/bus.h"
#include "logging.h"
#include "errors.h"
#include "util/user_event.h"
#include "input/input_gamepad.h"
#include "stream/connection/session_connection.h"
#include "stream/audio/session_audio.h"
#include "stream/video/session_video.h"
#include "stream/adaptive_bitrate.h"
#include "app_session.h"
#include "backend/pcmanager/worker/worker.h"
#include "app_settings.h"

#if TARGET_WEBOS
#include "platform/webos/game_mode.h"
#include "platform/webos/stream_priority.h"
#endif

#include <stdlib.h>
#include <stdio.h>

static void session_apply_decoder_env(const session_t *session) {
#if TARGET_WEBOS
    /* C5 native compositor is 120Hz. SDL_webOSGetRefreshRate often returns 144
     * (HDMI VRR max). Snapping PTS to 144Hz on a 120Hz plane is 5:6 pulldown —
     * pan hitch at every FPS. Wall-clock PTS tested best on-device for PAN. */
    setenv("SS4S_SMOOTH_PACING", "0", 1);
    setenv("SS4S_NDL_SMOOTH_PACING", "0", 1);
    setenv("SS4S_PANEL_PHASE_PACING", "0", 1);
    unsetenv("SS4S_SMOOTH_PACING_HOST_ONLY");
    unsetenv("SS4S_PRESENTATION_OFFSET_US");
    unsetenv("SS4S_SMOOTH_PACING_INTERVAL_US");
    unsetenv("SS4S_NDL_PACING_INTERVAL_US");
    unsetenv("SS4S_SMOOTH_PACING_MAX_DRIFT_FRAMES");
    unsetenv("SS4S_PANEL_PHASE_INTERVAL_US");
    setenv("SS4S_PAUSE_AT_DECODE_TIME", "1", 1);
    commons_log_info("Session", "Stream pacing: wall-clock PTS (native 120Hz, no 144 VRR grid)");
    (void) session;
#else
    (void) session;
#endif
}

/* Auto-reconnect policy: a stream that dies with a network error is resumed in
 * place (no USER_STREAM_CLOSE/FINISHED, so input and the CTM bridge stay up and
 * the UI shows "Connecting..." instead of bouncing back to the launcher).
 * Bounded by attempts AND wall time; a stream that stayed up for a while resets
 * the attempt budget so an occasional blip never exhausts it (flap guard). */
#define SESSION_RECONNECT_MAX_ATTEMPTS 8
#define SESSION_RECONNECT_MAX_ELAPSED_MS 90000
#define SESSION_RECONNECT_STABLE_MS 30000

static bool session_worker_reconnect_allowed(int attempts, Uint32 since);

static bool session_worker_reconnect_wait(session_t *session, int attempt);

int session_worker(session_t *session) {
    app_t *app = session->app;
    session_set_state(session, STREAMING_CONNECTING);
    bus_pushevent(USER_STREAM_CONNECTING, NULL, NULL);
    streaming_error(session, GS_OK, "");
    PSERVER_DATA server = session->server;
    int appId = session->app_id;
    session->player = NULL;
    /* streamed = USER_STREAM_OPEN has been pushed; from then on every exit must
     * go through shutdown so USER_STREAM_CLOSE is pushed exactly once (input /
     * CTM bridge stop and UI reopen hang off it). */
    bool streamed = false;
    bool li_active = false;
    int reconnect_attempts = 0;
    Uint32 reconnect_since = 0;
    Uint32 stream_up_since = 0;
#if TARGET_WEBOS
    webos_game_mode_state_t *game_mode_state = NULL;
    webos_stream_priority_state_t *stream_prio = NULL;
    session->webos_game_mode = NULL;
#endif

#if FEATURE_INPUT_EVMOUSE
    if (!session->config.view_only && session->config.hardware_mouse) {
        session_evmouse_wait_ready(&session->input.evmouse);
    }
#endif

#if TARGET_WEBOS
    /* USB/net buffers must land before UDP sockets (LiStartConnection). */
    stream_prio = webos_stream_priority_enter();
#endif

    commons_log_info("Session", "Launch app %d (host currentGame=%d)...", appId, server->currentGame);
    if (session->config.stream.clientRefreshRateX100 > 0) {
        commons_log_info("Session",
                         "Stream mode %dx%dx%d, clientRefreshRateX100=%d (%.2f Hz), bitrate %d kbps",
                         session->config.stream.width, session->config.stream.height,
                         session->config.stream.fps, session->config.stream.clientRefreshRateX100,
                         session->config.stream.clientRefreshRateX100 / 100.0,
                         session->config.stream.bitrate);
    } else {
        commons_log_info("Session", "Stream mode %dx%dx%d, bitrate %d kbps (no fractional refresh rate)",
                         session->config.stream.width, session->config.stream.height,
                         session->config.stream.fps, session->config.stream.bitrate);
    }
    GS_CLIENT client = app_gs_client_new(app);
    /* Host keeps SDL/Vorbis channel order. webOS SMP/NDL remaps PCM to device
     * order in Feed (SS4S_WebOS_RemapPcm51ToDevice). Do not send surroundParams. */
    const char *surround_params = NULL;
    short gamepad_mask;
    int ret;
    connect:
    gamepad_mask = app_input_gamepads_mask(&app->input);
    ret = gs_start_app(client, server, &session->config.stream, appId, server->isGfe, session->config.sops,
                       session->config.local_audio, gamepad_mask, surround_params);
    if (ret != GS_OK) {
        const char *gs_error = NULL;
        gs_get_error(&gs_error);
        commons_log_error("Session", "Failed to launch session: gamestream returned %d, gs_error=%s", ret, gs_error);
        if (streamed && session_worker_reconnect_allowed(reconnect_attempts, reconnect_since)) {
            if (session_worker_reconnect_wait(session, ++reconnect_attempts)) {
                goto connect;
            }
            // Interrupted while waiting (user/background/quit): leave quietly
            streaming_error(session, GS_OK, "");
            goto shutdown;
        }
        if (!streamed) {
            session_set_state(session, STREAMING_ERROR);
        }
        if (gs_error) {
            streaming_error(session, ret, "Failed to launch session: %s (code %d)", gs_error, ret);
        } else {
            streaming_error(session, ret, "Failed to launch session: gamestream returned %d", ret);
        }
        if (streamed) {
            goto shutdown;
        }
        goto thread_cleanup;
    }

    commons_log_info("Session", "Audio %d channels",
                     CHANNEL_COUNT_FROM_AUDIO_CONFIGURATION(session->config.stream.audioConfiguration));

    session->player = SS4S_PlayerOpen();
    SS4S_PlayerSetWaitAudioVideoReady(session->player, true);
    SS4S_PlayerSetViewportSize(session->player, app->ui.width, app->ui.height);
    SS4S_PlayerSetUserdata(session->player, app);

    session_apply_decoder_env(session);
    session_video_prepare_stream();

#if TARGET_WEBOS
    if (session->app->settings.game_mode) {
        const bool hdr = session->app->settings.hdr &&
                         (session->config.stream.supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) != 0;
        game_mode_state = webos_game_mode_enter(hdr);
        session->webos_game_mode = game_mode_state;
    }
#endif

    int startResult = LiStartConnection(&server->serverInfo, &session->config.stream,
                                        session_connection_callbacks_prepare(session),
                                        &ss4s_dec_callbacks, &ss4s_aud_callbacks, session, 0, session, 0);
    if (startResult != 0) {
        commons_log_error("Session", "Failed to start connection: Limelight returned %d", startResult);
        if (streamed && session_worker_reconnect_allowed(reconnect_attempts, reconnect_since)) {
            SS4S_PlayerClose(session->player);
            session->player = NULL;
            if (session_worker_reconnect_wait(session, ++reconnect_attempts)) {
                goto connect;
            }
            streaming_error(session, GS_OK, "");
            goto shutdown;
        }
        if (!streamed) {
            session_set_state(session, STREAMING_ERROR);
        }
        switch (startResult) {
            case CALLBACKS_SESSION_ERROR_VDEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported video codec.");
                break;
            case CALLBACKS_SESSION_ERROR_VDEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open video decoder.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_UNSUPPORTED:
                streaming_error(session, GS_WRONG_STATE, "Unsupported audio codec.");
                break;
            case CALLBACKS_SESSION_ERROR_ADEC_ERROR:
                streaming_error(session, GS_WRONG_STATE, "Failed to open audio backend.");
                break;
            default: {
                if (!streaming_errno) {
                    streaming_error(session, GS_WRONG_STATE, "Failed to start connection: Limelight returned %d (%s)",
                                    startResult, strerror(startResult));
                }
                break;
            }
        }
        if (streamed) {
            goto shutdown;
        }
        goto thread_cleanup;
    }
    li_active = true;
    session_set_state(session, STREAMING_STREAMING);
    bus_pushevent(USER_STREAM_OPEN, NULL, NULL);
    if (session->config.auto_adjust_bitrate) {
        adaptive_bitrate_config_t abr_config = {
            .gs_client = client,
            .server = server,
            .initial_bitrate = session->config.stream.bitrate,
            .mode = (abr_mode_t) session->config.abr_mode,
            .recovery_only = false,
        };
        session->abr = adaptive_bitrate_start(&abr_config);
    }
    streamed = true;
    stream_up_since = SDL_GetTicks();
    SDL_LockMutex(session->mutex);
    while (!session->interrupted) {
        // Wait until interrupted
        SDL_CondWait(session->cond, session->mutex);
    }
    /* Decide on auto-reconnect while still holding the mutex, so re-arming
     * interrupted can't race a concurrent session_interrupt(). */
    bool reconnect = session->interrupt_reason == STREAMING_INTERRUPT_NETWORK && !session->quitapp;
    if (reconnect) {
        if (SDL_TICKS_PASSED(SDL_GetTicks(), stream_up_since + SESSION_RECONNECT_STABLE_MS)) {
            reconnect_attempts = 0;
        }
        if (reconnect_attempts == 0) {
            reconnect_since = SDL_GetTicks();
        }
        reconnect = session_worker_reconnect_allowed(reconnect_attempts, reconnect_since);
        if (reconnect) {
            session->interrupted = false;
        }
    }
    SDL_UnlockMutex(session->mutex);
    if (reconnect) {
        commons_log_warn("Session", "Connection lost with a network error; trying to resume");
        session_set_state(session, STREAMING_CONNECTING);
        bus_pushevent(USER_STREAM_CONNECTING, NULL, NULL);
        LiStopConnection();
        li_active = false;
        /* Host is unreachable: stop the ABR service without the restore
         * round-trips; a successful reconnect starts a fresh one. */
        adaptive_bitrate_stop(session->abr, false);
        session->abr = NULL;
        SS4S_PlayerClose(session->player);
        session->player = NULL;
        streaming_error(session, GS_OK, "");
        if (session_worker_reconnect_wait(session, ++reconnect_attempts)) {
#if FEATURE_INPUT_EVMOUSE
            if (!session->config.view_only && session->config.hardware_mouse) {
                // The network interrupt stopped the evmouse worker; bring it back
                session_evmouse_restart(&session->input.evmouse);
                session_evmouse_wait_ready(&session->input.evmouse);
            }
#endif
            goto connect;
        }
        // Interrupted during the backoff: exit quietly through normal shutdown
    }
    shutdown:
    bus_pushevent(USER_STREAM_CLOSE, NULL, NULL);

    session_set_state(session, STREAMING_DISCONNECTING);
    if (li_active) {
        LiStopConnection();
        li_active = false;
    }

    if (session->quitapp) {
        commons_log_info("Session", "Sending app quit request ...");
        gs_quit_app(client, server);
    }
    worker_context_t update_ctx = {
            .app = app,
            .manager = pcmanager,
    };
    uuidstr_fromstr(&update_ctx.uuid, server->uuid);
    pcmanager_update_by_host(&update_ctx, server->serverInfo.address, server->extPort, true);

    // Keep the error state (if any) so the finish handler shows the dialog
    session_set_state(session, streaming_errno != GS_OK ? STREAMING_ERROR : STREAMING_NONE);
    thread_cleanup:
#if TARGET_WEBOS
    session->webos_game_mode = NULL;
    webos_game_mode_restore(game_mode_state);
    game_mode_state = NULL;
    webos_stream_priority_leave(stream_prio);
    stream_prio = NULL;
#endif
    /* Restore only on a clean exit: streaming_errno != GS_OK means the session
     * ended in error/disconnect and the host is likely unreachable -- the
     * restore round-trips would just block teardown on timeouts. */
    adaptive_bitrate_stop(session->abr, streaming_errno == GS_OK);
    session->abr = NULL;
    session_connection_callbacks_reset(session);
    if (session->player != NULL) {
        SS4S_PlayerClose(session->player);
    }
    gs_destroy(client);
    bus_pushevent(USER_STREAM_FINISHED, NULL, NULL);
    app_bus_post(app, (bus_actionfunc) app_session_destroy, app);
    return 0;
}

static bool session_worker_reconnect_allowed(int attempts, Uint32 since) {
    return attempts < SESSION_RECONNECT_MAX_ATTEMPTS &&
           !SDL_TICKS_PASSED(SDL_GetTicks(), since + SESSION_RECONNECT_MAX_ELAPSED_MS);
}

/* Interruptible backoff before a reconnect attempt. Returns true when the wait
 * elapsed and the retry should proceed, false when the session got interrupted
 * (user quit/suspend, background, app shutdown) in the meantime. */
static bool session_worker_reconnect_wait(session_t *session, int attempt) {
    Uint32 delay = attempt <= 1 ? 500 : (Uint32) (attempt - 1) * 1000;
    if (delay > 5000) {
        delay = 5000;
    }
    commons_log_info("Session", "Reconnect attempt %d/%d in %u ms", attempt, SESSION_RECONNECT_MAX_ATTEMPTS, delay);
    SDL_LockMutex(session->mutex);
    Uint32 deadline = SDL_GetTicks() + delay;
    while (!session->interrupted) {
        Uint32 now = SDL_GetTicks();
        if (SDL_TICKS_PASSED(now, deadline)) {
            break;
        }
        SDL_CondWaitTimeout(session->cond, session->mutex, deadline - now);
    }
    bool proceed = !session->interrupted;
    SDL_UnlockMutex(session->mutex);
    return proceed;
}
