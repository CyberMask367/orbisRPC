/* installer.c - orbisRPC Setup, stripped to the bone:
 * install files -> token -> done.
 * One confirm up front; everything after flows forward.
 *
 * Payload placement is the whole job: one copy of orbisrpc.bin into
 * /data/GoldHEN/bin/elf. GoldHEN's payload launcher reads
 * this directory; never this app - an installer that also
 * injects fails in GoldHEN's eyes. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include <orbis/libkernel.h>
#include <orbis/SystemService.h>
#include "ui.h"
#include "icfg.h"

#ifndef SETUP_VERSION
#define SETUP_VERSION "1.0.0"
#endif
#define DAEMON_ELF "/app0/assets/daemon.elf"
#define EVICT_ELF "/app0/assets/evict.elf"
#define INST_DIR "/data/orbisRPC"
#define INST_LOG "/data/orbisRPC/install.log"
#define PAYLOAD_BIN "/data/GoldHEN/bin/elf/orbisrpc.bin"
#define EVICT_BIN "/data/GoldHEN/bin/elf/evict.elf"

/* Stage log: every copy step records errno + sizes to a file we can read
 * back over FTP. The dialog alone can't say WHICH stage failed. */
static const char *g_stage = "-";

static void ilog(const char *tag, int err, long expect, long got){
    FILE *f;
    g_stage = tag;
    f = fopen(INST_LOG, "a");
    if(!f) f = fopen("/data/install.log", "a");   /* fallback if sandbox blocks */
    if(!f) return;
    fprintf(f, "%ld %s errno=%d(%s) expect=%ld got=%ld\n",
            (long)time(NULL), tag, err, err ? strerror(err) : "ok", expect, got);
    fclose(f);
}

/* Stage marker without errno semantics (rc values, not errnos). */
static void ilogv(const char *tag, long a, long b){
    FILE *f;
    g_stage = tag;
    f = fopen(INST_LOG, "a");
    if(!f) f = fopen("/data/install.log", "a");
    if(!f) return;
    fprintf(f, "%ld %s a=%ld b=%ld\n", (long)time(NULL), tag, a, b);
    fclose(f);
}

/* --- crash guard ----------------------------------------------------
 * The wizard has died twice mid-flow with Sony's crash reporter eating
 * the core file, so the fault address never reached us. These handlers
 * run async-signal-safe only (no stdio, no malloc): they append the
 * signal, faulting address and current stage to install.log, then exit
 * cleanly. A crash becomes evidence instead of a crash dialog. */
static volatile sig_atomic_t g_crashing = 0;

static size_t h_s(char *b, size_t cap, size_t p, const char *s){
    while(*s && p + 1 < cap) b[p++] = *s++;
    return p;
}
static size_t h_u(char *b, size_t cap, size_t p, unsigned long v){
    char t[24];
    int n = 0;
    if(v == 0) t[n++] = '0';
    while(v && n < 24){ t[n++] = (char)('0' + v % 10); v /= 10; }
    while(n && p + 1 < cap) b[p++] = t[--n];
    return p;
}
static size_t h_x(char *b, size_t cap, size_t p, unsigned long long v){
    static const char hx[] = "0123456789abcdef";
    char t[20];
    int n = 0;
    if(v == 0) t[n++] = '0';
    while(v && n < 20){ t[n++] = hx[v & 15]; v >>= 4; }
    while(n && p + 1 < cap) b[p++] = t[--n];
    return p;
}

static void crash_handler(int sig, siginfo_t *si, void *uc){
    char b[200];
    size_t p = 0;
    int fd;
    ssize_t w;
    (void)uc;
    if(g_crashing) _exit(99);
    g_crashing = 1;
    p = h_s(b, sizeof b, p, "CRASH sig=");
    p = h_u(b, sizeof b, p, (unsigned)sig);
    p = h_s(b, sizeof b, p, " addr=0x");
    p = h_x(b, sizeof b, p, si ? (unsigned long long)(uintptr_t)si->si_addr : 0);
    p = h_s(b, sizeof b, p, " stage=");
    p = h_s(b, sizeof b, p, g_stage ? g_stage : "-");
    if(p + 1 < sizeof b) b[p++] = '\n';
    fd = open(INST_LOG, O_WRONLY | O_APPEND | O_CREAT, 0666);
    if(fd < 0) fd = open("/data/install.log", O_WRONLY | O_APPEND | O_CREAT, 0666);
    if(fd >= 0){ w = write(fd, b, p); (void)w; close(fd); }
    _exit(99);
}

