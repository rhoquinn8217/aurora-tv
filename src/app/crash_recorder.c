/* The crash recorder; crash_recorder.h says what it records and why. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "crash_recorder.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL.h>

#include "app.h"

#if defined(__linux__)

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#endif

#ifndef APP_VERSION
#define APP_VERSION "unknown"
#endif

/* ⓘ The bridge core's: <app>/logs/<name>, the folder every log of ours is in. */
const char *ctm_log_path(const char *name);

#define CRASH_LOG "crash.log"
/* Past this at start, the old records are set aside as crash.old.log. */
#define CRASH_LOG_KEEP_BYTES (256L * 1024L)

static Uint32 s_test_at = 0;

/* ⭐ Its own function, never inlined, so a test's record names it. */
__attribute__((noinline)) static void crash_now(void) {
    volatile int *volatile nowhere = NULL;
    *nowhere = 1;
}

static int crash_thread(void *unused) {
    (void) unused;
    SDL_Delay(500);
    crash_now();
    return 0;
}

void crash_recorder_test(bool on_thread) {
    if (on_thread) {
        SDL_Thread *thread = SDL_CreateThread(crash_thread, "crash-test", NULL);
        if (thread != NULL) {
            SDL_DetachThread(thread);
        }
        return;
    }
    /* Half a second on: the control port's reply has gone by then. */
    s_test_at = SDL_GetTicks() + 500;
    if (s_test_at == 0) {
        s_test_at = 1;
    }
}

void crash_recorder_tick(void) {
    if (s_test_at != 0 && SDL_TICKS_PASSED(SDL_GetTicks(), s_test_at)) {
        s_test_at = 0;
        crash_now();
    }
}

#if defined(__linux__)

/* ⛔⛔ EVERYTHING FROM on_crash DOWN RUNS INSIDE A SIGNAL HANDLER, in a process
 * that has just faulted. Only calls that are safe there: open, write, read,
 * pread, close, sigaction, raise, prctl, syscall, clock_gettime. No stdio, no
 * malloc, no locks, no SDL. Its buffers are static: one crash is written at a
 * time (s_handling), and the next thread to fault is not let in. */

static const int k_signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
static const char *const k_signal_names[] = {"SIGSEGV", "SIGBUS", "SIGILL", "SIGFPE", "SIGABRT"};
#define SIGNAL_COUNT ((int) (sizeof k_signals / sizeof k_signals[0]))

/* What each signal did before this recorder: a crash is handed on to it. */
static struct sigaction s_previous[SIGNAL_COUNT];
static volatile sig_atomic_t s_handling = 0;

static char s_log_path[512];

/* The app's own symbol table, read from /proc/self/exe when a crash comes.
 * ⓘ The package keeps it (the build does not strip), so a record can name the
 * function an address is in without anything outside the TV. */
static int s_exe_fd = -1;
static uint64_t s_symtab_off, s_symtab_size, s_strtab_off, s_strtab_size;
static uintptr_t s_exe_bias;   /* 0 for a fixed-address build, the load address for a PIE */
extern char __executable_start[];
extern char etext[];

#if defined(__arm__)
/* A Thumb function's symbol has its lowest bit set; the code itself does not. */
#define CODE_MASK (~(uintptr_t) 1)
#else
#define CODE_MASK (~(uintptr_t) 0)
#endif

#define ALT_STACK_BYTES (64 * 1024)
static char s_alt_stack[ALT_STACK_BYTES];

/* ------------------------------------------------------------- the writer */

static struct {
    int fd;
    int used;
    char buf[1024];
} s_out;

static void out_flush(void) {
    int done = 0;
    while (done < s_out.used) {
        const ssize_t n = write(s_out.fd, s_out.buf + done, (size_t) (s_out.used - done));
        if (n <= 0) {
            break;
        }
        done += (int) n;
    }
    s_out.used = 0;
}

static void out_mem(const char *s, int n) {
    for (int i = 0; i < n; ++i) {
        if (s_out.used == (int) sizeof s_out.buf) {
            out_flush();
        }
        s_out.buf[s_out.used++] = s[i];
    }
}

