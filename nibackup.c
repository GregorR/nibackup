#define _XOPEN_SOURCE 700 /* for realpath */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

#include "backup.h"
#include "nibackup.h"
#include "notify.h"

/* privilege reduction utility functions */
void reduceToSysAdmin(void);
void reduceToUser(void);

/* method to trigger a full update occasionally */
void *periodicFull(void *nivp);

int main(int argc, char **argv)
{
    NiBackup ni;
    pthread_t nth, fth;
    struct stat sbuf;
    int tmpi;

    if (argc < 3) {
        fprintf(stderr, "Use: nibackup <source> <dest>\n");
        return 1;
    }

    if (argc > 3) {
        /* already have the notify fd */
        ni.notifFd = atoi(argv[3]);
    } else {
        ni.notifFd = -1;
    }

    /* reduce our privileges */
    reduceToSysAdmin();

    /* source and destination must be real paths */
    ni.source = realpath(argv[1], NULL);
    if (ni.source == NULL) {
        perror(argv[1]);
        return 1;
    }
    ni.dest = realpath(argv[2], NULL);
    if (ni.dest == NULL) {
        perror(argv[2]);
        return 1;
    }

    /* cache */
    ni.sourceLen = strlen(ni.source);
    ni.destLen = strlen(ni.dest);

    /* start the notify monitor */
    notifyInit(&ni, ni.notifFd);

    /* reduce our privileges more */
    reduceToUser();

    /* so that /proc/self/fd is readable and not root-owned, self-exec */
    if (stat("/proc/self/fd", &sbuf) == 0) {
        if (sbuf.st_uid == 0) {
            char buf[128];
            snprintf(buf, 128, "%d", ni.notifFd);
            execl("/proc/self/exe", argv[0], ni.source, ni.dest, buf, NULL);
            perror("execl");
            return 1;
        }
    }

    /* now we can safely make the notify fd cloexec */
    tmpi = fcntl(ni.notifFd, F_GETFD);
    if (tmpi < 0) {
        perror("notify");
        return 1;
    }
    if (fcntl(ni.notifFd, F_SETFD, tmpi | FD_CLOEXEC) < 0) {
        perror("notify");
        return 1;
    }

    /* for *at functions */
    ni.sourceFd = open(ni.source, O_RDONLY);
    if (ni.sourceFd < 0) {
        perror(ni.source);
        return 1;
    }
    ni.destFd = open(ni.dest, O_RDONLY);
    if (ni.destFd < 0) {
        perror(ni.dest);
        return 1;
    }

    /* and the notify thread */
    pthread_create(&nth, NULL, notifyLoop, &ni);

    backupInit(ni.sourceFd);

    /* perform the initial backup */
    fprintf(stderr, "Initial sync...\n");
    backupRecursive(&ni, ni.sourceFd, ni.destFd);

    /* and schedule full backups */
    pthread_create(&fth, NULL, periodicFull, &ni);

    /* then continuous backup */
    fprintf(stderr, "Entering continuous mode.\n");
    while (sem_wait(&ni.qsem) == 0) {
        NotifyQueue *ev;

        /* wait for 10 seconds of messages */
        sleep(10);
        fprintf(stderr, "Incremental backup.\n");
        do {
            pthread_mutex_lock(&ni.qlock);
            ev = ni.notifs;
            ni.notifs = ev->next;
            pthread_mutex_unlock(&ni.qlock);

            if (ev->file) {
                backupContaining(&ni, ev->file);
                free(ev->file);
            } else {
                fprintf(stderr, "Periodic full sync.\n");
                backupRecursive(&ni, ni.sourceFd, ni.destFd);
            }

            free(ev);
        } while (sem_trywait(&ni.qsem) == 0);
    }

    pthread_join(nth, NULL);

    return 0;
}

void reduceToSysAdmin()
{
    if (geteuid() == 0) {
        cap_t caps;
        cap_value_t setCaps[3];

        /* clear current caps */
        caps = cap_get_proc();
        if (caps == NULL) {
            perror("cap_get_proc");
            exit(1);
        }
        if (cap_clear(caps) != 0) {
            perror("cap_clear");
            exit(1);
        }

        /* reduce to SYS_ADMIN|SET*ID */
        setCaps[0] = CAP_SETUID;
        setCaps[1] = CAP_SETGID;
        setCaps[2] = CAP_SYS_ADMIN;
        cap_set_flag(caps, CAP_EFFECTIVE, 3, setCaps, CAP_SET);
        cap_set_flag(caps, CAP_PERMITTED, 3, setCaps, CAP_SET);
        if (cap_set_proc(caps) != 0) {
            perror("cap_set_proc");
            exit(1);
        }

        /* make sure we don't lose it */
        if (prctl(PR_SET_KEEPCAPS, 1) < 0) {
            perror("PR_SET_KEEPCAPS");
            exit(1);
        }

        /* are we setuid? */
        if (getuid() != 0) {
            setgid(getgid());
            setuid(getuid());

        } else {
            /* are we sudo? */
            char *sudoUid, *sudoGid;
            sudoUid = getenv("SUDO_UID");
            sudoGid = getenv("SUDO_GID");

            if (sudoUid && sudoGid) {
                int uid, gid;
                uid = atoi(sudoUid);
                gid = atoi(sudoGid);
                setgid(gid);
                setuid(uid);
            }

        }

        if (geteuid() == 0) {
            fprintf(stderr, "DO NOT RUN AS ROOT!\n");
            exit(1);
        }

        /* and get rid of the setuid cap */
        cap_set_flag(caps, CAP_EFFECTIVE, 2, setCaps, CAP_CLEAR);
        cap_set_flag(caps, CAP_PERMITTED, 2, setCaps, CAP_CLEAR);
        if (cap_set_proc(caps) != 0) {
            perror("cap_set_proc");
            exit(1);
        }

        cap_free(caps);
    }
}

/* get rid of our final capability */
void reduceToUser()
{
    cap_t caps;

    /* clear current caps */
    caps = cap_get_proc();
    if (caps == NULL) {
        perror("cap_get_proc");
        exit(1);
    }
    if (cap_clear(caps) != 0) {
        perror("cap_clear");
        exit(1);
    }

    /* reduce to SYS_ADMIN */
    if (cap_set_proc(caps) != 0) {
        perror("cap_set_proc");
        exit(1);
    }

    cap_free(caps);
}

/* method to trigger a full update occasionally */
void *periodicFull(void *nivp)
{
    NiBackup *ni = (NiBackup *) nivp;

    while (1) {
        NotifyQueue *ev;

        sleep(21600);

        ev = malloc(sizeof(NotifyQueue));
        if (ev == NULL) continue;
        ev->next = NULL;
        ev->file = NULL;

        pthread_mutex_lock(&ni->qlock);
        if (ni->notifs == NULL) {
            ni->notifs = ni->lastNotif = ev;
        } else {
            ni->lastNotif->next = ev;
            ni->lastNotif = ev;
        }

        pthread_mutex_unlock(&ni->qlock);
        sem_post(&ni->qsem);
    }
}
