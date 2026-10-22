/*
 * The crash recorder: when the app dies of a fault, a record of where.
 *
 * ⭐ WHY IT EXISTS (2026-10-20). The app crashed on the C1 as Play started a
 * stream, and nothing said where. webOS only marks that a crash happened
 * (/tmp/crash_component), the C1 keeps no /var/log/dbg-log, and the app had no
 * handler of its own. So the same crash could only be guessed at from the logs
 * around it.
 *
 * ⓘ On SIGSEGV, SIGBUS, SIGILL, SIGFPE and SIGABRT it appends one block to
 * logs/crash.log, beside the bridge core's logs: the time on the same clock as
 * tv-keyboard.log, the build, the signal and the address, the thread, the
 * registers, the code addresses found on the stack, and the code that was
 * mapped. Addresses inside the app are named by function from the app's own
 * symbol table; ones in a library carry the library and an offset into it.
 * Then the crash goes on to whatever handled it before, so webOS still records
 * it as it always has.
 *
 * ⚠️ THE STACK IS SCANNED, NOT UNWOUND. The webOS build has no unwind tables
 * (its .ARM.exidx is 8 bytes), so backtrace() stops at the first frame. Every
 * word on the stack that points into code is listed instead, nearest first:
 * the real callers are among them, beside stale words from earlier calls.
 */
#pragma once

#include <stdbool.h>

/* Arms the recorder, or arms it again if something has replaced it since.
 * Called before SDL_Init and again once the app is up, since a library loaded
 * in between may install a handler of its own. */
void crash_recorder_install(void);

/* One line for the control port: armed or not, and where the record goes. */
void crash_recorder_describe(char *buf, int size);

/* Crashes the app on purpose, to prove the recorder: on the app's own loop
 * half a second from now, or on a thread of its own. */
void crash_recorder_test(bool on_thread);

/* From the app's loop: the half-second crash above, when one was asked for. */
void crash_recorder_tick(void);
