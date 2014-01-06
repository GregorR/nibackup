/*
 * nils.c: Utility to list backups
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "arg.h"
#include "buffer.h"
#include "metadata.h"

#define REP(into, func, bad, err, args) do { \
    (into) = func args; \
    if ((into) == (bad)) { \
        perror(err); \
    } \
} while (0)

struct NiLsOpt_ {
    int history;    /* -H */

    long long newest; /* -t */

    int dir;        /* -d */
    int llong;      /* -l */
    int recursive;  /* -R */
};
typedef struct NiLsOpt_ NiLsOpt;

BUFFER(charp, char *);

static size_t direntLen;

/* usage statement */
static void usage(void);

/* select a given file or directory in the backup */
static void lsSelected(NiLsOpt *opt, int sourceDir, char *selection);

/* list a directory's contents */
static void lsDir(NiLsOpt *opt, int sourceDir, struct Buffer_char *fullName);

/* list a single file or directory */
static int ls(NiLsOpt *opt, int sourceDir, char *name, size_t longestName);

/* print this metadata */
static void lsMeta(BackupMetadata *meta);

/* strcmp for qsort */
static int strppcmp(const void *l, const void *r);

int main(int argc, char **argv)
{
    ARG_VARS;
    NiLsOpt opt;
    const char *backupDir = NULL;
    char *selection = NULL;
    long long maxAge, newest;
    int setAge = 0, setTime = 0;
    int sourceFd;
    long name_max;

    memset(&opt, 0, sizeof(opt));

    ARG_NEXT();
    while (argType) {
        if (argType != ARG_VAL) {
            ARGV(H, history, opt.history)
            ARGV(d, directory, opt.dir)
            ARGV(l, long, opt.llong)
            ARGV(R, recursive, opt.recursive)
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
                newest = atoll(arg);
                setTime = 1;
                if (newest == 0 && strcmp(arg, "0")) {
                    fprintf(stderr, "Invalid restoration time\n");
                    return 1;
                }

            } else {
                usage();
                return 1;

            }

        } else {
            if (!backupDir) {
                backupDir = arg;

            } else if (!selection) {
                selection = arg;

            } else {
                usage();
                return 1;

            }

        }

        ARG_NEXT();
    }

    if (!backupDir || (setAge && setTime)) {
        usage();
        return 1;
    }

    if (!setTime) {
        if (!setAge) maxAge = 0;
        newest = time(NULL) - maxAge;
    }
    opt.newest = newest;

    /* open the backup directory... */
    SFE(sourceFd, open, -1, backupDir, (backupDir, O_RDONLY));

    /* find our dirent size... */
    name_max = fpathconf(sourceFd, _PC_NAME_MAX);
    if (name_max == -1)
        name_max = 255;
    direntLen = sizeof(struct dirent) + name_max + 1;

    /* select */
    if (selection) {
        lsSelected(&opt, sourceFd, selection);
    } else {
        struct Buffer_char fullName;

        /* or list everything */
        INIT_BUFFER(fullName);
        lsDir(&opt, sourceFd, &fullName);
        FREE_BUFFER(fullName);
    }

    return 0;
}

/* usage statement */
static void usage()
{
    fprintf(stderr, "Use: nibackup-ls [options] <backup> [selection]\n"
                    "Options\n"
                    "  -a|--age <time>:\n"
                    "      List files as they existed <time> seconds ago.\n"
                    "  -t|--time <time>:\n"
                    "      List files as they existed at time <time>. Incompatible with -a.\n"
                    "  -H|--history:\n"
                    "      Show modification history for listed files.\n"
                    "  -d|--directory:\n"
                    "      List [selection]'s directory entry, not content.\n"
                    "  -l|--long:\n"
                    "      List in long format.\n"
                    "  -R|--recursive:\n"
                    "      List subdirectories recursively.\n");
}

