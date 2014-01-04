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

#ifndef METADATA_H
#define METADATA_H

/* backup metadata */
struct BackupMetadata_ {
    char type;
    int mode;
    int uid;
    int gid;
    long long size;
    long long mtime;
    long long ctime;
};
typedef struct BackupMetadata_ BackupMetadata;

/* backup types */
#define MD_TYPE_NONEXIST       'n'
#define MD_TYPE_FILE           'f'
#define MD_TYPE_DIRECTORY      'd'
#define MD_TYPE_LINK           'l'
#define MD_TYPE_FIFO           'p'
#define MD_TYPE_OTHER          'x'

/* open a file and retrieve its metadata */
int openMetadata(BackupMetadata *meta, int *fd, int dirfd, const char *name);

/* read serialized metadata */
int readMetadata(BackupMetadata *meta, int dirfd, const char *name);

/* write serialized metadata */
int writeMetadata(BackupMetadata *meta, int dirfd, const char *name);

/* Compare metadata. Returns 0 if equal, 1 otherwise. */
int cmpMetadata(BackupMetadata *l, BackupMetadata *r);

/* utility function to copy a file sparsely */
int copySparse(int ffd, int ddirfd, const char *dname);

#endif
