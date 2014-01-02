#ifndef NIBACKUP_H
#define NIBACKUP_H

#include <pthread.h>
#include <semaphore.h>

#include "notify.h"

struct NiBackup_ {
    size_t sourceLen;
    const char *source;
    size_t destLen;
    const char *dest;

    pthread_mutex_t qlock;
    sem_t qsem;
    NotifyQueue *notifs, *lastNotif;
    void *notifData;
};
typedef struct NiBackup_ NiBackup;

#endif
