#include "trace_sky.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "trace_door.h"
#include "sha256.h"
#include "files.h"

#define MODULE_ID "steelsky"

// TERMinator caps one asset at 64 MB. The CD sky.dsk is 72 MB, so it goes as
// several assets and the module stitches them back together when it reads.
// 32 MB parts keep each upload a sane length over a BBS link.
#define PART_BYTES (32 * 1024 * 1024)
#define MAX_PARTS  8

// Two module builds: one that uses the mouse, one that does not. The door picks
// from the Query reply, so an older TRACE client still plays (by keyboard)
// rather than downloading 72 MB and then failing to start.
static tdoor_blob_t g_moduleMouse, g_moduleBasic;
static tdoor_blob_t *g_module;
static tdoor_blob_t g_parts[MAX_PARTS];     // sky.dsk, split
static int          g_partCount;
static tdoor_blob_t g_cpt;                  // sky.cpt (ScummVM's own data)
static tdoor_blob_t g_dnr;                  // sky.dnr
static char         g_error[256];
static size_t       g_total;

const char *trace_sky_load_error(void) { return g_error; }
size_t trace_sky_data_size(void) { return g_total; }

// Everything the module sends comes here. Right now that is only the player's
// saved games, arriving chunk by chunk as the game writes them.
static void on_message(const unsigned char *data, size_t len)
{
    files_receive(data, len);
}

static void send_chunk(const char *head, const void *payload, size_t len)
{
    tdoor_send(head, payload, len);
}

// Reads a file beside the door binary and chops it into blobs of at most
// PART_BYTES, hashing each so TERMinator can cache them independently.
static bool load_split(const char *filename, tdoor_blob_t *parts, int *count, int maxParts)
{
    char path[1024];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    FILE *fp = NULL;
    if (n > 0) {
        path[n] = '\0';
        char *slash = strrchr(path, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(path) - (size_t)(slash + 1 - path), "%s", filename);
            fp = fopen(path, "rb");
        }
    }
    if (!fp) fp = fopen(filename, "rb");
    if (!fp) {
        snprintf(g_error, sizeof g_error, "%s is missing", filename);
        return false;
    }

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    *count = 0;
    for (long off = 0; off < size; off += PART_BYTES) {
        if (*count >= maxParts) {
            snprintf(g_error, sizeof g_error, "%s needs more than %d parts", filename, maxParts);
            fclose(fp);
            return false;
        }
        size_t want = (size_t)(size - off);
        if (want > PART_BYTES) want = PART_BYTES;

        uint8_t *buf = (uint8_t *)malloc(want);
        if (!buf || fread(buf, 1, want, fp) != want) {
            snprintf(g_error, sizeof g_error, "could not read %s", filename);
            free(buf);
            fclose(fp);
            return false;
        }
        parts[*count].data = buf;
        parts[*count].size = want;
        sha256_hex(buf, want, parts[*count].hash);
        (*count)++;
    }
    fclose(fp);
    return true;
}

bool trace_sky_load_files(void)
{
    tdoor_init(MODULE_ID, on_message);
    g_error[0] = '\0';

    bool mouse = tdoor_load_blob("steelsky.wasm",       &g_moduleMouse, 32 * 1024 * 1024);
    bool basic = tdoor_load_blob("steelsky-basic.wasm", &g_moduleBasic, 32 * 1024 * 1024);
    if (!basic && mouse) g_moduleBasic = g_moduleMouse;
    if (!mouse && basic) g_moduleMouse = g_moduleBasic;
    if (!mouse && !basic) {
        snprintf(g_error, sizeof g_error, "steelsky.wasm is missing");
        return false;
    }
    if (!load_split("sky.dsk", g_parts, &g_partCount, MAX_PARTS)) return false;
    if (!tdoor_load_blob("sky.cpt", &g_cpt, 4 * 1024 * 1024)) {
        snprintf(g_error, sizeof g_error, "sky.cpt is missing (it ships with ScummVM)");
        return false;
    }
    // The engine refuses to start on any other size, so catch it here where we
    // can say something useful rather than inside the module.
    if (g_cpt.size != 419427) {
        snprintf(g_error, sizeof g_error, "sky.cpt is %zu bytes, expected 419427", g_cpt.size);
        return false;
    }
    if (!tdoor_load_blob("sky.dnr", &g_dnr, 1024 * 1024)) {
        snprintf(g_error, sizeof g_error, "sky.dnr is missing");
        return false;
    }

    g_total = g_cpt.size + g_dnr.size;
    for (int i = 0; i < g_partCount; i++) g_total += g_parts[i].size;
    return true;
}

bool trace_sky_detect(void)
{
    // No mouse in the list on purpose: the module has a keyboard cursor, so an
    // older TRACE client can still play.
    static const char *const need[] = { "assets=1", "send=1", "audio=1", NULL };
    tdoor_init(MODULE_ID, on_message);
    if (!tdoor_detect(need)) return false;
    g_module = tdoor_has("mouse=1") ? &g_moduleMouse : &g_moduleBasic;
    return g_module->data != NULL;
}

// One progress bar across every asset, because the player cares about the whole
// download, not about our chunking.
static void (*g_progress)(int);
static size_t g_sent, g_grand, g_currentPartSize;

static void part_progress(int percent)
{
    if (!g_progress || !g_grand) return;
    const size_t done = g_sent + (size_t)((double)g_currentPartSize * percent / 100.0);
    g_progress((int)(done * 100 / g_grand));
}

static bool send_one(tdoor_blob_t *b)
{
    g_currentPartSize = b->size;
    if (!tdoor_send_asset(b, g_progress ? part_progress : NULL)) return false;
    g_sent += b->size;
    return true;
}

bool trace_sky_send_data(void (*progress)(int))
{
    g_progress = progress;
    g_sent = 0;
    g_grand = trace_sky_data_size();

    for (int i = 0; i < g_partCount; i++)
        if (!send_one(&g_parts[i])) return false;
    if (!send_one(&g_cpt)) return false;
    if (!send_one(&g_dnr)) return false;
    if (progress) progress(100);
    return true;
}

bool trace_sky_open(void)
{
    // exclusive=1: the picture covers the whole screen and the module takes the
    // keyboard. The game draws its own menus, so the door is done after this.
    if (g_module == NULL || !tdoor_open(g_module, "exclusive=1", NULL)) return false;

    // Name the assets. sky.dsk is a comma-separated list because it was split.
    char msg[1024];
    int n = snprintf(msg, sizeof msg, "files sky.dsk=");
    for (int i = 0; i < g_partCount; i++)
        n += snprintf(msg + n, sizeof msg - n, "%s%s", i ? "," : "", g_parts[i].hash);
    snprintf(msg + n, sizeof msg - n, " sky.cpt=%s sky.dnr=%s", g_cpt.hash, g_dnr.hash);

    tdoor_send_text(msg);

    // Hand the player their saved games before the engine looks for them.
    files_send_all(send_chunk);
    return true;
}

void trace_sky_wait(void)  { tdoor_wait_closed(); }
void trace_sky_close(void) { tdoor_close("quit", 8000); }

const char *trace_sky_capabilities(void) { return tdoor_info(); }
const char *trace_sky_close_reason(void) { return tdoor_last_close_reason(); }
