/*
 * notify.c: fanotify interface
 *
 * Copyright (c) 2014, Gregor Richards
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define _XOPEN_SOURCE 700 /* for lstat */

#include <linux/fcntl.h>
#include <linux/limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nibackup.h"
#include "notify.h"

/* initialize the notification queue for this instance */
int notifyInit(NiBackup *ni, int fd)
{
    int tmpi;

    if (fd < 0) {
        /* init fanotify */
        fd = fanotify_init(
            FAN_CLASS_CONTENT,
            O_PATH | O_CLOEXEC);
        if (fd == -1) {
            /* critical */
            perror("fanotify_init");
            exit(1);
        }

        /* and mark all events under the source */
        tmpi = fanotify_mark(
            fd, FAN_MARK_ADD | FAN_MARK_MOUNT,
            FAN_CLOSE_WRITE | /*FAN_MODIFY |*/ FAN_ONDIR | FAN_EVENT_ON_CHILD,
            FAN_NOFD, ni->source);
        if (tmpi < 0) {
            perror("fanotify_mark");
            exit(1);
        }
    }

    /* then initialize the locks */
    if (pthread_mutex_init(&ni->qlock, NULL) < 0) {
        perror("pthread_mutex_init");
        exit(1);
    }
    if (sem_init(&ni->qsem, 0, 0) < 0) {
        perror("sem_init");
        exit(1);
    }

    /* and save our data */
    ni->notifs = ni->lastNotif = NULL;
    ni->notifFd = fd;
    return fd;
}

/* enqueue this event */
void enqueue(NiBackup *ni, char *file)
{
    NotifyQueue *ev;

    /* make sure it's in the source */
    if (strncmp(ni->source, file, ni->sourceLen)) {
        free(file);
        return;
    }

    pthread_mutex_lock(&ni->qlock);

    /* create this event */
    ev = malloc(sizeof(NotifyQueue));
    if (ev == NULL) {
        /* FIXME */
        perror("malloc");
        exit(1);
    }
    ev->next = NULL;
    ev->file = file;

    /* and add it */
    if (ni->notifs == NULL) {
        ni->notifs = ni->lastNotif = ev;
    } else {
        ni->lastNotif->next = ev;
        ni->lastNotif = ev;
    }

    /* notify */
    pthread_mutex_unlock(&ni->qlock);
    sem_post(&ni->qsem);
}

/* the notification loop */
void *notifyLoop(void *nivp)
{
    NiBackup *ni = (NiBackup *) nivp;
    int fd = ni->notifFd;

    char buf[4096];
    char pathBuf[1024];
    const struct fanotify_event_metadata *metadata;
    ssize_t len;

    while ((len = read(fd, buf, sizeof(buf))) != -1) {
        metadata = (struct fanotify_event_metadata *) buf;
        while (FAN_EVENT_OK(metadata, len)) {
            /* FIXME: handle FAN_NOFD by forcing reset */
            if (metadata->fd != FAN_NOFD && metadata->fd >= 0) {
                struct stat lsb;
                char *realPath;

                sprintf(pathBuf, "/proc/self/fd/%d", metadata->fd);
                if (lstat(pathBuf, &lsb) != -1) {
                    realPath = malloc(lsb.st_size + 1);
                    /* FIXME */
                    if (realPath) {
                        ssize_t rllen = readlink(pathBuf, realPath, lsb.st_size);
                        if (rllen > 0) {
                            /* got the real path */
                            realPath[rllen] = 0;
                            enqueue(ni, realPath);
                        } else {
                            free(realPath);
                        }
                    }
                }

                close(metadata->fd);
            }

            metadata = FAN_EVENT_NEXT(metadata, len);
        }
    }

    /* FIXME */
    close(fd);
    return NULL;
}
