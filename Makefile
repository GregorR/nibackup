CC=gcc
#CFLAGS=-Wall -Werror -std=c99 -pedantic -g
CFLAGS=-O3 -g
LIBS=-pthread -lcap

INSTALL=install -s

DESTDIR=
PREFIX=/usr
BIN_PREFIX=$(PREFIX)/bin

NIBACKUP_OBJS=backup.o exclude.o metadata.o nibackup.o notify.o
NIPURGE_OBJS=metadata.o nipurge.o
NIRESTORE_OBJS=metadata.o nirestore.o
NILS_OBJS=metadata.o nils.o
BINARIES=nibackup nibackup-purge nibackup-restore nibackup-ls

all: $(BINARIES)

nibackup: $(NIBACKUP_OBJS)
	$(CC) $(CFLAGS) $(NIBACKUP_OBJS) $(LIBS) -o nibackup

nibackup-purge: $(NIPURGE_OBJS)
	$(CC) $(CFLAGS) $(NIPURGE_OBJS) -o nibackup-purge

nibackup-restore: $(NIRESTORE_OBJS)
	$(CC) $(CFLAGS) $(NIRESTORE_OBJS) -o nibackup-restore

nibackup-ls: $(NILS_OBJS)
	$(CC) $(CFLAGS) $(NILS_OBJS) -o nibackup-ls

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

install: all
	mkdir -p $(DESTDIR)$(BIN_PREFIX)
	for bin in $(BINARIES); do \
	    $(INSTALL) $$bin $(DESTDIR)$(BIN_PREFIX); \
	done

clean:
	rm -f $(NIBACKUP_OBJS) $(NIPURGE_OBJS) $(NIRESTORE_OBJS) $(NILS_OBJS) \
	    nibackup nibackup-purge nibackup-restore nibackup-ls \
	    deps

deps:
	-$(CC) -MM *.c > deps

include deps
