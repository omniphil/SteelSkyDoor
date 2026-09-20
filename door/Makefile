# Builds the Beneath a Steel Sky door. Small, because the game runs on the
# caller's machine -- this side only works out what they can run, sends it once,
# and gets out of the way.
MAKEFLAGS += -r
.SUFFIXES:

CC     ?= gcc
CFLAGS ?= -O2 -std=gnu99 -Wall

SRC = main.c door.c trace_sky.c trace_door.c sha256.c files.c
OBJ = $(SRC:.c=.o)
TARGET = steelskydoor

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $^ -o $@
	@ls -l $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

# Refreshes the build outputs, and fills in the game data if it is missing.
#
# BOTH modules are copied, every time. They are build outputs, so "only if
# missing" is wrong for them: it silently keeps a stale module next to a new
# door, which looks exactly like the fix not working. The game data is left
# alone -- it is 72 MB and never changes.
install: $(TARGET)
	@for w in steelsky.wasm steelsky-basic.wasm; do \
		cp ../module/$$w . 2>/dev/null || \
		echo "  !! $$w missing -- run make -C ../module"; done
	@for f in sky.dsk sky.cpt sky.dnr; do \
		test -f $$f || cp ../data/$$f . 2>/dev/null || \
		echo "  !! $$f missing -- run ../tools/get_game.sh"; done
	@mkdir -p saves
	@echo "Ready. Run: ./$(TARGET) <dir containing door32.sys>"

clean:
	rm -f $(OBJ) $(TARGET)

.PHONY: all install clean
