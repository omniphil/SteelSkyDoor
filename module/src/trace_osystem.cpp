// System headers first: forbidden.h poisons libc names.
#include <pthread.h>
#include <stdlib.h>

#define FORBIDDEN_SYMBOL_EXCEPTION_time_h

// System headers first: common/forbidden.h poisons libc names.
#include <time.h>

#define FORBIDDEN_SYMBOL_EXCEPTION_time_h

#include "trace_osystem.h"

#include "trace_graphics.h"
#include "trace_assets.h"
#include "trace_mixer.h"
#include "trace_fs.h"
#include "trace_saves.h"
#include "trace_stage.h"

#include "trace_mutex.h"
#include "backends/saves/default/default-saves.h"
#include "backends/timer/default/default-timer.h"
#include "backends/events/default/default-events.h"
#include "backends/keymapper/hardware-input.h"
#include "common/archive.h"
#include "common/config-manager.h"
#include "common/textconsole.h"

#include <string.h>

extern "C" {
#include "trace_api.h"
}

OSystem_TRACE::OSystem_TRACE() {
	_traceGraphics = new TraceGraphicsManager();
	_graphicsManager = _traceGraphics;
	_traceGraphics->setBackend(this);
	_mixerManager = new TraceMixerManager();
	_archive = new TraceArchive();
	// ScummVM asserts on this being null even for a platform with no disk.
	_fsFactory = new TraceFilesystemFactory();
	_eventLock = malloc(sizeof(pthread_mutex_t));
	pthread_mutex_init((pthread_mutex_t *)_eventLock, nullptr);
}

OSystem_TRACE::~OSystem_TRACE() {
	if (_eventLock) {
		pthread_mutex_destroy((pthread_mutex_t *)_eventLock);
		free(_eventLock);
	}
	delete _archive;
	// _graphicsManager and _mixerManager are deleted by the modular backends.
}

void OSystem_TRACE::initBackend() {
	_timerManager = new DefaultTimerManager();
	_eventManager = new DefaultEventManager(this);
	// Saves live on the BBS: the module has no disk, and anything kept only in
	// the sandbox would vanish when the picture closes.
	if (!_savefileManager) {
		_traceSaves = new TraceSaveFileManager();
		_savefileManager = _traceSaves;
	}

	_mixerManager->init();

	ModularGraphicsBackend::initBackend();
	ModularMixerBackend::initBackend();
}

// --- input ------------------------------------------------------------------

void OSystem_TRACE::pushKey(int scancode, bool down, int ascii) {
	Common::Event e;
	e.type = down ? Common::EVENT_KEYDOWN : Common::EVENT_KEYUP;
	e.kbd.keycode = (Common::KeyCode)scancode;
	e.kbd.ascii = ascii;
	e.kbd.flags = 0;
	pthread_mutex_lock((pthread_mutex_t *)_eventLock);
	_events.push(e);
	pthread_mutex_unlock((pthread_mutex_t *)_eventLock);
}

void OSystem_TRACE::pushMouseMove(int x, int y) {
	// The graphics manager draws the cursor, so it needs to know too.
	if (_traceGraphics) _traceGraphics->setMousePos(x, y);
	Common::Event e;
	e.type = Common::EVENT_MOUSEMOVE;
	e.mouse.x = x;
	e.mouse.y = y;
	pthread_mutex_lock((pthread_mutex_t *)_eventLock);
	_events.push(e);
	pthread_mutex_unlock((pthread_mutex_t *)_eventLock);
}

void OSystem_TRACE::pushMouseButton(int button, bool down) {
	Common::Event e;
	if (button == 1)
		e.type = down ? Common::EVENT_LBUTTONDOWN : Common::EVENT_LBUTTONUP;
	else
		e.type = down ? Common::EVENT_RBUTTONDOWN : Common::EVENT_RBUTTONUP;
	e.mouse.x = _traceGraphics ? _traceGraphics->mouseX() : 0;
	e.mouse.y = _traceGraphics ? _traceGraphics->mouseY() : 0;
	pthread_mutex_lock((pthread_mutex_t *)_eventLock);
	_events.push(e);
	pthread_mutex_unlock((pthread_mutex_t *)_eventLock);
}

void OSystem_TRACE::pushEngineAction(int customType) {
	Common::Event e;
	e.type = Common::EVENT_CUSTOM_ENGINE_ACTION_START;
	e.customType = (uint32)customType;
	pthread_mutex_lock((pthread_mutex_t *)_eventLock);
	_events.push(e);
	pthread_mutex_unlock((pthread_mutex_t *)_eventLock);
}

void OSystem_TRACE::pushQuit() {
	Common::Event e;
	e.type = Common::EVENT_QUIT;
	// 🚨 Under the lock like every other push: this one runs on the module's
	// input thread while the game thread is popping in pollEvent(). It was the
	// only pusher that didn't take it, which is a corrupted queue waiting to
	// happen -- and the crash it causes lands nowhere near here.
	pthread_mutex_lock((pthread_mutex_t *)_eventLock);
	_events.push(e);
	pthread_mutex_unlock((pthread_mutex_t *)_eventLock);
	_quit = true;
}

bool OSystem_TRACE::pollEvent(Common::Event &event) {
	trace_stage("pollEvent");
	g_pollCount++;
	// The engine polls far more often than it idles, and holds nothing here, so
	// this is the best place to keep the sound fed.
	pumpFromGameThread();

	pthread_mutex_lock((pthread_mutex_t *)_eventLock);
	const bool got = !_events.empty();
	if (got) event = _events.pop();
	pthread_mutex_unlock((pthread_mutex_t *)_eventLock);
	return got;
}