/* select a given file or directory in the backup */
static void lsSelected(NiLsOpt *opt, int sourceDir, char *selection)
{
    char *part, *nextPart, *saveptr;
    int newSourceDir;
    struct Buffer_char fullName;

    INIT_BUFFER(fullName);
    WRITE_BUFFER(fullName, selection, strlen(selection));

    part = strtok_r(selection, "/", &saveptr);
    while ((nextPart = strtok_r(NULL, "/", &saveptr))) {
        char *dir;
        selection = NULL;

        /* descend into this directory */
        SF(dir, malloc, NULL, (strlen(part) + 4));
        sprintf(dir, "nid%s", part);
        SFE(newSourceDir, openat, -1, part, (sourceDir, dir, O_RDONLY));
        free(dir);
        close(sourceDir);
        sourceDir = newSourceDir;

        part = nextPart;
    }

    /* now "part" is the last part, so list that */
    if (ls(opt, sourceDir, part, strlen(part)) && !opt->dir) {
        char *dir;

        /* list the content as well */
        WRITE_ONE_BUFFER(fullName, 0); fullName.bufused--;
        printf("\n\n%s:\n", fullName.buf);
        SF(dir, malloc, NULL, (strlen(part) + 4));
        sprintf(dir, "nid%s", part);
        SFE(newSourceDir, openat, -1, part, (sourceDir, dir, O_RDONLY));
        free(dir);
        close(sourceDir);
        sourceDir = newSourceDir;

        lsDir(opt, sourceDir, &fullName);
    }

    FREE_BUFFER(fullName);
}

/* list a directory's content */
static void lsDir(NiLsOpt *opt, int sourceDir, struct Buffer_char *fullName)
{
    int hdirfd;
    DIR *dh;
    struct dirent *de, *der;
    size_t fnl, fnls, i, longestName;

    struct Buffer_charp names, dirs;

    fnl = fullName->bufused;
    if (fnl > 0) WRITE_ONE_BUFFER(*fullName, '/');
    fnls = fullName->bufused;

    SF(de, malloc, NULL, (direntLen));
    SF(hdirfd, dup, -1, (sourceDir));
    SF(dh, fdopendir, NULL, (hdirfd));

    /* first just get out the list of files */
    INIT_BUFFER(names);
    longestName = 0;
    while (1) {
        char *dname;
        size_t nameLen;

        if (readdir_r(dh, de, &der) != 0) break;
        if (der == NULL) break;
        if (strncmp(de->d_name, "nii", 3)) continue;
        SF(dname, strdup, NULL, (de->d_name + 3));
        WRITE_ONE_BUFFER(names, dname);

        nameLen = strlen(dname);
        if (nameLen > longestName) longestName = nameLen;
    }

    closedir(dh);

    /* sort them */
    qsort(names.buf, names.bufused, sizeof(char *), strppcmp);

    /* list them */
    if (opt->recursive) INIT_BUFFER(dirs);
    for (i = 0; i < names.bufused; i++) {
        if (ls(opt, sourceDir, names.buf[i], longestName) && opt->recursive) {
            WRITE_ONE_BUFFER(dirs, names.buf[i]);
        } else {
            free(names.buf[i]);
        }
    }
    FREE_BUFFER(names);

    /* then recurse */
    if (opt->recursive) {
        for (i = 0; i < dirs.bufused; i++) {
            char *dir;
            int newSourceDir;

            /* say where we're recursing */
            fullName->bufused = fnls;
            WRITE_BUFFER(*fullName, dirs.buf[i], strlen(dirs.buf[i])+1);
            fullName->bufused--;

            printf("\n\n%s:\n", fullName->buf);

            /* then list it */
            SF(dir, malloc, NULL, (strlen(dirs.buf[i]) + 4));
            sprintf(dir, "nid%s", dirs.buf[i]);
            SFE(newSourceDir, openat, -1, dirs.buf[i], (sourceDir, dir, O_RDONLY));
            free(dir);
            lsDir(opt, newSourceDir, fullName);
            close(newSourceDir);

            free(dirs.buf[i]);
        }
        FREE_BUFFER(dirs);
    }

    free(de);

    fullName->bufused = fnl;
}

