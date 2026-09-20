// ScummVM's OSystem, implemented on the TRACE module API.
//
// This is the whole point of the door: ScummVM is built so that a platform only
// has to supply OSystem, and every engine then works. The TRACE API happens to
// line up almost exactly --
//
//     screen   -> trace_present()          (via TraceGraphicsManager)
//     audio    -> trace_audio_write()      (via TraceMixerManager)
//     input    <- trace_on_input()         (queued, drained by pollEvent)
//     files    <- trace_asset_read()       (via TraceArchive)
//     saves    -> trace_send() to the door (the BBS keeps the player's saves)
//     clock    <- trace_time_ms()
//     quit     -> trace_quit()
//
// so this file is mostly plumbing rather than emulation.
#ifndef TRACE_OSYSTEM_H
#define TRACE_OSYSTEM_H

#include "backends/modular-backend.h"
#include "common/events.h"
#include "common/queue.h"

class TraceGraphicsManager;
class TraceArchive;
class TraceSaveFileManager;

class OSystem_TRACE : public ModularMixerBackend, public ModularGraphicsBackend,
                      public Common::EventSource {
public:
	OSystem_TRACE();
	~OSystem_TRACE() override;

	void initBackend() override;

	bool pollEvent(Common::Event &event) override;

	Common::MutexInternal *createMutex() override;
	uint32 getMillis(bool skipRecord = false) override;
	void delayMillis(uint msecs) override;
	void getTimeAndDate(TimeDate &td, bool skipRecord = false) const override;
	void quit() override;
	void logMessage(LogMessageType::Type type, const char *message) override;
	void addSysArchivesToSearchSet(Common::SearchSet &s, int priority = 0) override;

	// Without this the keymapper warns "No hardware inputs were registered"
	// and silently maps nothing -- so the game's own shortcuts, including
	// skipping the intro, do nothing at all.
	Common::HardwareInputSet *getHardwareInputSet() override;

	// --- called by the module, not by ScummVM ---

	// Push an input event in from trace_on_input(). These run on the HOST
	// thread while the game thread is inside pollEvent(), so the queue is
	// guarded -- an unlocked Common::Queue reallocating under a concurrent pop
	// corrupts it, and the game wedges a few hundred frames later with nothing
	// to show for it.
	void pushKey(int scancode, bool down, int ascii);
	void pushMouseMove(int x, int y);
	void pushMouseButton(int button, bool down);
	void pushQuit();

	// Inject one of the engine's own actions directly. The keymapper is meant
	// to turn keys into these, but it has proved unreliable here -- and a
	// player who cannot reach the menu is stuck.
	void pushEngineAction(int customType);

	// The door names the game files; this is what makes them visible to the engine.
	bool addGameFile(const char *name, const char *hashes);

	// Called from the GAME thread, right after a frame goes out: audio, saves
	// and ScummVM's timers. Doing this from trace_update() instead would mean
	// two threads in the mixer, which is what the OSystem mutex exists to stop
	// -- and with a correct mutex that contends instead of racing.
	void pumpFromGameThread();

	TraceGraphicsManager *graphics() const { return _traceGraphics; }
	TraceSaveFileManager *saves() const { return _traceSaves; }
	bool wantsQuit() const { return _quit; }

private:
	TraceGraphicsManager *_traceGraphics = nullptr;
	TraceArchive *_archive = nullptr;
	TraceSaveFileManager *_traceSaves = nullptr;
	Common::Queue<Common::Event> _events;
	// An opaque pthread_mutex_t. It is not declared here because <pthread.h>
	// drags in <time.h>, which ScummVM's forbidden.h has already poisoned by
	// the time any of its headers have been included.
	void *_eventLock = nullptr;
	bool _quit = false;
};

#endif
