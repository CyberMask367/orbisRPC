/* detect.c
 * Foreground-game detection on PS4.
 *
 * Primary signal: sceShellCoreUtilIsAppLaunched() — resolved at runtime via
 * dlopen/dlsym (libSceShellCoreUtil.sprx). Returns 1 when a user app (game)
 * is in the foreground vs the home screen. This is the reliable "playing"
 * indicator; it stays 0 on the home screen so presence clears.
 *
 * Fallback (if ShellCoreUtil can't be resolved in the payload context):
 * sceUserServiceGetForegroundUser >= 0 + most-recent app.xml heuristic.
 *
 * Title naming is best-effort:
 *   a) /data/app/<titleid>/app.xml <title> for the most-recently-modified dir
 *   b) app.db tbl_app_static scan (crude byte scan)
 *   c) titleId (CUSAxxxxx) as last resort
 *
 * NOTE: mapping the foreground app to its pid/titleId is not exposed by the
 * public SDK, so the NAME is heuristic. The core signal (a game is in the
 * foreground) is reliable via ShellCoreUtil.
 */
#include "detect.h"
#include "log.h"
#include "sfo.h"
#include "tmdb.h"
#include "nametable.h"
#ifdef ORBISRPC_SDK_PAYLOAD
/* Payload-SDK build: dlopen/dlsym come from the SDK libc (dlfcn);
 * there is no UserService here — user_init() below degrades. */
#include <dlfcn.h>
#include <stdint.h>
#else
#include <orbis/UserService.h>
#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#endif
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef ORBISRPC_SDK_PAYLOAD
#include <sys/types.h>
#include <sys/sysctl.h>

static int is_title_prefix(const char *n);

/* --- SDK-build foreground/title signals (no ShellCoreUtil/UserService) ---
 * A launched game shows up as an "eboot.bin" process; its identity comes
 * from the freshest savedata dir (gameplay writes saves continuously).
 * Both facts verified live via probes before wiring them in. */
/* Count eboot.bin processes. Changes in the set mean a launch or a close;
 * callers use it to drop stale caches immediately instead of waiting. */
static int proc_eboot_count(void){
    int mib[4] = { 1, 14, 8, 0 };
    size_t sz = 0;
    int n = 0;
    if(sysctl(mib, 4, NULL, &sz, NULL, 0) != 0) return -1;
    static unsigned char buf[256*1024];
    if(sz > sizeof buf) return -1;
    if(sysctl(mib, 4, buf, &sz, NULL, 0) != 0) return -1;
    size_t off = 0;
    while(off + 4 <= sz){
        int recsz = *(int *)(buf + off);
        if(recsz <= 0 || off + (size_t)recsz > sz) break;
        if(recsz >= 479 && !memcmp(buf + off + 447, "eboot.bin", 10))
            n++;
        off += (size_t)recsz;
    }
    return n;
}
static int proc_has_eboot(void){
    int mib[4] = { 1, 14, 8, 0 };
    size_t sz = 0;
    if(sysctl(mib, 4, NULL, &sz, NULL, 0) != 0) return 0;
    static unsigned char buf[256*1024];
    if(sz > sizeof buf) return 0;
    if(sysctl(mib, 4, buf, &sz, NULL, 0) != 0) return 0;
    size_t off = 0;
    while(off + 4 <= sz){
        int recsz = *(int *)(buf + off);
        if(recsz <= 0 || off + (size_t)recsz > sz) break;
        if(recsz >= 479 && !memcmp(buf + off + 447, "eboot.bin", 10))
            return 1;
        off += (size_t)recsz;
    }
    return 0;
}

