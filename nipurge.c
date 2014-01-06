/*
 * nipurge.c: Utility to purge old backup increments
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

#define _XOPEN_SOURCE 700

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "arg.h"
#include "metadata.h"

#define SF(into, func, bad, err, args) do { \
    (into) = func args; \
    if ((into) == (bad)) { \
        perror(err); \
        exit(1); \
    } \
} while (0)

static size_t direntLen;

/* FIXME: this should not be a global */
static int dryRun = 0;
static int verbose = 0;

/* usage statement */
static void usage(void);

/* purge this directory */
static void purgeDir(long long maxAge, int inDeadDir, int dirfd);

/* purge this backup */
static void purge(long long maxAge, int inDeadDir, int dirfd, char *name);

int main(int argc, char **argv)
{
    ARG_VARS;
    const char *backupDir = NULL;
    long long maxAge, oldest;
    int setAge = 0, setTime = 0;
    int fd;
    long name_max;

    ARG_NEXT();
    while (argType) {
        if (argType != ARG_VAL) {
            ARGV(n, dry-run, dryRun)
            ARGN(a, age) {
                ARG_GET();
                maxAge = atoll(arg);
                setAge = 1;
                if (maxAge <= 0 && strcmp(arg, "0")) {
                    fprintf(stderr, "Invalid age\n");
                    return 1;
                }

            } else ARGN(t, time) {
                ARG_GET();
                oldest = atoll(arg);
                setTime = 1;
                if (oldest == 0 && strcmp(arg, "0")) {
                    fprintf(stderr, "Invalid purge time\n");
                    return 1;
                }

            } else ARGN(v, verbose) {
                ARG_GET();
                verbose = atoi(arg);

            } else {
                usage();
                return 1;

            }

        } else {
            if (!backupDir) {
                backupDir = arg;

            } else {
                usage();
                return 1;

            }

        }

        ARG_NEXT();
    }

    if (!backupDir || setAge == setTime) {
        usage();
        return 1;
    }

    if (setAge)
        oldest = time(NULL) - maxAge;

    /* open the backup directory... */
    SF(fd, open, -1, backupDir, (backupDir, O_RDONLY));

    /* find our dirent size... */
    name_max = fpathconf(fd, _PC_NAME_MAX);
    if (name_max == -1)
        name_max = 255;
    direntLen = sizeof(struct dirent) + name_max + 1;

    /* and begin the purge */
    purgeDir(oldest, 0, fd);

    return 0;
}

/* usage statement */
void usage()
{
    fprintf(stderr, "Use: nibackup-purge [options] <-a age|-t time> <backup>\n"
                    "Options:\n"
                    "  -a|--age <time>:\n"
                    "      Purge overridden data older than <time> seconds.\n"
                    "  -t|--time <time>:\n"
                    "      Purge overridden data changed before time <time>.\n"
                    "  -n|--dry-run:\n"
                    "      Just say what would be purged, don't purge.\n"
                    "  -v|--verbose <verbosity>:\n"
                    "      Be more verbose.\n");
}

/* purge this directory */
void purgeDir(long long oldest, int inDeadDir, int dirfd)
{
    int hdirfd;
    DIR *dh;
    struct dirent *de, *der;

    SF(de, malloc, NULL, "malloc", (direntLen));
    SF(hdirfd, dup, -1, "dup", (dirfd));
    SF(dh, fdopendir, NULL, "fdopendir", (hdirfd));

    while (1) {
        if (readdir_r(dh, de, &der) != 0) break;
        if (der == NULL) break;

        /* looking for increment files */
        if (strncmp(de->d_name, "nii", 3)) continue;

        /* purge this */
        purge(oldest, inDeadDir, dirfd, de->d_name + 3);
    }

    closedir(dh);

    free(de);
}

static const char pseudos[] = "cmd";

