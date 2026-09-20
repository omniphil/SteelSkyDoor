// The TRACE module for Beneath a Steel Sky.
//
// TERMinator runs this in its WASM sandbox. It owns an OSystem_TRACE, starts
// ScummVM's sky engine on its own thread (the engine's main loop blocks, as it
// does on every platform), and forwards input in and frames out.
//
// The door never sends pictures: it sends the game once, and everything after
// that happens here.
// System headers must come BEFORE any ScummVM header: common/forbidden.h
// poisons a pile of libc names to stop engines calling them directly, and the
// poison macros wreck <time.h> if it is pulled in afterwards. The exceptions
// below are the same mechanism ScummVM's own backends use.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define FORBIDDEN_SYMBOL_EXCEPTION_time_h
#define FORBIDDEN_SYMBOL_EXCEPTION_exit

#include "common/scummsys.h"
#include "common/system.h"
#include "common/config-manager.h"
#include "common/str.h"
#include "base/commandLine.h"
#include "base/plugins.h"
#include "backends/keymapper/keymapper.h"
#include "engines/metaengine.h"
#include "audio/mixer.h"
#include "engines/sky/sky.h"

#include "trace_osystem.h"
#include "trace_graphics.h"
#include "trace_mixer.h"
#include "trace_saves.h"
#include "trace_status.h"
#include "trace_stage.h"

extern "C" {
#include "trace_api.h"
}

// ScummVM's global, which everything reaches through.
extern OSystem *g_system;

