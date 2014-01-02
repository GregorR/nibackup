#define _XOPEN_SOURCE 700 /* for realpath */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "nibackup.h"
#include "notify.h"

int main(int argc, char **argv)
{
    NiBackup ni;
    pthread_t nth;

    if (argc != 3) {
        fprintf(stderr, "Use: nibackup <source> <dest>\n");
        return 1;
    }

    ni.source = realpath(argv[1], NULL);
    if (ni.source == NULL) {
        perror("realpath");
        return 1;
    }
    ni.dest = realpath(argv[2], NULL);
    if (ni.dest == NULL) {
        perror("realpath");
        return 1;
    }

    ni.sourceLen = strlen(ni.source);
    ni.destLen = strlen(ni.dest);

    /* start the notify monitor */
    notifyInit(&ni);

    /* and the notify thread */
    pthread_create(&nth, NULL, notifyLoop, &ni);

    while (sem_wait(&ni.qsem) == 0) {
        NotifyQueue *ev;

        pthread_mutex_lock(&ni.qlock);
        ev = ni.notifs;
        ni.notifs = ev->next;
        pthread_mutex_unlock(&ni.qlock);

        printf("%s\n", ev->file);
        free(ev->file);
        free(ev);
    }

    pthread_join(nth, NULL);

    return 0;
}
