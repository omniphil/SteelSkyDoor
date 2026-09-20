#include "files.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_SAVE_BYTES (4 * 1024 * 1024)   /* a BASS save is a few KB; this is slack */

static char g_folder[768];

/* Everything must resolve beside the binary: a BBS runs a door with the working
 * directory set to its own root or a node temp dir, not the door's folder. */
static void beside_exe(const char *name, char *out, size_t outLen)
{
    char path[512];
    ssize_t n = readlink("/proc/self/exe", path, (sizeof path) - 1);
    if (n > 0) {
        path[n] = '\0';
        char *slash = strrchr(path, '/');
        if (slash) {
            *(slash + 1) = '\0';
            const size_t dirLen = strlen(path), nameLen = strlen(name);
            /* Truncating here would silently point the player's saves at a
             * different folder, so fall back to a relative path instead of
             * writing half of one. */
            if (dirLen + nameLen + 1 <= outLen) {
                memcpy(out, path, dirLen);
                memcpy(out + dirLen, name, nameLen + 1);
                return;
            }
        }
    }
    snprintf(out, outLen, "%s", name);
}

void files_init(const char *handle, int user_record)
{
    char base[512];
    char who[64];
    beside_exe("saves", base, sizeof base);
    mkdir(base, 0755);

    snprintf(who, sizeof who, "%s", (handle && *handle) ? handle : "Player");
    for (char *p = who; *p; p++)
        if (*p == '/' || *p == '\\' || *p == ' ' || *p == '.') *p = '_';

    snprintf(g_folder, sizeof g_folder, "%s/%s-%d", base, who, user_record);
    mkdir(g_folder, 0755);
}

const char *files_folder(void) { return g_folder; }

/* A name from the module is untrusted. Allow only what a save is actually
 * called, so "../../evil" or an absolute path can never be written. */
static bool name_ok(const char *name)
{
    if (!name || !*name || strlen(name) > 48) return false;
    for (const char *p = name; *p; p++) {
        const bool ok = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                        (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' || *p == '_';
        if (!ok) return false;
    }
    if (strstr(name, "..")) return false;
    return true;
}

int files_count(void)
{
    DIR *d = opendir(g_folder);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (e->d_name[0] != '.') n++;
    closedir(d);
    return n;
}

void files_send_all(void (*send)(const char *head, const void *payload, size_t len))
{
    DIR *d = opendir(g_folder);
    if (!d || !send) { if (d) closedir(d); return; }

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || !name_ok(e->d_name)) continue;

        char path[1024];
        snprintf(path, sizeof path, "%s/%.200s", g_folder, e->d_name);
        FILE *fp = fopen(path, "rb");
        if (!fp) continue;

        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (size < 0 || size > MAX_SAVE_BYTES) { fclose(fp); continue; }

        unsigned char buf[FILES_CHUNK];
        for (long off = 0; off < size; off += FILES_CHUNK) {
            size_t want = (size_t)(size - off);
            if (want > FILES_CHUNK) want = FILES_CHUNK;
            if (fread(buf, 1, want, fp) != want) break;

            char head[128];
            snprintf(head, sizeof head, "save %.48s %ld %ld", e->d_name, off, size);
            send(head, buf, want);
        }
        fclose(fp);
    }
    closedir(d);
}

/* "put <name> <offset> <total>\n<bytes>"
 *
 * Chunks are written into a .part file and only moved into place once the whole
 * save has arrived, so a dropped carrier can't leave a half-written save that
 * the game would later fail to load. */
bool files_receive(const unsigned char *data, size_t len)
{
    if (!data || len < 5 || memcmp(data, "put ", 4) != 0) return false;

    const unsigned char *nl = memchr(data, '\n', len);
    if (!nl) return false;

    char head[160];
    size_t headLen = (size_t)(nl - data);
    if (headLen >= sizeof head) return false;
    memcpy(head, data, headLen);
    head[headLen] = '\0';

    char name[64];
    unsigned long off = 0, total = 0;
    if (sscanf(head + 4, "%63s %lu %lu", name, &off, &total) != 3) return false;
    if (!name_ok(name) || total > MAX_SAVE_BYTES || off > total) return false;

    const unsigned char *payload = nl + 1;
    const size_t payLen = len - (size_t)(payload - data);
    if (off + payLen > total) return false;

    char part[1024], final[1024];
    snprintf(part, sizeof part, "%.900s/%.48s.part", g_folder, name);
    snprintf(final, sizeof final, "%.900s/%.48s", g_folder, name);

    /* A zero-length save means the game deleted it. */
    if (total == 0) {
        unlink(final);
        unlink(part);
        return true;
    }

    FILE *fp = fopen(part, off == 0 ? "wb" : "r+b");
    if (!fp && off != 0) fp = fopen(part, "wb");   /* first chunk went missing */
    if (!fp) return false;
    if (fseek(fp, (long)off, SEEK_SET) != 0) { fclose(fp); return false; }
    const bool wrote = fwrite(payload, 1, payLen, fp) == payLen;
    fclose(fp);
    if (!wrote) return false;

    if (off + payLen >= total) {
        /* Complete: swap it in. */
        rename(part, final);
    }
    return true;
}
