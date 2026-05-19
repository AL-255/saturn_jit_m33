# x48ng_build — wrapper for building the x48ng submodule on Apple Silicon
# with SDL as the video backend.
#
# Targets:
#   all       (default) — build the x48ng binary; copy to ./x48ng
#   patch     apply idempotent macOS tweaks to ../x48ng/Makefile
#   clean     clean object files in the submodule and remove ./x48ng
#   distclean full clean and restore the unpatched upstream Makefile

X48NG_DIR  := $(CURDIR)/../x48ng
UI4X_DIR   := $(X48NG_DIR)/src/ui4x
SERIAL_C   := $(X48NG_DIR)/src/core/serial.c
SDL_C      := $(UI4X_DIR)/src/sdl.c
SDL_PATCH  := $(CURDIR)/sdl_macos.patch
BIN        := $(CURDIR)/x48ng

# Apple Silicon Homebrew prefix.
HOMEBREW_PREFIX ?= /opt/homebrew

# Expose keg-only formulae (readline, ncurses) to pkg-config.
export PKG_CONFIG_PATH := $(HOMEBREW_PREFIX)/opt/readline/lib/pkgconfig:$(HOMEBREW_PREFIX)/opt/ncurses/lib/pkgconfig:$(PKG_CONFIG_PATH)

# No plain `lua` formula on this host; use luajit (pkg-config provides `luajit`).
LUA_VERSION ?= luajit

# Sentinel that records we have applied the macOS source patches.
PATCH_STAMP := $(CURDIR)/.patched

# ui4x's Makefile doesn't include glib but src/api.c #include's <glib.h>.
# Inject glib's include flags via CPPFLAGS so the sub-make picks them up
# through GNU make's default %.o: %.c rule.
GLIB_CFLAGS := $(shell pkg-config --cflags glib-2.0)
export CPPFLAGS := $(GLIB_CFLAGS)

# Variables that disable GTK in both the outer build and the ui4x sub-build,
# and force SDL3 on. WITH_GTK is exported so the top Makefile's nested
# `make -C src/ui4x` call inherits it.
export WITH_GTK := no

# libui4x's api.c calls gtk_* functions unconditionally (the FRONTEND_GTK
# switch arms aren't guarded by HAS_GTK), so we link in a small stub TU
# that satisfies those symbols without dragging in gtk4.
GTK_STUB_OBJ := $(CURDIR)/gtk_stubs.o

MAKE_OVERRIDES := \
	WITH_SDL=yes \
	WITH_SDL2=no \
	WITH_GTK=no \
	LUA_VERSION=$(LUA_VERSION) \
	GTK_CFLAGS= \
	GTK_LDLIBS= \
	LDLIBS=$(GTK_STUB_OBJ)

.PHONY: all build patch clean distclean

all: build

# Idempotent: only edits files if the Linux-isms are still present.
$(PATCH_STAMP):
	@if grep -q -- '-Wl,--no-as-needed' $(X48NG_DIR)/Makefile; then \
		echo "[patch] strip -Wl,--no-as-needed from $(X48NG_DIR)/Makefile"; \
		cp $(X48NG_DIR)/Makefile $(X48NG_DIR)/Makefile.x48ng_build.orig; \
		sed -i.tmp -e 's/[[:space:]]*-Wl,--no-as-needed//' $(X48NG_DIR)/Makefile; \
		rm -f $(X48NG_DIR)/Makefile.tmp; \
	fi
	@if grep -q '^#if defined( __FreeBSD__ )$$' $(SERIAL_C); then \
		echo "[patch] extend __FreeBSD__ guard in serial.c to also cover __APPLE__"; \
		cp $(SERIAL_C) $(SERIAL_C).x48ng_build.orig; \
		sed -i.tmp -e 's/^#if defined( __FreeBSD__ )$$/#if defined( __FreeBSD__ ) || defined( __APPLE__ )/' $(SERIAL_C); \
		rm -f $(SERIAL_C).tmp; \
	fi
	@if ! grep -q 'X48NG_DEBUG_SDL_EVENTS' $(SDL_C); then \
		echo "[patch] apply sdl_macos.patch (SDL_RaiseWindow + debug logging)"; \
		cp $(SDL_C) $(SDL_C).x48ng_build.orig; \
		patch -p1 -d $(X48NG_DIR) -i $(SDL_PATCH); \
	fi
	@touch $@

patch: $(PATCH_STAMP)

$(GTK_STUB_OBJ): $(CURDIR)/gtk_stubs.c
	$(CC) -O2 -Wall -Wextra -c $< -o $@

build: patch $(GTK_STUB_OBJ)
	$(MAKE) -C $(X48NG_DIR) $(MAKE_OVERRIDES)
	@cp $(X48NG_DIR)/dist/x48ng $(BIN)
	@echo "Built: $(BIN)"
	@file $(BIN)

clean:
	-$(MAKE) -C $(X48NG_DIR) clean $(MAKE_OVERRIDES)
	rm -f $(BIN) $(GTK_STUB_OBJ)

distclean:
	-$(MAKE) -C $(X48NG_DIR) distclean $(MAKE_OVERRIDES)
	rm -f $(BIN)
	@if [ -f $(X48NG_DIR)/Makefile.x48ng_build.orig ]; then \
		echo "[patch] restoring original $(X48NG_DIR)/Makefile"; \
		mv $(X48NG_DIR)/Makefile.x48ng_build.orig $(X48NG_DIR)/Makefile; \
	fi
	@if [ -f $(SERIAL_C).x48ng_build.orig ]; then \
		echo "[patch] restoring original $(SERIAL_C)"; \
		mv $(SERIAL_C).x48ng_build.orig $(SERIAL_C); \
	fi
	@if [ -f $(SDL_C).x48ng_build.orig ]; then \
		echo "[patch] restoring original $(SDL_C)"; \
		mv $(SDL_C).x48ng_build.orig $(SDL_C); \
	fi
	rm -f $(PATCH_STAMP)
