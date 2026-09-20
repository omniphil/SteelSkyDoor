// ScummVM MixerManager on top of trace_audio_write().
//
// ScummVM's mixer is pull-based: it renders into a buffer when asked. TRACE's
// audio is push-based, so the module pumps this from its own loop, taking as
// much as trace_audio_room() will accept and no more.
//
// TRACE audio is S16 stereo at 44100 Hz, which is what ScummVM's mixer wants
// anyway, so no resampling is needed.
#ifndef TRACE_MIXER_H
#define TRACE_MIXER_H

#include <stdio.h>
#include <string.h>              // before ScummVM's headers: forbidden.h poisons memset's neighbours

#include "backends/mixer/mixer.h"
#include "audio/mixer_intern.h"
#include "common/config-manager.h"
#include "trace_stage.h"

extern "C" {
#include "trace_api.h"
}

class TraceMixerManager : public MixerManager {
public:
	void init() override {
		_mixer = new Audio::MixerImpl(44100);
		_mixer->setReady(true);
	}

	void suspendAudio() override { _suspended = true; }
	int resumeAudio() override { _suspended = false; return 0; }

	// Called from the module's loop. Keeps roughly a tenth of a second queued,
	// which is what trace_api.h asks for.
	void pump() {
		if (!_mixer || _suspended) return;
		enforceLevels();
#if TRACE_SOUND_DEBUG
		noteActivity();
		reportLevels();
#endif
		// Top the queue up to a target rather than handing over one chunk per
		// idle: a single chunk per call underruns whenever the engine is busy,
		// which is exactly when it sounds worst. Filling it to the brim is no
		// better -- that is just latency -- so aim for the 50-100 ms
		// trace_api.h asks for and stop.
		int32 room = trace_audio_room();
		// trace_audio_room() reports FREE space, not what is queued, so learn
		// the queue's size from the largest room ever seen -- which is the
		// empty queue, on the first pump before anything plays.
		if (room > _capacity) _capacity = room;

		for (int i = 0; i < kMaxChunksPerPump; i++) {
			if (_capacity - room >= kTargetFrames) break;   // ~100 ms is queued
			if (room < kChunkFrames) break;                 // no space for a chunk
			trace_stage("mixCallback");
			const int mixed = _mixer->mixCallback((byte *)_buf, kChunkFrames * 4);
			trace_stage("mixed");

			// 🚨 mixCallback() renders the WHOLE buffer and returns how many
			// frames were NON-SILENT -- it is not a byte count and not the
			// amount to send. Three things follow, and getting any of them
			// wrong sounds like static:
			//
			//  - Always hand over the full chunk. A short return just means a
			//    channel ran dry part way; the rest of the buffer is real
			//    silence and the stream still needs it.
			//  - A return of 0 does NOT mean "skip this chunk". Dropping it
			//    leaves a hole in a stream that plays at a fixed rate.
			//  - On 0 with clamping off, mixer.cpp deliberately leaves the
			//    buffer UNTOUCHED ("nothing to clamp"), so it still holds the
			//    previous chunk. Silence has to be written by hand.
			if (mixed <= 0)
				memset(_buf, 0, sizeof _buf);
#if TRACE_SOUND_DEBUG
			// The peak of what the mixer ACTUALLY produced. If the sound-type
			// volumes were being applied this would fall with them; if it
			// stays pinned near full scale they are being ignored somewhere
			// between setVolumeForSoundType() and the samples.
			for (int k = 0; k < kChunkFrames * 2; k++) {
				int v = _buf[k] < 0 ? -_buf[k] : _buf[k];
				if (v > _peak) _peak = v;
			}
#endif

			// The queue may take less than offered. Anything it refuses is
			// gone -- the mixer has already advanced past those frames and
			// cannot re-render them -- so stop rather than tear a second hole.
			const int32 took = trace_audio_write(_buf, kChunkFrames);
			if (took < kChunkFrames) break;
			room = trace_audio_room();
		}
	}

private:
	// 🚨 music_volume does NOT reach the music this game actually plays.
	//
	// With no MIDI device, sky picks AdLib, and the OPL emulator hands itself
	// to the mixer as kPlainSoundType at full channel volume (audio/chip.cpp).
	// Engine::defaultSyncSoundSettings() then pins kPlainSoundType to
	// kMaxMixerVolume unconditionally -- music_volume is only consulted for
	// kMusicSoundType, which sky uses solely for its digital tracks. So
	// lowering music_volume reaches AdLib's own channel volume and nothing
	// else, and the synth still arrives at the mixer at 100% while the speech
	// sits at speech_volume. That is the music drowning out the cast.
	//
	// It has to be re-applied rather than set once: every syncSoundSettings()
	// puts it back to maximum, and the control panel calls that whenever the
	// player touches the settings.
	void enforceLevels() {
		set(Audio::Mixer::kPlainSoundType,  g_volSynth);
		set(Audio::Mixer::kMusicSoundType,  g_volMusic);
		set(Audio::Mixer::kSFXSoundType,    g_volSfx);
		set(Audio::Mixer::kSpeechSoundType, g_volSpeech);
	}

