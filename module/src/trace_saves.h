// The player's saved games, kept on the BBS.
//
// A module has no disk, and a save that only lived in the sandbox would vanish
// the moment the picture closed -- which for a ten-hour adventure is the same
// as having no saves at all. So a save is held in memory while the game runs
// and pushed to the door, which keeps it under the caller's account. On the way
// in the door sends them back, so the player's games follow them to whatever
// machine they dial in from.
//
// The wire format matches the Wolf3D door's, so both ends behave the same way:
//
//     door -> module    save <name> <offset> <total>\n<bytes>
//     module -> door    put <name> <offset> <total>\n<bytes>
#ifndef TRACE_SAVES_H
#define TRACE_SAVES_H

#include "common/savefile.h"
#include "common/hashmap.h"
#include "common/hash-str.h"
#include "common/array.h"

class TraceSaveFileManager : public Common::SaveFileManager {
public:
	Common::OutSaveFile *openForSaving(const Common::String &name, bool compress = true) override;
	Common::InSaveFile *openForLoading(const Common::String &name) override;
	Common::InSaveFile *openRawFile(const Common::String &name) override;
	bool removeSavefile(const Common::String &name) override;
	Common::StringArray listSavefiles(const Common::String &pattern) override;
	void updateSavefilesList(Common::StringArray &lockedFiles) override;
	bool exists(const Common::String &name) override;

	// --- the module's side ---

	// A piece of a save arriving from the door, before the game starts.
	void receiveChunk(const char *name, uint32 offset, uint32 total,
	                  const byte *data, uint32 len);

	// Called when the game closes a save it was writing: queues it for the door.
	void store(const Common::String &name, const byte *data, uint32 len);

	// Pushes queued saves to the door, a chunk at a time, as room allows.
	// Returns true while anything is still waiting to go.
	bool pump();

	bool hasPending() const { return !_outbox.empty(); }

private:
	typedef Common::HashMap<Common::String, Common::Array<byte>,
	                        Common::IgnoreCase_Hash, Common::IgnoreCase_EqualTo> FileMap;
	FileMap _files;                       // what the player has, by name
	Common::Array<Common::String> _outbox; // names still to send
	uint32 _outOffset = 0;                 // how far through the current one
};

#endif
