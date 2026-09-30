CC ?= cc
CFLAGS ?= -O2 -g
WARNINGS = -Wall -Wextra -Wpedantic

.PHONY: all
all: tools/frametap tools/unpack_raw10

tools/frametap: tools/frametap.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $< $(LDLIBS)

tools/unpack_raw10: tools/unpack_raw10.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(WARNINGS) -std=c11 $(LDFLAGS) -o $@ $< $(LDLIBS)
