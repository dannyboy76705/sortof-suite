# sortof-suite: file-list orderers for archivers. One small program per ordering.
#   sortof-natural    natural order (path by path, digit runs by value)    
#   sortof-extension  natural order by extension, then name, then folder   
#   sortof-minhash    similarity order (MinHash)
#   sortof-cdcsort    keeps the order, chains identical files behind their first instance
#   sortof-palscore   scene order for JPEG frame lists (colour histograms, duplicate chaining)
# Needs gcc; sortof-palscore also needs libjpeg (apt install libjpeg-dev).
# To add a program: write sortof-NAME.c that includes sortof-common.h, add NAME to PROGS.
# To pull one: remove it from PROGS (and delete the file).
CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local
MANDIR  ?= $(PREFIX)/share/man

PROGS = natural extension minhash cdcsort palscore
BINS  = $(addprefix sortof-,$(PROGS))

all: $(BINS)

sortof-common.o: sortof-common.c sortof-common.h
	$(CC) $(CFLAGS) -pthread -c -o $@ $<

sortof-natural: sortof-natural.c sortof-common.o sortof-common.h
	$(CC) $(CFLAGS) -pthread -o $@ $< sortof-common.o

sortof-extension: sortof-extension.c sortof-common.o sortof-common.h
	$(CC) $(CFLAGS) -pthread -o $@ $< sortof-common.o

sortof-cdcsort: sortof-cdcsort.c sortof-common.o sortof-common.h sortof-sketch.h
	$(CC) $(CFLAGS) -pthread -o $@ $< sortof-common.o

sortof-minhash: sortof-minhash.c sortof-common.o sortof-common.h sortof-sketch.h
	$(CC) $(CFLAGS) -pthread -o $@ $< sortof-common.o -lm

sortof-palscore: sortof-palscore.c sortof-common.o sortof-common.h
	$(CC) $(CFLAGS) -pthread -o $@ $< sortof-common.o -ljpeg -lm

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BINS) $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(MANDIR)/man1
	install -m 644 man/*.1 $(DESTDIR)$(MANDIR)/man1

clean:
	rm -f $(BINS) sortof-common.o

.PHONY: all install clean
