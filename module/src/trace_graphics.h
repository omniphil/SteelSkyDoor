// ScummVM GraphicsManager on top of TRACE.
//
// Beneath a Steel Sky is a 320x200 8-bit game, so the engine hands us paletted
// pixels and a 256-entry palette. We keep both, composite the mouse cursor on
// top, expand to BGRA and hand the frame to trace_present(). TERMinator scales
// it to the rect the door opened.
//
// NullGraphicsManager already implements the whole GraphicsManager interface as
// no-ops, so this only overrides what actually has to do something.
#ifndef TRACE_GRAPHICS_H
#define TRACE_GRAPHICS_H

#include "backends/graphics/null/null-graphics.h"
#include "graphics/surface.h"
#include "common/rect.h"
#include "trace_stage.h"

extern "C" {
#include "trace_api.h"
}

class OSystem_TRACE;

class TraceGraphicsManager : public NullGraphicsManager {
public:
	// Who to call after a frame goes out. OSystem is a virtual base, so the
	// backend cannot be recovered from g_system by a cast -- it hands itself
	// over instead.
	void setBackend(OSystem_TRACE *b) { _backend = b; }
	TraceGraphicsManager() {
		_screen.create(320, 200, Graphics::PixelFormat::createFormatCLUT8());
		memset(_palette, 0, sizeof(_palette));
		memset(_cursorPalette, 0, sizeof(_cursorPalette));
	}

	~TraceGraphicsManager() override {
		_screen.free();
		_cursor.free();
		free(_frame);
	}

	void initSize(uint width, uint height, const Graphics::PixelFormat *format = nullptr) override {
		trace_stage("initSize");
		// NullGraphicsManager keeps its OWN _width/_height and answers
		// getOverlayWidth()/getOverlayHeight() from them. They are uninitialised
		// until its initSize runs, and in wasm that means ZERO -- so the GUI
		// theme tries to build a 0x0 overlay, fails, the builtin fails the same
		// way, and ScummVM aborts with "Failed to load any GUI theme".
		NullGraphicsManager::initSize(width, height, format);
		if ((int)width != _screen.w || (int)height != _screen.h) {
			_screen.free();
			_screen.create(width, height, Graphics::PixelFormat::createFormatCLUT8());
			free(_frame);
			_frame = nullptr;
		}
	}

	int16 getWidth() const override  { return (int16)_screen.w; }
	int16 getHeight() const override { return (int16)_screen.h; }

	// The overlay is the same size as the screen here: there is no window to be
	// bigger than. Answered directly rather than leaning on the base class.
	int16 getOverlayWidth() const override  { return (int16)_screen.w; }
	int16 getOverlayHeight() const override { return (int16)_screen.h; }

	Graphics::PixelFormat getScreenFormat() const override {
		return Graphics::PixelFormat::createFormatCLUT8();
	}

	void setPalette(const byte *colors, uint start, uint num) override {
		trace_stage("setPalette");
		for (uint i = 0; i < num && start + i < 256; i++) {
			_palette[start + i][0] = colors[i * 3 + 0];
			_palette[start + i][1] = colors[i * 3 + 1];
			_palette[start + i][2] = colors[i * 3 + 2];
		}
	}

	void grabPalette(byte *colors, uint start, uint num) const override {
		for (uint i = 0; i < num && start + i < 256; i++) {
			colors[i * 3 + 0] = _palette[start + i][0];
			colors[i * 3 + 1] = _palette[start + i][1];
			colors[i * 3 + 2] = _palette[start + i][2];
		}
	}

	void setCursorPalette(const byte *colors, uint start, uint num) override {
		for (uint i = 0; i < num && start + i < 256; i++) {
			_cursorPalette[start + i][0] = colors[i * 3 + 0];
			_cursorPalette[start + i][1] = colors[i * 3 + 1];
			_cursorPalette[start + i][2] = colors[i * 3 + 2];
		}
		_cursorPaletteSet = true;
	}

	void copyRectToScreen(const void *buf, int pitch, int x, int y, int w, int h) override {
		trace_stage("copyRectToScreen");
		if (!buf) return;
		const byte *src = (const byte *)buf;
		for (int row = 0; row < h; row++) {
			int dy = y + row;
			if (dy < 0 || dy >= _screen.h) continue;
			int copyW = w;
			if (x + copyW > _screen.w) copyW = _screen.w - x;
			if (copyW <= 0) continue;
			memcpy((byte *)_screen.getBasePtr(x, dy), src + (size_t)row * pitch, copyW);
		}
	}

	Graphics::Surface *lockScreen() override { trace_stage("lockScreen"); return &_screen; }
	void unlockScreen() override {}

	void fillScreen(uint32 col) override { _screen.fillRect(Common::Rect(0, 0, _screen.w, _screen.h), col); }
	void fillScreen(const Common::Rect &r, uint32 col) override { _screen.fillRect(r, col); }

	// The engine draws its own shake for earthquakes; TRACE has no window to
	// move, so we offset the frame when we present it.
	void setShakePos(int shakeXOffset, int shakeYOffset) override {
		_shakeX = shakeXOffset;
		_shakeY = shakeYOffset;
	}

	bool showMouse(bool visible) override {
		trace_stage("showMouse");
		bool was = _cursorVisible;
		_cursorVisible = visible;
		return was;
	}

	void warpMouse(int x, int y) override { _mouseX = x; _mouseY = y; }

	void setMouseCursor(const void *buf, uint w, uint h, int hotspotX, int hotspotY,
	                    uint32 keycolor, const Graphics::PixelFormat *format,
	                    const byte *mask, frac_t scaleX, frac_t scaleY) override {
		trace_stage("setMouseCursor");
		_cursor.free();
		if (!buf || !w || !h) return;
		_cursor.create(w, h, Graphics::PixelFormat::createFormatCLUT8());
		memcpy(_cursor.getPixels(), buf, (size_t)w * h);
		_cursorHotX = hotspotX;
		_cursorHotY = hotspotY;
		_cursorKey = keycolor;
	}

	// The only method that actually reaches the player.
	void updateScreen() override { trace_stage("updateScreen"); present(); trace_stage("presented"); }

	// The module's own loop calls this after pumping events, so the picture
	// keeps up even when the engine hasn't asked for an update.
	void present();

	// How many frames the ENGINE has drawn. The module's watchdog uses this to
	// tell "still loading" from "never going to draw".
	int framesPresented() const { return _presented; }

	// Where the module puts the mouse, so the cursor is drawn in the right place.
	void setMousePos(int x, int y) { _mouseX = x; _mouseY = y; }
	int mouseX() const { return _mouseX; }
	int mouseY() const { return _mouseY; }

private:
	Graphics::Surface _screen;
	Graphics::Surface _cursor;
	uint32 *_frame = nullptr;          // BGRA, what trace_present() is given
	byte _palette[256][3];
	byte _cursorPalette[256][3];
	bool _cursorPaletteSet = false;
	bool _cursorVisible = false;
	int _cursorHotX = 0, _cursorHotY = 0;
	uint32 _cursorKey = 0xFFFFFFFF;
	OSystem_TRACE *_backend = nullptr;
	int _presented = 0;
	int _mouseX = 160, _mouseY = 100;
	int _shakeX = 0, _shakeY = 0;
};

#endif