static long scan_newest_save(char *out, size_t cap){
    static const char *users[] = { "1898cd02", "1898cd03", NULL };
    /* user dirs vary per console; probe the known ones plus a scan of
     * /user/home for anything looking like a user id dir. */
    char udirs[8][32];
    int ndirs = 0;
    DIR *hd = opendir("/user/home");
    if(hd){
        struct dirent *e;
        while((e = readdir(hd)) && ndirs < 8){
            if(e->d_name[0] == '.') continue;
            strncpy(udirs[ndirs], e->d_name, 31);
            udirs[ndirs][31] = 0;
            ndirs++;
        }
        closedir(hd);
    }
    for(int i = 0; users[i] && ndirs < 8; i++){
        int dup = 0;
        for(int k = 0; k < ndirs; k++) if(!strcmp(udirs[k], users[i])) dup = 1;
        if(!dup){ strncpy(udirs[ndirs], users[i], 31); udirs[ndirs][31] = 0; ndirs++; }
    }
    long best = -1;
    out[0] = 0;
    for(int u = 0; u < ndirs; u++){
        char spath[96];
        snprintf(spath, sizeof spath, "/user/home/%s/savedata", udirs[u]);
        DIR *sd = opendir(spath);
        if(!sd) continue;
        struct dirent *e;
        while((e = readdir(sd))){
            if(e->d_name[0] == '.') continue;
            if(strlen(e->d_name) != 9 || !is_title_prefix(e->d_name)) continue;
            char tp[160];
            snprintf(tp, sizeof tp, "%s/%s", spath, e->d_name);
            struct stat st;
            /* newest write inside the title dir wins */
            DIR *td = opendir(tp);
            long tb = -1;
            if(td){
                struct dirent *f;
                while((f = readdir(td))){
                    if(f->d_name[0] == '.') continue;
                    /* d_name can be up to 255 chars; tp already holds up
                     * to ~150 — skip overlong names instead of overflowing. */
                    if(strlen(f->d_name) > 64) continue;
                    char fp[256];
                    int wn = snprintf(fp, sizeof fp, "%s/%s", tp, f->d_name);
                    if(wn <= 0 || (size_t)wn >= sizeof fp) continue;
                    struct stat fs;
                    if(stat(fp, &fs) == 0 && fs.st_mtime > tb) tb = fs.st_mtime;
                }
                closedir(td);
            }
            if(tb < 0){
                struct stat ds;
                if(stat(tp, &ds) == 0) tb = ds.st_mtime;
            }
            if(tb > best){ best = tb; strncpy(out, e->d_name, cap-1); out[cap-1] = 0; }
        }
        closedir(sd);
    }
    return out[0] ? 0 : -1;
}
#endif

static int s_user_inited = 0;
static int s_user_ok = 0;
static int user_init(void){
    if(s_user_inited) return s_user_ok ? 0 : -1;
    s_user_inited = 1;
#ifdef ORBISRPC_SDK_PAYLOAD
    /* No UserService in payload-SDK builds: ShellCoreUtil (dlopen) is the
     * only foreground signal; without it we report inactive (fail closed). */
    log_msg("UserService unavailable in SDK build; ShellCoreUtil only");
    return -1;
#else
    /* UserService is an external module -> load via internal id */
    uint32_t r = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_USER_SERVICE);
    if(r != 0){ int32_t ir=(int32_t)r; log_msg("load UserService fail %d", ir); return -1; }
    int32_t rc = sceUserServiceInitialize(NULL);
    if(rc != 0){ log_msg("UserService init fail %d", rc); return -1; }
    s_user_ok = 1;
    return 0;
#endif
}

/* --- ShellCoreUtil runtime resolution ------------------------------- */
/* PS4 SDK has no <dlfcn.h>; libkernel exports dlopen/dlsym directly. */
extern void *dlopen(const char *filename, int flags);
extern void *dlsym(void *handle, const char *symbol);
typedef int (*shellcore_isapplaunched_fn)(void);
static shellcore_isapplaunched_fn s_is_app_launched = NULL;
static int s_scu_tried = 0;

static int scu_init(void){
    if(s_scu_tried) return s_is_app_launched != NULL;
    s_scu_tried = 1;
    void *h = dlopen("libSceShellCoreUtil.sprx", 0);
    if(!h){ log_msg("ShellCoreUtil unavailable (dlopen fail); using foreground-user fallback"); return 0; }
    s_is_app_launched = (shellcore_isapplaunched_fn)(uintptr_t)dlsym(h, "sceShellCoreUtilIsAppLaunched");
    log_msg("ShellCoreUtil IsAppLaunched %s", s_is_app_launched ? "resolved" : "unavailable (dlsym fail)");
    return s_is_app_launched != NULL;
}

/* --- foreground-active: the core "a game is running" signal ---------- */
int detect_foreground_active(void){
#ifdef ORBISRPC_SDK_PAYLOAD
    /* Spawned processes see no ShellCoreUtil/UserService; the eboot.bin
     * process itself is the signal (system has no other eboot). */
    return proc_has_eboot();
#else
    if(scu_init() && s_is_app_launched){
        int on = s_is_app_launched();
        return (on != 0) ? 1 : 0;   /* 0 when sitting on the home screen */
    }
#endif
    /* fallback: foreground user exists. If UserService itself failed,
     * report inactive instead of guessing "playing". */
    if(user_init() != 0) return 0;
#ifdef ORBISRPC_SDK_PAYLOAD
    /* No UserService API in SDK builds (user_init always fails there,
     * so this is unreachable); fail closed regardless. */
    return 0;
#else
    int32_t fg = -1;
    int32_t rc = sceUserServiceGetForegroundUser(&fg);
    if(rc != 0){ log_msg("GetForegroundUser err %d", rc); return 0; }
    return (fg >= 0) ? 1 : 0;
#endif
}

