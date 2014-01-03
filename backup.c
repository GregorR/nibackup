/*
 * backup.c: Functions to perform incremental backup.
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

#define _XOPEN_SOURCE 700 /* for fdopendir */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "backup.h"
#include "metadata.h"
#include "nibackup.h"

/* utility function to call bsdiff, returning 0 if it succeeds */
static int bsdiff(const char *from, const char *to, const char *patch);

static size_t direntLen;

/* initialization for the backup procedures */
void backupInit(int source)
{
    long name_max;
    name_max = fpathconf(source, _PC_NAME_MAX);
    if (name_max == -1)
        name_max = 255;
    direntLen = sizeof(struct dirent) + name_max + 1;
}

/* recursively back up this path */
void backupRecursive(NiBackup *ni, int source, int dest)
{
    DIR *dh;
    struct dirent *de, *der;
    struct stat sbuf, tbuf;
    int sFd, dFd;
    int hSource, hDest;

    hSource = dup(source);
    if (hSource < 0) {
        perror("dup");
        return;
    }
    hDest = dup(dest);
    if (hDest < 0) {
        perror("dup");
        return;
    }

    /* stat the source (for st_dev)
     * FIXME: cache */
    if (fstat(source, &sbuf) != 0) {
        perror("fstat");
        return;
    }

    de = malloc(direntLen);
    if (de == NULL) return;

    /* go over source-dir files */
    if ((dh = fdopendir(hSource))) {
        /* for each file... */
        while (1) {
            if (readdir_r(dh, de, &der) != 0) break;
            if (der == NULL) break;

            /* skip . and .. */
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;

            /* otherwise, back it up */
            dFd = backupPath(ni, de->d_name, source, dest);

            /* and children */
            if (dFd >= 0) {
                sFd = openat(source, de->d_name, O_RDONLY);
                if (sFd >= 0) {
                    if (fstat(sFd, &tbuf) == 0 &&
                        sbuf.st_dev == tbuf.st_dev) {

                        backupRecursive(ni, sFd, dFd);
                    }
                    close(sFd);
                }
                close(dFd);
            }
        }

        closedir(dh);
    } else {
        close(hSource);
    }

    /* then go over dest-dir files, in case something was deleted */
    if ((dh = fdopendir(hDest))) {
        while (1) {
            if (readdir_r(dh, de, &der) != 0) break;
            if (der == NULL) break;

            /* only look at increment files (nii) */
            if (strncmp(de->d_name, "nii", 3)) continue;

            /* check if it's been deleted */
            if (faccessat(source, de->d_name + 3, F_OK, AT_SYMLINK_NOFOLLOW) != 0) {
                /* back it up */
                backupPath(ni, de->d_name + 3, source, dest);
            }
        }

        closedir(dh);
    } else {
        close(hDest);
    }

    free(de);
}

/* back up this path and all containing directories */
void backupContaining(NiBackup *ni, char *path)
{
    int source = -1, dest = -1,
        newSource = -1, newDest = -1;
    char *part, *saveptr;

    /* first off, remove the source */
    if (strncmp(path, ni->source, ni->sourceLen)) goto done;
    path += ni->sourceLen;
    if (path[0] != '/') goto done;
    path++;

    /* now start from here and back up */
    source = dup(ni->sourceFd);
    if (source < 0) goto done;
    dest = dup(ni->destFd);
    if (dest < 0) goto done;

    while ((part = strtok_r(path, "/", &saveptr))) {
        path = NULL;

        /* back it up */
        newDest = backupPath(ni, part, source, dest);
        close(dest);
        dest = newDest;

        /* and follow this path in the source */
        if (dest >= 0) {
            newSource = openat(source, part, O_RDONLY);
            if (newSource < 0) {
                /* FIXME */
                goto done;
            }
            close(source);
            source = newSource;
        }
    }

done:
    if (source >= 0) close(source);
    if (dest >= 0) close(dest);
}

static const char pseudos[] = "cmd"; /* content, metadata, directory */

/* back up this path, returning an open fd to the backup directory if
 * applicable */
