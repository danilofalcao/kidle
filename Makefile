CC      = gcc
CFLAGS  = -Wall -Wextra -pedantic -std=c11 $(shell pkg-config --cflags gio-2.0 glib-2.0)
LDFLAGS = $(shell pkg-config --libs gio-2.0 glib-2.0)

TARGET  = kidle
SRC     = kidle.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: all clean