# Builds all apps and assembles an SD card layout in sd/.
# Windows: docker run --rm -v "${PWD}:/src" -w /src devkitpro/devkita64:latest make
APPS := diag modetest sysmodule
TID  := 420000000000A5F1

.PHONY: all clean sd $(APPS)

all: sd

$(APPS):
	@$(MAKE) --no-print-directory -C $@

sd: $(APPS)
	@rm -rf sd
	@mkdir -p sd/switch/aspectfix sd/atmosphere/contents/$(TID)/flags
	@cp diag/aspectfix-diag.nro modetest/aspectfix-modetest.nro sd/switch/aspectfix/
	@cp sysmodule/aspectfixd.nsp sd/atmosphere/contents/$(TID)/exefs.nsp
	@touch sd/atmosphere/contents/$(TID)/flags/boot2.flag
	@echo "SD layout ready in sd/ (copy its contents to the SD card root)"

clean:
	@for d in $(APPS); do $(MAKE) --no-print-directory -C $$d clean; done
	@rm -rf sd