int backupPath(NiBackup *ni, char *name, int source, int destDir)
{
    char *pseudo = NULL, *pseudoD, *pseudo2, *pseudo2D;
    int i, ifd = -1, ffd = -1, rfd = -1, wroteData = 0;
    size_t namelen;
    unsigned long long lastIncr, curIncr;
    char incrBuf[4*sizeof(int)+1];
    ssize_t rd;
    BackupMetadata lastMeta, meta;

    /* space for our pseudofiles: ni?<name>/<ull>.{old,new} */
    namelen = strlen(name);
    pseudo = malloc(namelen + (4*sizeof(unsigned long long)) + 9);
    if (pseudo == NULL) /* FIXME */ goto done;
    pseudo2 = malloc(namelen + (4*sizeof(unsigned long long)) + 9);
    if (pseudo2 == NULL) /* FIXME */ goto done;
    pseudoD = pseudo + namelen + 3;
    pseudo2D = pseudo2 + namelen + 3;
    sprintf(pseudo, "ni?%s", name);
    sprintf(pseudo2, "ni?%s", name);

    /* get our increment file */
    pseudo[2] = 'i';
    ifd = openat(destDir, pseudo, O_RDWR | O_CREAT, 0600);
    if (ifd < 0) {
        perror(pseudo);
        goto done;
    }
    if (flock(ifd, LOCK_EX) != 0) {
        perror(pseudo);
        goto done;
    }

    /* make all the pseudo-dirs */
    for (i = 0; pseudos[i]; i++) {
        pseudo[2] = pseudos[i];
        if (mkdirat(destDir, pseudo, 0700) < 0) {
            if (errno != EEXIST) {
                perror(pseudo);
                goto done;
            }
        }
    }

    /* find our last increment */
    incrBuf[0] = 0;
    rd = read(ifd, incrBuf, sizeof(incrBuf) - 1);
    if (rd < 0) {
        goto done;
    }
    incrBuf[rd] = 0;
    lastIncr = atoll(incrBuf);
    if (lseek(ifd, 0, SEEK_SET) < 0) {
        goto done;
    }
    curIncr = lastIncr + 1;

    /* open the file and get its metadata */
    if (openMetadata(&meta, &ffd, source, name) != 0) {
        perror(name);
        goto done;
    }

    /* read in the old metadata */
    pseudo[2] = 'm';
    sprintf(pseudoD, "/%llu.new", lastIncr);
    if (readMetadata(&lastMeta, destDir, pseudo) != 0) {
        perror(name);
        goto done;
    }

    /* check if it's changed */
    if (cmpMetadata(&lastMeta, &meta) == 0) {
        /* no change, no increment */
        goto done;
    }

    /* write out the new metadata */
    pseudo[2] = 'm';
    sprintf(pseudoD, "/%llu.new", curIncr);
    if (writeMetadata(&meta, destDir, pseudo) != 0) {
        perror(name);
        goto done;
    }

    /* and the new data */
    pseudo[2] = 'c';
    sprintf(pseudoD, "/%llu.new", curIncr);
    if (meta.type == MD_TYPE_LINK) {
        /* just get the link target */
        char *linkTarget = malloc(meta.size);
        ssize_t rllen;
        int ofd;
        if (linkTarget == NULL) {
            perror("malloc");
            goto done;
        }

        rllen = readlinkat(source, name, linkTarget, meta.size);
        if (rllen <= 0) {
            perror(name);
            free(linkTarget);
            goto done;
        }

        /* and write it out */
        ofd = openat(destDir, pseudo, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (ofd < 0) {
            perror(name);
            free(linkTarget);
            goto done;
        }

        if (write(ofd, linkTarget, rllen) != rllen) {
            perror(name);
            close(ofd);
            free(linkTarget);
            goto done;
        }

        close(ofd);
        free(linkTarget);
        wroteData = 1;

    } else if (meta.type == MD_TYPE_FILE) {
        /* a regular file, this we can copy */
        if (copySparse(source, name, destDir, pseudo) != 0) {
            perror(name);
            goto done;
        }
        wroteData = 1;

    } else if (meta.type == MD_TYPE_DIRECTORY) {
        /* need to return the directory fd so the caller can deal with it */
        pseudo[2] = 'd';
        *pseudoD = 0;
        rfd = openat(destDir, pseudo, O_RDONLY);

    }

    /* we can now safely mark the increment file */
    if (lseek(ifd, 0, SEEK_SET) == 0) {
        sprintf(incrBuf, "%llu", curIncr);
        write(ifd, incrBuf, strlen(incrBuf));
    }

    /* rename the old metadata */
    pseudo[2] = 'm';
    pseudo2[2] = 'm';
    sprintf(pseudoD, "/%llu.new", lastIncr);
    sprintf(pseudo2D, "/%llu.old", lastIncr);
    renameat(destDir, pseudo, destDir, pseudo2);

    /* create the content patchfile */
    if (wroteData) {
        int lastIncrFd, curIncrFd, patchFd;
        char lastIncrBuf[15+4*sizeof(int)];
        char curIncrBuf[15+4*sizeof(int)];
        char patchBuf[15+4*sizeof(int)];

        /* the current increment */
        pseudo[2] = 'c';
        sprintf(pseudoD, "/%llu.new", curIncr);
        curIncrFd = openat(destDir, pseudo, O_RDONLY);

        if (curIncrFd >= 0) {
            sprintf(curIncrBuf, "/proc/self/fd/%d", curIncrFd);

            /* the last increment */
            sprintf(pseudoD, "/%llu.new", lastIncr);
            lastIncrFd = openat(destDir, pseudo, O_RDONLY);

            if (lastIncrFd >= 0) {
                sprintf(lastIncrBuf, "/proc/self/fd/%d", lastIncrFd);

                /* and the patch */
                sprintf(pseudoD, "/%llu.bsp", lastIncr);
                patchFd = openat(destDir, pseudo, O_RDWR | O_CREAT | O_TRUNC, 0600);

                if (patchFd >= 0) {
                    sprintf(patchBuf, "/proc/self/fd/%d", patchFd);

                    if (bsdiff(curIncrBuf, lastIncrBuf, patchBuf) == 0) {
                        /* remove the original */
                        sprintf(pseudoD, "/%llu.new", lastIncr);
                        unlinkat(destDir, pseudo, 0);
                    } else {
                        wroteData = 0; /* FIXME, bad name */
                    }

                    close(patchFd);
                }
                close(lastIncrFd);
            }
            close(curIncrFd);
        }
    }

    if (!wroteData) {
        /* if we failed to patch, just rename */
        pseudo[2] = 'c';
        sprintf(pseudoD, "/%llu.new", lastIncr);
        pseudo2[2] = 'c';
        sprintf(pseudo2D, "/%llu.old", lastIncr);
        renameat(destDir, pseudo, destDir, pseudo2);
    }

done:
    if (ffd >= 0) close(ffd);
    if (ifd >= 0) close(ifd);
    free(pseudo);
    free(pseudo2);

    return rfd;
}

/* utility function to call bsdiff, returning 0 if it succeeds */
static int bsdiff(const char *from, const char *to, const char *patch)
{
    int status;
    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        /* child, call bsdiff */
        execlp("bsdiff", "bsdiff", from, to, patch, NULL);
        perror("bsdiff");
        exit(1);
        abort();
    }

    /* wait for bsdiff */
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    if (WEXITSTATUS(status) != 0)
        return -1;
    return 0;
}
