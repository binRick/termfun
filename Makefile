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

all: submodules $(BUILD)/fireworks $(BUILD)/fireworks-gfx $(BUILD)/matrix $(BUILD)/matrix-gfx $(BUILD)/ripples $(BUILD)/ripples-gfx $(BUILD)/fire $(BUILD)/fire-gfx $(BUILD)/kitty_probe

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

clean:
	rm -rf $(BUILD)

.PHONY: all submodules run run-gfx run-matrix run-matrix-gfx run-ripples run-ripples-gfx run-fire run-fire-gfx clean