static void out_str(const char *s) {
    out_mem(s, (int) strlen(s));
}

static void out_dec(long long v, int width) {
    char digits[24];
    int n = 0;
    const bool negative = v < 0;
    unsigned long long u = negative ? 0ull - (unsigned long long) v : (unsigned long long) v;
    do {
        digits[n++] = (char) ('0' + u % 10u);
        u /= 10u;
    } while (u != 0 && n < (int) sizeof digits);
    while (n < width && n < (int) sizeof digits) {
        digits[n++] = '0';
    }
    if (negative) {
        out_str("-");
    }
    while (n > 0) {
        out_mem(&digits[--n], 1);
    }
}

static void out_hex(uintptr_t v) {
    char digits[2 * sizeof v];
    for (int i = (int) sizeof digits - 1; i >= 0; --i) {
        digits[i] = "0123456789abcdef"[v & 0xfu];
        v >>= 4;
    }
    out_str("0x");
    out_mem(digits, (int) sizeof digits);
}

/* ------------------------------------------------------- the memory map */

#define MAPS_BUF_BYTES (128 * 1024)
#define MAX_MAPS 512

static char s_maps_buf[MAPS_BUF_BYTES];
static struct {
    uintptr_t start, end, offset;
    bool code;
    const char *path;
    int path_len;
} s_maps[MAX_MAPS];
static int s_map_count;

