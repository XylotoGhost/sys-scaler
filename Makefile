# Builds the app and the sysmodule, assembles the SD card layout in sd/ and
# (make dist) a release zip in dist/.
# Windows: docker run --rm -v "${PWD}:/src" -w /src devkitpro/devkita64:20260219 make dist
VERSION := $(strip $(shell cat VERSION))
TID     := 420000000000A5F1
APPS    := app sysmodule

.PHONY: all clean sd dist $(APPS)

all: sd

$(APPS):
	@$(MAKE) --no-print-directory -C $@

sd: $(APPS)
	@rm -rf sd
	@mkdir -p sd/switch/sys-scaler sd/atmosphere/contents/$(TID)/flags
	@cp app/sys-scaler.nro sd/switch/sys-scaler/
	@cp sysmodule/sys-scaler.nsp sd/atmosphere/contents/$(TID)/exefs.nsp
	@cp sysmodule/toolbox.json sd/atmosphere/contents/$(TID)/
	@touch sd/atmosphere/contents/$(TID)/flags/boot2.flag
	@echo "SD layout ready in sd/ (copy its contents to the SD card root)"

dist: sd
	@mkdir -p dist
	@rm -f dist/sys-scaler-v$(VERSION).zip
	@cd sd && zip -qr ../dist/sys-scaler-v$(VERSION).zip .
	@echo "Release: dist/sys-scaler-v$(VERSION).zip"

clean:
	@for d in $(APPS); do $(MAKE) --no-print-directory -C $$d clean; done
	@rm -rf sd dist
