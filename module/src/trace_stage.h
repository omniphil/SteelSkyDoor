// One word saying what the game thread is doing.
//
// "engine entered: yes" plus "0 frames" only narrows it to "somewhere inside
// ScummVM". This is updated as the thread passes each milestone, and by the
// OSystem calls the engine makes, so the diagnostic screen can name the last
// thing that happened before it stopped.
#ifndef TRACE_STAGE_H
#define TRACE_STAGE_H

// Plain assignment of a literal pointer: torn reads are impossible and a stale
// value is still useful. No locking, because this must work when locking is
// exactly what is broken.
extern const char *g_traceStage;
inline void trace_stage(const char *s) { g_traceStage = s; }


// The last few lines ScummVM logged. warning() and error() go through
// OSystem::logMessage, which normally disappears into TERMinator's debug
// output -- where a caller cannot see it. Keeping them here lets the
// diagnostic screen show what the engine itself thinks went wrong.
#define TRACE_LOG_LINES 4
#define TRACE_LOG_WIDTH 40
extern char g_traceLog[TRACE_LOG_LINES][TRACE_LOG_WIDTH];
extern int  g_traceLogCount;
void trace_note(const char *text);

// Set while the intro is playing, so the renderer can tell the player how to
// skip it. Nobody reads the door's text once the picture has taken over.
extern bool g_introHint;

// Set to 1 to put the live sound levels on screen and bind F7-F10 to change
// them, which is how the levels below were found. Kept because the balance is
// the kind of thing that gets revisited, and reaching it by rebuilding one
// guess at a time is painful.
// Override from the build with EXTRA_DEFS=-DTRACE_SOUND_DEBUG=1.
#ifndef TRACE_SOUND_DEBUG
#define TRACE_SOUND_DEBUG 0
#endif
#if TRACE_SOUND_DEBUG
extern char g_soundDebug[64];
#endif

// Live sound levels, 0-256, owned by the module rather than by ConfMan.
// The engine's syncSoundSettings() resets the mixer's levels whenever the
// player touches the control panel, so these are re-applied every pump.
extern int g_volSynth, g_volMusic, g_volSfx, g_volSpeech;

// How many times the engine has called in. If these keep rising while nothing
// is drawn, the engine is alive but stuck in a loop; if they freeze, it is
// blocked. Those are different bugs and the diagnostic could not tell them
// apart without this.
extern volatile unsigned g_pollCount, g_delayCount;

// trace_present() must not be called from two threads at once: the game thread
// draws frames while the watchdog may be drawing the diagnostic.
void trace_present_locked(const unsigned int *pixels, int w, int h, int flags);

#endif
