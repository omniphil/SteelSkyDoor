// Tests the door's half of the save round-trip on its own: a save goes out to
// the module in chunks, comes back the way the module sends it, and must land
// on disk byte-identical. Also checks that a hostile name is refused.
//
//   cc -o test_saves test_saves.c files.c && ./test_saves
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "files.h"

static int g_fails;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_fails++;
}

// What the module would have received, reassembled.
static unsigned char g_got[64 * 1024];
static size_t g_gotLen;
static char g_gotName[64];

static void capture(const char *head, const void *payload, size_t len)
{
    char name[64];
    unsigned long off = 0, total = 0;
    if (sscanf(head, "save %63s %lu %lu", name, &off, &total) != 3) return;
    snprintf(g_gotName, sizeof g_gotName, "%s", name);
    if (off + len <= sizeof g_got) {
        memcpy(g_got + off, payload, len);
        if (off + len > g_gotLen) g_gotLen = off + len;
    }
}

int main(void)
{
    printf("door save round-trip\n");
    files_init("TestUser", 42);
    printf("  saves folder: %s\n", files_folder());

    // A save big enough to span several chunks, so offsets are exercised.
    const size_t size = FILES_CHUNK * 2 + 777;
    unsigned char *orig = malloc(size);
    for (size_t i = 0; i < size; i++) orig[i] = (unsigned char)(i * 31 + (i >> 8));

    // --- the module writes a save: feed it in as "put" messages
    for (size_t off = 0; off < size; off += FILES_CHUNK) {
        size_t want = size - off;
        if (want > FILES_CHUNK) want = FILES_CHUNK;
        unsigned char msg[FILES_CHUNK + 128];
        const int headLen = snprintf((char *)msg, sizeof msg, "put SKY-VM.001 %zu %zu\n", off, size);
        memcpy(msg + headLen, orig + off, want);
        check(files_receive(msg, headLen + want), off == 0 ? "first chunk accepted" : "later chunk accepted");
    }

    // --- it must now be on disk, whole
    char path[1024];
    snprintf(path, sizeof path, "%s/SKY-VM.001", files_folder());
    FILE *fp = fopen(path, "rb");
    check(fp != NULL, "the save landed on disk");
    if (fp) {
        unsigned char *back = malloc(size + 16);
        const size_t n = fread(back, 1, size + 16, fp);
        fclose(fp);
        check(n == size, "it is the right length");
        check(n == size && memcmp(back, orig, size) == 0, "it is byte-identical");
        free(back);
    }
    snprintf(path, sizeof path, "%s/SKY-VM.001.part", files_folder());
    check(access(path, F_OK) != 0, "no half-written .part left behind");

    // --- and it must go back out to the module unchanged
    g_gotLen = 0;
    files_send_all(capture);
    check(strcmp(g_gotName, "SKY-VM.001") == 0, "sent back under the same name");
    check(g_gotLen == size, "sent back at the right length");
    check(g_gotLen == size && memcmp(g_got, orig, size) == 0, "sent back byte-identical");

    // --- a name from the module is untrusted
    {
        const char *evil = "put ../../../evil 0 4\nDOOM";
        check(!files_receive((const unsigned char *)evil, strlen(evil)),
              "a path-traversal name is refused");
        check(access("evil", F_OK) != 0 && access("../evil", F_OK) != 0,
              "and nothing was written outside the folder");
    }

    // --- a zero-length save means "delete"
    {
        const char *del = "put SKY-VM.001 0 0\n";
        check(files_receive((const unsigned char *)del, strlen(del)), "delete accepted");
        snprintf(path, sizeof path, "%s/SKY-VM.001", files_folder());
        check(access(path, F_OK) != 0, "the save was removed");
    }

    free(orig);
    printf(g_fails ? "\nFAILED (%d)\n" : "\nall checks passed\n", g_fails);
    return g_fails ? 1 : 0;
}
