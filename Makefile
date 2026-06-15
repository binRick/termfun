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

EFFECTS := fireworks fireworks-gfx matrix matrix-gfx ripples ripples-gfx \
           fire fire-gfx starfield starfield-gfx
APPS := taskman taskman-gfx filer filer-gfx 2048 2048-gfx sysmon sysmon-gfx

all: submodules $(addprefix $(BUILD)/,$(EFFECTS) $(APPS)) $(BUILD)/kitty_probe

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

$(BUILD)/matrix: $(BUILD)/matrix.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/matrix.o: matrix.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/matrix-gfx: $(BUILD)/matrix-gfx.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/matrix-gfx.o: matrix-gfx.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/ripples: $(BUILD)/ripples.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/ripples.o: ripples.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/ripples-gfx: $(BUILD)/ripples-gfx.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/ripples-gfx.o: ripples-gfx.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/fire: $(BUILD)/fire.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/fire.o: fire.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/fire-gfx: $(BUILD)/fire-gfx.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/fire-gfx.o: fire-gfx.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/starfield: $(BUILD)/starfield.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/starfield.o: starfield.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/starfield-gfx: $(BUILD)/starfield-gfx.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)

$(BUILD)/starfield-gfx.o: starfield-gfx.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/kitty_gfx.o: kitty_gfx.c kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# --- menu-driven TUI apps (termpaint cells + a kitty graphics backdrop) ---
# Each <app>.c is the cell program; <app>-gfx.c is a one-line shim that
# #defines the gfx build and #includes <app>.c, so the pair stays DRY.
$(BUILD)/tui.o: tui.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/taskman: $(BUILD)/taskman.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/taskman-gfx: $(BUILD)/taskman-gfx.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/taskman.o: taskman.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/taskman-gfx.o: taskman-gfx.c taskman.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/filer: $(BUILD)/filer.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/filer-gfx: $(BUILD)/filer-gfx.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/filer.o: filer.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/filer-gfx.o: filer-gfx.c filer.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/2048: $(BUILD)/2048.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/2048-gfx: $(BUILD)/2048-gfx.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/2048.o: 2048.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/2048-gfx.o: 2048-gfx.c 2048.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/sysmon: $(BUILD)/sysmon.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/sysmon-gfx: $(BUILD)/sysmon-gfx.o $(BUILD)/tui.o $(BUILD)/kitty_gfx.o $(LIB_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDLIBS)
$(BUILD)/sysmon.o: sysmon.c tui.h kitty_gfx.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<
$(BUILD)/sysmon-gfx.o: sysmon-gfx.c sysmon.c tui.h kitty_gfx.h | $(BUILD)
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

run-matrix: $(BUILD)/matrix
	./$(BUILD)/matrix

run-matrix-gfx: $(BUILD)/matrix-gfx
	./$(BUILD)/matrix-gfx

run-ripples: $(BUILD)/ripples
	./$(BUILD)/ripples

run-ripples-gfx: $(BUILD)/ripples-gfx
	./$(BUILD)/ripples-gfx

run-fire: $(BUILD)/fire
	./$(BUILD)/fire

run-fire-gfx: $(BUILD)/fire-gfx
	./$(BUILD)/fire-gfx

run-starfield: $(BUILD)/starfield
	./$(BUILD)/starfield

run-starfield-gfx: $(BUILD)/starfield-gfx
	./$(BUILD)/starfield-gfx

run-taskman: $(BUILD)/taskman
	./$(BUILD)/taskman
run-taskman-gfx: $(BUILD)/taskman-gfx
	./$(BUILD)/taskman-gfx
run-filer: $(BUILD)/filer
	./$(BUILD)/filer
run-filer-gfx: $(BUILD)/filer-gfx
	./$(BUILD)/filer-gfx
run-2048: $(BUILD)/2048
	./$(BUILD)/2048
run-2048-gfx: $(BUILD)/2048-gfx
	./$(BUILD)/2048-gfx
run-sysmon: $(BUILD)/sysmon
	./$(BUILD)/sysmon
run-sysmon-gfx: $(BUILD)/sysmon-gfx
	./$(BUILD)/sysmon-gfx

clean:
	rm -rf $(BUILD)

.PHONY: all submodules run run-gfx run-matrix run-matrix-gfx run-ripples run-ripples-gfx run-fire run-fire-gfx run-starfield run-starfield-gfx run-taskman run-taskman-gfx run-filer run-filer-gfx run-2048 run-2048-gfx run-sysmon run-sysmon-gfx clean
