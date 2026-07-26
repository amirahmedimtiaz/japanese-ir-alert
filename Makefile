CC ?= cc
CPPFLAGS ?=
CPPFLAGS += -D_POSIX_C_SOURCE=200809L
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
LDLIBS ?= -lcurl

TARGET := tanaken-alert
SOURCES := src/tanaken_alert.c

.PHONY: all clean test

all: $(TARGET)

test: $(TARGET)
	sh ./tests/run_tests.sh

$(TARGET): $(SOURCES)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $^ $(LDLIBS)

clean:
	rm -f $(TARGET)