static void crash_guard(void){
    static const int sigs[] = { SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP };
    struct sigaction sa;
    size_t i;
    memset(&sa, 0, sizeof sa);
    /* Header bug: the sa_sigaction macro expands to a member that does
     * not exist, so address the union directly. */
    sa.__sa_handler.__sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    for(i = 0; i < sizeof sigs / sizeof sigs[0]; i++)
        sigaction(sigs[i], &sa, NULL);
}

/* Preflight: the payload must exist inside this package. A PKG built
 * or installed without staged assets fails here with a precise message
 * instead of a mysterious write error three steps later. */
static int step_assets(void){
    FILE *a = fopen(DAEMON_ELF, "rb");
    if(a) fclose(a);
    if(!a){
        ui_ok("This install is missing its payload.\n\nReinstall the Setup PKG (do not just relaunch the old bubble), then open it again.");
        return -1;
    }
    return 0;
}

/* The sandbox makes stat()/fstat() lie about sizes (observed: 4096/4160
 * for a 2071552-byte file, which made a perfect copy report "denied").
 * open/read/write are truthful, so the proof is: count and hash what we
 * wrote, then count and hash what we read back. No stat anywhere. */
static unsigned long long fnv1a(const unsigned char *p, size_t n,
                                unsigned long long h){
    size_t i;
    for(i = 0; i < n; i++){
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static int copy_file(const char *src, const char *dst){
    FILE *in, *out;
    static unsigned char buf[65536];
    size_t n;
    long written = 0, reread = 0;
    unsigned long long hw = 14695981039346656037ULL, hr = 14695981039346656037ULL;
    char detail[96];

    in = fopen(src, "rb");
    if(!in){ ilog("src-open", errno, -1, -1); return -1; }
    out = fopen(dst, "wb");
    if(!out){ ilog("dst-open", errno, -1, -1); fclose(in); return -1; }
    while((n = fread(buf, 1, sizeof buf, in)) > 0){
        if(fwrite(buf, 1, n, out) != n){
            ilog("dst-write", errno, written, -1);
            fclose(in); fclose(out); return -1;
        }
        hw = fnv1a(buf, n, hw);
        written += (long)n;
    }
    fclose(in);
    if(fclose(out) != 0){ ilog("dst-close", errno, written, -1); return -1; }
    if(written <= 0){ ilog("src-empty", 0, written, -1); return -1; }

    /* Read-back proof: reopen and re-count/re-hash the destination. */
    in = fopen(dst, "rb");
    if(!in){ ilog("verify-open", errno, written, -1); return -1; }
    while((n = fread(buf, 1, sizeof buf, in)) > 0){
        hr = fnv1a(buf, n, hr);
        reread += (long)n;
    }
    fclose(in);
    snprintf(detail, sizeof detail, "h=%016llx/%016llx", hw, hr);
    if(written == reread && hw == hr){
        ilog("copy-ok", 0, written, reread);
        return 0;
    }
    ilog("verify-mismatch", 0, written, reread);
    ilog(detail, 0, 0, 0);
    g_stage = "verify-mismatch";
    return -1;
}

/* Existence check without stat() (the sandbox lies about stat sizes). */
static int exists(const char *p){
    FILE *f = fopen(p, "rb");
    if(!f) return 0;
    fclose(f);
    return 1;
}

/* mkdir -p (parents as needed). 0 ok or already there. */
static int mkdirs(const char *path){
    char tmp[256];
    size_t i, n;
    if(!path || !path[0]) return -1;
    n = strlen(path);
    if(n >= sizeof tmp) return -1;
    memcpy(tmp, path, n + 1);
    for(i = 1; i < n; i++){
        if(tmp[i] == '/'){
            tmp[i] = 0;
            mkdir(tmp, 0777);
            tmp[i] = '/';
        }
    }
    if(mkdir(path, 0777) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int step_files(void){
    char report[512];
    int ok = -1;
    int evict_ok = -1;
    mkdir(INST_DIR, 0777);
    mkdirs("/data/GoldHEN/bin/elf");
    /* Evict any running orbisRPC daemon before copying new payload.
     * Read PID from daemon.lock and send SIGTERM then SIGKILL. */
    {
        FILE *lf = fopen("/data/orbisRPC/daemon.lock", "rb");
        if(lf){
            int pid = 0;
            if(fscanf(lf, "%d", &pid) == 1 && pid > 0){
                if(kill(pid, SIGTERM) == 0){
                    ilog("evict-sigterm", 0, pid, 0);
                    sceKernelUsleep(500000);
                    if(kill(pid, SIGKILL) == 0){
                        ilog("evict-sigkill", 0, pid, 0);
                    } else {
                        ilog("evict-sigkill-fail", errno, pid, 0);
                    }
                } else {
                    ilog("evict-sigterm-fail", errno, pid, 0);
                }
            }
            fclose(lf);
        } else {
            ilog("evict-nolock", errno, 0, 0);
        }
    }
    /* Copy evict payload to payload folder for future manual use. */
    evict_ok = copy_file(EVICT_ELF, EVICT_BIN);
    ilog("evict-copy", evict_ok, 0, 0);
    /* Copy daemon payload. */
    ok = copy_file(DAEMON_ELF, PAYLOAD_BIN);
    ilog("daemon-copy", ok, 0, 0);
    /* Pre-save config from PKG asset so step_token() skips on fresh install.
     * Copy config.json from /app0/assets/config.json to /data/orbisRPC/config.json. */
    {
        FILE *src = fopen("/app0/assets/config.json", "rb");
        if(src){
            fclose(src);
            copy_file("/app0/assets/config.json", ICFG_PATH);
            ilog("cfg-copy", 0, 0, 0);
        } else {
            ilog("cfg-noasset", errno, 0, 0);
            /* Fallback: write template with pre-set token. */
            FILE *f = fopen(ICFG_PATH, "wb");
            if(f){
                fputs("{\"schema_version\":1,\"token\":\"MTM4MzAzODc1MzIzNjU4MjU0Mg.GaRPLA.Ehk_GTPSNxLZIIbCMYknnXmbv7mOK4w0ZRQZBk\",\"presence_state\":\"On PS4\"}", f);
                fclose(f);
            }
        }
    }
    snprintf(report, sizeof report,
             "Daemon payload:\n%s : %s%s%s%s\n\n"
             "Evict payload:\n%s : %s%s%s%s\n\n"
             "Start it from the payload launcher (/data/GoldHEN/bin/elf)\n"
             "and pick orbisrpc.",
             PAYLOAD_BIN, ok == 0 ? "OK" : "denied",
             ok == 0 ? "" : " [stage ", ok == 0 ? "" : g_stage, ok == 0 ? "" : "]",
             EVICT_BIN, evict_ok == 0 ? "OK" : "denied",
             evict_ok == 0 ? "" : " [stage ", evict_ok == 0 ? "" : g_stage, evict_ok == 0 ? "" : "]");
    ui_ok(report);
    if(ok != 0){
        ui_ok("Install failed: could not write the payload.\n\nStopping here.");
        return -1;
    }
    ilogv("report-done", ok, 0);
    /* Config template when missing; never clobbers learned titles. */
    {
        char tok[160];
        tok[0] = 0;
        icfg_token_load(ICFG_PATH, tok, sizeof tok);
        if(!token_valid(tok)){
            FILE *f = fopen(ICFG_PATH, "rb");
            if(!f){
                f = fopen(ICFG_PATH, "wb");
                if(f){
            fputs("{\"schema_version\":1,\"token\":\"MTM4MzAzODc1MzIzNjU4MjU0Mg.GaRPLA.Ehk_GTPSNxLZIIbCMYknnXmbv7mOK4w0ZRQZBk\",\"presence_state\":\"On PS4\"}", f);
                fclose(f);
            }
            } else {
                fclose(f);
            }
        }
    }
    ilogv("cfg-done", 0, 0);
    return 0;
}

/* 2: wifi check removed on purpose: it probed gateway.discord.gg from a
 * sandboxed app, whose network stack is not the payload's, and its
 * FAILED message read like a verdict on Discord itself. */

/* 2: token. Skips silently when one already validates. */
static void step_token(void){
    char tok[160];
    int tries, r;
    tok[0] = 0;
    ilogv("token-begun", 0, 0);
    icfg_token_load(ICFG_PATH, tok, sizeof tok);
    if(token_valid(tok)){
        ui_ok("A valid token is already saved.\n\nSkipping.");
        ilogv("token-skip", 0, 0);
        return;
    }
    for(tries = 0; tries < 3; tries++){
        r = ui_input("Discord token", "paste token, Done to save", tok, sizeof tok);
        if(r < 0){ ui_ok("Text input failed. Skipping."); return; }
        if(r == 0) return;
        if(token_valid(tok)){
            r = icfg_token_save(ICFG_PATH, tok);
            ui_ok(r == 0 ? "Token saved." : "Could not write config.");
            return;
        }
        ui_ok("That doesn't look like a token.\nCheck it and try again, or cancel to skip.");
    }
}

/* Status: read-only health snapshot for debugging. Never mutates.
 * Existence only — stat() sizes/mtimes are unreliable under the sandbox. */
static void step_status(void){
    char out[640];
    size_t used = 0;
    char tok[160];
    tok[0] = 0;
    icfg_token_load(ICFG_PATH, tok, sizeof tok);
    snprintf(out, sizeof out, "orbisRPC status:\n");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\ndaemon payload: %s", exists(PAYLOAD_BIN) ? "installed" : "missing");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\nevict payload: %s", exists(EVICT_BIN) ? "installed" : "missing");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\nlock: %s", exists("/data/orbisRPC/daemon.lock") ? "held (may be running)" : "free");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\nlog: %s", exists("/data/orbisRPC/log.txt") ? "present" : "none yet");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\ntoken: %s", token_valid(tok) ? "saved" : "missing/invalid");
    used += (size_t)snprintf(out + used, sizeof out - used,
        "\nlearned titles: %d", icfg_titles_count(ICFG_PATH));
    if(used >= sizeof out - 64) out[sizeof out - 64] = 0;
    ui_ok(out);
}

/* Clean exit: tear dialogs down, unload their modules, then _exit so the
 * CRT teardown path never runs (returning from main is where the wizard
 * has been dying: every crash landed at the end of a completed flow). */
static void finish(int code){
    ilogv(code == 0 ? "exit" : "fatal", code, 0);
    ui_shutdown();
    _exit(code);
}

int main(void){
    /* Dismiss the PS4 splash immediately — first. This way module
     * loading in ui_init() happens with the splash gone, so the
     * first dialog appears with no visible delay after the splash
     * disappears. Dialogs opened after this point won't be
     * auto-dismissed by the splash. */
    sceSystemServiceHideSplashScreen();
    char welcome[256];
    crash_guard();
    if(ui_init() != 0) finish(1);
    snprintf(welcome, sizeof welcome,
             "orbisRPC Setup %s\n\nInstalls the daemon payload and saves your Discord token.",
             SETUP_VERSION);
    ui_ok(welcome);
    if(step_assets() != 0) finish(1);
    ilogv("assets-ok", 0, 0);
    if(step_files() != 0) finish(1);
    step_token();
    ilogv("post-token", 0, 0);
    ui_ok("Setup " SETUP_VERSION " complete.\n\n"
            "evict.elf removed the old daemon.\n"
            "Both payloads are in /data/GoldHEN/bin/elf.\n"
            "Launch orbisrpc from the payload launcher.");
    finish(0);
}
