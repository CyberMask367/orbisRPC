#define _XOPEN_SOURCE 700
#include "../orbisrpc/jsonlite.h"
#include "../orbisrpc/b64.h"
#include "../orbisrpc/sfo.h"
#include "../orbisrpc/tmdb_crypto.h"
#include "../orbisrpc/updater.h"
#include "../orbisrpc/nametable.h"
#include "../orbisrpc/art.h"
#include "../orbisrpc/health.h"
#include "../orbisrpc/manifest.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static void test_json(void) {
    const char input[] = "{\"name\":\"A\\u00e9\",\"items\":[true,2,null]}";
    jl_val_t *root = jl_parse(input, sizeof(input) - 1);
    assert(root && root->type == JL_OBJECT);
    assert(strcmp(jl_obj_get(root, "name")->str, "A\xc3\xa9") == 0);
    assert(jl_arr_at(jl_obj_get(root, "items"), 0)->num == 1);
    assert(jl_arr_at(jl_obj_get(root, "items"), 1)->num == 2);
    assert(jl_arr_at(jl_obj_get(root, "items"), 2)->type == JL_NULL);
    jl_free(root);

    assert(jl_parse("{} trailing", 11) == NULL);
    assert(jl_parse("[1,]", 4) == NULL);
    assert(jl_parse("\"unterminated", 13) == NULL);
}

static void test_gateway_op_spoof(void) {
    /* string value containing `"op":` must not spoof the real op */
    const char input[] = "{\"note\":\"x \\\"op\\\":99 y\",\"op\":10,\"s\":42}";
    jl_val_t *r = jl_parse(input, sizeof(input)-1);
    assert(r && r->type == JL_OBJECT);
    const jl_val_t *op = jl_obj_get(r, "op");
    const jl_val_t *s = jl_obj_get(r, "s");
    assert(op && op->type == JL_NUMBER && (int)op->num == 10);
    assert(s && s->type == JL_NUMBER && (int)s->num == 42);
    jl_free(r);
    /* missing op -> NULL, not crash */
    const char no_op[] = "{\"t\":\"READY\"}";
    jl_val_t *r2 = jl_parse(no_op, sizeof(no_op)-1);
    assert(r2 && jl_obj_get(r2, "op") == NULL);
    jl_free(r2);
}

