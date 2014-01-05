/*
 * notify.c: fanotify/inotify interface
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

#include <errno.h>
#include <linux/fcntl.h>
#include <linux/limits.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/fanotify.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "exclude.h"
#include "nibackup.h"
#include "notify.h"

#define HASHTABLE_SZ 128

/* inotify watches are stored in two hash tables (one for watch #, one for
 * path) and an LRU queue */
struct InotifyWatch_ {
    struct InotifyWatch_
        *lruNext, *lruPrev,
        *idNext, *idPrev,
        *pathNext, *pathPrev;

    int id;
    char *path;
};
typedef struct InotifyWatch_ InotifyWatch;

static pthread_mutex_t watchesLock;
static int watchCount = 0;
static InotifyWatch watchesLRUHead, watchesLRUTail;
static InotifyWatch watchesTable[HASHTABLE_SZ];

/* initialize the notification queue for this instance */
void notifyInit(NiBackup *ni)
{
    int tmpi;
    int ffd, ifd;

    ffd = ni->fanotifFd;
    ifd = ni->inotifFd;

    if (ffd < 0) {
        /* init fanotify */
        ffd = fanotify_init(
            FAN_CLASS_CONTENT,
            O_PATH | O_CLOEXEC);
        if (ffd == -1) {
            /* critical */
            perror("fanotify_init");
            exit(1);
        }

        /* and mark all events under the source */
        tmpi = fanotify_mark(
            ffd, FAN_MARK_ADD | FAN_MARK_MOUNT,
            FAN_CLOSE_WRITE | /*FAN_MODIFY |*/ FAN_ONDIR | FAN_EVENT_ON_CHILD,
            FAN_NOFD, ni->source);
        if (tmpi < 0) {
            perror("fanotify_mark");
            exit(1);
        }
    }

    if (ifd < 0) {
        /* init inotify */
        ifd = inotify_init();
        if (ifd == -1) {
            /* critical */
            perror("inotify_init");
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
    pthread_mutex_init(&watchesLock, NULL);

    /* and save our data */
    ni->notifs = NULL;
    ni->fanotifFd = ffd;
    ni->inotifFd = ifd;
}

/* enqueue this event */
static void enqueue(NiBackup *ni, char *file)
{
    NotifyQueue *last, *cur, *ev;

    /* make sure it's in the source */
    if (strncmp(ni->source, file, ni->sourceLen) ||
        file[ni->sourceLen] != '/') {
        free(file);
        return;
    }

    /* handle exclusions */
    if (excluded(ni, file + ni->sourceLen + 1)) {
        free(file);
        return;
    }

    pthread_mutex_lock(&ni->qlock);

    /* check that it isn't already present */
    last = cur = ni->notifs;
    while (cur) {
        if (cur->file && !strcmp(cur->file, file)) {
            pthread_mutex_unlock(&ni->qlock);
            free(file);
            return;
        }
        last = cur;
        cur = cur->next;
    }

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
    if (last == NULL) {
        ni->notifs = ev;
    } else {
        last->next = ev;
    }

    /* notify */
    pthread_mutex_unlock(&ni->qlock);
    sem_post(&ni->qsem);
}

/* classic (Bernstein) hash */
static unsigned long hash(unsigned char *str)
{
    unsigned long hash = 5381;
    int c;

    while ((c = *str++))
        hash = ((hash << 5) + hash) ^ c; /* hash * 33 ^ c */

    return hash;
}

/* refresh an existing watch, if there is one */
static InotifyWatch *refreshWatch(NiBackup *ni, char *path)
{
    InotifyWatch *cur;
    unsigned long pHash = hash((unsigned char *) path) % HASHTABLE_SZ;

    /* find the existing watch */
    cur = &watchesTable[pHash];
    while (cur) {
        if (cur->path && !strcmp(cur->path, path)) break;
        cur = cur->pathNext;
    }

    /* if we found it, refresh it */
    if (cur) {
        cur->lruPrev->lruNext = cur->lruNext;
        cur->lruNext->lruPrev = cur->lruPrev;

        cur->lruPrev = watchesLRUTail.lruPrev;
        cur->lruPrev->lruNext = cur;

        cur->lruNext = &watchesLRUTail;

        watchesLRUTail.lruPrev = cur;
    }

    watchesLRUHead.lruNext = &watchesLRUTail;
    watchesLRUTail.lruPrev = &watchesLRUHead;

    return cur;
}

/* delete a watch */
static void delWatch(NiBackup *ni, InotifyWatch *w)
{
    /* remove it from all the lists */
    w->lruPrev->lruNext = w->lruNext;
    w->lruNext->lruPrev = w->lruPrev;
    w->idPrev->idNext = w->idNext;
    if (w->idNext) w->idNext->idPrev = w->idPrev;
    w->pathPrev->pathNext = w->pathNext;
    if (w->pathNext) w->pathNext->pathPrev = w->pathPrev;

    /* clear the watch */
    inotify_rm_watch(ni->inotifFd, w->id);
    watchCount--;

    /* and free all the used memory */
    free(w->path);
    free(w);
}

/* create a new watch */
static InotifyWatch *newWatch(NiBackup *ni, char *path)
{
    InotifyWatch *ret;
    unsigned long hval;

    /* make the structure */
    ret = calloc(sizeof(InotifyWatch), 1);
    if (ret == NULL) {
        free(path);
        return NULL;
    }

    /* perhaps remove an old one */
    if (watchCount >= ni->maxInotifyWatches) {
        if (watchesLRUHead.lruNext->path)
            delWatch(ni, watchesLRUHead.lruNext);
    }

    /* now set up this one */
    ret->path = path;
#define INOTIFY_MODE (IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF | IN_MOVED_FROM | IN_MOVED_TO)
    ret->id = inotify_add_watch(ni->inotifFd, path, INOTIFY_MODE);
    if (ret->id < 0 && errno == ENOSPC) {
        /* just out of watches, try clearing one */
        if (watchesLRUHead.lruNext->path)
            delWatch(ni, watchesLRUHead.lruNext);
        ret->id = inotify_add_watch(ni->inotifFd, path, INOTIFY_MODE);
    }
#undef INOTIFY_MODE
    if (ret->id < 0) {
        free(path);
        free(ret);
        return NULL;
    }
    watchCount++;

    /* and add it to the tables */
    ret->lruPrev = watchesLRUTail.lruPrev;
    ret->lruPrev->lruNext = ret;
    ret->lruNext = &watchesLRUTail;
    watchesLRUTail.lruPrev = ret;

    hval = ret->id % HASHTABLE_SZ;
    ret->idNext = watchesTable[hval].idNext;
    watchesTable[hval].idNext = ret;
    ret->idPrev = &watchesTable[hval];
    if (ret->idNext) ret->idNext->idPrev = ret;

    hval = hash((unsigned char *) path) % HASHTABLE_SZ;
    ret->pathNext = watchesTable[hval].pathNext;
    watchesTable[hval].pathNext = ret;
    ret->pathPrev = &watchesTable[hval];
    if (ret->pathNext) ret->pathNext->pathPrev = ret;

    return ret;
}

/* add or refresh a watch for this directory */
static void addWatch(NiBackup *ni, char *path)
{
    /* make sure it's in the source */
    if (strncmp(ni->source, path, ni->sourceLen) ||
        (path[ni->sourceLen] && path[ni->sourceLen] != '/')) {
        free(path);
        return;
    }

    /* refresh an existing watch */
    if (refreshWatch(ni, path)) {
        free(path);
        return;
    }

    /* OK, make a new watch for this */
    newWatch(ni, path);
}

/* get a watch by its watch descriptor */
static InotifyWatch *getWatchById(NiBackup *ni, int wd)
{
    InotifyWatch *w;
    unsigned long hval;

    hval = wd % HASHTABLE_SZ;
    w = &watchesTable[hval];
    while (w) {
        if (w->path && w->id == wd) break;
        w = w->idNext;
    }

    return w;
}

/* the fa-notification loop */
static void *fanotifyLoop(void *nivp)
{
    NiBackup *ni = (NiBackup *) nivp;
    int fd = ni->fanotifFd;

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
                char *realPath, *dirPath;

                sprintf(pathBuf, "/proc/self/fd/%d", metadata->fd);
                if (lstat(pathBuf, &lsb) != -1) {
                    realPath = malloc(4096); /* FIXME: Arbitrary, st_size seems wrong */
                    /* FIXME */
                    if (realPath) {
                        ssize_t rllen = readlink(pathBuf, realPath, 4095);
                        if (rllen > 0) {
                            size_t len;

                            /* got the real path */
                            realPath[rllen] = 0;

                            /* check for /proc's confusing " (deleted)" thing */
                            len = strlen(realPath);
                            if (len > 10 && !strcmp(realPath + len - 10, " (deleted)")) {
                                /* get rid of it */
                                len -= 10;
                                realPath[len] = 0;
                            }

                            /* get just the directory component */
                            dirPath = strdup(realPath);
                            if (dirPath) {
                                char *slash = strrchr(dirPath, '/');
                                if (slash) *slash = 0;
                            }

                            /* enqueue the real path */
                            enqueue(ni, realPath);

                            /* and watch the directory */
                            if (dirPath) {
                                pthread_mutex_lock(&watchesLock);
                                addWatch(ni, dirPath);
                                pthread_mutex_unlock(&watchesLock);
                            }
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

/* the i-notification loop */
static void *inotifyLoop(void *nivp)
{
    NiBackup *ni = nivp;
    int fd = ni->inotifFd;

    char buf[4096];
    ssize_t len;
    struct inotify_event *ie = NULL;

    InotifyWatch *watch;

    while ((len = read(fd, buf, sizeof(buf))) != -1) {
        char *cur = buf;

        /* so long as we still have an event... */
        while (len >= sizeof(struct inotify_event)) {
            ie = (struct inotify_event *) cur;
            pthread_mutex_lock(&watchesLock);

            /* find the associated watch */
            if ((watch = getWatchById(ni, ie->wd))) {
                char *notifPath;

                /* make the full path */
                if (ie->len) {
                    notifPath = malloc(strlen(watch->path) + strlen(ie->name) + 2);
                    if (notifPath)
                        sprintf(notifPath, "%s/%s", watch->path, ie->name);
                } else {
                    notifPath = strdup(watch->path);
                }

                /* and enqueue it */
                if (notifPath)
                    enqueue(ni, notifPath);

                /* as a special case, if the directory itself was removed or
                 * renamed, we need to kill the watch */
                if (ie->mask & (IN_DELETE_SELF|IN_MOVE_SELF))
                    delWatch(ni, watch);
            }

            pthread_mutex_unlock(&watchesLock);

            cur += sizeof(struct inotify_event) + ie->len;
            len -= sizeof(struct inotify_event) + ie->len;
        }
    }

    return NULL;
}

/* begin the notification threads */
void notifyThread(NiBackup *ni)
{
    pthread_create(&ni->fanotifTh, NULL, fanotifyLoop, ni);
    pthread_create(&ni->inotifTh, NULL, inotifyLoop, ni);
}