/* --- title naming ---------------------------------------------------- */
static int is_title_prefix(const char *n){
    /* CUSA (PS4) + PPSA (PS5-backport) + EU/JP/indie variants */
    return strncmp(n,"CUSA",4)==0 || strncmp(n,"PPSA",4)==0 ||
           strncmp(n,"PCSE",4)==0 || strncmp(n,"PCSB",4)==0 ||
           strncmp(n,"PCSG",4)==0 || strncmp(n,"EPSA",4)==0;
}
/* last resolved titleId (for Discord asset key); valid after a successful
 * detect_current_game / detect_name_for_title. */
static char s_last_titleid[16] = "";
static char s_last_art[256] = "";
static char s_res_titleid[16] = "";
static char s_res_name[128] = "";
static char s_res_art[256] = "";
static int s_res_valid = 0;
/* Media apps post Watching/Listening instead of Playing. IDs verified
 * against Sony TMDB (names resolve there too). */
int detect_media_type(const char *title_id){
    static const struct { const char *id; int type; } media[] = {
        { "CUSA00127", 3 }, /* Netflix -> Watching */
        { "CUSA01015", 3 }, /* YouTube -> Watching */
    };
    if(!title_id) return 0;
    for(unsigned i = 0; i < sizeof media/sizeof media[0]; i++){
        if(!strcmp(title_id, media[i].id)) return media[i].type;
    }
    return 0;
}

const char *detect_last_titleid(void){ return s_last_titleid[0] ? s_last_titleid : NULL; }
const char *detect_last_art(void){ return s_last_art[0] ? s_last_art : NULL; }
static void remember_resolved(const char *ti, const char *name, const char *art){
    if(!ti || !name) return;
    strncpy(s_res_titleid, ti, sizeof s_res_titleid - 1);
    s_res_titleid[sizeof s_res_titleid - 1] = 0;
    strncpy(s_res_name, name, sizeof s_res_name - 1);
    s_res_name[sizeof s_res_name - 1] = 0;
    if(art){
        strncpy(s_res_art, art, sizeof s_res_art - 1);
        s_res_art[sizeof s_res_art - 1] = 0;
    } else s_res_art[0] = 0;
    s_res_valid = 1;
}
static int cached_resolve(const char *ti, char *out_name, size_t cap){
    if(!s_res_valid || !ti || !ti[0] || strcmp(ti, s_res_titleid) != 0) return 0;
    strncpy(out_name, s_res_name, cap - 1);
    out_name[cap - 1] = 0;
    strncpy(s_last_art, s_res_art, sizeof s_last_art - 1);
    s_last_art[sizeof s_last_art - 1] = 0;
    return 1;
}
static void remember_titleid(const char *ti){
    if(!ti) return;
    strncpy(s_last_titleid, ti, sizeof s_last_titleid - 1);
    s_last_titleid[sizeof s_last_titleid - 1] = 0;
}
static long scan_one_appdir(const char *base, char *out, size_t cap, long best){
    if(!base || !out || cap < 2) return best;
    DIR *d = opendir(base);
    if(!d) return best;
    struct dirent *e;
    char path[256]; size_t plen;
    while((e=readdir(d))){
        if(e->d_name[0]=='.') continue;
        size_t l=strlen(e->d_name);
        if(l!=9 || !is_title_prefix(e->d_name)) continue;
        plen=snprintf(path,sizeof path,"%s/%s/app.xml",base,e->d_name);
        if(plen>=sizeof path) continue;
        struct stat st2;
        if(stat(path,&st2)==0){
            const char *ti = e->d_name;
            if(st2.st_mtime > best || best<0){ best=st2.st_mtime; if(cap>1)strncpy(out,ti,cap-1); out[cap-1]=0; }
        }
    }
    closedir(d);
    return best;
}
static long scan_recent_titleid(char *out, size_t cap){
    out[0]=0;
    long best = scan_one_appdir("/user/app", out, cap, -1);
    best = scan_one_appdir("/data/app", out, cap, best);
    return (out[0])? 0 : -1;
}

