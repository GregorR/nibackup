/*
 * metadata.c: Functions for handling NiBackup metadata files
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
#define _GNU_SOURCE /* for SEEK_DATA|SEEK_HOLE */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "metadata.h"

/* utility function to open a file and retrieve its metadata */
int openMetadata(BackupMetadata *meta, int *fd, int dirfd, const char *name)
{
    struct stat lsbuf, fsbuf;
    int doOpen = 0;

    if (fstatat(dirfd, name, &lsbuf, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno != ENOENT) {
            /* real error */
            return -1;
        }

        /* file doesn't exist */
        memset(meta, 0, sizeof(BackupMetadata));
        meta->type = MD_TYPE_NONEXIST;
        *fd = -1;
        return 0;
    }

    /* normal case */
    if (S_ISREG(lsbuf.st_mode)) {
        doOpen = 1;
        meta->type = MD_TYPE_FILE;
    } else if (S_ISDIR(lsbuf.st_mode)) {
        doOpen = 1;
        meta->type = MD_TYPE_DIRECTORY;
    } else if (S_ISLNK(lsbuf.st_mode)) {
        meta->type = MD_TYPE_LINK;
    } else if (S_ISFIFO(lsbuf.st_mode)) {
        meta->type = MD_TYPE_FIFO;
    } else {
        meta->type = MD_TYPE_OTHER;
    }

    /* mabye open it */
    if (!doOpen) {
        *fd = -1;
    } else {
        *fd = openat(dirfd, name, O_RDONLY);
        if (*fd < 0) return -1;

        /* make sure we opened the right thing */
        if (fstat(*fd, &fsbuf) != 0) {
            close(*fd);
            return -1;
        }
        if (lsbuf.st_mode != fsbuf.st_mode ||
            lsbuf.st_ino != fsbuf.st_ino ||
            lsbuf.st_dev != fsbuf.st_dev) {
            errno = EIO;
            close(*fd);
            return -1;
        }
    }

    /* and transfer the stat */
    meta->mode = lsbuf.st_mode;
    meta->uid = lsbuf.st_uid;
    meta->gid = lsbuf.st_gid;
    meta->size = lsbuf.st_size;
    meta->mtime = lsbuf.st_mtime;
    meta->ctime = lsbuf.st_ctime;

    return 0;
}

/* utility function to read serialized metadata */
int readMetadata(BackupMetadata *meta, int dirfd, const char *name)
{
    int fd;
    FILE *fh;

    fd = openat(dirfd, name, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            /* nonexistent */
            memset(meta, 0, sizeof(BackupMetadata));
            meta->type = MD_TYPE_NONEXIST;
            return 0;
        }
        return -1;
    }
    fh = fdopen(fd, "r");

    /* backup metadata:
    char type;
    int mode;
    int uid;
    int gid;
    long long size;
    long long mtime;
    long long ctime;
    */
    if (fscanf(fh, "%c\n%d\n%d\n%d\n%lld\n%lld\n%lld\n",
            &meta->type, &meta->mode, &meta->uid, &meta->gid,
            &meta->size, &meta->mtime, &meta->ctime) != 7) {
        errno = EIO;
        fclose(fh);
        return -1;
    }

    fclose(fh);
    return 0;
}

/* utility function to write serialized metadata */
int writeMetadata(BackupMetadata *meta, int dirfd, const char *name)
{
    int fd;
    FILE *fh;

    fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return -1;
    fh = fdopen(fd, "w");

    fprintf(fh, "%c\n%d\n%d\n%d\n%lld\n%lld\n%lld\n",
        meta->type, meta->mode, meta->uid, meta->gid,
        meta->size, meta->mtime, meta->ctime);

    fclose(fh);
    return 0;
}

/* Utility function to compare metadata. Returns 0 if equal, 1 otherwise. */
int cmpMetadata(BackupMetadata *l, BackupMetadata *r)
{
    if (l->type == r->type &&
        l->mode == r->mode &&
        l->uid == r->uid &&
        l->gid == r->gid &&
        l->size == r->size &&
        l->mtime == r->mtime &&
        l->ctime == r->ctime)
        return 0;

    return 1;
}

/* utility function to copy a file sparsely */
int copySparse(int sdirfd, const char *sname, int ddirfd, const char *dname)
{
    char *buf = NULL;
    size_t bufsz = 4096;
    ssize_t rd;
    off_t dataStart, dataEnd = 0, toRd;
    int ifd = -1, ofd = -1, ret = -1;

    buf = malloc(bufsz);
    if (buf == NULL) {
        perror("malloc");
        goto done;
    }

    ifd = openat(sdirfd, sname, O_RDONLY);
    if (ifd < 0) {
        perror(sname);
        goto done;
    }
    ofd = openat(ddirfd, dname, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (ofd < 0) {
        perror(sname);
        goto done;
    }

    /* read a sparse chunk at a time */
    while (1) {
        /* find the next data offset */
        dataStart = lseek(ifd, dataEnd, SEEK_DATA);
        if (dataStart < 0)
            break;
        dataEnd = lseek(ifd, dataStart, SEEK_HOLE);
        if (dataEnd < 0) {
            perror("lseek");
            goto done;
        }
        if (lseek(ifd, dataStart, SEEK_SET) < 0) {
            perror("lseek");
            goto done;
        }
        if (lseek(ofd, dataStart, SEEK_SET) < 0) {
            perror("lseek");
            goto done;
        }

        /* read the right amount of data in */
        toRd = dataEnd - dataStart;
        while ((rd = read(ifd, buf, (toRd > bufsz ? bufsz : toRd))) >= 0) {
            if (write(ofd, buf, rd) != rd) {
                perror("write");
                goto done;
            }
            toRd -= rd;
            if (toRd == 0) break;
        }
        if (rd < 0) {
            perror("read");
            goto done;
        }
    }

    ret = 0;

done:
    free(buf);
    if (ifd >= 0) close(ifd);
    if (ofd >= 0) close(ofd);
    return ret;
}

