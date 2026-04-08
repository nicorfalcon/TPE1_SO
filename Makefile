CC      = gcc
CFLAGS  = -Wall -std=c11 -Iinclude
LDFLAGS = -lrt -lpthread

SRCDIR = src
BINDIR = .

.PHONY: all clean

all: master vista player

master: $(SRCDIR)/master.c
	$(CC) $(CFLAGS) -o $(BINDIR)/$@ $< $(LDFLAGS)

vista: $(SRCDIR)/vista.c
	$(CC) $(CFLAGS) -o $(BINDIR)/$@ $< $(LDFLAGS)

player: $(SRCDIR)/player.c
	$(CC) $(CFLAGS) -o $(BINDIR)/$@ $< $(LDFLAGS)

clean:
	rm -f $(BINDIR)/master $(BINDIR)/vista $(BINDIR)/player