namespace {

OSystem_TRACE *g_trace = nullptr;
pthread_t g_gameThread;
bool g_started = false;
bool g_quitting = false;
TraceStatus g_status;
}
const char *g_traceStage = "not started";
namespace {
bool g_filesNamed = false, g_threadUp = false, g_engineEntered = false;

// The engine's run() is protected, as it is meant to be driven by ScummVM's
// own main(). We only want the one engine, so reach it directly.
class SkyRunner : public Sky::SkyEngine {
public:
	explicit SkyRunner(OSystem *s) : Sky::SkyEngine(s) {}
	Common::Error go() { return run(); }
};

void *gameMain(void *) {
	// ScummVM's main() does this before any engine runs; without it the music
	// plugins are unregistered and the engine dies at startup with
	// "Music plugins must be loaded prior to calling this method".
	trace_stage("plugins");
	PluginManager::instance().init();
	PluginManager::instance().loadAllPlugins();

	// ScummVM normally gets here via its launcher, which creates a "target"
	// (a config domain naming the game) and makes it active. There is no
	// launcher here -- the door already decided what we are playing -- so set
	// one up directly. The keymapper refuses to register game keymaps without
	// an active domain.
	trace_stage("config");
	// A SIXTH thing ScummVM's main() does that a module must repeat. Without
	// it not one default is registered, so every ConfMan.getInt() below the
	// engine reads a key that was never given a value -- which is why the
	// music came out at a different level from the speech.
	Base::registerDefaults();
	ConfMan.registerDefault("path", "trace-assets");
	if (!ConfMan.hasGameDomain("sky"))
		ConfMan.addGameDomain("sky");
	ConfMan.setActiveDomain("sky");
	ConfMan.set("gameid", "sky");
	ConfMan.set("engineid", "sky");
	ConfMan.set("path", "trace-assets");
	ConfMan.set("description", "Beneath a Steel Sky");

	// Speech AND subtitles. The CD version is here for the voice acting, but a
	// caller may have the sound down or a rough link, so the text stays on too.
	ConfMan.setBool("speech_mute", false);
	ConfMan.setBool("subtitles", true);

	// Sound levels. These mirror the module's own g_vol* values, which are
	// what actually reach the mixer -- TraceMixerManager re-applies those
	// every pump, because syncSoundSettings() resets the mixer whenever the
	// player touches the F5 panel. Setting them here too keeps the engine's
	// own state honest: sky.cpp halves music_volume into the 0-127 range it
	// hands to the AdLib driver, and the panel's slider reads from it.
	//
	// 🚨 The one that matters is sfx_volume, and it is not obvious why.
	// The atmospheric background score is NOT music to this engine:
	// intro.cpp's LOOPBG/PLAYBG and the in-game ambience play on
	// kSFXSoundType. So the score rides on the sound-EFFECTS channel, and
	// music_volume -- along with the AdLib synth's own volume and the mixer's
	// kMusicSoundType -- has nothing to do with it. That bed is mastered far
	// hotter than the voices too, hence 20 against 256 for everything else.
	// 2026-09-20: 200/8 -> 215/20 after listening on a second PC, where the
	// whole mix came out quieter. Raising sfx_volume is what lifts the score.
	// 128 of 256 puts the control panel's music slider in the MIDDLE, with room
	// to go both ways. It is only a starting position, not the loudness: sky.cpp
	// turns it into the AdLib driver's own 0-127 volume (music_volume >> 1 = 64),
	// and g_volSynth below is set so that 64 sounds exactly like the old 215/107
	// did. Raising the slider from there makes the music louder than our tuned
	// level, lowering it quieter, which is what a slider is for.
	ConfMan.setInt("music_volume", 128);
	ConfMan.setInt("sfx_volume", 20);
	ConfMan.setInt("speech_volume", 256);

	// ScummVM's main() registers the engine's keymaps before running it, taking
	// them from the MetaEngine. The sky engine asserts on its shortcuts keymap
	// existing, so this is not optional even though we never show ScummVM's UI.
	// SkyMetaEngine has no header of its own, so reach it through the plugin
	// the LINK_PLUGIN(SKY) table registered.
	{
		trace_stage("keymaps");
		Common::Keymapper *keymapper0 = g_system->getEventManager()->getKeymapper();
		keymapper0->clear();

		// ScummVM's main() does this before adding any keymap. Without it the
		// keymapper warns "No hardware inputs were registered" and maps nothing
		// -- so the game's own shortcuts do nothing, including skipping the
		// intro (which is kSkyActionSkip, an action, not a raw Escape).
		keymapper0->registerHardwareInputSet(g_system->getHardwareInputSet(),
		                                     g_system->getKeymapperDefaultBindings());

		Common::Keymap *globalKeymap = g_system->getEventManager()->getGlobalKeymap();
		if (globalKeymap) keymapper0->addGlobalKeymap(globalKeymap);

		// 🚨 GuiManager::enableKeymap() switches the keymapper between GUI and
		// Game keymaps, and initialising the GUI (which the engine's debugger
		// forces) can leave it in GUI mode -- with every game keymap disabled.
		// That is why F5, Escape and the rest did nothing at all.
		keymapper0->setEnabledKeymapType(Common::Keymap::kKeymapTypeGame);

		const PluginList &plugins = PluginMan.getPlugins(PLUGIN_TYPE_ENGINE);
		for (uint i = 0; i < plugins.size(); i++) {
			MetaEngine &meta = plugins[i]->get<MetaEngine>();
			Common::KeymapArray keymaps = meta.initKeymaps("sky");
			Common::Keymapper *keymapper = g_system->getEventManager()->getKeymapper();
			for (uint k = 0; k < keymaps.size(); k++)
				keymapper->addGameKeymap(keymaps[k]);
		}
	}

	g_engineEntered = true;
	trace_stage("engine ctor");
	SkyRunner *engine = new SkyRunner(g_system);
	trace_stage("engine run");
	engine->go();
	trace_stage("engine returned");
	delete engine;

	// The player quit from the game's own control panel.
	trace_quit(0);
	return nullptr;
}

// The sky engine's own actions (engines/sky/detection.h). The keymapper should
// deliver these, but it has not worked reliably here, so the module sends the
// important ones itself.
enum {
	kSkyActionOpenControlPanel = 3,
	kSkyActionConfirm          = 4,
	kSkyActionSkip             = 5,
	kSkyActionSkipLine         = 6,
	kSkyActionPause            = 7,
};

// Is the intro still running? The engine tracks this itself, so ask it rather
// than guessing from a timer -- a player who sits through half the intro and
// then presses Escape should still get what they expect.
bool introPlaying() {
	return Sky::SkyEngine::_systemVars && !Sky::SkyEngine::_systemVars->pastIntro;
}

// Set-1 scancodes to characters, for the build that cannot use trace_text_input.
// Crude next to the real thing -- it assumes a US layout -- but it is enough to
// name a saved game, which is the only place this game takes typing.
char asciiFromScancode(int sc, bool shift) {
	static const char *kLower = "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0"
	                            "asdfghjkl;'`" "\0" "\\zxcvbnm,./";
	static const char *kUpper = "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0"
	                            "ASDFGHJKL:\"~" "\0" "|ZXCVBNM<>?";
	if (sc == 0x39) return ' ';
	if (sc < 0 || sc >= (int)strlen(kLower)) return 0;
	const char c = (shift ? kUpper : kLower)[sc];
	return c ? c : 0;
}

// Set-1 scancodes to ScummVM key codes. Only what a point-and-click needs:
// the engine itself is driven almost entirely by the mouse.
Common::KeyCode keyFromScancode(int sc, int &ascii) {
	ascii = 0;
	switch (sc) {
	case 0x01: return Common::KEYCODE_ESCAPE;
	case 0x1C: ascii = 13; return Common::KEYCODE_RETURN;
	case 0x39: ascii = ' '; return Common::KEYCODE_SPACE;
	case 0x0E: return Common::KEYCODE_BACKSPACE;
	case 0x0F: ascii = 9;  return Common::KEYCODE_TAB;
	case 0x48: return Common::KEYCODE_UP;
	case 0x50: return Common::KEYCODE_DOWN;
	case 0x4B: return Common::KEYCODE_LEFT;
	case 0x4D: return Common::KEYCODE_RIGHT;
	case 0x3B: return Common::KEYCODE_F1;
	case 0x3C: return Common::KEYCODE_F2;
	case 0x3D: return Common::KEYCODE_F3;
	case 0x3E: return Common::KEYCODE_F4;
	case 0x3F: return Common::KEYCODE_F5;
	case 0x40: return Common::KEYCODE_F6;
	default:   return Common::KEYCODE_INVALID;
	}
}

// The keyboard cursor, for TRACE clients with no mouse. Arrows nudge it and
// Enter clicks, so the game plays without a pointer -- the same approach the
// Micropolis door uses, and the one console point-and-click ports took.
int g_curX = 160, g_curY = 100;
bool g_haveRealMouse = false;
// True once TERMinator has sent a real typed character, so we stop guessing at
// them from scancodes.
bool g_haveTextInput = false;
// Whether the control panel is believed to be up. Only ever a belief: the
// player can also close the panel by clicking its own buttons, which the
// module never sees. Being wrong wastes one Escape press, nothing more.
bool g_panelOpen = false;
// Whether the intro proper has begun, as opposed to the logo screens that run
// first. Drives the "ESC skips the intro" hint.
int  g_introStarted = 0;        // when the intro proper began, 0 until it has
int  g_introFirstSeen = 0;
// How long after the intro starts before the skip hint appears.
const int kIntroHintDelayMs = 5000;

// A breadcrumb for the crash report. TE_OUT_LOG lines are kept by TERMinator
// as the module's output tail, and when a module dies rather than quitting the
// client puts the last one in the Closed report the door gets ("...|last: ..."),
// so the sysop's log says what the module was doing when it went. The engine
// stage (g_traceStage, updated by every OSystem call) rides along, because
// "opening the control panel" is only half the story without "...from inside
// delay()". Cheap enough to leave in: a handful of lines a session.
void sky_breadcrumb(const char *what) {
	char line[96];
	const char *stage = g_traceStage ? g_traceStage : "?";
	int n = snprintf(line, sizeof(line), "steelsky: %s [%s]", what, stage);
	if (n > 0)
		trace_log(line, (int32_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

void moveCursor(int dx, int dy) {
	g_curX += dx;
	g_curY += dy;
	if (g_curX < 0) g_curX = 0;
	if (g_curY < 0) g_curY = 0;
	if (g_curX > 319) g_curX = 319;
	if (g_curY > 199) g_curY = 199;
	if (g_trace) g_trace->pushMouseMove(g_curX, g_curY);
}

} // namespace

// --- the TRACE module contract ---------------------------------------------

extern "C" {

int32_t trace_init(void) {
	g_trace = new OSystem_TRACE();
	g_system = g_trace;
#ifndef SKY_NO_MOUSE
	// Only the mouse build imports this. WASM imports are static, so a module
	// that imports a host function the player's TERMinator lacks will not start
	// at all -- and they would have downloaded 72 MB first. The door ships a
	// keyboard-only build too and picks from the Query reply.
	// HIDDEN, not POINTER: we still get absolute positions, but Windows' own
	// pointer is hidden over the picture. The game draws its own cursor (the
	// graphics manager composites it), so showing both looks wrong.
	trace_mouse_mode(TRACE_MOUSE_HIDDEN);
	// The game lets you name a saved game, so it needs real typed characters --
	// with the player's own layout, shift and accents. Tier 1 API, so only the
	// mouse build asks for it; the basic build falls back to the scancode table.
#ifndef SKY_NO_MOUSE
	trace_text_input(1);
#endif
#endif
	trace_set_tick(50);            // the engine runs at 50Hz; audio needs pumping
	return 0;
}

void *trace_alloc(int32_t size) { return size > 0 ? malloc((size_t)size) : nullptr; }
void trace_free(void *p) { free(p); }

void trace_on_resize(int32_t w, int32_t h) {
	(void)w; (void)h;              // we always render 320x200 and let the host scale
}

// The door tells us which assets hold the game, then we start.
//
//   files sky.dsk=<h1>,<h2> sky.cpt=<h> sky.dnr=<h>
//
// sky.dsk may name several hashes because the CD version is 72 MB and a single
// asset is capped at 64 MB.
void trace_on_data(const char *data, int32_t length) {
	if (!data || length <= 0 || !g_trace) return;

	// A piece of one of the player's saved games, coming back from the BBS:
	//   save <name> <offset> <total>\n<bytes>
	if (length > 5 && memcmp(data, "save ", 5) == 0) {
		const char *nl = (const char *)memchr(data, '\n', length);
		if (!nl) return;
		char name[64]; unsigned off = 0, total = 0;
		if (sscanf(data + 5, "%63s %u %u", name, &off, &total) == 3 && g_trace->saves())
			g_trace->saves()->receiveChunk(name, off, total,
			                               (const byte *)nl + 1,
			                               (uint32)(length - (nl + 1 - data)));
		return;
	}

	if (length >= 4 && memcmp(data, "quit", 4) == 0) {
		if (g_started) g_trace->pushQuit();
		else trace_quit(0);
		return;
	}

	if (length > 6 && memcmp(data, "files ", 6) == 0 && !g_started) {
		Common::String rest(data + 6, length - 6);
		// split on spaces into name=hashes pairs
		uint start = 0;
		while (start < rest.size()) {
			uint end = start;
			while (end < rest.size() && rest[end] != ' ' && rest[end] != '\n') end++;
			Common::String pair(rest.c_str() + start, end - start);
			const uint eq = pair.findFirstOf('=');
			if (eq != Common::String::npos) {
				Common::String name(pair.c_str(), eq);
				Common::String hashes(pair.c_str() + eq + 1);
				if (!g_trace->addGameFile(name.c_str(), hashes.c_str())) {
					const char *msg = "steelsky: an asset the door named is missing";
					trace_log(msg, (int32_t)strlen(msg));
					trace_quit(1);
					return;
				}
			}
			start = end + 1;
		}

		g_trace->initBackend();
		g_started = true;

		// ScummVM is stack-hungry and a wasm thread's default stack is small,
		// so ask for a real one -- the Wolf3D module does the same. Without it
		// the game thread dies without a word and the picture just closes.
		pthread_attr_t attr;
		pthread_attr_init(&attr);
		pthread_attr_setstacksize(&attr, 8 * 1024 * 1024);
		g_filesNamed = true;
		if (pthread_create(&g_gameThread, &attr, gameMain, nullptr) != 0) {
			const char *msg = "steelsky: could not start the game thread";
			trace_log(msg, (int32_t)strlen(msg));
			trace_quit(1);
		}
		g_threadUp = true;
		pthread_attr_destroy(&attr);
	}
}

void trace_on_input(int32_t type, int32_t flags, int32_t a, int32_t b, int32_t c) {
	if (!g_trace) return;

	// While the diagnostic screen is up the game is never going to start, so
	// any key gets the caller out rather than stranding them until they drop
	// the carrier.
	if (g_status.showing() && type == 1 && (flags & 1)) {
		trace_quit(0);
		return;
	}

	// A character the player typed, already through their own keyboard layout.
	// The game only takes typing when naming a saved game.
	if (type == TRACE_INPUT_TEXT) {
		if (a > 0 && a < 0x7F) {
			g_haveTextInput = true;
			g_trace->pushKey(Common::KEYCODE_INVALID, true, a);
		}
		return;
	}

	switch (type) {
	case 1: {                                   // TE_IN_KEY
		const bool down = (flags & 1) != 0;

		// Enter in the CONTROL PANEL means confirm, and the panel only takes that
		// as an action -- kSkyActionConfirm, not a key and not a click
		// (control.cpp's save-name dialog, "enter pressed").
		//
		// 🚨 This has to sit ABOVE the keyboard-cursor block, for two separate
		// reasons that each hid it on their own: that block returns early on
		// Enter, AND it is skipped entirely once g_haveRealMouse latches -- which
		// any TERMinator that sends mouse positions does the moment the player
		// nudges the mouse, even if they then play from the keyboard.
		// Sent as well as whatever else Enter does, never instead: the game loop
		// has no case for Confirm (sky.cpp handleKey), so it is a no-op there and
		// a stale g_panelOpen cannot cost the player "use".
		if (down && a == 0x1C && g_panelOpen) {
			sky_breadcrumb("enter: confirm (panel)");
			g_trace->pushEngineAction(kSkyActionConfirm);
		}

		// Without a mouse, the arrows drive the cursor instead of the engine.
		if (!g_haveRealMouse && down) {
			const int step = (flags & 4) ? 10 : 2;   // Shift moves faster
			switch (a) {
			case 0x4B: moveCursor(-step, 0); return;
			case 0x4D: moveCursor(step, 0);  return;
			case 0x48: moveCursor(0, -step); return;
			case 0x50: moveCursor(0, step);  return;
			case 0x1C: g_trace->pushMouseButton(1, true);
			           g_trace->pushMouseButton(1, false); return;   // Enter = use
			case 0x39: g_trace->pushMouseButton(2, true);
			           g_trace->pushMouseButton(2, false); return;   // Space = look
			default: break;
			}
		}
		// Escape, in a game that has no Escape binding.
		//
		// The engine maps Escape to Skip, which skips the intro and CLOSES the
		// control panel (control.cpp waits for kSkyActionSkip). It has no
		// Escape-OPENS-the-menu binding at all -- F5 is the only way in -- so
		// the module asks for the panel itself. Which means Escape has to mean
		// two different things depending on whether the panel is already up,
		// and only the module can decide which.
		//
		// The latch is a belief, not a fact: the player can also close the
		// panel with its own buttons, which we never see. Being wrong costs
		// one keypress. Past the intro Skip is a no-op in the game loop
		// (guarded on !pastIntro), and an unexpected Open is ignored by the
		// panel, so a wrong guess is always harmless.
		if (down && a == 0x01 && !introPlaying()) {
			if (!g_panelOpen) {
				sky_breadcrumb("esc: opening the control panel");
				g_trace->pushEngineAction(kSkyActionOpenControlPanel);
				g_panelOpen = true;
				break;   // 🚨 and NOTHING else -- see below
			}
			// The latch says the panel is already up, so the Escape KEY goes
			// through instead. This is the path the crashing build tripped on,
			// and the latch is only a belief -- so say so in the breadcrumb.
			sky_breadcrumb("esc: panel believed open, sending the key");
			g_panelOpen = false;   // fall through: the key itself closes it
		}
		// 🚨 Sending the Escape key TOO is what made this look dead. The queue
		// would be [Open, Skip]: the game loop opens the panel, and the very
		// first delay() inside the panel eats the Skip that is still sitting
		// there and closes it again -- the panel flashed open and shut in one
		// frame. (That stray Skip is also what the crashing build tripped on.)
		// So it is one or the other: the action to open, the key to close.
		// F5 opens the panel through the keymapper, bypassing the latch above,
		// so note it or the next Escape would try to open an open panel.
		if (down && a == 0x3F) {
			sky_breadcrumb("f5: control panel");
			g_panelOpen = true;
		}

#if TRACE_SOUND_DEBUG
		// Live level control, so the balance can be found by ear in one
		// sitting instead of one guess per rebuild. F7/F8 effects and the
		// background bed, F9/F10 the score. Both music paths move together:
		// the synth (kPlainSoundType) and digital tracks (kMusicSoundType).
		if (down && (a >= 0x41 && a <= 0x44)) {
			const int step = 16;
			if (a == 0x41) g_volSfx -= step;
			if (a == 0x42) g_volSfx += step;
			if (a == 0x43) { g_volMusic -= step; g_volSynth -= step; }
			if (a == 0x44) { g_volMusic += step; g_volSynth += step; }
			if (g_volSfx   < 0) g_volSfx   = 0;   if (g_volSfx   > 256) g_volSfx   = 256;
			if (g_volMusic < 0) g_volMusic = 0;   if (g_volMusic > 256) g_volMusic = 256;
			if (g_volSynth < 0) g_volSynth = 0;   if (g_volSynth > 256) g_volSynth = 256;
			break;
		}
#endif


		int ascii = 0;
		const Common::KeyCode kc = keyFromScancode(a, ascii);
		if (kc != Common::KEYCODE_INVALID) {
			g_trace->pushKey(kc, down, ascii);
		} else if (down && !g_haveTextInput) {
			// No text_input on this client, so make a character ourselves --
			// otherwise a saved game cannot be named.
			const char ch = asciiFromScancode(a, (flags & 4) != 0);
			if (ch) g_trace->pushKey(Common::KEYCODE_INVALID, true, (unsigned char)ch);
		}
		break;
	}
#ifndef SKY_NO_MOUSE
	case TRACE_INPUT_MOUSE_POS:
		g_haveRealMouse = true;
		if (flags & 1) {                        // over the picture
			g_curX = b; g_curY = c;
			g_trace->pushMouseMove(b, c);
		}
		break;
	case TRACE_INPUT_MOUSE_BUTTON:
		if (a == 1 || a == 3)
			g_trace->pushMouseButton(a == 1 ? 1 : 2, (flags & 1) != 0);
		break;
#endif /* SKY_NO_MOUSE */

	case 6:                                     // TE_IN_QUIT
		if (g_started) g_trace->pushQuit(); else trace_quit(0);
		g_quitting = true;
		break;
	default:
		break;
	}
}

// Called by the host after each batch of events, and at the tick rate. The game
// runs on its own thread, so this only keeps the sound topped up.
void trace_update(void) {
	if (!g_trace) return;

	// Audio, saves and timers are pumped from the GAME thread instead (see
	// TraceGraphicsManager::present), so this thread never enters the mixer.
	// If the game still hasn't drawn anything, say what we know instead of
	// leaving the caller staring at black.
	const char *lines[4];
	int n = 0;
	lines[n++] = g_filesNamed   ? "game files named by the door: yes" : "game files named by the door: NO";
	lines[n++] = g_threadUp     ? "game thread started:          yes" : "game thread started:          NO";
	lines[n++] = g_engineEntered? "engine entered:               yes" : "engine entered:               NO";
	char stage[48] = "last thing done: ";
	{
		const char *st = g_traceStage ? g_traceStage : "?";
		int p2 = 17;
		while (*st && p2 < 46) stage[p2++] = *st++;
		stage[p2] = 0;
	}
	lines[n++] = stage;
	// Keep the on-screen hint in step with the engine's own intro flag -- but
	// not over the three logo screens that come first.
	//
	// doIntro() opens with _mainIntroSeq, which is only the Virgin, Revolution
	// and Gibbons screens, and on the CD version those are SILENT: the engine
	// starts music for the floppy intro only. The real intro is the first
	// thing with a voice on it, so the first speech is the cue. Latched,
	// because the hint should not blink out between lines of dialogue.
	if (g_started && introPlaying()) {
		if (!g_introStarted) {
			if (g_system->getMixer()->hasActiveChannelOfType(Audio::Mixer::kSpeechSoundType))
				g_introStarted = trace_time_ms();
			// A backstop, so a silent or mis-detected intro still tells the
			// player how to get out of it. The logos run about 5 seconds now.
			else if (g_introFirstSeen && trace_time_ms() - g_introFirstSeen > 20000)
				g_introStarted = trace_time_ms();
			if (!g_introFirstSeen) g_introFirstSeen = trace_time_ms();
		}
	} else {
		g_introStarted = 0;
	}
	// Then hold it back a few seconds more. The first voice arrives with the
	// opening titles, and a line of green text over those reads as part of
	// the film -- let the intro establish itself first.
	g_introHint = g_started && introPlaying() && g_introStarted &&
	              trace_time_ms() - g_introStarted > kIntroHintDelayMs;

	const int drawn = (g_started && g_trace->graphics())
	                  ? g_trace->graphics()->framesPresented() : 0;
	g_status.tick(drawn, lines, n);
}

} // extern "C"
