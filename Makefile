CC=gcc
CFLAGS=-Wall -Werror -std=c99 -pedantic -g
LIBS=-pthread -lcap

NIBACKUP_OBJS=backup.o nibackup.o notify.o
NIPURGE_OBJS=nipurge.o

all: nibackup nibackup-purge

nibackup: $(NIBACKUP_OBJS)
	$(CC) $(CFLAGS) $(NIBACKUP_OBJS) $(LIBS) -o nibackup

nibackup-purge: $(NIPURGE_OBJS)
	$(CC) $(CFLAGS) $(NIPURGE_OBJS) -o nibackup-purge

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) nibackup