static void test_json_oom_safe(void) {    assert(jl_parse("true", 4) != NULL);
    assert(jl_parse("false", 5) != NULL);
    assert(jl_parse("null", 4) != NULL);
    jl_val_t *n = jl_parse("123.5", 5);
    assert(n && n->type == JL_NUMBER);
    jl_free(n);
    /* incomplete pair must fail cleanly, no leak/crash */
    assert(jl_parse("{\"a\":", 5) == NULL);
    assert(jl_parse("{\"a\":1", 6) == NULL);
}
static void test_json_hostile(void) {
    /* 200-deep nesting must be rejected, not stack-smash */
    char deep[420];
    memset(deep, '[', 200);
    memset(deep+200, ']', 200);
    deep[400] = 0;
    assert(jl_parse(deep, 400) == NULL);
    /* 60-deep is fine */
    char okd[130];
    memset(okd, '[', 60);
    memset(okd+60, ']', 60);
    okd[120] = 0;
    jl_val_t *r = jl_parse(okd, 120);
    assert(r && r->type == JL_ARRAY);
    jl_free(r);
    /* number running exactly to buffer end (no NUL past it) */
    char num[16];
    memcpy(num, "{\"s\":41250}", 11);
    jl_val_t *r2 = jl_parse(num, 11);
    assert(r2);
    assert(jl_obj_get(r2, "s")->num == 41250);
    jl_free(r2);
    /* absurd number token fails cleanly */
    char big[80];
    memset(big, '9', 70);
    big[70] = 0;
    assert(jl_parse(big, 70) == NULL);
}
static void test_sfo(void) {
    /* minimal synthetic param.sfo: header + 1 entry (TITLE="Terraria") */
    unsigned char sfo[128];
    char out[64];
    memset(sfo, 0, sizeof sfo);
    sfo[0]=0x00; sfo[1]='P'; sfo[2]='S'; sfo[3]='F';
    sfo[4]=0x01; sfo[5]=0x02;
    sfo[8]=36; sfo[12]=48; sfo[16]=1;   /* keys@36 data@48 */
    sfo[20]=0; sfo[21]=0; sfo[22]=0x04; sfo[23]=0; /* key_off=0 fmt=0x0004 */
    sfo[24]=9; sfo[28]=16; sfo[32]=0;   /* len=9 max=16 data_off=0 */
    memcpy(sfo+36, "TITLE", 6);
    memcpy(sfo+48, "Terraria", 9);
    assert(sfo_title(sfo, 64, out, sizeof out) == 0);
    assert(strcmp(out, "Terraria") == 0);
    /* malformed: bad magic, truncated, insane count, OOB offsets */
    unsigned char bad[64];
    memset(bad, 0, sizeof bad);
    assert(sfo_title(bad, sizeof bad, out, sizeof out) != 0);
    assert(sfo_title(sfo, 10, out, sizeof out) != 0);
    sfo[16]=200; /* count overflow */
    assert(sfo_title(sfo, 64, out, sizeof out) != 0);
    sfo[16]=1; sfo[8]=200; /* key_off OOB */
    assert(sfo_title(sfo, 64, out, sizeof out) != 0);
    sfo[8]=36;
    /* tiny output buffer still safe */
    assert(sfo_title(sfo, 64, out, 4) == 0);
    assert(out[3] == 0);
    /* unterminated key region: must fail, not read OOB */
    memset(sfo, 0x41, sizeof sfo);
    sfo[0]=0x00; sfo[1]='P'; sfo[2]='S'; sfo[3]='F';
    sfo[8]=36; sfo[12]=48; sfo[16]=1;
    sfo[20]=0; sfo[22]=0x04; sfo[24]=9; sfo[28]=16; sfo[32]=0;
    assert(sfo_title(sfo, 64, out, sizeof out) != 0);
}
static void test_tmdb(void) {
    unsigned char dig[20];
    char hex[41];
    int i;
    /* SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d4d */
    tmdb_sha1((const unsigned char *)"abc", 3, dig);
    for(i=0;i<20;i++) sprintf(hex+2*i, "%02x", dig[i]);
    assert(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0);
    /* HMAC-SHA1 RFC 2202 case 1 */
    {
        unsigned char key[20];
        memset(key, 0x0b, 20);
        tmdb_hmac_sha1(key, 20, (const unsigned char *)"Hi There", 8, dig);
        for(i=0;i<20;i++) sprintf(hex+2*i, "%02x", dig[i]);
        assert(strcmp(hex, "b617318655057264e28bc0b6fb378c8ef146be00") == 0);
    }
    /* URL path must match the hash Sony's live service accepts */
    {
        char path[128];
        assert(tmdb_path("CUSA00740", path, sizeof path) == 0);
        assert(strcmp(path, "/tmdb2/CUSA00740_00_95C83DE844D155477CB77C577A24D735F2E9AC08/CUSA00740_00.json") == 0);
        assert(tmdb_path("junk!", path, sizeof path) != 0);
        assert(tmdb_path("CUSA00740", path, 10) != 0);
    }
    /* response parse: name + icon URL */
    {
        const char body[] = "{\"names\":[{\"name\":\"Terraria\"}],\"icons\":[{\"icon\":\"http://x/y/icon0.png\",\"type\":\"512x512\"}]}";
        char name[64], icon[128];
        assert(tmdb_parse(body, sizeof(body)-1, name, sizeof name, icon, sizeof icon) == 0);
        assert(strcmp(name, "Terraria") == 0);
        assert(strcmp(icon, "http://x/y/icon0.png") == 0);
        /* non-URL icon rejected, name still wins */
        const char body2[] = "{\"names\":[{\"name\":\"X\"}],\"icons\":[{\"icon\":\"not a url\"}]}";
        assert(tmdb_parse(body2, sizeof(body2)-1, name, sizeof name, icon, sizeof icon) == 0);
        assert(icon[0] == 0);
        /* no names -> fail */
        assert(tmdb_parse("{\"icons\":[]}", 12, name, sizeof name, icon, sizeof icon) != 0);
    }
}
static void test_updater(void) {
    assert(updater_cmp("0.4.0", "0.4.0") == 0);
    assert(updater_cmp("v0.4.1", "0.4.0") > 0);
    assert(updater_cmp("0.4.0", "v0.4.1") < 0);
    assert(updater_cmp("0.10.0", "0.9.9") > 0);
    assert(updater_cmp("1.0", "1.0.0") == 0);
    assert(updater_cmp(NULL, "0.1") < 0);
    unsigned char elf[64];
    memset(elf, 0, sizeof elf);
    assert(updater_elf_ok(elf, sizeof elf) == 0);
    assert(updater_elf_ok(NULL, 100) == 0);
    assert(updater_elf_ok(elf, 10) == 0);
    elf[0]=0x7f; elf[1]='E'; elf[2]='L'; elf[3]='F';
    elf[4]=2; elf[5]=1; elf[18]=62; elf[19]=0;
    assert(updater_elf_ok(elf, sizeof elf) == 1);
    elf[18]=99;
    assert(updater_elf_ok(elf, sizeof elf) == 0);
}
static void test_nametable(void) {
    char out[64];
    assert(nametable_lookup("CUSA00740", out, sizeof out) == 0);
    assert(strcmp(out, "Terraria") == 0);
    assert(nametable_lookup("CUSA00411", out, sizeof out) == 0);
    assert(strcmp(out, "Grand Theft Auto V") == 0);
    assert(nametable_lookup("XXXX99999", out, sizeof out) != 0);
    assert(nametable_lookup(NULL, out, sizeof out) != 0);
    /* tiny buffer: truncated but terminated */
    assert(nametable_lookup("CUSA00740", out, 4) == 0);
    assert(out[3] == 0);
}
static void test_art_parse(void) {
    /* Real external-assets response shape (verified against live API). */
    const char *body = "[{\"url\":\"https://example.com/a.png\","
        "\"external_asset_path\":\"external/ABC/https/example.com/a.png\"},"
        "{\"url\":\"https://example.com/b.png\","
        "\"external_asset_path\":\"external/DEF/https/example.com/b.png\"}]";
    char out[128] = {0};
    assert(art_parse_mp(body, strlen(body),
                        "https://example.com/b.png", out, sizeof out) == 1);
    assert(strcmp(out, "mp:external/DEF/https/example.com/b.png") == 0);
    assert(art_parse_mp(body, strlen(body),
                        "https://example.com/zzz.png", out, sizeof out) == 0);
    assert(art_parse_mp("[]", 2, "x", out, sizeof out) == 0);
    assert(art_parse_mp(NULL, 0, "x", out, sizeof out) == 0);
    assert(art_resolve_mp("1", "t", "https://example.com/a.png", out, sizeof out) == 0);
}

