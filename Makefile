# floppy_image: one Makefile for Windows, Linux and macOS.
#
#   Windows  MSYS2 gcc, SDL2:                make
#   Linux    gcc, libsdl2-dev, pkg-config:   make
#   macOS    Xcode tools, SDL2:              make
#
# make test builds and runs the unit tests, which need neither SDL nor a drive.

ifeq ($(origin CC),default)
  CC := gcc
endif
UNAME := $(shell uname -s 2>/dev/null)
# another folder keeps two systems' builds apart in one checkout
BUILD   ?= build
VERSION ?= $(shell head -1 VERSION 2>/dev/null || echo 0.0)

# c99 rather than gnu99 keeps the code portable; the systems' POSIX and Win32 calls are switched on by name below.
# Header dependencies are written next to each object; a universal macOS build passes DEPFLAGS= (see the workflow).
DEPFLAGS ?= -MMD -MP
CFLAGS  := -std=c99 -O2 -g -Wall -Wextra -pedantic $(DEPFLAGS) -I$(BUILD) -DFI_VERSION='"$(VERSION)"'
# Nuklear's headers are not -pedantic clean, so they are system headers to the compiler
NKFLAGS := -isystem third_party/nuklear
CORE    := src/fimg.c src/fdrive.c src/fjob.c src/fplat.c
SRC     := src/main.c src/fgui.c $(CORE)

ON_WINDOWS := $(filter Windows_NT,$(OS))$(findstring MINGW,$(UNAME))$(findstring MSYS,$(UNAME))
ifneq ($(ON_WINDOWS),)
  X       := .exe
  # msvcrt's printf has no %zu or %lld; this makes MinGW use its own conforming one
  CFLAGS  += -D__USE_MINGW_ANSI_STDIO=1 -D_WIN32_WINNT=0x0601
  LDFLAGS := -mwindows
  SDL_CONFIG ?= sdl2-config
  # SDL2 linked statically: the program runs without SDL2.dll, so a user just unpacks and double-clicks
  SDL_LIBS ?= $(filter-out -lSDL2main -mwindows -lmingw32,$(shell $(SDL_CONFIG) --static-libs))
  LIBS    := $(SDL_LIBS) -lcomdlg32 -static -static-libgcc
  # MSYS2 make does not always pass TMP / TEMP on, and gcc then tries a system folder; keep its temp files here
  $(shell mkdir -p $(BUILD)/tmp)
  export TMP := $(CURDIR)/$(BUILD)/tmp
  export TEMP := $(TMP)
  # The compiler's own folder first on PATH: other toolchains can ship an incompatible libmpfr that makes cc1 die
  # silently.
  CC_BIN := $(shell command -v $(CC) 2>/dev/null)
  ifneq ($(CC_BIN),)
    CC_DIR := $(patsubst %/,%,$(dir $(CC_BIN)))
    export PATH := $(CC_DIR):$(PATH)
  endif
  NKFLAGS += $(shell $(SDL_CONFIG) --cflags | sed 's/-Dmain=SDL_main//') -I$(shell $(SDL_CONFIG) --prefix)/include
  EXTRA_OBJ := $(BUILD)/app_res.o
else ifeq ($(UNAME),Darwin)
  X       :=
  SDL_CONFIG ?= sdl2-config
  SDL_LIBS ?= $(shell $(SDL_CONFIG) --libs)
  NKFLAGS += $(shell $(SDL_CONFIG) --cflags) -I$(shell $(SDL_CONFIG) --prefix)/include
  LIBS    := $(SDL_LIBS) -lm
else
  X       :=
  # glibc hides POSIX calls (pread, realpath, ...) under -std=c99 without _DEFAULT_SOURCE
  CFLAGS  += -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
  NKFLAGS += $(shell pkg-config --cflags sdl2)
  LIBS    := $(shell pkg-config --libs sdl2) -lm
endif

EXE ?= floppy_image$(X)
OBJ := $(patsubst %.c,$(BUILD)/%.o,$(SRC)) $(BUILD)/src/nk.o $(BUILD)/gen_font.o $(EXTRA_OBJ)

all: $(EXE)

$(EXE): $(OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

$(BUILD)/src/main.o $(BUILD)/src/fgui.o: CFLAGS += $(NKFLAGS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# third-party code: warnings off, and the language its own build assumes
$(BUILD)/src/nk.o: src/nk.c
	@mkdir -p $(dir $@)
	$(CC) -O2 -w -I$(BUILD) $(NKFLAGS) -c $< -o $@

# The font built in as a C array ("ld -r -b binary" is GNU ld only). HOSTCC: bin2c runs here even when CC cross-builds.
HOSTCC ?= cc
ifneq ($(ON_WINDOWS),)
  HOSTCC := $(CC)
endif
ifeq ($(UNAME),Darwin)
  HOSTCC := clang
endif
$(BUILD)/bin2c$(X): tools/bin2c.c
	@mkdir -p $(BUILD)
	$(HOSTCC) -O1 -o $@ $<

$(BUILD)/gen_font.c: third_party/nuklear/extra_font/DroidSans.ttf $(BUILD)/bin2c$(X)
	$(BUILD)/bin2c$(X) $< $@ font_ttf

$(BUILD)/gen_font.o: $(BUILD)/gen_font.c
	$(CC) -O1 -w -c $< -o $@

$(BUILD)/app_res.o: res/app.rc res/app.ico
	@mkdir -p $(BUILD)
	windres -I res res/app.rc -o $@

# ---- test builds, in a folder of their own. FDR_TEST_DRIVE adds the stand-in drive, an ordinary file: it is only ever
# compiled in here, so the program itself can list nothing but the PC's floppy drives. scripts/package.sh refuses a program
# that has it.
TBUILD := $(BUILD)/testbuild
TEST_EXE := $(TBUILD)/test_core$(X)
TEST_OBJ := $(patsubst %.c,$(TBUILD)/%.o,$(CORE) tests/test_core.c)
TESTGUI_EXE := $(TBUILD)/floppy_image_test$(X)
TESTGUI_OBJ := $(patsubst %.c,$(TBUILD)/%.o,$(SRC)) $(BUILD)/src/nk.o $(BUILD)/gen_font.o $(EXTRA_OBJ)

$(TBUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -DFDR_TEST_DRIVE -c $< -o $@

$(TBUILD)/src/main.o $(TBUILD)/src/fgui.o: CFLAGS += $(NKFLAGS)

$(TEST_EXE): $(TEST_OBJ)
	$(CC) -o $@ $^

test: $(TEST_EXE)
	$(TEST_EXE) $(TBUILD)

# the window on the stand-in drive
testgui: $(TESTGUI_EXE)

$(TESTGUI_EXE): $(TESTGUI_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

clean:
	rm -rf $(BUILD) $(EXE)

.PHONY: all test testgui clean

-include $(OBJ:.o=.d) $(TEST_OBJ:.o=.d) $(TESTGUI_OBJ:.o=.d)
