// The game's files, served straight out of TRACE's asset store.
//
// The door uploads the game data once; TERMinator caches it by SHA-256 and the
// module reads it through trace_asset_read(). So ScummVM's file layer is backed
// by the asset store instead of a disk, and nothing is ever held in memory:
// sky.dsk is 72 MB and the engine seeks all over it.
//
// One wrinkle: TERMinator caps a single asset at 64 MB, and the CD sky.dsk is
// 72 MB. So a file may be split across SEVERAL assets, and this maps a read at
// any offset onto whichever asset holds it. The engine never sees the seam.
#ifndef TRACE_ASSETS_H
#define TRACE_ASSETS_H

#include "common/archive.h"
#include "common/stream.h"
#include "common/str.h"
#include "common/array.h"

// One file, possibly spread over several assets, in order.
struct TraceAssetFile {
	Common::String name;                  // e.g. "sky.dsk"
	Common::Array<Common::String> hashes; // the assets it is made of, in order
	Common::Array<int32> sizes;           // each asset's size
	int32 total = 0;
};

class TraceArchive : public Common::Archive {
public:
	// Adds a file. `hashes` is a comma-separated list of SHA-256s: more than one
	// when the file had to be split to fit the asset cap.
	bool addFile(const Common::String &name, const Common::String &hashes);

	bool hasFile(const Common::Path &path) const override;
	int listMembers(Common::ArchiveMemberList &list) const override;
	const Common::ArchiveMemberPtr getMember(const Common::Path &path) const override;
	Common::SeekableReadStream *createReadStreamForMember(const Common::Path &path) const override;

	const TraceAssetFile *find(const Common::String &name) const;

private:
	Common::Array<TraceAssetFile> _files;
};

#endif
