# Host build of the Macintosh core: make -C components/macintosh/host
#   build/mac_host <rom> [frames] [out.ppm] [ram_kb]   (see host_main.c)
MUSASHI   := ../../../../esp32emu/components/musashi
MAC       := ..
BUILD     := build
CFLAGS    := -O2 -g -Wall -DM68K_INSTRUCTION_HOOK=2 -I$(MAC)/include -I$(MAC) -I$(MUSASHI) -I$(MUSASHI)/musashi -I$(MUSASHI)/generated

MAC_SRCS  := $(wildcard $(MAC)/*.c)
M68K_SRCS := $(MUSASHI)/musashi/m68kcpu.c $(MUSASHI)/musashi/m68kdasm.c \
             $(MUSASHI)/musashi/softfloat/softfloat.c $(MUSASHI)/generated/m68kops.c
MAC_OBJS  := $(patsubst $(MAC)/%.c,$(BUILD)/mac/%.o,$(MAC_SRCS))
M68K_OBJS := $(patsubst $(MUSASHI)/%.c,$(BUILD)/m68k/%.o,$(M68K_SRCS))

$(BUILD)/mac_host: host_main.c $(MAC_OBJS) $(M68K_OBJS)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/mac/%.o: $(MAC)/%.c $(wildcard $(MAC)/*.h $(MAC)/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m68k/%.o: $(MUSASHI)/%.c $(MUSASHI)/m68kconf.h $(wildcard $(MUSASHI)/musashi/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: clean
