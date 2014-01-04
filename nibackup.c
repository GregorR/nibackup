/*
 * nibackup.c: Entry point for the main daemon
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

#define _XOPEN_SOURCE 700 /* for realpath */
#define _GNU_SOURCE /* for pthread_tryjoin_np */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/capability.h>
#include <sys/prctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "arg.h"
#include "backup.h"
#include "exclude.h"
#include "nibackup.h"
#include "notify.h"

/* usage statement */
static void usage(void);

/* privilege reduction utility functions */
static void reduceToSysAdmin(void);
static void reduceToUser(void);

/* background function for full backup */
static void *fullBackup(void *nivp);

/* method to trigger a full update occasionally */
static void *periodicFull(void *nivp);

int main(int argc, char **argv)
{
    NiBackup ni;
    pthread_t notifTh,
              cycleTh,
              fullTh;
    struct stat sbuf;
    int i, tmpi, argi;
    char *exclusionsFile = NULL;

    ni.source = NULL;
    ni.dest = NULL;
    ni.verbose = 0;
    ni.waitAfterNotif = 10;
    ni.fullSyncCycle = 21600;
    ni.noRootDotfiles = 0;
    ni.threads = 16;
    ni.notifFd = -1;

    for (argi = 1; argi < argc; argi++) {
        char *arg = argv[argi];

        if (arg[0] == '-') {
            ARGN(w, notification-wait) {
                arg = argv[++argi];
                ni.waitAfterNotif = atoi(arg);

            } else ARGN(F, full-sync-cycle) {
                arg = argv[++argi];
                ni.fullSyncCycle = atoi(arg);

            } else ARGN(x, exclude-from) {
                arg = argv[++argi];
                exclusionsFile = arg;

            } else ARG(., no-root-dotfiles) {
                ni.noRootDotfiles = 1;

            } else ARGN(j, threads) {
                arg = argv[++argi];
                ni.threads = atoi(arg);
                if (ni.threads <= 0) ni.threads = 1;

            } else ARG(v, verbose) {
                ni.verbose = 1;

            } else ARGN(@, notification-fd) {
                arg = argv[++argi];
                ni.notifFd = atoi(arg);

            } else {
                usage();
                return 1;
            }

        } else {
            if (!ni.source) {
                ni.source = arg;

            } else if (!ni.dest) {
                ni.dest = arg;

            } else {
                usage();
                return 1;

            }
        }
    }

    if (!ni.source || !ni.dest) {
        usage();
        return 1;
    }

    /* reduce our privileges */
    reduceToSysAdmin();

    /* source and destination must be real paths */
    ni.source = realpath(ni.source, NULL);
    if (ni.source == NULL) {
        perror(argv[1]);
        return 1;
    }
    ni.dest = realpath(ni.dest, NULL);
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
            char **nargv;
            char buf[128];
            nargv = malloc((argc + 3) * sizeof(char *));
            if (!nargv) {
                perror("malloc");
                return 1;
            }
            snprintf(buf, 128, "%d", ni.notifFd);
            for (argi = 0; argi < argc; argi++) nargv[argi] = argv[argi];
            nargv[argi++] = "--notification-fd";
            nargv[argi++] = buf;
            nargv[argi++] = NULL;
            execv("/proc/self/exe", nargv);
            perror("execv");
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

    /* load our exclusions */
    if (exclusionsFile) {
        loadExclusions(&ni, exclusionsFile);
    } else {
        ni.exclusions = NULL;
    }

    /* and the notify thread */
    pthread_create(&notifTh, NULL, notifyLoop, &ni);

    backupInit(ni.sourceFd);

    /* perform the initial backup */
    fprintf(stderr, "Starting initial sync.\n");
    pthread_create(&fullTh, NULL, fullBackup, &ni);

    /* and schedule full backups */
    pthread_create(&cycleTh, NULL, periodicFull, &ni);

    /* make the threads for continuous backup */
    if (ni.threads > 1) {
        if (sem_init(&ni.bsem, 0, 0) < 0) {
            perror("sem_init");
            return 1;
        }
        for (i = 0; i < ni.threads; i++) {
            sem_post(&ni.bsem);
        }
        ni.blocks = malloc(ni.threads * sizeof(pthread_mutex_t));
        if (ni.blocks == NULL) {
            perror("malloc");
            return 1;
        }
        ni.bth = malloc(ni.threads * sizeof(pthread_t));
        if (ni.bth == NULL) {
            perror("malloc");
            return 1;
        }
        ni.brunning = malloc(ni.threads * sizeof(int));
        if (ni.brunning == NULL) {
            perror("malloc");
            return 1;
        }
        for (i = 0; i < ni.threads; i++) {
            if (pthread_mutex_init(&ni.blocks[i], NULL) < 0) {
                perror("pthraed_mutex_init");
                return 1;
            }
            ni.brunning[i] = 0;
        }
    }

    /* then continuous backup */
    fprintf(stderr, "Entering continuous mode.\n");
    while (sem_wait(&ni.qsem) == 0) {
        NotifyQueue *ev, *evn;
        time_t iStart, iEnd;

        /* wait for 10 seconds of messages */
        sleep(ni.waitAfterNotif);
        if (ni.verbose) fprintf(stderr, "Incremental backup.\n");

        /* pull off current messages */
        pthread_mutex_lock(&ni.qlock);
        ev = ni.notifs;
        ni.notifs = ni.lastNotif = NULL;
        pthread_mutex_unlock(&ni.qlock);

        /* then back them up */
        if (ni.verbose) iStart = time(NULL);
        while (ev) {
            if (ev->file) {
                if (ni.verbose) fprintf(stderr, "%s\n", ev->file);
                backupContaining(&ni, ev->file);
                free(ev->file);
            } else {
                if (pthread_tryjoin_np(fullTh, NULL) == 0) {
                    if (ni.verbose) fprintf(stderr, "Starting full sync.\n");
                    pthread_create(&fullTh, NULL, fullBackup, &ni);
                }
            }

            evn = ev->next;
            free(ev);
            ev = evn;
        }

        if (ni.verbose) {
            iEnd = time(NULL);
            fprintf(stderr, "Finished incremental backup in %d seconds.\n",
                (int) (iEnd - iStart));
        }
    }

    pthread_join(notifTh, NULL);
    pthread_join(cycleTh, NULL);
    pthread_join(fullTh, NULL);

    return 0;
}

