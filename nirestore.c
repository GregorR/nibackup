/*
 * nirestore.c: Utility to restore backups
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
#include "metadata.h"

#define SF(into, func, bad, err, args) do { \
    (into) = func args; \
    if ((into) == (bad)) { \
        perror(err); \
        exit(1); \
    } \
} while (0)

#define REP(into, func, bad, err, args) do { \
    (into) = func args; \
    if ((into) == (bad)) { \
        perror(err); \
    } \
} while (0)

static size_t direntLen;

/* usage statement */
static void usage(void);

/* select a given file or directory in the backup */
static void restoreSelected(long long newest, int sourceDir, int targetDir, char *selection);

/* restore a directory's contents */
static void restoreDir(long long newest, int sourceDir, int targetDir);

/* restore a single file or directory */
static void restore(long long newest, int sourceDir, int targetDir, char *name);

/* restore the data from this backup */
static int restoreData(int sourceDir, int targetDir, char *name, unsigned long long restIncr, unsigned long long curIncr);

/* utility function to call bspatch, returning 0 if it succeeds */
static int bspatch(const char *from, const char *to, const char *patch);

int main(int argc, char **argv)
{
    const char *backupDir = NULL, *targetDir = NULL;
    char *selection = NULL;
    long long maxAge, newest;
    int setAge = 0, setTime = 0;
    int sourceFd, targetFd;
    long name_max;
    int argi;

    for (argi = 1; argi < argc; argi++) {
        char *arg = argv[argi];

        if (arg[0] == '-') {
            ARGN(a, age) {
                arg = argv[++argi];
                maxAge = atoll(arg);
                setAge = 1;
                if (maxAge <= 0 && strcmp(arg, "0")) {
                    fprintf(stderr, "Invalid age\n");
                    return 1;
                }

            } else ARGN(t, time) {
                arg = argv[++argi];
                newest = atoll(arg);
                setTime = 1;
                if (newest == 0 && strcmp(arg, "0")) {
                    fprintf(stderr, "Invalid restoration time\n");
                    return 1;
                }

            } else ARGN(i, selection) {
                selection = argv[++argi];

            } else {
                usage();
                return 1;

            }

        } else {
            if (!backupDir) {
                backupDir = arg;

            } else if (!targetDir) {
                targetDir = arg;

            } else {
                usage();
                return 1;

            }

        }
    }

    if (!backupDir || (setAge && setTime)) {
        usage();
        return 1;
    }

    if (!setTime) {
        if (!setAge) maxAge = 0;
        newest = time(NULL) - maxAge;
    }

    /* open the backup directory... */
    SF(sourceFd, open, -1, backupDir, (backupDir, O_RDONLY));

    /* and the target directory... */
    if (targetDir) {
        SF(targetFd, open, -1, targetDir, (targetDir, O_RDONLY));
    } else {
        targetFd = -1;
    }

    /* find our dirent size... */
    name_max = fpathconf(sourceFd, _PC_NAME_MAX);
    if (name_max == -1)
        name_max = 255;
    direntLen = sizeof(struct dirent) + name_max + 1;

    /* select */
    if (selection) {
        restoreSelected(newest, sourceFd, targetFd, selection);
    } else {
        /* or restore everything */
        restoreDir(newest, sourceFd, targetFd);
    }

    return 0;
}

/* usage statement */
static void usage()
{
    fprintf(stderr, "Use: nibackup-restore [options] <backup> [target]\n"
                    "    If target is unspecified, just lists files that would be restored.\n"
                    "Options\n"
                    "  -a|--age <time>:\n"
                    "      Restore files as they existed <time> seconds ago.\n"
                    "  -i|--selection <path>:\n"
                    "      Restore only <path>.\n");
}

/* select a given file or directory in the backup */
static void restoreSelected(long long newest, int sourceDir, int targetDir, char *selection)
{
    char *part, *nextPart, *saveptr;
    int newSourceDir;

    part = strtok_r(selection, "/", &saveptr);
    while ((nextPart = strtok_r(NULL, "/", &saveptr))) {
        char *dir;
        selection = NULL;

        /* descend into this directory */
        SF(dir, malloc, NULL, "malloc", (strlen(part) + 4));
        sprintf(dir, "nid%s", part);
        SF(newSourceDir, openat, -1, part, (sourceDir, dir, O_RDONLY));
        free(dir);
        close(sourceDir);
        sourceDir = newSourceDir;

        part = nextPart;
    }

    /* now "part" is the last part, so restore that */
    restore(newest, sourceDir, targetDir, part);
}

