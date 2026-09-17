/* A command port for driving the app from a terminal.
 *
 * ⭐ WHY IT EXISTS. A hardware run needs the app launched, a stream started and
 * devices bridged, and every one of those needed someone at the TV with the
 * remote. This lets an SSH session on the TV do all of it, so a test can be
 * driven and read from the PC.
 *
 * ⛔ LOOPBACK ONLY. It listens on 127.0.0.1 and nowhere else, so only something
 * already running on the TV -- an SSH session, in practice -- can reach it.
 * Nothing on the network can. Built only with AURORA_TERMINAL_CONTROL.
 *
 * ⓘ One line in, one reply out, then the connection closes. From an SSH
 * session on the TV:
 *
 *     (printf 'devices\n'; sleep 1) | nc 127.0.0.1 48070
 *
 * ⚠️ The sleep matters: busybox nc stops reading when its input ends, and a
 * reply sent after that is lost. `help` lists the commands, and every command
 * and the first line of its reply go to the app log, /var/log/dbg-log. */

#ifndef CONTROL_SERVER_H
#define CONTROL_SERVER_H

typedef struct app_t app_t;

#define CONTROL_SERVER_PORT 48070

#if defined(AURORA_TERMINAL_CONTROL)

/* Starts listening. Call once the UI and the bus are up. A port already in use
 * is logged and left alone: the app runs normally without it. */
void control_server_start(app_t *app);

/* Stops listening. A command already waiting on the UI is abandoned. */
void control_server_stop(void);

#else

static inline void control_server_start(app_t *app) { (void) app; }

static inline void control_server_stop(void) {}

#endif

#endif /* CONTROL_SERVER_H */
