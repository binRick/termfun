TERMPAINT := termpaint
BUILD := build

CC ?= cc
CFLAGS ?= -O2 -g
CFLAGS += -std=gnu11 -Wall -Wextra -I$(TERMPAINT)
LDLIBS += -lm

# defines meson normally supplies; -include stdarg.h papers over a missing
# include in termpaintx_ttyrescue.c that only bites on macOS
LIB_CFLAGS := -DTERMPAINT_RESCUE_EMBEDDED -DTERMPAINT_RESCUE_PATH='"/usr/libexec"' \
              -include stdarg.h -Wno-unused-parameter

LIB_SRCS := termpaint.c termpaint_event.c termpaint_input.c \
            termpaintx.c termpaintx_ttyrescue.c ttyrescue.c
LIB_OBJS := $(LIB_SRCS:%.c=$(BUILD)/termpaint/%.o)

all: submodules $(BUILD)/fireworks $(BUILD)/fireworks-gfx $(BUILD)/kitty_probe

submodules:
	@test -f $(TERMPAINT)/termpaint.h || git submodule update --init

$(BUILD)/fireworks: $(BUILD)/fireworks.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/fireworks-gfx: $(BUILD)/fireworks-gfx.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/fireworks.o: fireworks.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/fireworks-gfx.o: fireworks-gfx.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/kitty_gfx.o: kitty_gfx.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/termpaint/%.o: $(TERMPAINT)/%.c | $(BUILD)/termpaint
	$(CC) $(CFLAGS) $(LIB_CFLAGS) -c -o $@ $<

$(BUILD) $(BUILD)/termpaint:
	mkdir -p $@

run: $(BUILD)/fireworks
	./$(BUILD)/fireworks

$(BUILD)/kitty_probe: kitty_probe.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $<

run-gfx: $(BUILD)/fireworks-gfx
	./$(BUILD)/fireworks-gfx

clean:
	rm -rf $(BUILD)

.PHONY: all submodules run run-gfx clean
