#include "trace_graphics.h"
#include "trace_osystem.h"
#include "trace_stage.h"
#include "font8x8.h"

#include <stdlib.h>
#include <string.h>

// CLUT8 -> BGRA, cursor composited on top, then out to TERMinator.
//
// 320x200 is 64,000 pixels, so doing this every frame costs nothing; there is
// no point tracking dirty rectangles until something says otherwise.
void TraceGraphicsManager::present() {
	const int w = _screen.w, h = _screen.h;
	if (w <= 0 || h <= 0) return;

	if (!_frame) {
		_frame = (uint32 *)malloc((size_t)w * h * 4);
		if (!_frame) return;
	}

	// The shake offset moves the picture inside the frame rather than moving a
	// window, so the edges have to be filled rather than left as stale pixels.
	const uint32 black = 0xFF000000u;

	for (int y = 0; y < h; y++) {
		uint32 *dst = _frame + (size_t)y * w;
		int sy = y - _shakeY;
		if (sy < 0 || sy >= h) {
			for (int x = 0; x < w; x++) dst[x] = black;
			continue;
		}
		const byte *src = (const byte *)_screen.getBasePtr(0, sy);
		for (int x = 0; x < w; x++) {
			int sx = x - _shakeX;
			if (sx < 0 || sx >= w) { dst[x] = black; continue; }
			const byte *c = _palette[src[sx]];
			dst[x] = ((uint32)c[2]) | ((uint32)c[1] << 8) | ((uint32)c[0] << 16) | 0xFF000000u;
		}
	}

	// The cursor is a paletted sprite with a transparent key colour. ScummVM
	// keeps it separate from the screen, so we composite it here.
	if (_cursorVisible && _cursor.getPixels()) {
		const byte (*pal)[3] = _cursorPaletteSet ? _cursorPalette : _palette;
		const int cx = _mouseX - _cursorHotX;
		const int cy = _mouseY - _cursorHotY;
		for (int y = 0; y < _cursor.h; y++) {
			const int dy = cy + y;
			if (dy < 0 || dy >= h) continue;
			const byte *srow = (const byte *)_cursor.getBasePtr(0, y);
			uint32 *drow = _frame + (size_t)dy * w;
			for (int x = 0; x < _cursor.w; x++) {
				const int dx = cx + x;
				if (dx < 0 || dx >= w) continue;
				const byte idx = srow[x];
				if (idx == _cursorKey) continue;          // transparent
				const byte *c = pal[idx];
				drow[dx] = ((uint32)c[2]) | ((uint32)c[1] << 8) |
				           ((uint32)c[0] << 16) | 0xFF000000u;
			}
		}
	}

	// 320x200 is 8:5, but the game was drawn for a 4:3 display -- the same
	// stretched-pixel look every DOS game had. TRACE does that for us.
	// A line telling the player how to get past the intro. Drawn into the
	// outgoing frame only -- the game's own screen is untouched.
	if (g_introHint) {
		static const char *kHint = "ESC skips the intro";
		const int tw = 19 * FONT_W;
		int tx = (w - tw) / 2, ty = h - 14;
		for (int j = -2; j < FONT_H + 2; j++)              // a dark strip behind it
			for (int i = -4; i < tw + 4; i++) {
				const int px = tx + i, py = ty + j;
				if (px >= 0 && px < w && py >= 0 && py < h)
					_frame[py * w + px] = 0xFF000000u;
			}
		for (const char *c = kHint; *c; c++, tx += FONT_W) {
			if (*c < FONT_FIRST || *c > FONT_LAST) continue;
			const uint8_t *g = font8x8[(int)*c - FONT_FIRST];
			for (int j = 0; j < FONT_H; j++)
				for (int i = 0; i < FONT_W; i++)
					if ((g[j] & (0x80 >> i)) && tx + i < w && ty + j < h)
						_frame[(ty + j) * w + tx + i] = 0xFFB0FFB0u;
		}
	}

#if TRACE_SOUND_DEBUG
	// Temporary: the sound levels actually in force, top-left.
	if (g_soundDebug[0]) {
		int tx = 2, ty = 2;
		for (const char *c = g_soundDebug; *c; c++, tx += FONT_W) {
			if (*c < FONT_FIRST || *c > FONT_LAST) continue;
			const uint8_t *g = font8x8[(int)*c - FONT_FIRST];
			for (int j = -1; j <= FONT_H; j++)
				for (int i = -1; i <= FONT_W; i++)
					if (tx + i < w && ty + j < h && tx + i >= 0 && ty + j >= 0)
						_frame[(ty + j) * w + tx + i] = 0xFF000000u;
			for (int j = 0; j < FONT_H; j++)
				for (int i = 0; i < FONT_W; i++)
					if ((g[j] & (0x80 >> i)) && tx + i < w && ty + j < h)
						_frame[(ty + j) * w + tx + i] = 0xFFFFFF60u;
		}
	}
#endif

	_presented++;
	trace_present_locked(_frame, w, h, TRACE_PRESENT_ASPECT_4_3);

	// NOTHING else happens here. Pumping the mixer from inside present() was a
	// mistake: the engine can be holding the mixer's own mutex when it calls
	// updateScreen(), and re-entering the mixer there wedges it. A recursive
	// mutex allows the re-entry, which is the opposite of what is wanted --
	// mutual exclusion is not re-entrancy safety. The pump happens in
	// delayMillis() instead, where the engine idles holding nothing.
}
