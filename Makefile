CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -pedantic
LDFLAGS = -lrt -lpthread

INCLUDE = -I include
SRCDIR  = src
BINDIR  = bin

TARGETS = $(BINDIR)/master $(BINDIR)/player $(BINDIR)/vista

.PHONY: all clean

all: $(BINDIR) $(TARGETS)

$(BINDIR):
	mkdir -p $(BINDIR)

$(BINDIR)/master: $(SRCDIR)/master.c
	$(CC) $(CFLAGS) $(INCLUDE) $^ -o $@ $(LDFLAGS)

$(BINDIR)/player: $(SRCDIR)/player.c
	$(CC) $(CFLAGS) $(INCLUDE) $^ -o $@ $(LDFLAGS)

$(BINDIR)/vista: $(SRCDIR)/vista.c
	$(CC) $(CFLAGS) $(INCLUDE) $^ -o $@ $(LDFLAGS)

clean:
	rm -rf $(BINDIR)