// --- the rest of the platform ----------------------------------------------

Common::MutexInternal *OSystem_TRACE::createMutex() {
	trace_stage("mutex: allocating");
	TraceMutexInternal *m = new TraceMutexInternal();
	trace_stage("mutex: made");
	return m;
}

uint32 OSystem_TRACE::getMillis(bool skipRecord) {
	trace_stage("getMillis");
	return (uint32)trace_time_ms();
}

void OSystem_TRACE::delayMillis(uint msecs) {
	trace_stage("delayMillis");
	g_delayCount++;
	// The engine's idle point, and the only place we know it holds no lock of
	// its own -- so this is where audio, saves and timers get their turn.
	pumpFromGameThread();
	// This MUST really sleep. The engine runs on its own thread, so blocking
	// here costs the host nothing -- and without it the game spins as fast as
	// the CPU allows: a 30-second run presented 415,953 frames instead of about
	// 1,500, flooding trace_present() and burning a core for nothing.
	struct timespec ts;
	ts.tv_sec = msecs / 1000;
	ts.tv_nsec = (long)(msecs % 1000) * 1000000L;
	nanosleep(&ts, nullptr);
}

void OSystem_TRACE::getTimeAndDate(TimeDate &td, bool skipRecord) const {
	// A module has no clock beyond trace_time_ms(), and no business knowing the
	// player's date. Saves are named by the engine, not by this.
	memset(&td, 0, sizeof(td));
	td.tm_year = 100;  // 2000
	td.tm_mday = 1;
}

void OSystem_TRACE::quit() {
	trace_quit(0);
}

char g_traceLog[TRACE_LOG_LINES][TRACE_LOG_WIDTH];
int  g_traceLogCount = 0;
bool g_introHint = false;
#if TRACE_SOUND_DEBUG
char g_soundDebug[64] = {0};
#endif
// Set by ear on the BBS, 2026-09-20. The odd one is the effects channel at 8
// out of 256: the game's atmospheric background score is not music to this
// engine -- intro.cpp's LOOPBG and the in-game ambience play as SOUND EFFECTS
// -- and that bed is mastered far hotter than anything else. 16 was too loud
// and 0 silenced it, so 8 is the midpoint between the two values actually
// heard rather than a level tested on its own.
// g_volSynth is 150 rather than 90 to pay for re-centring the panel's music slider:
// the AdLib driver's own volume went 107 -> 64 with it (adlibchannel.cpp scales
// linearly by _musicVolume), and 150 x 64 == 90 x 107, so the music sounds the same.
int g_volSynth = 150, g_volMusic = 90, g_volSfx = 20, g_volSpeech = 40;
volatile unsigned g_pollCount = 0, g_delayCount = 0;

// One presenter at a time, whichever thread it is.
static pthread_mutex_t g_presentLock = PTHREAD_MUTEX_INITIALIZER;
void trace_present_locked(const unsigned int *pixels, int w, int h, int flags) {
	pthread_mutex_lock(&g_presentLock);
	trace_present(pixels, w, h, flags);
	pthread_mutex_unlock(&g_presentLock);
}

// Keep the tail of what the engine says, trimmed to fit the screen. warning()
// and error() come through here, and they normally vanish into TERMinator's
// debug output where a caller cannot see them.
void trace_note(const char *text) {
	if (!text) return;
	while (*text == '\n' || *text == '\r' || *text == ' ') text++;
	if (!*text) return;
	if (g_traceLogCount >= TRACE_LOG_LINES) {
		for (int i = 1; i < TRACE_LOG_LINES; i++)
			memcpy(g_traceLog[i - 1], g_traceLog[i], TRACE_LOG_WIDTH);
		g_traceLogCount = TRACE_LOG_LINES - 1;
	}
	int n = 0;
	char *dst = g_traceLog[g_traceLogCount];
	while (*text && n < TRACE_LOG_WIDTH - 1) {
		dst[n++] = (*text == '\n' || *text == '\r') ? ' ' : *text;
		text++;
	}
	dst[n] = 0;
	g_traceLogCount++;
}

void OSystem_TRACE::logMessage(LogMessageType::Type type, const char *message) {
	(void)type;
	if (!message) return;
	trace_note(message);
	trace_log(message, (int32)strlen(message));
}

void OSystem_TRACE::pumpFromGameThread() {
	trace_stage("pump: mixer");
	if (_mixerManager) ((TraceMixerManager *)_mixerManager)->pump();
	trace_stage("pump: saves");
	if (_traceSaves) _traceSaves->pump();
	// ScummVM's timers are driven by the backend; the null backend does this in
	// its own loop. Nothing runs them otherwise.
	trace_stage("pump: timers");
	if (_timerManager) ((DefaultTimerManager *)_timerManager)->checkTimers();
	trace_stage("running");
}

bool OSystem_TRACE::addGameFile(const char *name, const char *hashes) {
	return _archive && _archive->addFile(name, hashes);
}

Common::HardwareInputSet *OSystem_TRACE::getHardwareInputSet() {
	using namespace Common;
	CompositeHardwareInputSet *set = new CompositeHardwareInputSet();
	set->addHardwareInputSet(new MouseHardwareInputSet(defaultMouseButtons));
	set->addHardwareInputSet(new KeyboardHardwareInputSet(defaultKeys, defaultModifiers));
	return set;
}

void OSystem_TRACE::addSysArchivesToSearchSet(Common::SearchSet &s, int priority) {
	// This is the hook that makes the game's files findable. There is no
	// filesystem; everything comes from the asset store.
	if (_archive) s.add("trace-assets", _archive, priority, false);
}