/* usage statement */
static void usage()
{
    fprintf(stderr, "Use: nibackup [options] <source> <target>\n"
                    "Options:\n"
                    "  -w|--notification-wait <time>:\n"
                    "      Wait <time> seconds after notifications arrive before syncing.\n"
                    "  -F|--full-sync-cycle <time>:\n"
                    "      Perform a full sync every <time> seconds.\n"
                    "  -x|--exclude-from <file>:\n"
                    "      Load exclusions (fully-anchored regexes) from <file>.\n"
                    "  -.|--no-root-dotfiles:\n"
                    "      Do not back up dotfiles in the root of <source> (useful for homedirs).\n"
                    "  -j|--threads <threads>:\n"
                    "      Use <threads> threads for backup.\n"
                    "  -v|--verbose:\n"
                    "      Be more verbose.\n");
}

static void reduceToSysAdmin()
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
static void reduceToUser()
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


/* background function for full backup */
static void *fullBackup(void *nivp)
{
    time_t fStart, fEnd;
    NiBackup *ni = (NiBackup *) nivp;
    if (ni->verbose) fStart = time(NULL);
    backupRecursive(ni);
    if (ni->verbose) {
        fEnd = time(NULL);
        fprintf(stderr, "Finished full sync in %d seconds.\n", (int) (fEnd - fStart));
    }

    return NULL;
}

/* method to trigger a full update occasionally */
static void *periodicFull(void *nivp)
{
    NiBackup *ni = (NiBackup *) nivp;

    while (1) {
        NotifyQueue *ev;

        sleep(ni->fullSyncCycle);

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

    return NULL;
}
