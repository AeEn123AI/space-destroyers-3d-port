# Space Destroyers 3D - top-level build
#   make assets   regenerate build/assets from the .sb3 (needs rsvg-convert, ffmpeg, python3 + numpy + Pillow)
#   make pc       Linux/SDL2 build -> build/pc/space-destroyers-3d (+ assets/)
#   make 3ds      3DS .3dsx (needs devkitPro)            make cia   .cia (needs tools/bin/makerom + bannertool)
#   make dist     everything into dist/
SB3      := Space Destroyers 3D.sb3
CC       ?= cc
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS   := $(shell sdl2-config --libs)
COMMON   := src/common/game.c src/common/audio.c src/common/image.c src/common/font.c
PCSRC    := src/pc/main_pc.c $(COMMON)
CFLAGS_PC := -std=gnu99 -O2 -Wall -Isrc/common -Ithird_party $(SDL_CFLAGS)
PCOUT    := build/pc

.PHONY: all assets pc 3ds cia dist clean
all: pc

build/assets/.stamp: Space\ Destroyers\ 3D.sb3 tools/build_assets.py
	python3 tools/build_assets.py "$(SB3)" build/assets
	touch $@
assets: build/assets/.stamp
src/common/costumes_gen.h: build/assets/.stamp

$(PCOUT)/space-destroyers-3d: $(PCSRC) src/common/*.h build/assets/.stamp
	mkdir -p $(PCOUT)
	$(CC) $(CFLAGS_PC) -o $@ $(PCSRC) $(SDL_LIBS) -lGL -lm
	rm -rf $(PCOUT)/assets && cp -r build/assets/pc $(PCOUT)/assets
pc: $(PCOUT)/space-destroyers-3d

3ds: build/assets/.stamp
	$(MAKE) -f Makefile.3ds
cia: build/assets/.stamp
	$(MAKE) -f Makefile.3ds cia

dist: pc cia
	mkdir -p dist/screenshots
	cp space-destroyers-3d.3dsx dist/space-destroyers-3d.3dsx
	cp space-destroyers-3d.cia dist/space-destroyers-3d.cia
	rm -rf build/zip && mkdir -p build/zip/space-destroyers-3d
	cp -r $(PCOUT)/space-destroyers-3d $(PCOUT)/assets build/zip/space-destroyers-3d/
	cp README.md tools/fonts/*LICENSE* build/zip/space-destroyers-3d/
	rm -f dist/space-destroyers-3d-linux.zip
	cd build/zip && zip -qr ../../dist/space-destroyers-3d-linux.zip space-destroyers-3d

clean:
	rm -rf build/pc build/3ds build/zip space-destroyers-3d.3dsx space-destroyers-3d.cia space-destroyers-3d.elf space-destroyers-3d.smdh
