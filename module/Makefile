# Builds steelsky.wasm: ScummVM's sky engine with an OSystem backend on TRACE.
#
# Run ../tools/get_scummvm.sh first -- ScummVM is fetched, not vendored.
MAKEFLAGS += -r
.SUFFIXES:

WASI_SDK ?= $(HOME)/tools/wasi-sdk-34.0-x86_64-linux
CXX       = $(WASI_SDK)/bin/clang++

SCUMMVM  = $(firstword $(wildcard ../third_party/scummvm native/scummvm))
TRACE_INCLUDE = $(firstword $(wildcard ../../../TERMinator-Windows/trace/include) src)

# These link flags are NOT optional: gamesandbox requires the module to IMPORT
# one shared memory ("env" "memory"). Without -pthread and --import-memory
# --shared-memory the module exports its own and the host refuses to start it.
# 512 MB because the CD game data is streamed, but the engine still wants room.
MAX_MEMORY = 536870912

# ScummVM needs RTTI (common/stream.cpp, common/file.cpp, audio/mixer.cpp and
# base/plugins.h all use dynamic_cast). Never add -fno-rtti here.
CXXFLAGS = --target=wasm32-wasip1-threads -O2 -fno-exceptions -pthread -DNDEBUG \
           -DHAVE_CONFIG_H -I$(SCUMMVM) -I$(SCUMMVM)/engines -Isrc -I$(TRACE_INCLUDE) \
           -Iconfig -w $(EXTRA_DEFS)
LDFLAGS  = -mexec-model=reactor \
           -Wl,--import-memory,--export-memory,--shared-memory,--max-memory=$(MAX_MEMORY) \
           -Wl,--export=wasi_thread_start -Wl,--export-dynamic -Wl,--strip-all \
           -Wl,-z,stack-size=4194304

# Our backend.
OWN_SRC = src/skymodule.cpp src/trace_osystem.cpp src/trace_graphics.cpp \
          src/trace_assets.cpp src/trace_saves.cpp
# 🚨 Headers must be prerequisites too. Most of this backend lives in headers
# (the mixer, the mutex, the status screen), so without these a change to one
# leaves the target "up to date" and make silently ships the PREVIOUS module --
# which looks exactly like a fix having no effect.
OWN_HDR = $(wildcard src/*.h)

# The parts of ScummVM the sky engine actually reaches. Everything that failed
# to build was an optional third-party library (zlib, alsa, gif, mpeg2, a52dec,
# soxr), another platform's code (Atari, MFC, x86 SIMD) or an optional feature
# (auto-update, event recorder, DLC) -- none of which sky needs.
SCUMM_DIRS = engines/sky common graphics audio image gui engines base \
             backends/keymapper backends/saves/default backends/timer/default \
             backends/events/default backends/audiocd/default
SCUMM_FILES = backends/modular-backend.cpp backends/base-backend.cpp \
              backends/saves/savefile.cpp backends/audiocd/audiocd-stream.cpp \
              backends/fs/abstract-fs.cpp

OBJDIR = obj

# TWO modules are shipped, and both are needed.
#
# 🚨 WASM imports are static: a module that names trace_mouse_mode cannot load
# at all on a TERMinator that does not export it, however carefully the code
# avoids calling it. So the mouse build is kept apart from a basic one that
# never mentions the Tier 1 calls, and the door picks between them. Building
# only the first is how an older client ends up downloading 3 MB and then
# failing to start.
OUT = steelsky.wasm

all: $(OUT) deploy

# Separate object directory AND output name: the two builds differ by a -D, so
# sharing objects would link the wrong ones into whichever built second, and
# building both as steelsky.wasm and renaming would find that file already up
# to date and ship the mouse build under both names.
basic:
	@$(MAKE) --no-print-directory OUT=steelsky-basic.wasm \
	    EXTRA_DEFS=-DSKY_NO_MOUSE OBJDIR=$(OBJDIR)-basic steelsky-basic.wasm

# One rule, because ScummVM's own build system assumes configure ran.
$(OUT): $(OWN_SRC) $(OWN_HDR) config/config.h $(SCUMMVM)/engines/plugins_table.h
	@mkdir -p $(OBJDIR)
	@echo "compiling ScummVM (this takes a few minutes the first time)..."
	@rm -f $(OBJDIR)/failed-sources.txt
	@for f in $$(cd $(SCUMMVM) && find $(SCUMM_DIRS) -maxdepth 3 -name '*.cpp' 2>/dev/null) $(SCUMM_FILES); do \
	    o=$(OBJDIR)/$$(echo $$f | tr '/' '_' | sed 's/.cpp$$/.o/'); \
	    if [ ! -f $$o ] || [ $(SCUMMVM)/$$f -nt $$o ]; then \
	        $(CXX) $(CXXFLAGS) -c $(SCUMMVM)/$$f -o $$o 2>/dev/null \
	            || echo $$f >> $(OBJDIR)/failed-sources.txt; \
	    fi; done
# A file that didn't compile leaves no .o and says nothing, so the build sails on and dies much later at the
# LINK, naming whichever object happened to reference the missing symbol. That is how a missing
# third_party/scummvm/backends/ folder once turned into 500 'undefined symbol: g_system' errors pointing at
# audio_chip.o -- the real casualty was common/system.cpp, which defines g_system and had quietly failed on a
# missing header. So: keep shrugging failures off (most are meant to fail, see above), but write down WHICH.
	@if [ -s $(OBJDIR)/failed-sources.txt ]; then \
	    echo "note: $$(wc -l < $(OBJDIR)/failed-sources.txt) ScummVM sources didn't compile -- expected for"; \
	    echo "      optional libraries and other platforms. If the link below fails, read this list FIRST:"; \
	    echo "        $(OBJDIR)/failed-sources.txt"; \
	 fi
	@for f in $(OWN_SRC); do \
	    $(CXX) $(CXXFLAGS) -c $$f -o $(OBJDIR)/own_$$(basename $${f%.cpp}).o || exit 1; done
	$(CXX) $(CXXFLAGS) $(OBJDIR)/*.o $(LDFLAGS) -o $@
	@ls -l $@

# configure normally writes these; a sky-only build needs very little.
config/config.h:
	@mkdir -p config
	@printf '#ifndef CONFIG_H\n#define CONFIG_H\n#define SCUMM_LITTLE_ENDIAN\n#define USE_RGB_COLOR\n#define PLUGIN_PREFIX ""\n#define PLUGIN_SUFFIX ""\n#endif\n' > $@

$(SCUMMVM)/engines/plugins_table.h:
	@echo 'LINK_PLUGIN(SKY)' > $(SCUMMVM)/engines/plugins_table.h
	@echo 'LINK_PLUGIN(SKY_DETECTION)' > $(SCUMMVM)/engines/detection_table.h

# The door reads steelsky.wasm from beside its own binary, and door/ is the folder
# that actually goes to the BBS -- module/ is only where make writes. Nothing
# copied between them, which is how door/ once went a whole day holding the
# morning's module while module/ looked correctly built. So the build does it.
#
# Deliberately depends on the two real names rather than $(OUT): a tuning build
# (make OUT=steelsky-tune.wasm ...) names its own target, so it never lands here
# and cannot quietly replace the module the BBS is serving.
DOORDIR = ../door
deploy: steelsky.wasm
	@cp -f steelsky.wasm $(DOORDIR)/
	@echo "deployed -> $(DOORDIR)/ (steelsky.wasm)"

clean:
	rm -rf $(OBJDIR) $(OBJDIR)-basic steelsky.wasm steelsky-basic.wasm

.PHONY: all basic clean deploy