/* purge this backup */
void purge(long long oldest, int inDeadDir, int dirfd, char *name)
{
    char *pseudo, *pseudoD;
    int i, ifd, dfd, tmpi;
    char incrBuf[4*sizeof(unsigned long long)+1];
    unsigned long long curIncr, oldIncr, ii;
    BackupMetadata curMeta;

    /* make room for our pseudos: ni?<name>/<ull>.{old,new} */
    SF(pseudo, malloc, NULL, "malloc", (strlen(name) + (4*sizeof(unsigned long long)) + 9));
    pseudoD = pseudo + strlen(name) + 3;
    sprintf(pseudo, "nii%s", name);

    /* open and lock the increment file */
    SF(ifd, openat, -1, pseudo, (dirfd, pseudo, O_RDONLY));
    SF(tmpi, flock, -1, pseudo, (ifd, LOCK_EX));

    /* read in the current increment */
    incrBuf[read(ifd, incrBuf, sizeof(incrBuf))] = 0;
    curIncr = atoll(incrBuf);
    if (curIncr == 0) goto done;

    /* and the current metadata */
    pseudo[2] = 'm';
    sprintf(pseudoD, "/%llu.met", curIncr);
    if (readMetadata(&curMeta, dirfd,  pseudo, 1) != 0) goto done;

    /* now find the first dead increment */
    for (oldIncr = curIncr - (inDeadDir ? 0 : 1); oldIncr > 0; oldIncr--) {
        struct stat sbuf;
        sprintf(pseudoD, "/%llu.met", oldIncr);
        if (fstatat(dirfd, pseudo, &sbuf, 0) == 0) {
            if (sbuf.st_mtime < oldest) {
                /* this is old enough to purge */
                break;
            }
        }
    }

    /* we can also remove any following deletions */
    for (ii = oldIncr + 1; ii <= curIncr; ii++) {
        BackupMetadata meta;
        pseudo[2] = 'm';
        sprintf(pseudoD, "/%llu.met", ii);
        if (readMetadata(&meta, dirfd, pseudo, 1) == 0) {
            if (meta.type == MD_TYPE_NONEXIST) oldIncr = ii;
            else break;
        } else break;
    }

    if (oldIncr == 0) goto done;

    /* maybe just say what we would have done */
    if (dryRun || verbose) {
        fprintf(stderr, "Purge %s <= %llu%s\n", name, oldIncr, (oldIncr == curIncr) ? " (all)" : "");
    }

    if (!dryRun) {
        /* delete all the old increments */
        for (ii = oldIncr; ii > 0; ii--) {
            /* metadata */
            pseudo[2] = 'm';
            sprintf(pseudoD, "/%llu.met", ii);
            unlinkat(dirfd, pseudo, 0);

            /* and content */
            pseudo[2] = 'c';
            sprintf(pseudoD, "/%llu.dat", ii);
            unlinkat(dirfd, pseudo, 0);
            sprintf(pseudoD, "/%llu.bsp", ii);
            unlinkat(dirfd, pseudo, 0);
            sprintf(pseudoD, "/%llu.x3p", ii);
            unlinkat(dirfd, pseudo, 0);
        }
    }

    /* recurse to subdirectories */
    pseudo[2] = 'd';
    *pseudoD = 0;
    dfd = openat(dirfd, pseudo, O_RDONLY);
    if (dfd >= 0) {
        purgeDir(oldest, inDeadDir || (curMeta.type != MD_TYPE_DIRECTORY), dfd);
        close(dfd);
    }

    /* now we may have gotten rid of the file entirely */
    if (!dryRun) {
        for (i = 0; pseudos[i]; i++) {
            pseudo[2] = pseudos[i];
            if (unlinkat(dirfd, pseudo, AT_REMOVEDIR) != 0) break;
        }
        if (!pseudos[i]) {
            /* completely removed this file, so remove the increment file as well */
            pseudo[2] = 'i';
            unlinkat(dirfd, pseudo, 0);
        }
    }

done:
    close(ifd);
    free(pseudo);
}
