#ifndef NOTIFY_H
#define NOTIFY_H

struct NiBackup_;

/* a notification queue */
struct NotifyQueue_ {
    struct NotifyQueue_ *next;
    char *file;
};
typedef struct NotifyQueue_ NotifyQueue;

/* initialize the notification queue for this instance */
int notifyInit(struct NiBackup_ *ni, int fd);

/* the notification loop */
void *notifyLoop(void *nivp);

#endif