/* Best name on the box: /user/appmeta/<id>/pronunciation.xml holds the
 * display title in its first <text> element (works for every game that
 * ships speech data, e.g. Terraria). Small file, single read. */
static int pronunc_title(const char *titleId, char *out, size_t cap){
    char path[256]; snprintf(path,sizeof path,"/user/appmeta/%s/pronunciation.xml",titleId);
    int fd=open(path,O_RDONLY);
    if(fd<0){ log_msg("appmeta open fail %s err=%d", path, errno); return -1; }
    char buf[2048]; ssize_t n=read(fd,buf,sizeof buf-1); close(fd);
    if(n<=0) return -1; buf[n]=0;
    const char *p=strstr(buf,"<text>");
    if(!p) return -1;
    p+=6;
    while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++;
    size_t i=0;
    while(*p && *p!='<' && *p!='\n' && *p!='\r' && i<cap-1){ out[i++]=*p++; }
    out[i]=0;
    while(i>0 && (out[i-1]==' '||out[i-1]=='\t')) out[--i]=0;
    return (i>1)?0:-1;
}

/* Game-process-safe TITLE read: the running game's own param.sfo via the
 * app0 mount plus on-disc sce_sys copies. Small single read, works on ANY
 * console with zero setup. Returns 0 on success. */
static int sfo_file_title(const char *titleId, char *out, size_t cap){
    char path[256];
    const char *fixed[] = { "app0/sce_sys/param.sfo", "/app0/sce_sys/param.sfo", NULL };
    for(int i = 0; fixed[i]; i++){
        int fd = open(fixed[i], O_RDONLY);
        if(fd < 0) continue;
        unsigned char buf[4096];
        ssize_t n = read(fd, buf, sizeof buf);
        close(fd);
        if(n > 0 && sfo_title(buf, (size_t)n, out, cap) == 0) return 0;
    }
    if(titleId && titleId[0]){
        const char *bases[] = { "/user/app", "/data/app", NULL };
        for(int b = 0; bases[b]; b++){
            snprintf(path, sizeof path, "%s/%s/sce_sys/param.sfo", bases[b], titleId);
            int fd = open(path, O_RDONLY);
            if(fd < 0) continue;
            unsigned char buf[4096];
            ssize_t n = read(fd, buf, sizeof buf);
            close(fd);
            if(n > 0 && sfo_title(buf, (size_t)n, out, cap) == 0) return 0;
        }
    }
    return -1;
}

static int appxml_title(const char *titleId, char *out, size_t cap){
    const char *bases[] = { "/user/app", "/data/app", NULL };
    for(int b=0; bases[b]; b++){
    char path[256]; snprintf(path,sizeof path,"%s/%s/app.xml",bases[b],titleId);
    int fd=open(path,O_RDONLY); if(fd<0) continue;
    char buf[640]; ssize_t n=read(fd,buf,sizeof buf-1); close(fd);
    if(n<=0) continue; buf[n]=0;
    char *p=strstr(buf,"<title>");
    if(!p) p=strstr(buf,"titleName");
    if(!p) continue;
    p = strchr(p, '>');
    if(!p) continue;
    p++;
    while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n') p++; /* trim leading ws */
    size_t i=0;
    while(*p && *p!='<' && *p!='\n' && *p!='\r' && i<cap-1){ out[i++]=*p++; }
    out[i]=0;
    while(i>0 && (out[i-1]==' '||out[i-1]=='\t')) out[--i]=0; /* trim trailing */
    if(i>0) return 0;
    }
    return -1;
}

/* NOTE: an earlier revision byte-scanned app.db for titles. Removed:
 * the packed record layout makes the title/contentId boundary ambiguous
 * and the scanner returned wrong names (worse than raw IDs). Exact
 * sources above plus Sony TMDB cover every case instead. */

