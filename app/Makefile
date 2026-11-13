APP_TITLE    := sys-scaler
TARGET       := sys-scaler
# FreeType and whatever it depends on in the installed portlibs.
FREETYPE      = $(PORTLIBS)/bin/aarch64-none-elf-pkg-config freetype2
EXTRA_CFLAGS  = $(shell $(FREETYPE) --cflags)
EXTRA_LIBS    = $(shell $(FREETYPE) --libs)

include $(dir $(lastword $(MAKEFILE_LIST)))../common/app.mk
