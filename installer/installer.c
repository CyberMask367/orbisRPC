/* installer.c - orbisRPC Setup: Inject -> token -> network test -> tweaks.
 *
 * NAVIGATION LAW (from the last installer trauma): every No/cancel moves
 * FORWARD to the next step. Nothing ever dumps back to the start. The only
 * backward motion is none; the only exits are Done or explicit close.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ui.h"
#include "send.h"
#include "icfg.h"
#include "nettest.h"

#define DAEMON_ELF "/app0/assets/daemon.elf"

static void step_inject_body(void){
    int port = 0;
    if(ui_progress_open("Injecting orbisRPC...") != 0){
        ui_ok("Could not open progress dialog. Continuing.");
        return;
    }
    ui_progress_set(0);
    if(send_file_loopback(DAEMON_ELF, &port, ui_progress_set) == 0){
        char msg[128];
        ui_progress_close();
        snprintf(msg, sizeof msg, "Daemon injected via loader port %d.\n\nIt boots on its own from here.", port);
        ui_ok(msg);
    } else {
        ui_progress_close();
        ui_ok("Inject failed on every loader port (9090/9021/9020).\n\nIs GoldHEN's BinLoader running? Continuing anyway.");
    }
}

static void step_inject(void){
    int q = ui_confirm("Inject the orbisRPC daemon now?\n\nSends it to the console's own loader (native, no PC).");
    if(q != 1) return; /* No -> next step, never back. */
    step_inject_body();
}

static void step_token(void){
    char tok[160];
    int tries, r;
    int q = ui_confirm("Set your Discord token?\n\nNeeded for presence. Stored only on this console.");
    if(q != 1) return;
    tok[0] = 0;
    icfg_token_load(ICFG_PATH, tok, sizeof tok);
    for(tries = 0; tries < 3; tries++){
        r = ui_input("Discord token", "paste token, Done to save", tok, sizeof tok);
        if(r < 0){ ui_ok("Text input failed. Token step skipped."); return; }
        if(r == 0) return; /* cancel -> next step */
        if(token_valid(tok)){
            r = icfg_token_save(ICFG_PATH, tok);
            ui_ok(r == 0 ? "Token saved." : "Could not write config. Continuing.");
            return;
        }
        ui_ok("That doesn't look like a token.\nCheck it and try again, or cancel to skip.");
    }
}

static void step_nettest(void){
    static const struct { const char *label; const char *host; int port; } t[] = {
        { "Discord gateway", "gateway.discord.gg", 443 },
        { "Discord API", "discord.com", 443 },
        { "Google DNS (internet)", "8.8.8.8", 53 },
        { "Sony TMDB (expected: blocked)", "tmdb.np.dl.playstation.net", 80 },
    };
    char out[512];
    size_t used = 0;
    unsigned i;
    int q = ui_confirm("Run the network test?\n\nChecks what this console can actually reach.");
    if(q != 1) return;
    if(ui_progress_open("Testing network...") != 0) return;
    used = (size_t)snprintf(out, sizeof out, "Network test:\n");
    for(i = 0; i < sizeof t/sizeof t[0]; i++){
        int ok;
        ui_progress_msg(t[i].label);
        ok = net_probe(t[i].host, t[i].port, 6);
        used += (size_t)snprintf(out + used, sizeof out - used,
                                 "\n%s: %s", t[i].label, ok ? "OK" : "FAIL");
        if(used >= sizeof out - 64) break;
        ui_progress_set((i + 1) * 100 / 4);
    }
    ui_progress_close();
    ui_ok(out);
}

static void step_tweaks(void){
    char buf[256];
    long n;
    char *end;
    /* Presence text */
    if(ui_confirm("Change the idle presence text?") == 1){
        buf[0] = 0;
        icfg_get_str(ICFG_PATH, "presence_state", buf, sizeof buf);
        if(ui_input("Idle text", "e.g. On PS4", buf, sizeof buf) == 1 && buf[0]){
            if(icfg_set_str(ICFG_PATH, "presence_state", buf) != 0)
                ui_ok("Could not save. Continuing.");
        }
    }
    /* Poll interval */
    if(ui_confirm("Change the poll interval (seconds)?") == 1){
        if(ui_input("Poll interval", "5 to 60", buf, sizeof buf) == 1 && buf[0]){
            n = strtol(buf, &end, 10);
            if(end != buf && *end == 0 && n >= 5 && n <= 60){
                if(icfg_set_int(ICFG_PATH, "poll_interval_s", n) != 0)
                    ui_ok("Could not save. Continuing.");
            } else {
                ui_ok("Must be a number 5..60. Keeping current.");
            }
        }
    }
    /* Home art */
    if(ui_confirm("Change the idle tile art?") == 1){
        buf[0] = 0;
        icfg_get_str(ICFG_PATH, "home_art", buf, sizeof buf);
        if(ui_input("Idle art", "URL, asset key, or empty", buf, sizeof buf) == 1){
            if(icfg_set_str(ICFG_PATH, "home_art", buf) != 0)
                ui_ok("Could not save. Continuing.");
        }
    }
    /* Debug logging */
    {
        int d = ui_confirm("Turn debug logging on?");
        if(d == 1) icfg_set_int(ICFG_PATH, "debug", 1);
        else if(d == 0) icfg_set_int(ICFG_PATH, "debug", 0);
    }
    ui_ok("Settings saved (where edited).");
}

int main(void){
    char tok[160];
    int first_run;
    tok[0] = 0;
    icfg_token_load(ICFG_PATH, tok, sizeof tok);
    first_run = !token_valid(tok);
    if(!first_run){
        /* Returning user: inject? no -> settings? no -> exit. Forward only. */
        if(ui_confirm("orbisRPC Setup.\n\nInject the daemon?") == 1){
            step_inject_body();
            ui_ok("Done.");
            return 0;
        }
        if(ui_confirm("Open settings instead?") == 1)
            step_tweaks();
        return 0;
    }
    /* First run wizard: every No skips forward. */
    step_inject();
    step_token();
    step_nettest();
    ui_ok("Setup complete.\n\nLaunch a game and watch Discord.");
    return 0;
}
