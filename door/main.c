// Beneath a Steel Sky, as a BBS door.
//
// Revolution Software's 1994 cyberpunk adventure, released as freeware with its
// source in 2003, running on ScummVM's `sky` engine compiled to WebAssembly and
// drawn on the caller's own machine through TRACE.
//
// The door's job is small: work out whether the caller can run it, send the
// game once (TERMinator caches it by hash, so only the first call pays), and
// then get out of the way. The game draws its own menus and its own save
// screens, exactly as it did on a 1994 PC.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "door.h"
#include "trace_sky.h"
#include "files.h"

#define CSI "\033["
#define SOURCE_URL "https://github.com/omniphil/SteelSkyDoor"

static void cls(void) { door_write(CSI "0m" CSI "2J" CSI "H"); }

/* The name in its own colours, the way the client writes it. */
static void write_terminator(void)
{
    door_write(CSI "1;35m" "TERM" CSI "1;36m" "inator" CSI "0m");
}

static void title(void)
{
    cls();
    door_write(CSI "1;36m"
               "        ===============================================\r\n"
               "          B E N E A T H   A   S T E E L   S K Y\r\n"
               "        ===============================================\r\n" CSI "0m");
    door_write(CSI "1;34m" "                    BBS door by JSONBourne\r\n" CSI "0m" "\r\n");
}

static void press_any_key(void)
{
    door_write(CSI "0;37m\r\n  Press any key to return to the BBS...\r\n" CSI "0m");
    door_read_char();
}

static bool detect_with_animation(void)
{
    door_write(CSI "0;37m  Checking your terminal for TRACE graphics");
    usleep(250000);
    bool found = trace_sky_detect();
    for (int i = 0; i < 3; i++) { door_write("."); usleep(200000); }
    door_write(CSI "0m\r\n");
    return found;
}

static void progress(int percent)
{
    char buf[128];
    int filled = percent * 40 / 100;
    char bar[41];
    for (int i = 0; i < 40; i++) bar[i] = i < filled ? '#' : '.';
    bar[40] = '\0';
    snprintf(buf, sizeof buf, "\r" CSI "0;36m  [%s] %3d%%" CSI "0m", bar, percent);
    door_write(buf);
}

static void goodbye(void)
{
    cls();
    door_write(CSI "1;36m\r\n  Thanks for playing BENEATH A STEEL SKY.\r\n\r\n" CSI "0m");
    door_write(CSI "1;34m" "  BBS door by JSONBourne\r\n\r\n" CSI "0m");

    /* The player has just been sent GPL software, so this is where they're told
     * where its source is. */
    door_write(CSI "0;37m  The game here runs on ScummVM's sky engine, free software under\r\n");
    door_write("  the GNU GPL. Beneath a Steel Sky is (c) Revolution Software,\r\n");
    door_write("  released as freeware by its authors. Art by Dave Gibbons.\r\n");
    door_write("  Source: " CSI "1;37m" SOURCE_URL "\r\n" CSI "0m");
    press_any_key();
}

int main(int argc, char *argv[])
{
    door_init(argc > 1 ? argv[1] : NULL);
    files_init(door_info.handle, door_info.user_record);
    title();

    if (!trace_sky_load_files()) {
        door_write(CSI "1;31m  This door is not installed properly:\r\n    ");
        door_write(trace_sky_load_error());
        door_write("\r\n" CSI "0m");
        press_any_key();
        door_cleanup();
        return 1;
    }

    if (!detect_with_animation()) {
        door_write(CSI "0;37m\r\n  This game needs TRACE graphics, which means ");
        write_terminator();
        door_write(CSI "0;37m" ".\r\n" CSI "0m");
        door_write(CSI "0;90m  Any other terminal can't draw it: it's a 320x200 adventure,\r\n"
                       "  not something that fits in text.\r\n" CSI "0m");
        press_any_key();
        door_cleanup();
        return 0;
    }

    /* Be honest about the download before starting it: this is 69 MB, which is
     * a long first call. It only happens once -- TERMinator caches by hash. */
    char note[256];
    snprintf(note, sizeof note,
             CSI "0;37m\r\n  Sending the game: %.0f MB.\r\n"
             CSI "0;90m  This happens once. Your terminal keeps it, so next time you\r\n"
             "  dial in the game starts straight away.\r\n\r\n" CSI "0m",
             trace_sky_data_size() / 1048576.0);
    door_write(note);

    if (!trace_sky_send_data(progress)) {
        door_write(CSI "1;31m\r\n  The game couldn't be sent to your terminal.\r\n" CSI "0m");
        press_any_key();
        door_cleanup();
        return 1;
    }

    {
        const int saves = files_count();
        char line[192];
        if (saves > 0)
            snprintf(line, sizeof line,
                     CSI "0;32m\r\n\r\n  Sending your %d saved game%s across.\r\n" CSI "0m",
                     saves, saves == 1 ? "" : "s");
        else
            snprintf(line, sizeof line,
                     CSI "0;90m\r\n\r\n  No saved games yet -- the BBS keeps them for you once you save.\r\n" CSI "0m");
        door_write(line);
    }
    door_write(CSI "0;37m\r\n  Starting.\r\n"
                   "    ESC           skip the intro\r\n"
                   "    ESC or F5     the game's own menu: save, load, quit\r\n"
                   "  Quit from that menu to come back to the BBS.\r\n" CSI "0m");
    sleep(2);

    if (!trace_sky_open()) {
        door_write(CSI "1;31m\r\n  The game couldn't be started on your terminal.\r\n" CSI "0m");
        press_any_key();
        door_cleanup();
        return 1;
    }

    cls();              /* so nothing flashes behind the picture when it closes */
    const time_t began = time(NULL);
    trace_sky_wait();
    const int played = (int)(time(NULL) - began);
    trace_sky_close();

    /* If the picture opened and shut again straight away, the module didn't run
     * on the caller's machine. Say why rather than pretending all was well. */
    if (played < 3) {
        cls();
        title();
        door_write(CSI "1;31m  The game closed straight away.\r\n\r\n" CSI "0m");
        const char *why = trace_sky_close_reason();
        door_write(CSI "1;33m  Reason: " CSI "0m");
        door_write((why && *why) ? why : "(none given)");
        door_write("\r\n\r\n");
        door_write(CSI "0;90m  Your terminal offers: " CSI "0m");
        door_write(trace_sky_capabilities());
        door_write("\r\n\r\n" CSI "0;37m  Please send those two lines to the sysop.\r\n" CSI "0m");
        press_any_key();
        door_cleanup();
        return 1;
    }

    goodbye();
    door_cleanup();
    return 0;
}
