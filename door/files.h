// The player's saved games, kept on the BBS under their account.
//
// A TRACE module has no disk, so saves travel over the wire: the door sends
// what the player already has on the way in, and stores whatever the game
// writes on the way out. That way a ten-hour adventure survives hanging up, and
// the player's games follow them to whatever machine they dial in from.
//
//     door -> module    save <name> <offset> <total>\n<bytes>
//     module -> door    put  <name> <offset> <total>\n<bytes>
#pragma once
#include <stdbool.h>
#include <stddef.h>

// Chunk size, shared by both ends. Must match kChunk in the module's
// trace_saves.cpp, or a save arrives in pieces that don't line up.
#define FILES_CHUNK 3000

// Picks the folder for this caller and makes sure it exists.
void files_init(const char *handle, int user_record);

// Where this player's saves live, for messages.
const char *files_folder(void);

// Sends every save the player has to the module, in chunks.
void files_send_all(void (*send)(const char *head, const void *payload, size_t len));

// Takes a "put ..." message from the module. Returns false if it was malformed
// or the name was refused.
bool files_receive(const unsigned char *data, size_t len);

// How many saves the player has, for the welcome screen.
int files_count(void);
