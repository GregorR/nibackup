CC=gcc
CFLAGS=-Wall -Werror -std=c99 -pedantic -g
#CFLAGS=-g
LIBS=-pthread -lcap

NIBACKUP_OBJS=backup.o metadata.o nibackup.o notify.o
NIPURGE_OBJS=metadata.o nipurge.o
NIRESTORE_OBJS=metadata.o nirestore.o

all: nibackup nibackup-purge nibackup-restore

nibackup: $(NIBACKUP_OBJS)
	$(CC) $(CFLAGS) $(NIBACKUP_OBJS) $(LIBS) -o nibackup

nibackup-purge: $(NIPURGE_OBJS)
	$(CC) $(CFLAGS) $(NIPURGE_OBJS) -o nibackup-purge

nibackup-restore: $(NIRESTORE_OBJS)
	$(CC) $(CFLAGS) $(NIRESTORE_OBJS) -o nibackup-restore

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(NIBACKUP_OBJS) $(NIPURGE_OBJS) $(NIRESTORE_OBJS) nibackup nibackup-purge nibackup-restore
