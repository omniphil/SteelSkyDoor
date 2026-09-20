#include "trace_saves.h"

#include "common/memstream.h"

#include <stdio.h>
#include <string.h>

extern "C" {
#include "trace_api.h"
}

// Keep a chunk well inside TRACE_SEND_MAX so the header fits too.
static const uint32 kChunk = 3000;

namespace {

// A save the game is writing. ScummVM gives us a WriteStream and tells us
// nothing until it is closed, so we buffer and hand the bytes over on destruction.
class TraceOutSave : public Common::OutSaveFile {
public:
	TraceOutSave(TraceSaveFileManager *mgr, const Common::String &name,
	             Common::MemoryWriteStreamDynamic *ws)
	    : Common::OutSaveFile(ws), _mgr(mgr), _name(name), _ws(ws) {}

	~TraceOutSave() override { finalize(); }

	// ScummVM tells us nothing while the game writes, so the bytes go to the
	// door here, when it closes the save.
	void finalize() override {
		if (_done) return;
		_done = true;
		Common::OutSaveFile::finalize();
		if (_ws && _mgr) _mgr->store(_name, _ws->getData(), _ws->size());
	}

private:
	TraceSaveFileManager *_mgr;
	Common::String _name;
	Common::MemoryWriteStreamDynamic *_ws;
	bool _done = false;
};

} // namespace

Common::OutSaveFile *TraceSaveFileManager::openForSaving(const Common::String &name, bool compress) {
	(void)compress;    // the door stores what we give it; no need to compress twice
	return new TraceOutSave(this, name,
	                        new Common::MemoryWriteStreamDynamic(DisposeAfterUse::YES));
}

Common::InSaveFile *TraceSaveFileManager::openForLoading(const Common::String &name) {
	return openRawFile(name);
}

Common::InSaveFile *TraceSaveFileManager::openRawFile(const Common::String &name) {
    FileMap::iterator it = _files.find(name);
	if (it == _files.end()) return nullptr;
	const Common::Array<byte> &d = it->_value;
	if (d.empty()) return nullptr;
	// The stream copies nothing; the map owns the bytes for the session.
	return new Common::MemoryReadStream(&d[0], d.size(), DisposeAfterUse::NO);
}

bool TraceSaveFileManager::removeSavefile(const Common::String &name) {
	if (!_files.contains(name)) return false;
	_files.erase(name);
	// Tell the door by sending an empty file; it deletes on a zero-length save.
	store(name, nullptr, 0);
	return true;
}

Common::StringArray TraceSaveFileManager::listSavefiles(const Common::String &pattern) {
	Common::StringArray out;
	for (FileMap::const_iterator it = _files.begin(); it != _files.end(); ++it)
		if (it->_key.matchString(pattern, true)) out.push_back(it->_key);
	return out;
}

void TraceSaveFileManager::updateSavefilesList(Common::StringArray &lockedFiles) {
	(void)lockedFiles;   // nothing is locked: one caller, one game
}

bool TraceSaveFileManager::exists(const Common::String &name) {
	return _files.contains(name);
}

void TraceSaveFileManager::receiveChunk(const char *name, uint32 offset, uint32 total,
                                        const byte *data, uint32 len) {
	Common::Array<byte> &f = _files[Common::String(name)];
	if (f.size() < total) f.resize(total);
	for (uint32 i = 0; i < len && offset + i < total; i++)
		f[offset + i] = data[i];
}

void TraceSaveFileManager::store(const Common::String &name, const byte *data, uint32 len) {
	Common::Array<byte> &f = _files[name];
	f.clear();
	f.resize(len);
	if (len && data) memcpy(&f[0], data, len);

	// Queue it for the door, replacing any earlier copy still waiting.
	for (uint i = 0; i < _outbox.size(); i++)
		if (_outbox[i].equalsIgnoreCase(name)) { _outbox.remove_at(i); break; }
	_outbox.push_back(name);
	if (_outbox.size() == 1) _outOffset = 0;
}

bool TraceSaveFileManager::pump() {
	if (_outbox.empty()) return false;

	const Common::String &name = _outbox[0];
	FileMap::iterator it = _files.find(name);
	if (it == _files.end()) { _outbox.remove_at(0); _outOffset = 0; return !_outbox.empty(); }

	const Common::Array<byte> &d = it->_value;
	const uint32 total = d.size();

	char head[128];
	const int headLen = snprintf(head, sizeof head, "put %s %u %u\n",
	                             name.c_str(), _outOffset, total);

	uint32 want = total - _outOffset;
	if (want > kChunk) want = kChunk;

	// Don't build the message unless the link will take it.
	if ((uint32)trace_send_room() < (uint32)headLen + want) return true;

	byte msg[128 + kChunk];
	memcpy(msg, head, headLen);
	if (want) memcpy(msg + headLen, &d[_outOffset], want);
	if (trace_send(msg, (int32_t)(headLen + want)) <= 0) return true;

	_outOffset += want;
	if (_outOffset >= total) {          // that file is done
		_outbox.remove_at(0);
		_outOffset = 0;
	}
	return !_outbox.empty();
}
