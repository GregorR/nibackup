#define _XOPEN_SOURCE 700 /* for fdopendir */
#define _GNU_SOURCE /* for SEEK_DATA|SEEK_HOLE */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "backup.h"
#include "nibackup.h"

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
#define TYPE_NONEXIST       'n'
#define TYPE_FILE           'f'
#define TYPE_DIRECTORY      'd'
#define TYPE_LINK           'l'
#define TYPE_FIFO           'p'
#define TYPE_OTHER          'x'

/* utility function to open a file and retrieve its metadata */
static int openMetadata(BackupMetadata *meta, int *fd, int dirfd, const char *name);

/* utility function to read serialized metadata */
static int readMetadata(BackupMetadata *meta, int dirfd, const char *name);

/* utility function to write serialized metadata */
static int writeMetadata(BackupMetadata *meta, int dirfd, const char *name);

/* Utility function to compare metadata. Returns 0 if equal, 1 otherwise. */
static int cmpMetadata(BackupMetadata *l, BackupMetadata *r);

/* utility function to copy a file sparsely */
static int copySparse(int sdirfd, const char *sname, int ddirfd, const char *dname);

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
                    backupRecursive(ni, sFd, dFd);
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

            /* only look at increment files (ni_i_) */
            if (strncmp(de->d_name, "ni_i_", 5)) continue;

            /* check if it's been deleted */
            if (faccessat(source, de->d_name + 5, F_OK, AT_SYMLINK_NOFOLLOW) != 0) {
                /* back it up */
                fprintf(stderr, "%s\n", de->d_name + 5);
                backupPath(ni, de->d_name + 5, source, dest);
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
    /* FIXME */
}

static const char pseudos[] = "cmd"; /* content, metadata, directory */

/* back up this path, returning an open fd to the backup directory if
 * applicable */
int backupPath(NiBackup *ni, char *name, int source, int destDir)
{
    char *pseudo = NULL, *pseudoD;
    int i, ifd = -1, ffd = -1, rfd = -1, wroteData = 0;
    size_t namelen;
    unsigned long long lastIncr, curIncr;
    char incrBuf[4*sizeof(int)];
    ssize_t rd;
    BackupMetadata lastMeta, meta;

    /* space for our pseudofiles: ni_?_<name>/<ull>.{old,new} */
    namelen = strlen(name);
    pseudo = malloc(namelen + (4*sizeof(unsigned long long)) + 11);
    if (pseudo == NULL) /* FIXME */ goto done;
    pseudoD = pseudo + namelen + 5;

    /* make all the pseudo-dirs */
    sprintf(pseudo, "ni_?_%s", name);
    for (i = 0; pseudos[i]; i++) {
        pseudo[3] = pseudos[i];
        if (mkdirat(destDir, pseudo, 0700) < 0) {
            if (errno != EEXIST) {
                perror(pseudo);
                goto done;
            }
        }
    }

    /* get our increment file */
    pseudo[3] = 'i';
    ifd = openat(destDir, pseudo, O_RDWR | O_CREAT, 0600);
    if (ifd < 0) {
        perror(pseudo);
        goto done;
    }
    if (flock(ifd, LOCK_EX) != 0) {
        perror(pseudo);
        goto done;
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
    pseudo[3] = 'm';
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
    pseudo[3] = 'm';
    sprintf(pseudoD, "/%llu.new", curIncr);
    if (writeMetadata(&meta, destDir, pseudo) != 0) {
        perror(name);
        goto done;
    }

    /* and the new data */
    pseudo[3] = 'c';
    sprintf(pseudoD, "/%llu.new", curIncr);
    if (meta.type == TYPE_LINK) {
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

    } else if (meta.type == TYPE_FILE) {
        /* a regular file, this we can copy */
        if (copySparse(source, name, destDir, pseudo) != 0) {
            perror(name);
            goto done;
        }
        wroteData = 1;

    } else if (meta.type == TYPE_DIRECTORY) {
        /* need to return the directory fd so the caller can deal with it */
        pseudo[3] = 'd';
        *pseudoD = 0;
        rfd = openat(destDir, pseudo, O_RDWR);

    }

    /* FIXME: make incremental */
    (void) wroteData;

    /* finally, write out the increment file */
    if (lseek(ifd, 0, SEEK_SET) == 0) {
        sprintf(incrBuf, "%llu", curIncr);
        write(ifd, incrBuf, strlen(incrBuf));
    }

done:
    if (ffd >= 0) close(ffd);
    if (ifd >= 0) close(ifd);
    free(pseudo);

    return rfd;
}

/* utility function to open a file and retrieve its metadata */
static int openMetadata(BackupMetadata *meta, int *fd, int dirfd, const char *name)
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
        meta->type = TYPE_NONEXIST;
        *fd = -1;
        return 0;
    }

    /* normal case */
    if (S_ISREG(lsbuf.st_mode)) {
        doOpen = 1;
        meta->type = TYPE_FILE;
    } else if (S_ISDIR(lsbuf.st_mode)) {
        doOpen = 1;
        meta->type = TYPE_DIRECTORY;
    } else if (S_ISLNK(lsbuf.st_mode)) {
        meta->type = TYPE_LINK;
    } else if (S_ISFIFO(lsbuf.st_mode)) {
        meta->type = TYPE_FIFO;
    } else {
        meta->type = TYPE_OTHER;
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
static int readMetadata(BackupMetadata *meta, int dirfd, const char *name)
{
    int fd;
    FILE *fh;

    fd = openat(dirfd, name, O_RDONLY);
    if (fd < 0) {
        if (errno == ENOENT) {
            /* nonexistent */
            memset(meta, 0, sizeof(BackupMetadata));
            meta->type = TYPE_NONEXIST;
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
static int writeMetadata(BackupMetadata *meta, int dirfd, const char *name)
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
static int cmpMetadata(BackupMetadata *l, BackupMetadata *r)
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
static int copySparse(int sdirfd, const char *sname, int ddirfd, const char *dname)
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
