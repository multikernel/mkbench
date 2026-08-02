CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -std=gnu11 -D_GNU_SOURCE
LDLIBS = -lpthread -lm

SRCS := src/mkbench.c src/common.c src/check.c src/memlat.c src/membw.c \
        src/pingpong.c src/atomics.c src/lockbench.c src/ipc.c src/wakeup.c
OBJS := $(SRCS:src/%.c=build/%.o)

mkbench: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDLIBS)

build/%.o: src/%.c src/common.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

clean:
	rm -rf build mkbench

.PHONY: clean
