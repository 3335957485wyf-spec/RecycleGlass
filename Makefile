# RecycleGlass-C — build with MinGW-w64 (gcc)
#
#   make            -> build/RecycleGlass.exe
#   make preview    -> build/render_dump.exe (offline card renderer)
#   make clean

CC      = gcc
WINDRES = windres

SRC     = src/main.c src/render.c src/glass.c src/shell.c src/config.c \
          src/i18n.c src/droptarget.c src/menu.c src/icon.c
OBJ     = $(SRC:src/%.c=build/%.o)
RES     = build/app.res

CFLAGS  = -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter \
          -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 \
          -finput-charset=UTF-8 -municode
LDFLAGS = -municode -mwindows -static-libgcc
LIBS    = -lgdiplus -lshell32 -lole32 -loleaut32 -luuid -lshlwapi \
          -lgdi32 -luser32 -ladvapi32 -lcomdlg32

all: build/RecycleGlass.exe

build/RecycleGlass.exe: $(OBJ) $(RES)
	$(CC) $(LDFLAGS) -o $@ $(OBJ) $(RES) $(LIBS)

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build/app.res: res/app.rc res/app-icon.ico res/app-icon-1920.png | build
	$(WINDRES) -I src -I res res/app.rc -O coff -o $@

# Offline preview: renders one card frame to a PNG, no desktop needed.
preview: build/render_dump.exe

build/render_dump.exe: tools/render_dump.c src/render.c src/i18n.c | build
	$(CC) $(CFLAGS) -o $@ tools/render_dump.c src/render.c src/i18n.c \
	      -lgdiplus -lgdi32 -luser32 -lole32

# Icon sheet: every Lucide icon in the set, for checking the path parser.
icons: build/icon_dump.exe

build/icon_dump.exe: tools/icon_dump.c src/icon.c | build
	$(CC) $(CFLAGS) -o $@ tools/icon_dump.c src/icon.c -lgdiplus -lgdi32 -luser32

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean preview icons
