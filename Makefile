CC=gcc
CFLAGS=-Wall -Werror -std=c99 -pedantic -g
LIBS=-pthread -lcap

OBJS=backup.o nibackup.o notify.o

all: nibackup

nibackup: $(OBJS)
	$(CC) $(CFLAGS) $(OBJS) $(LIBS) -o nibackup

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) nibackup
