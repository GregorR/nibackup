/*
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

#ifndef NIBACKUP_H
#define NIBACKUP_H

#include <pthread.h>
#include <semaphore.h>

#include "notify.h"

struct NiBackup_ {
    size_t sourceLen;
    const char *source;
    int sourceFd;

    size_t destLen;
    const char *dest;
    int destFd;

    /* configuration */
    int verbose;
    int waitAfterNotif;
    int fullSyncCycle;
    int noRootDotfiles;
    int threads;

    /* notification thread info */
    pthread_mutex_t qlock;
    sem_t qsem;
    NotifyQueue *notifs, *lastNotif;
    int notifFd;

    /* threads for actual backup */
    sem_t bsem;
    pthread_mutex_t *blocks;
    pthread_t *bth;
    int *brunning;

    /* exclusions */
    struct Exclusion_ *exclusions;
};
typedef struct NiBackup_ NiBackup;

#endif
