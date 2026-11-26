CC ?= gcc
CFLAGS := -std=c11 -O3 -Wall -Wextra -D_GNU_SOURCE -pthread -Isrc
LDFLAGS := -pthread -lm

SRCS := \
	src/edb_util.c \
	src/pal_linux.c \
	src/config.c \
	src/metrics.c \
	src/pager.c \
	src/wal.c \
	src/index.c \
	src/storage.c \
	src/sql_parser.c \
	src/sql_exec.c \
	src/protocol.c \
	src/reactor_linux.c \
	src/server.c \
	src/cli.c \
	src/bench.c \
	src/main.c

TARGET := elliottdb

.PHONY: all static clean install

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LDFLAGS)

static: $(SRCS)
	$(CC) $(CFLAGS) -static -o $(TARGET) $(SRCS) $(LDFLAGS)

clean:
	rm -f $(TARGET)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/local/bin/$(TARGET)
