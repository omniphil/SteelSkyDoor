// Beneath a Steel Sky -- the door's TRACE glue.
//
// Everything generic lives in trace_door.[ch], which is a copy of
// BBSGames/TraceDoor. Never edit that one here; edit it there and run sync.sh.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads steelsky.wasm and the game files from beside the door binary.
// sky.dsk is split into as many assets as it takes to stay under TERMinator's
// 64 MB cap -- the CD version is 72 MB.
bool trace_sky_load_files(void);

// What went wrong, if trace_sky_load_files() returned false.
const char *trace_sky_load_error(void);

// How much the player's machine will have to fetch, in bytes, before it can
// play. Worth telling them: the CD game is about 69 MB.
size_t trace_sky_data_size(void);

bool trace_sky_detect(void);

// Uploads the game data. `progress` is called with 0..100 across the whole lot,
// not per asset, so the bar means something.
bool trace_sky_send_data(void (*progress)(int));

// Opens the picture over the whole screen and starts the game.
bool trace_sky_open(void);

void trace_sky_wait(void);
void trace_sky_close(void);

const char *trace_sky_capabilities(void);
const char *trace_sky_close_reason(void);

#ifdef __cplusplus
}
#endif