int detect_current_game(char *out_name, size_t cap, char *out_path, size_t p_cap){
    if(!out_name || cap==0) return -1;
    out_name[0] = 0;
    if(out_path && p_cap) out_path[0] = 0;
    if(!detect_foreground_active()) return -1;
    char titleId[16]=""; int named=0, have_tid=0;
#ifdef ORBISRPC_SDK_PAYLOAD
    /* Spawned processes see no ShellCoreUtil/UserService and /data/app is
     * invisible; identity comes from the freshest savedata instead. */
    if(scan_newest_save(titleId,sizeof titleId)==0){
        have_tid = 1;
        remember_titleid(titleId);
        s_last_art[0] = 0;
    } else return -1;
#else
    have_tid = (scan_recent_titleid(titleId,sizeof titleId)==0);
    if(have_tid){
        remember_titleid(titleId);
        s_last_art[0] = 0;
    }
#endif
    if(have_tid){
        remember_titleid(titleId);
        s_last_art[0] = 0;
#ifdef ORBISRPC_SDK_PAYLOAD
        /* Launch/close churns the eboot set; a fresh set means the old
         * cached identity may belong to a dead game — drop it so the
         * new title resolves immediately instead of lingering. */
        {
            static int last_eboots = -1;
            int nboots = proc_eboot_count();
            if(nboots >= 0 && nboots != last_eboots){
                if(last_eboots >= 0){
                    s_res_valid = 0;
                    log_msg("eboot set changed %d -> %d; resolve cache dropped",
                            last_eboots, nboots);
                }
                last_eboots = nboots;
            }
        }
#endif
        if(cached_resolve(titleId, out_name, cap)){
            log_dbg("name: %s via cache", out_name);
            named = 1;
        }
        /* cheap, game-process-safe sources first; Sony TMDB (network)
         * resolves anything local sources miss, on any console. */
        if(!named && pronunc_title(titleId, out_name, cap)==0){ named=1; log_msg("name: %s via appmeta", out_name); }
        if(!named){ if(sfo_file_title(titleId, out_name, cap)==0){ named=1; log_msg("name: %s via sfo", out_name); } }
        if(!named){ if(appxml_title(titleId, out_name, cap)==0){ named=1; log_msg("name: %s via appxml", out_name); } }
        if(!named){ if(nametable_lookup(titleId, out_name, cap)==0){ named=1; log_msg("name: %s via table", out_name); } }
        if(!named){
            char art[256] = "";
            if(tmdb_resolve(titleId, out_name, cap, art, sizeof art)==0){
                named = 1;
                strncpy(s_last_art, art, sizeof s_last_art-1);
            } else s_last_art[0] = 0;
        }
        if(!named){ strncpy(out_name, titleId, cap-1); out_name[cap-1]=0; }
        remember_resolved(titleId, out_name, s_last_art);
    }else{
        remember_titleid("");
        s_last_art[0] = 0;
        strncpy(out_name, "(unknown game)", cap-1); out_name[cap-1]=0;
    }
    if(out_path){ snprintf(out_path, p_cap, "/data/orbisRPC/.lastgame/%s", titleId[0]?titleId:"unknown"); }
    return 0;
}

/* Resolve a display name for a KNOWN title id (plugin mode: the plugin is
 * loaded into the game process and knows the titleid from the GoldHEN SDK,
 * so we skip the foreground-app heuristics entirely). */
int detect_name_for_title(const char *titleId, char *out_name, size_t cap){
    if(!titleId || !titleId[0] || !out_name || cap==0) return -1;
    remember_titleid(titleId);
    s_last_art[0] = 0;
    if(cached_resolve(titleId, out_name, cap)){
        log_dbg("name: %s via cache", out_name);
        return 0;
    }
    /* Game-process-safe only: small reads plus one bounded network
     * lookup; no multi-megabyte scans anywhere in this codebase. */
    if(pronunc_title(titleId, out_name, cap)==0){ log_msg("name: %s via appmeta", out_name); remember_resolved(titleId, out_name, ""); return 0; }
    else log_msg("name: appmeta miss for %s", titleId);
    if(sfo_file_title(titleId, out_name, cap)==0){ log_msg("name: %s via sfo", out_name); remember_resolved(titleId, out_name, ""); return 0; }
    if(appxml_title(titleId, out_name, cap)==0){ log_msg("name: %s via appxml", out_name); remember_resolved(titleId, out_name, ""); return 0; }
    if(nametable_lookup(titleId, out_name, cap)==0){ log_msg("name: %s via table", out_name); remember_resolved(titleId, out_name, ""); return 0; }
    {
        char art[256] = "";
        if(tmdb_resolve(titleId, out_name, cap, art, sizeof art)==0){
            strncpy(s_last_art, art, sizeof s_last_art-1);
            remember_resolved(titleId, out_name, art);
            return 0; /* tmdb_resolve already logged */
        }
        s_last_art[0] = 0;
    }
    strncpy(out_name, titleId, cap-1); out_name[cap-1]=0;
    remember_resolved(titleId, out_name, "");
    return 0;
}