/* restore a directory's contents */
static void restoreDir(long long newest, int sourceDir, int targetDir)
{
    int hdirfd;
    DIR *dh;
    struct dirent *de, *der;

    SF(de, malloc, NULL, "malloc", (direntLen));
    SF(hdirfd, dup, -1, "dup", (sourceDir));
    SF(dh, fdopendir, NULL, "fdopendir", (hdirfd));

    if (targetDir == -1)
        printf("<\n");

    /* restore each file */
    while (1) {
        if (readdir_r(dh, de, &der) != 0) break;
        if (der == NULL) break;

        /* looking for increment files */
        if (strncmp(de->d_name, "nii", 3)) continue;

        /* restore this */
        restore(newest, sourceDir, targetDir, de->d_name + 3);
    }
    closedir(dh);

    if (targetDir == -1)
        printf(">\n");

    free(de);
}

/* restore a single file or directory */
static void restore(long long newest, int sourceDir, int targetDir, char *name)
{
    char *pseudo, *pseudoD;
    int ifd, tmpi, status;
    char incrBuf[4*sizeof(unsigned long long)+1];
    unsigned long long curIncr, oldIncr;
    BackupMetadata meta;

    /* make room for our pseudos: ni?<name>/<ull>.{old,new} */
    SF(pseudo, malloc, NULL, "malloc", (strlen(name) + (4*sizeof(unsigned long long)) + 9));
    pseudoD = pseudo + strlen(name) + 3;
    sprintf(pseudo, "nii%s", name);

    /* open and lock the increment file */
    SF(ifd, openat, -1, name, (sourceDir, pseudo, O_RDONLY));
    SF(tmpi, flock, -1, pseudo, (ifd, LOCK_EX));

    /* read in the current increment */
    incrBuf[read(ifd, incrBuf, sizeof(incrBuf))] = 0;
    curIncr = atoll(incrBuf);
    if (curIncr == 0) goto done;

    /* now find the acceptable increment */
    for (oldIncr = curIncr; oldIncr > 0; oldIncr--) {
        struct stat sbuf;
        pseudo[2] = 'm';
        sprintf(pseudoD, "/%llu.%s", oldIncr, (oldIncr == curIncr) ? "new" : "old");
        if (fstatat(sourceDir, pseudo, &sbuf, 0) == 0) {
            if (sbuf.st_mtime <= newest)
                break;
        }
    }

    /* maybe skip it */
    if (oldIncr == 0) goto done;

    /* load in the metadata */
    SF(tmpi, readMetadata, -1, pseudo, (&meta, sourceDir, pseudo));

    if (targetDir == -1)
        printf("%s\n", name);

    /* restore the data if applicable */
    status = 0;
    if (targetDir >= 0) {
        switch (meta.type) {
            case MD_TYPE_FILE:
            case MD_TYPE_LINK:
                status = restoreData(sourceDir, targetDir, name, oldIncr, curIncr);

                if (status == 0 && meta.type == MD_TYPE_LINK) {
                    /* convert the data into a link */
                    char *linkTarget;
                    int lfd;
                    ssize_t rd;

                    SF(linkTarget, malloc, NULL, "malloc", (meta.size + 1));
                    REP(lfd, openat, -1, name, (targetDir, name, O_RDONLY));
                    if (lfd != -1) {
                        REP(rd, read, -1, "read", (lfd, linkTarget, meta.size));
                        linkTarget[rd] = 0;
                        close(lfd);

                        REP(status, unlinkat, -1, name, (targetDir, name, 0));
                        if (status == 0) REP(status, symlinkat, -1, name, (linkTarget, targetDir, name));
                    }
                    free(linkTarget);
                }
                break;

            case MD_TYPE_DIRECTORY:
                REP(status, mkdirat, -1, name, (targetDir, name, 0700));
                break;

            case MD_TYPE_FIFO:
                REP(status, mkfifoat, -1, name, (targetDir, name, 0600));
                break;
        }
    }

    /* if this was a directory, restore its content */
    if (status == 0 && meta.type == MD_TYPE_DIRECTORY) {
        int newSourceDir, newTargetDir;
        pseudo[2] = 'd';
        *pseudoD = 0;
        REP(newSourceDir, openat, -1, pseudo, (sourceDir, pseudo, O_RDONLY));
        if (targetDir >= 0)
            REP(newTargetDir, openat, -1, name, (targetDir, name, O_RDONLY));
        else
            newTargetDir = -1;

        if (newSourceDir != -1 && (targetDir == -1 || newTargetDir != -1))
            restoreDir(newest, newSourceDir, newTargetDir);

        if (newSourceDir != -1) close(newSourceDir);
        if (newTargetDir != -1) close(newTargetDir);
    }

    /* now restore the file metadata */
    if (targetDir >= 0 && status == 0 && meta.type != MD_TYPE_NONEXIST) {
        struct timespec times[2];

        if (meta.type != MD_TYPE_LINK)
            REP(status, fchmodat, -1, name, (targetDir, name, meta.mode, 0));

        times[0].tv_sec = times[1].tv_sec = meta.mtime;
        times[0].tv_nsec = times[1].tv_nsec = 0;

        REP(status, utimensat, -1, name, (targetDir, name, times, AT_SYMLINK_NOFOLLOW));
        REP(status, fchownat, -1, name, (targetDir, name, meta.uid, meta.gid, AT_SYMLINK_NOFOLLOW));
    }


done:
    close(ifd);
    free(pseudo);
}