/* List a single file or directory. Returns 1 for directories. */
static int ls(NiLsOpt *opt, int sourceDir, char *name, size_t longestName)
{
    char *pseudo, *pseudoD;
    int ifd, tmpi;
    char incrBuf[4*sizeof(unsigned long long)+1];
    unsigned long long curIncr, oldIncr;
    BackupMetadata meta;
    struct stat sbuf;

    meta.type = MD_TYPE_NONEXIST;

    /* make room for our pseudos: ni?<name>/<ull>.{old,new} */
    SF(pseudo, malloc, NULL, (strlen(name) + (4*sizeof(unsigned long long)) + 9));
    pseudoD = pseudo + strlen(name) + 3;
    sprintf(pseudo, "nii%s", name);

    /* open and lock the increment file */
    SFE(ifd, openat, -1, name, (sourceDir, pseudo, O_RDONLY));
    SFE(tmpi, flock, -1, pseudo, (ifd, LOCK_SH));

    /* read in the current increment */
    incrBuf[read(ifd, incrBuf, sizeof(incrBuf))] = 0;
    curIncr = atoll(incrBuf);
    if (curIncr == 0) goto done;

    /* now find the acceptable increment */
    pseudo[2] = 'm';
    for (oldIncr = curIncr; oldIncr > 0; oldIncr--) {
        sprintf(pseudoD, "/%llu.met", oldIncr);
        if (fstatat(sourceDir, pseudo, &sbuf, 0) == 0) {
            if (sbuf.st_mtime <= opt->newest)
                break;
        }
    }

    /* maybe skip it */
    if (oldIncr == 0) goto done;

    /* load in the metadata */
    SFE(tmpi, readMetadata, -1, pseudo, (&meta, sourceDir, pseudo, 0));

    /* skip it if it doesn't exist */
    if (meta.type == MD_TYPE_NONEXIST && !opt->history) goto done;

    /* list out the metadata, possibly in long format */
    printf("%-*s ", (int) longestName, name);
    if (opt->history)
        printf("%11llu %5llu ", (unsigned long long) sbuf.st_mtime, oldIncr);
    if (opt->llong)
        lsMeta(&meta);
    putchar('\n');

    if (opt->history) {
        /* list full history as well */
        unsigned long long ii;
        for (ii = curIncr; ii > 0; ii--) {
            if (ii == oldIncr) continue;

            sprintf(pseudoD, "/%llu.met", ii);

            if (fstatat(sourceDir, pseudo, &sbuf, 0) == 0 &&
                readMetadata(&meta, sourceDir, pseudo, 0) == 0) {
                printf("%*llu %5llu ", (int) longestName + 12, (unsigned long long) sbuf.st_mtime, ii);
                if (opt->llong)
                    lsMeta(&meta);
                putchar('\n');
            }
        }
        putchar('\n');
    }

done:
    close(ifd);
    free(pseudo);

    return (meta.type == MD_TYPE_DIRECTORY);
}

/* print out metadata */
static void lsMeta(BackupMetadata *meta) {
    char mbuf[4];
    int i, mode, sticky;
    mbuf[3] = 0;
    /* format: <type><mode> <uid>:<gid> <size> <mtime> */

    /* <type> */
    putchar((meta->type == MD_TYPE_FILE) ? '-' : meta->type);

    /* <mode> */
    mode = meta->mode;
    sticky = mode >> 9;
    for (i = 0; i < 3; i++) {
        mbuf[0] = mbuf[1] = mbuf[2] = '-';
        if (mode & 0400) mbuf[0] = 'r';
        if (mode & 0200) mbuf[1] = 'w';
        if (mode & 0100) {
            if (sticky & 4) {
                mbuf[2] = 's';
            } else {
                mbuf[2] = 'x';
            }
        } else if (sticky & 4) {
            mbuf[2] = 'S';
        }
        fputs(mbuf, stdout);

        mode <<= 3;
        sticky <<= 1;
    }

    /* and the rest */
    printf(" %5d:%-5d %12lld %lld", meta->uid, meta->gid, meta->size, meta->mtime);
}


/* strcmp for qsort */
static int strppcmp(const void *l, const void *r)
{
    return strcmp(*((char **) l), *((char **) r));
}
