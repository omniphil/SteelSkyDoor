// A screen the module can draw by itself, when the game hasn't drawn one.
//
// A black picture is the least useful thing a door can show: it could be a
// failed upload, a dead game thread, an engine still loading, or a palette that
// never arrived. This watches for the game not presenting anything and puts up
// what the module actually knows instead, so a caller can read it out.
//
// It draws straight to TRACE and never touches the engine, so it works even
// when nothing else does.
#ifndef TRACE_STATUS_H
#define TRACE_STATUS_H

#include <stdint.h>
#include <string.h>

#include "font8x8.h"
#include "trace_stage.h"

extern "C" {
#include "trace_api.h"
}

class TraceStatus {
public:
	// Called from trace_update(). Returns true if it drew something, which
	// means the game still hasn't.
	bool tick(int engineFrames, const char *const *lines, int lineCount) {
		const int now = trace_time_ms();
		// The count is CUMULATIVE, so "> 0" would disarm this for good the
		// moment the engine drew a single frame and then wedged. Watch it
		// CHANGING instead.
		if (engineFrames != _lastCount) {
			_lastCount = engineFrames;
			_lastEngineFrame = now;
			_showing = false;
			return false;
		}
		// Give the engine a fair run at it before butting in: the intro opens
		// with three seconds of deliberate black.
		if (now - _lastEngineFrame < 6000) return false;
		if (now - _lastDraw < 500) return true;      // 2 fps is plenty
		_lastDraw = now;
		_showing = true;
		draw(lines, lineCount, now);
		return true;
	}

	// True while this screen is up, so the module can let the caller out with a
	// keypress instead of stranding them in a game that will never start.
	bool showing() const { return _showing; }

private:
	void draw(const char *const *lines, int lineCount, int now) {
		memset(_frame, 0, sizeof _frame);
		fill(0, 0, W, 10, 0xFF202060);
		text(4, 1, "STEEL SKY - nothing has been drawn yet", 0xFFFFFFFF);

		int y = 18;
		for (int i = 0; i < lineCount; i++, y += 9)
			text(6, y, lines[i], 0xFFC0C0C0);

		// What the engine itself said, which is usually more telling than
		// anything the module can work out about itself.
		if (g_traceLogCount > 0) {
			y += 6;
			text(6, y, "what the engine said:", 0xFF80C0FF);
			y += 10;
			for (int i = 0; i < g_traceLogCount; i++, y += 9)
				text(6, y, g_traceLog[i], 0xFFFFA0A0);
		}

		// Say how many frames the engine managed before it stopped: "0" and
		// "37" mean very different things.
		// Spell zero out: "0" and "8" are near-identical in this font and the
		// difference between them is the whole diagnosis.
		char fr[48] = "frames drawn by the game: ";
		if (_lastCount <= 0) {
			memcpy(fr + 26, "NONE", 5);
		} else {
			int v = _lastCount, d = 0, p2 = 26;
			char num[12];
			while (v) { num[d++] = (char)('0' + v % 10); v /= 10; }
			while (d) fr[p2++] = num[--d];
			fr[p2] = 0;
		}
		text(6, H - 26, fr, 0xFFFFD060);

		// Rising numbers mean the engine is alive but not drawing; frozen ones
		// mean it is blocked. Watch them change between redraws.
		{
			char hb[48] = "engine calls: poll ";
			int p2 = 19;
			unsigned vals[2] = { g_pollCount, g_delayCount };
			for (int k = 0; k < 2; k++) {
				unsigned v = vals[k];
				char num[12]; int d = 0;
				if (!v) num[d++] = '0';
				while (v) { num[d++] = (char)('0' + v % 10); v /= 10; }
				while (d) hb[p2++] = num[--d];
				if (k == 0) { const char *s2 = "  delay "; while (*s2) hb[p2++] = *s2++; }
			}
			hb[p2] = 0;
			text(6, H - 46, hb, 0xFF80FF80);
		}
		text(6, H - 38, "press any key to return to the BBS", 0xFF60FF60);

		char secs[40] = "waiting ";
		int n = (now - _lastEngineFrame) / 1000;
		char num[12];
		int d = 0;
		if (!n) num[d++] = '0';
		while (n) { num[d++] = (char)('0' + n % 10); n /= 10; }
		int p = 8;
		while (d) secs[p++] = num[--d];
		secs[p++] = 's'; secs[p] = 0;
		text(6, H - 14, secs, 0xFF909090);

		trace_present_locked(_frame, W, H, TRACE_PRESENT_ASPECT_4_3);
	}

	void fill(int x, int y, int w, int h, uint32_t c) {
		for (int j = 0; j < h; j++)
			for (int i = 0; i < w; i++)
				if (x + i < W && y + j < H) _frame[(y + j) * W + x + i] = c;
	}

	void text(int x, int y, const char *s, uint32_t c) {
		for (; *s; s++, x += FONT_W) {
			if (*s < FONT_FIRST || *s > FONT_LAST) continue;
			const uint8_t *g = font8x8[(int)*s - FONT_FIRST];
			for (int j = 0; j < FONT_H; j++)
				for (int i = 0; i < FONT_W; i++)
					if ((g[j] & (0x80 >> i)) && x + i < W && y + j < H)
						_frame[(y + j) * W + x + i] = c;
		}
	}

	static const int W = 320, H = 200;
	uint32_t _frame[W * H];
	int _lastEngineFrame = 0;
	int _lastCount = -1;
	bool _showing = false;
	int _lastDraw = 0;
};

#endif
