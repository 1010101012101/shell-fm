
SOURCE	:= $(wildcard *.c)
OBJECT	:= $(subst .c,.o,$(SOURCE))
BINARY	:= shell-fm
LDFLAGS := $(shell pkg-config --exists ao && pkg-config ao --libs)
LDFLAGS	:= $(shell [ -f /usr/lib/pkgconfig/ao.pc ] && pkg-config ao --libs)
CFLAGS	:= -Os -Wall -W -I./include/ $(LIBAO)

.PHONY: clean

shell-fm	: $(OBJECT)
	gcc -o $(BINARY) $(CFLAGS) $(OBJECT) $(shell pkg-config mad --libs) $(LDFLAGS)

clean		:
	rm -f $(OBJECT) $(BINARY)