static uintptr_t parse_hex(const char **p) {
    uintptr_t v = 0;
    for (;; ++*p) {
        const char c = **p;
        if (c >= '0' && c <= '9') {
            v = (v << 4) | (uintptr_t) (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v = (v << 4) | (uintptr_t) (c - 'a' + 10);
        } else {
            return v;
        }
    }
}

/* ⛔ ONLY WHAT IS WANTED IS KEPT: code from a named file, and the mapping the
 * stack pointer is in. The app has more than 500 mappings (568 on the C1,
 * 2026-10-21), and a table of every one filled before the main thread's stack,
 * which comes last, so its stack was never scanned. */
static void read_maps(uintptr_t sp) {
    s_map_count = 0;
    const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    size_t used = 0;
    for (;;) {
        const ssize_t n = read(fd, s_maps_buf + used, sizeof s_maps_buf - 1 - used);
        if (n <= 0) {
            break;
        }
        used += (size_t) n;
    }
    close(fd);
    s_maps_buf[used] = '\0';

    /* 00010000-0013f000 r-xp 00000000 b3:0d 1234   /path/to/file */
    const char *p = s_maps_buf;
    while (*p != '\0' && s_map_count < MAX_MAPS) {
        const char *eol = strchr(p, '\n');
        if (eol == NULL) {
            break;   /* a line cut off by the buffer's end */
        }
        const uintptr_t start = parse_hex(&p);
        if (*p == '-') {
            ++p;
            const uintptr_t end = parse_hex(&p);
            if (*p == ' ' && eol - p > 6) {
                ++p;
                const bool code = p[2] == 'x';
                p += 5;
                const uintptr_t offset = parse_hex(&p);
                /* Past the device and the inode, to the path if there is one. */
                for (int field = 0; field < 2 && p < eol; ++field) {
                    while (p < eol && *p == ' ') {
                        ++p;
                    }
                    while (p < eol && *p != ' ') {
                        ++p;
                    }
                }
                while (p < eol && *p == ' ') {
                    ++p;
                }
                /* ⓘ "Code" is a file's: on this system the stacks are
                 * executable too, and an anonymous or [stack] mapping made
                 * every pointer into a stack look like a return address. */
                const bool named_code = code && eol > p && *p != '[';
                if (named_code || (sp >= start && sp < end)) {
                    s_maps[s_map_count].start = start;
                    s_maps[s_map_count].end = end;
                    s_maps[s_map_count].offset = offset;
                    s_maps[s_map_count].code = named_code;
                    s_maps[s_map_count].path = p;
                    s_maps[s_map_count].path_len = (int) (eol - p);
                    ++s_map_count;
                }
            }
        }
        p = eol + 1;
    }
}

static int map_of(uintptr_t addr) {
    for (int i = 0; i < s_map_count; ++i) {
        if (addr >= s_maps[i].start && addr < s_maps[i].end) {
            return i;
        }
    }
    return -1;
}

static bool in_app(uintptr_t addr) {
    return addr >= (uintptr_t) __executable_start && addr < (uintptr_t) etext;
}

static bool is_code(uintptr_t addr) {
    if (in_app(addr)) {
        return true;
    }
    const int m = map_of(addr);
    return m >= 0 && s_maps[m].code;
}

/* -------------------------------------------- code addresses, and their names */

#define MAX_CODE 48

static struct {
    uintptr_t value;    /* as found: a register or a word on the stack */
    uintptr_t lookup;   /* where its function is looked for */
    uintptr_t where;    /* the stack offset it was found at */
    uintptr_t exact_start, near_start;
    uint32_t exact_name, near_name;
    bool exact, near;
} s_code[MAX_CODE];
static int s_code_count;

static int add_code(uintptr_t value, bool return_address, uintptr_t where) {
    if (s_code_count >= MAX_CODE) {
        return -1;
    }
    const int i = s_code_count++;
    memset(&s_code[i], 0, sizeof s_code[i]);
    s_code[i].value = value;
    /* ⓘ A return address is the instruction AFTER the call, which can be past
     * the end of a function that ends in a call: one back stays inside it. */
    s_code[i].lookup = (value & CODE_MASK) - (return_address ? 1u : 0u);
    s_code[i].where = where;
    return i;
}

/* One pass over the app's symbol table, for every address at once. */
static void name_code(void) {
    if (s_exe_fd < 0 || s_symtab_size == 0) {
        return;
    }
    static ElfW(Sym) chunk[256];
    for (uint64_t off = 0; off < s_symtab_size;) {
        uint64_t want = s_symtab_size - off;
        if (want > sizeof chunk) {
            want = sizeof chunk;
        }
        const ssize_t got = pread(s_exe_fd, chunk, (size_t) want, (off_t) (s_symtab_off + off));
        if (got < (ssize_t) sizeof chunk[0]) {
            return;
        }
        const int n = (int) ((size_t) got / sizeof chunk[0]);
        off += (uint64_t) n * sizeof chunk[0];
        for (int s = 0; s < n; ++s) {
            const ElfW(Sym) *sym = &chunk[s];
            if ((sym->st_info & 0xf) != STT_FUNC || sym->st_shndx == SHN_UNDEF || sym->st_name == 0) {
                continue;
            }
            const uintptr_t start = ((uintptr_t) sym->st_value & CODE_MASK) + s_exe_bias;
            for (int c = 0; c < s_code_count; ++c) {
                const uintptr_t a = s_code[c].lookup;
                if (!in_app(a) || a < start) {
                    continue;
                }
                if (sym->st_size != 0 && a < start + (uintptr_t) sym->st_size) {
                    if (!s_code[c].exact || start > s_code[c].exact_start) {
                        s_code[c].exact = true;
                        s_code[c].exact_start = start;
                        s_code[c].exact_name = sym->st_name;
                    }
                } else if (!s_code[c].near || start > s_code[c].near_start) {
                    s_code[c].near = true;
                    s_code[c].near_start = start;
                    s_code[c].near_name = sym->st_name;
                }
            }
        }
    }
}

static void out_code(int c) {
    const uintptr_t value = s_code[c].value;
    out_hex(value);
    out_str(" ");
    if (in_app(s_code[c].lookup)) {
        const bool named = s_code[c].exact || s_code[c].near;
        if (named && s_strtab_size != 0) {
            const uint32_t name_at = s_code[c].exact ? s_code[c].exact_name : s_code[c].near_name;
            const uintptr_t start = s_code[c].exact ? s_code[c].exact_start : s_code[c].near_start;
            static char name[96];
            const ssize_t got = pread(s_exe_fd, name, sizeof name - 1, (off_t) (s_strtab_off + name_at));
            name[got > 0 ? got : 0] = '\0';
            out_str(name[0] != '\0' ? name : "?");
            out_str(s_code[c].exact ? "+" : " (or past it) +");
            out_hex((value & CODE_MASK) - start);
        } else {
            out_str("in the app, no symbol");
        }
        return;
    }
    const int m = map_of(s_code[c].lookup);
    if (m < 0) {
        out_str("not in any mapping");
        return;
    }
    const char *path = s_maps[m].path;
    int len = s_maps[m].path_len;
    for (int i = len - 1; i >= 0; --i) {
        if (path[i] == '/') {
            path += i + 1;
            len -= i + 1;
            break;
        }
    }
    out_mem(path, len);
    out_str("+");
    out_hex((value & CODE_MASK) - s_maps[m].start + s_maps[m].offset);
}

/* --------------------------------------------------------------- the record */

static const char *code_words(int sig, int code) {
    if (code <= 0) {
        return code == SI_TKILL ? "sent by the app itself (abort or raise)" : "sent, not a fault";
    }
    switch (sig) {
        case SIGSEGV:
            return code == SEGV_MAPERR ? "nothing is mapped at the address"
                                       : "the address is mapped, but not for this access";
        case SIGBUS:
            return code == BUS_ADRALN ? "a misaligned address" : "an address with no memory behind it";
        case SIGILL:
            return "an instruction the processor refused";
        case SIGFPE:
            return code == FPE_INTDIV ? "an integer divided by zero" : "an arithmetic fault";
        default:
            return "a fault";
    }
}

static void record(int index, const siginfo_t *info, const void *context) {
    s_out.fd = open(s_log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (s_out.fd < 0) {
        s_out.fd = STDERR_FILENO;
    }
    s_out.used = 0;

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    out_str("=== crash at ");
    out_dec((long long) now.tv_sec, 0);
    out_str(".");
    out_dec(now.tv_nsec / 1000000L, 3);
    out_str(", build " APP_VERSION " ===\n");

    const int sig = k_signals[index];
    const int code = info != NULL ? info->si_code : 0;
    out_str("signal ");
    out_dec(sig, 0);
    out_str(" ");
    out_str(k_signal_names[index]);
    out_str(", code ");
    out_dec(code, 0);
    out_str(": ");
    out_str(code_words(sig, code));
    if (info != NULL && code > 0) {
        out_str("; address ");
        out_hex((uintptr_t) info->si_addr);
    }
    out_str("\n");

    char thread_name[17] = "";
    prctl(PR_GET_NAME, thread_name, 0, 0, 0);
    const long tid = syscall(SYS_gettid);
    out_str("thread ");
    out_dec(tid, 0);
    out_str(" '");
    out_str(thread_name);
    out_str(tid == (long) getpid() ? "' (the app's main thread)" : "'");
    out_str("; stream ");
    out_str(global != NULL && global->session != NULL ? "running or starting" : "none");
    out_str("\n");
    out_flush();

    s_code_count = 0;
    uintptr_t sp = 0;
    int pc = -1, lr = -1;
#if defined(__arm__)
    const mcontext_t *mc = &((const ucontext_t *) context)->uc_mcontext;
    sp = mc->arm_sp;
    pc = add_code(mc->arm_pc, false, 0);
    lr = add_code(mc->arm_lr, true, 0);
#elif defined(__aarch64__)
    const mcontext_t *mc = &((const ucontext_t *) context)->uc_mcontext;
    sp = mc->sp;
    pc = add_code(mc->pc, false, 0);
    lr = add_code(mc->regs[30], true, 0);
#elif defined(__x86_64__)
    const mcontext_t *mc = &((const ucontext_t *) context)->uc_mcontext;
    sp = (uintptr_t) mc->gregs[REG_RSP];
    pc = add_code((uintptr_t) mc->gregs[REG_RIP], false, 0);
#else
    (void) context;
#endif
    read_maps(sp);

    /* ⚠️ Read only within the stack's own mapping, and only so far: a word past
     * its end would fault in here. */
    const int stack_map = map_of(sp);
    const int first_stack_word = s_code_count;
    if (stack_map >= 0) {
        uintptr_t end = s_maps[stack_map].end;
        if (end - sp > 4096u) {
            end = sp + 4096u;
        }
        for (uintptr_t a = sp & ~(uintptr_t) (sizeof(uintptr_t) - 1); a + sizeof(uintptr_t) <= end;
             a += sizeof(uintptr_t)) {
            const uintptr_t word = *(const uintptr_t *) a;
            if (is_code(word) && add_code(word, true, a - sp) < 0) {
                break;
            }
        }
    }
    name_code();

    if (pc >= 0) {
        out_str("pc ");
        out_code(pc);
        out_str("\n");
    }
    if (lr >= 0) {
        out_str("lr ");
        out_code(lr);
        out_str("\n");
    }
    out_str("sp ");
    out_hex(sp);
    out_str("\n");
#if defined(__arm__)
    const unsigned long regs[] = {mc->arm_r0, mc->arm_r1, mc->arm_r2, mc->arm_r3, mc->arm_r4, mc->arm_r5,
                                  mc->arm_r6, mc->arm_r7, mc->arm_r8, mc->arm_r9, mc->arm_r10, mc->arm_fp,
                                  mc->arm_ip, mc->arm_cpsr};
    for (int r = 0; r < (int) (sizeof regs / sizeof regs[0]); ++r) {
        out_str(r == 11 ? " fp " : r == 12 ? " ip " : r == 13 ? " cpsr " : r == 0 ? "r0 " : " r");
        if (r > 0 && r < 11) {
            out_dec(r, 0);
            out_str(" ");
        }
        out_hex(regs[r]);
    }
    out_str("\n");
#endif

    if (stack_map < 0) {
        out_str(s_map_count == 0 ? "stack: not scanned, the memory map could not be read\n"
                                 : "stack: not scanned, its mapping is not in the memory map\n");
    } else {
        out_str("code addresses on the stack, nearest first (the callers are among them):\n");
        for (int c = first_stack_word; c < s_code_count; ++c) {
            out_str("  sp+");
            out_hex(s_code[c].where);
            out_str(" ");
            out_code(c);
            out_str("\n");
        }
        if (s_code_count == MAX_CODE) {
            out_str("  (no more listed)\n");
        }
    }

    out_str("code mapped:\n");
    for (int m = 0; m < s_map_count; ++m) {
        if (!s_maps[m].code) {
            continue;
        }
        out_str("  ");
        out_hex(s_maps[m].start);
        out_str("-");
        out_hex(s_maps[m].end);
        out_str(" +");
        out_hex(s_maps[m].offset);
        out_str(" ");
        out_mem(s_maps[m].path, s_maps[m].path_len);
        out_str("\n");
    }
    const struct sigaction *next = &s_previous[index];
    out_str(next->sa_handler == SIG_DFL ? "=== handed on to the default ===\n\n"
                                        : "=== handed on to the handler that was there before ===\n\n");
    out_flush();
    if (s_out.fd != STDERR_FILENO) {
        close(s_out.fd);
    }
}

static int signal_index(int sig) {
    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        if (k_signals[i] == sig) {
            return i;
        }
    }
    return -1;
}

static void on_crash(int sig, siginfo_t *info, void *context) {
    const int index = signal_index(sig);
    if (index >= 0 && __sync_lock_test_and_set(&s_handling, 1) == 0) {
        record(index, info, context);
        sigaction(sig, &s_previous[index], NULL);
    } else {
        /* ⛔ A second crash while the first is written, or a handler installed
         * after this one handing the crash back: the default, so it cannot go
         * round between the two. */
        struct sigaction dfl;
        memset(&dfl, 0, sizeof dfl);
        dfl.sa_handler = SIG_DFL;
        sigaction(sig, &dfl, NULL);
    }
    /* ⓘ A fault happens again the moment this returns, now to the handler
     * restored above, with its own details intact. A signal that was SENT, and
     * abort's SIGABRT, would not come again by itself: it is raised, and
     * arrives as soon as this returns. */
    if (info == NULL || info->si_code <= 0 || sig == SIGABRT) {
        raise(sig);
    }
}

static bool is_ours(const struct sigaction *action) {
    return (action->sa_flags & SA_SIGINFO) != 0 && action->sa_sigaction == on_crash;
}

/* -------------------------------------------- set up, outside any handler */

static void find_symbols(void) {
    s_exe_fd = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (s_exe_fd < 0) {
        return;
    }
    ElfW(Ehdr) eh;
    if (pread(s_exe_fd, &eh, sizeof eh, 0) != (ssize_t) sizeof eh || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_shentsize != sizeof(ElfW(Shdr)) || eh.e_shnum == 0) {
        return;
    }
    const size_t bytes = (size_t) eh.e_shnum * sizeof(ElfW(Shdr));
    ElfW(Shdr) *sh = malloc(bytes);
    if (sh == NULL) {
        return;
    }
    if (pread(s_exe_fd, sh, bytes, (off_t) eh.e_shoff) == (ssize_t) bytes) {
        for (int i = 0; i < eh.e_shnum; ++i) {
            if (sh[i].sh_type == SHT_SYMTAB && sh[i].sh_link < eh.e_shnum) {
                s_symtab_off = sh[i].sh_offset;
                s_symtab_size = sh[i].sh_size;
                s_strtab_off = sh[sh[i].sh_link].sh_offset;
                s_strtab_size = sh[sh[i].sh_link].sh_size;
                break;
            }
        }
    }
    free(sh);
    s_exe_bias = eh.e_type == ET_DYN ? (uintptr_t) __executable_start : 0;
}

static void prepare(void) {
    snprintf(s_log_path, sizeof s_log_path, "%s", ctm_log_path(CRASH_LOG));
    struct stat st;
    if (stat(s_log_path, &st) == 0 && st.st_size > CRASH_LOG_KEEP_BYTES) {
        char old_path[sizeof s_log_path];
        snprintf(old_path, sizeof old_path, "%s", ctm_log_path("crash.old.log"));
        rename(s_log_path, old_path);
    }
    find_symbols();
    /* ⓘ A stack of its own for the main thread, so a crash that is the stack
     * running out can still be written. Other threads write on their own. */
    stack_t alt;
    memset(&alt, 0, sizeof alt);
    alt.ss_sp = s_alt_stack;
    alt.ss_size = sizeof s_alt_stack;
    sigaltstack(&alt, NULL);
}

void crash_recorder_install(void) {
    static bool prepared = false;
    if (!prepared) {
        prepared = true;
        prepare();
    }
    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        struct sigaction current;
        if (sigaction(k_signals[i], NULL, &current) != 0 || is_ours(&current)) {
            continue;
        }
        struct sigaction mine;
        memset(&mine, 0, sizeof mine);
        mine.sa_sigaction = on_crash;
        sigemptyset(&mine.sa_mask);
        mine.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
        struct sigaction before;
        if (sigaction(k_signals[i], &mine, &before) == 0) {
            s_previous[i] = before;
        }
    }
}

void crash_recorder_describe(char *buf, int size) {
    int armed = 0, chained = 0;
    for (int i = 0; i < SIGNAL_COUNT; ++i) {
        struct sigaction current;
        if (sigaction(k_signals[i], NULL, &current) == 0 && is_ours(&current)) {
            ++armed;
            if (s_previous[i].sa_handler != SIG_DFL) {
                ++chained;
            }
        }
    }
    snprintf(buf, (size_t) size,
             "armed for %d of %d signals (SEGV BUS ILL FPE ABRT), %d handed on to a handler that was there"
             " before; the app's symbols %s; the record goes to %s",
             armed, SIGNAL_COUNT, chained, s_symtab_size != 0 ? "found" : "NOT found",
             s_log_path[0] != '\0' ? s_log_path : "(not set up)");
}

#else

void crash_recorder_install(void) {}

void crash_recorder_describe(char *buf, int size) {
    snprintf(buf, (size_t) size, "not on this system");
}

#endif