static void test_health_safe_mode(void) {
    /* Unclean-boot marker semantics: normal reboots never count. */
    char dir[] = "/tmp/orx_health_XXXXXX";
    assert(mkdtemp(dir) != NULL);
    health_set_base(dir);
    health_mark_clean();
    /* clean boot x3: counter stays 0, never safe mode */
    assert(health_boot_note_crash() == 0);
    health_mark_healthy();
    assert(health_boot_note_crash() == 0);
    health_mark_healthy();
    assert(health_boot_note_crash() == 0);
    /* now crash repeatedly WITHOUT going healthy: 1, 2, then safe */
    assert(health_boot_note_crash() == 0); /* count=1 */
    assert(health_boot_note_crash() == 0); /* count=2 */
    assert(health_boot_note_crash() == 1); /* count=3 -> safe mode */
    /* recovery clears */
    health_mark_healthy();
    assert(health_boot_note_crash() == 0);
    health_mark_healthy();
}

static void test_health_stage_activate(void) {
    /* Atomic staging: bad .new never touches live; rollback restores. */
    char dir[] = "/tmp/orx_stage_XXXXXX";
    assert(mkdtemp(dir) != NULL);
    health_set_base(dir);
    char live[256], tmp[256], bak[256];
    snprintf(live, sizeof live, "%s/live.bin", dir);
    snprintf(tmp, sizeof tmp, "%s/live.bin.new", dir);
    snprintf(bak, sizeof bak, "%s/live.bin.bak", dir);
    /* live = valid ELF stand-in (>=64B, ELF magic via updater_image_ok?
     * use real check: write 64 zero bytes won't pass; stage path only
     * needs .new validation, so craft minimal ELF header). */
    unsigned char elf[128];
    memset(elf, 0, sizeof elf);
    elf[0] = 0x7f; elf[1] = 'E'; elf[2] = 'L'; elf[3] = 'F';
    elf[4] = 2; elf[5] = 1; elf[18] = 62;
    FILE *f = fopen(live, "wb");
    assert(f); assert(fwrite(elf, 1, sizeof elf, f) == sizeof elf); fclose(f);
    /* corrupt .new is refused, live untouched */
    f = fopen(tmp, "wb");
    assert(f); assert(fwrite("garbage-not-elf-at-all......................"
                            "..............................", 1, 64, f) == 64);
    fclose(f);
    assert(health_stage_activate(live) != 0);
    f = fopen(live, "rb");
    assert(f);
    unsigned char chk[4];
    assert(fread(chk, 1, 4, f) == 4);
    fclose(f);
    assert(chk[0] == 0x7f && chk[1] == 'E');
    /* valid .new activates, backup created, rollback restores */
    f = fopen(tmp, "wb");
    assert(f);
    elf[7] = 0x42;
    assert(fwrite(elf, 1, sizeof elf, f) == sizeof elf);
    fclose(f);
    assert(health_stage_activate(live) == 0);
    f = fopen(bak, "rb");
    assert(f); fclose(f);
    assert(health_verify_or_rollback(live) == 0);
    /* corrupt live + backup present -> rollback */
    f = fopen(live, "wb");
    assert(f); assert(fwrite("XX", 1, 2, f) == 2); fclose(f);
    assert(health_verify_or_rollback(live) == 1);
    f = fopen(live, "rb");
    assert(f);
    assert(fread(chk, 1, 4, f) == 4);
    fclose(f);
    assert(chk[0] == 0x7f);
}