	void set(Audio::Mixer::SoundType t, int v) {
		if (_mixer->getVolumeForSoundType(t) != v)
			_mixer->setVolumeForSoundType(t, v);
	}

#if TRACE_SOUND_DEBUG
	// Puts the levels actually in force on the screen. Two separate fixes made
	// no audible difference, which means one of these numbers is not what the
	// code says it should be -- so read them rather than reason about them.
	void reportLevels() {
		const int now = trace_time_ms();
		if (now - _lastReport < 1000) return;
		_lastReport = now;
		const int cfg = ConfMan.hasKey("music_volume") ? ConfMan.getInt("music_volume") : -1;
		// Which types are actually SOUNDING is the missing fact: it says
		// whether the music is the AdLib synth (plain) or a digital track
		// (music), and those take different volume knobs.
		// Latched over a few seconds: sampling on the instant mostly catches
		// gaps, and the OPL channel is started permanent=true so it reads as
		// active for ever whether or not a note is sounding.
		static const char kNames[4] = { 'P', 'M', 'F', 'S' };
		char act[5];
		for (int i = 0; i < 4; i++)
			act[i] = (now - _lastActive[i] < 3000) ? kNames[i] : '-';
		act[4] = 0;
		(void)cfg;
		snprintf(g_soundDebug, sizeof g_soundDebug,
		         "F7/8 sfx%d  F9/10 mus%d  sp%d %s pk%d",
		         g_volSfx, g_volMusic, g_volSpeech, act, _peak);
		_peak = 0;
	}
	// Sampled often, reported rarely: hasActiveChannelOfType() takes the
	// mixer lock, so this is throttled rather than run every pump.
	void noteActivity() {
		const int now = trace_time_ms();
		if (now - _lastSample < 100) return;
		_lastSample = now;
		static const Audio::Mixer::SoundType kTypes[4] = {
			Audio::Mixer::kPlainSoundType, Audio::Mixer::kMusicSoundType,
			Audio::Mixer::kSFXSoundType,   Audio::Mixer::kSpeechSoundType };
		for (int i = 0; i < 4; i++)
			if (_mixer->hasActiveChannelOfType(kTypes[i])) _lastActive[i] = now;
	}

	int _lastReport = 0;
	int _lastSample = 0;
	int _lastActive[4] = { 0, 0, 0, 0 };
	int _peak = 0;
#endif

	static const int kChunkFrames = 1024;
	static const int kTargetFrames = 4410;    // ~100 ms at 44100 Hz
	static const int kMaxChunksPerPump = 6;   // a cap, so one pump cannot stall the game
	int16 _buf[kChunkFrames * 2];
	int32 _capacity = 0;
	bool _suspended = false;
};

#endif
