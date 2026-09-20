// A filesystem that isn't there.
//
// A TRACE module has no disk: the game's files come from the asset store (see
// trace_assets.h) and the player's saves go to the door. But ScummVM assumes a
// platform has *some* filesystem and asserts on the factory being null, so this
// supplies one that politely reports an empty world.
//
// Everything the engine actually opens is found through the SearchSet that
// OSystem_TRACE::addSysArchivesToSearchSet() installs, which never goes through
// here.
#ifndef TRACE_FS_H
#define TRACE_FS_H

#include "backends/fs/abstract-fs.h"
#include "backends/fs/fs-factory.h"

class TraceFSNode : public AbstractFSNode {
public:
	explicit TraceFSNode(const Common::String &path = "") : _path(path) {}

	AbstractFSNode *getChild(const Common::String &name) const override {
		return new TraceFSNode(_path.empty() ? name : _path + "/" + name);
	}
	AbstractFSNode *getParent() const override { return new TraceFSNode(); }

	bool exists() const override { return false; }
	bool getChildren(AbstractFSList &list, ListMode mode, bool hidden) const override {
		(void)list; (void)mode; (void)hidden;
		return true;                       // an empty directory, not an error
	}
	Common::U32String getDisplayName() const override { return Common::U32String(_path); }
	Common::String getName() const override { return _path; }
	Common::String getPath() const override { return _path; }
	bool isDirectory() const override { return true; }
	bool isReadable() const override { return false; }
	bool isWritable() const override { return false; }

	Common::SeekableReadStream *createReadStream() override { return nullptr; }
	Common::SeekableWriteStream *createWriteStream(bool atomic) override {
		(void)atomic;
		return nullptr;
	}
	bool createDirectory() override { return false; }

private:
	Common::String _path;
};

class TraceFilesystemFactory : public FilesystemFactory {
public:
	AbstractFSNode *makeRootFileNode() const override { return new TraceFSNode(); }
	AbstractFSNode *makeCurrentDirectoryFileNode() const override { return new TraceFSNode(); }
	AbstractFSNode *makeFileNodePath(const Common::String &path) const override {
		return new TraceFSNode(path);
	}
};

#endif