/* restore the data from this backup */
static int restoreData(int sourceDir, int targetDir, char *name, unsigned long long restIncr, unsigned long long curIncr)
{
    char *pseudo, *pseudoD;
    unsigned long long ii;
    int tmpi, ret = -1;

    SF(pseudo, malloc, NULL, "malloc", (strlen(name) + (4*sizeof(unsigned long long)) + 9));
    pseudoD = pseudo + strlen(name) + 3;
    sprintf(pseudo, "nic%s", name);

    /* find fully-defined content */
    for (ii = restIncr; ii <= curIncr; ii++) {
        sprintf(pseudoD, "/%llu.%s", ii, (ii == curIncr) ? "new" : "old");
        tmpi = faccessat(sourceDir, pseudo, R_OK, 0);
        if (tmpi == 0) break;
    }

    if (ii > curIncr) {
        /* didn't find full data! */
        fprintf(stderr, "Restore data for %s not found!\n", name);
        goto done;
    }

    /* copy in this version */
    if (copySparse(sourceDir, pseudo, targetDir, name) != 0) {
        perror(name);
        goto done;
    }

    /* then start patching */
    ret = 0;
    for (ii--; ii >= restIncr; ii--) {
        int fda, fdb, fdp;
        char aBuf[15+4*sizeof(int)];
        char bBuf[15+4*sizeof(int)];
        char pBuf[15+4*sizeof(int)];

        /* file a */
        fda = openat(targetDir, name, O_RDWR);
        if (fda >= 0) {
            sprintf(aBuf, "/proc/self/fd/%d", fda);

            /* file b */
            fdb = openat(targetDir, name, O_RDWR);

            if (fdb >= 0) {
                sprintf(bBuf, "/proc/self/fd/%d", fdb);

                /* and the patch */
                sprintf(pseudoD, "/%llu.bsp", ii);
                fdp = openat(sourceDir, pseudo, O_RDONLY);

                if (fdp >= 0) {
                    sprintf(pBuf, "/proc/self/fd/%d", fdp);

                    if (bspatch(aBuf, bBuf, pBuf) != 0) ret = -1;

                    close(fdp);
                } else {
                    perror(pseudo);
                    ret = -1;
                }
                close(fdb);
            } else {
                perror(name);
                ret = -1;
            }
            close(fda);
        } else {
            perror(name);
            ret = -1;
        }

    }

done:
    free(pseudo);
    return ret;
}

/* utility function to call bspatch, returning 0 if it succeeds */
static int bspatch(const char *from, const char *to, const char *patch)
{
    int status;
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* child, call bspatch */
        execlp("bspatch", "bspatch", from, to, patch, NULL);
        perror("bspatch");
        exit(1);
        abort();
    }

    /* wait for bspatch */
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}
