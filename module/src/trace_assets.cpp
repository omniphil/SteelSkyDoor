#include "trace_assets.h"

#include "common/archive.h"
#include "common/textconsole.h"
#include "trace_stage.h"

extern "C" {
#include "trace_api.h"
}

namespace {

// A seekable stream over one file, which may span several assets.
class TraceAssetStream : public Common::SeekableReadStream {
public:
	explicit TraceAssetStream(const TraceAssetFile &file) : _file(file) {}

	uint32 read(void *dataPtr, uint32 dataSize) override {
		byte *out = (byte *)dataPtr;
		uint32 done = 0;
		while (done < dataSize && _pos < _file.total) {
			// Which asset holds _pos, and how far into it?
			int32 base = 0, part = -1;
			for (uint i = 0; i < _file.sizes.size(); i++) {
				if (_pos < base + _file.sizes[i]) { part = (int32)i; break; }
				base += _file.sizes[i];
			}
			if (part < 0) break;

			const int32 inPart = (int32)(_pos - base);
			int32 want = (int32)(dataSize - done);
			const int32 left = _file.sizes[part] - inPart;
			if (want > left) want = left;          // don't read past this asset

			trace_stage("asset read");
			const int32 got = trace_asset_read(_file.hashes[part].c_str(), inPart,
			                                   out + done, want);
			trace_stage("asset read done");
			if (got <= 0) { _eos = true; break; }
			done += (uint32)got;
			_pos += got;
		}
		if (done < dataSize) _eos = true;
		return done;
	}

	bool eos() const override { return _eos; }
	int64 pos() const override { return _pos; }
	int64 size() const override { return _file.total; }

	bool seek(int64 offset, int whence = SEEK_SET) override {
		int64 target = offset;
		if (whence == SEEK_CUR) target = _pos + offset;
		else if (whence == SEEK_END) target = _file.total + offset;
		if (target < 0 || target > _file.total) return false;
		_pos = target;
		_eos = false;
		return true;
	}

private:
	const TraceAssetFile &_file;
	int64 _pos = 0;
	bool _eos = false;
};

class TraceMember : public Common::ArchiveMember {
public:
	TraceMember(const TraceArchive *a, const Common::String &name) : _a(a), _name(name) {}
	Common::SeekableReadStream *createReadStream() const override {
		return _a->createReadStreamForMember(Common::Path(_name));
	}
	Common::SeekableReadStream *createReadStreamForAltStream(Common::AltStreamType) const override {
		return nullptr;
	}
	Common::String getName() const override { return _name; }
	Common::Path getPathInArchive() const override { return Common::Path(_name); }
	Common::String getFileName() const override { return _name; }
private:
	const TraceArchive *_a;
	Common::String _name;
};

} // namespace

bool TraceArchive::addFile(const Common::String &name, const Common::String &hashes) {
	TraceAssetFile f;
	f.name = name;

	// Split the comma-separated hash list and ask TERMinator how big each is.
	Common::String cur;
	for (uint i = 0; i <= hashes.size(); i++) {
		if (i == hashes.size() || hashes[i] == ',') {
			if (!cur.empty()) {
				const int32 sz = trace_asset_size(cur.c_str());
				if (sz <= 0) return false;         // the door promised an asset we don't have
				f.hashes.push_back(cur);
				f.sizes.push_back(sz);
				f.total += sz;
				cur.clear();
			}
		} else {
			cur += hashes[i];
		}
	}
	if (f.hashes.empty()) return false;
	_files.push_back(f);
	return true;
}

const TraceAssetFile *TraceArchive::find(const Common::String &name) const {
	for (uint i = 0; i < _files.size(); i++) {
		// The engine asks for "sky.dsk", "SKY.DSK" and so on; match either way.
		if (_files[i].name.equalsIgnoreCase(name)) return &_files[i];
	}
	return nullptr;
}

bool TraceArchive::hasFile(const Common::Path &path) const {
	return find(path.baseName()) != nullptr;
}

int TraceArchive::listMembers(Common::ArchiveMemberList &list) const {
	for (uint i = 0; i < _files.size(); i++)
		list.push_back(Common::ArchiveMemberPtr(new TraceMember(this, _files[i].name)));
	return (int)_files.size();
}

const Common::ArchiveMemberPtr TraceArchive::getMember(const Common::Path &path) const {
	if (!hasFile(path)) return Common::ArchiveMemberPtr();
	return Common::ArchiveMemberPtr(new TraceMember(this, path.baseName()));
}

Common::SeekableReadStream *TraceArchive::createReadStreamForMember(const Common::Path &path) const {
	trace_stage("opening a game file");
	const TraceAssetFile *f = find(path.baseName());
	if (!f) return nullptr;
	return new TraceAssetStream(*f);
}