static void test_manifest(void) {
    const char *json = "{\"version\":\"1.0.0\",\"channel\":\"stable\","
        "\"min_version\":\"0.9.0\",\"platform\":\"ps4-goldhen\","
        "\"assets\":[{\"name\":\"orbisrpc.bin\","
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}]}";
    manifest_t m;
    assert(manifest_parse(json, strlen(json), &m) == 0);
    assert(strcmp(m.version, "1.0.0") == 0);
    assert(strcmp(m.channel, "stable") == 0);
    char hex[65] = {0};
    assert(manifest_find(&m, "orbisrpc.bin", hex) == 0);
    assert(!strcmp(hex, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
    assert(manifest_find(&m, "nope.bin", hex) != 0);
    assert(manifest_gate(&m) == 1);
    assert(manifest_is_newer(&m, "0.9.0") == 1);
    assert(manifest_is_newer(&m, "1.0.0") == 0);
    /* wrong channel / platform refused */
    const char *bad = "{\"version\":\"9.9.9\",\"channel\":\"beta\","
        "\"platform\":\"ps5\",\"assets\":[{\"name\":\"x.bin\","
        "\"sha256\":\"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef\"}]}";
    manifest_t m2;
    assert(manifest_parse(bad, strlen(bad), &m2) == 0);
    assert(manifest_gate(&m2) == 0);
    /* malformed rejected */
    assert(manifest_parse("{}", 2, &m) != 0);
    assert(manifest_parse("not json", 8, &m) != 0);
    /* hash check: real sha256 of "abc" must match */
    const char *jh = "{\"version\":\"1\",\"assets\":[{\"name\":\"a\","
        "\"sha256\":\"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"}]}";
    manifest_t m3;
    assert(manifest_parse(jh, strlen(jh), &m3) == 0);
    assert(manifest_check(&m3, "a", (const unsigned char *)"abc", 3) == 0);
    assert(manifest_check(&m3, "a", (const unsigned char *)"abd", 3) != 0);
}

static void test_base64(void) {
    char out[32];
    assert(b64_encode((const unsigned char *)"", 0, out) == 0);
    assert(strcmp(out, "") == 0);
    assert(b64_encode((const unsigned char *)"f", 1, out) == 4);
    assert(strcmp(out, "Zg==") == 0);
    assert(b64_encode((const unsigned char *)"fo", 2, out) == 4);
    assert(strcmp(out, "Zm8=") == 0);
    assert(b64_encode((const unsigned char *)"foo", 3, out) == 4);
    assert(strcmp(out, "Zm9v") == 0);
}

int main(void) {
    test_json();
    test_gateway_op_spoof();
    test_json_oom_safe();
    test_json_hostile();
    test_tmdb();
    test_updater();
    test_sfo();
    test_nametable();
    test_base64();
    test_art_parse();
    test_health_safe_mode();
    test_health_stage_activate();
    test_manifest();
    puts("utility tests passed");
    return 0;
}
