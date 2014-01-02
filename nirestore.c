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

#define SF(into, func, bad, err, args) do { \
    (into) = func args; \
    if ((into) == (bad)) { \
        perror(err); \
        exit(1); \
    } \
} while (0)

static size_t direntLen;

/* select a given file or directory in the backup */
static void restoreSelected(long long newest, int sourceDir, int targetDir, char *selection);

/* restore a directory's contents */
static void restoreDir(long long newest, int sourceDir, int targetDir);

/* restore a single file or directory */
static void restore(long long newest, int sourceDir, int targetDir, char *name);

int main(int argc, char **argv)
{
    const char *backupDir, *targetDir, *selection;
    long long maxAge, newest;
    int sourceFd, targetFd;
    long name_max;

    if (argc < 4) {
        fprintf(stderr, "Use: nibackup-restore <backup directory> <target directory> <age> [selection]\n");
        return 1;
    }

    backupDir = argv[1];
    targetDir = argv[2];

    maxAge = atoll(argv[3]);
    if (maxAge <= 0 && strcmp(argv[3], "0")) {
        fprintf(stderr, "Invalid age!\n");
        return 1;
    }
    newest = time(NULL) - maxAge;

    selection = NULL;
    if (argc > 4)
        selection = argv[4];

    /* open the backup directory... */
    SF(sourceFd, open, -1, backupDir, (backupDir, O_RDONLY));

    /* and the target directory... */
    SF(targetFd, open, -1, targetDir, (targetDir, O_RDONLY));

    /* find our dirent size... */
    name_max = fpathconf(fd, _PC_NAME_MAX);
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

/* select a given file or directory in the backup */
static void restoreSelected(long long newest, int sourceDir, int targetDir, char *selection)
{
    char *part, *nextPart, *saveptr;
    int newSourceDir;

    part = strtok_r(selection, &saveptr);
    while ((nextPart = strtok_r(NULL, &saveptr))) {
        char *dir;
        selection = NULL;

        /* descend into this directory */
        SF(dir, malloc, NULL, "malloc", (strlen(part) + 4));
        sprintf(dir, "nid%s");
        SF(newSourceDir, openat, -1, dir, (sourceDir, dir, O_RDONLY));
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
    int sfd;

    SF(de, malloc, NULL, "malloc", (direntLen));
    SF(hdirfd, dup, -1, "dup", (dirfd));
    SF(dh, fdopendir, NULL, "fdopendir", (hdirfd));

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

    free(de);
}

/* restore a single file or directory */
static void restore(long long newest, int sourceDir, int targetDir, char *name);

/* purge this backup */
void purge(long long oldest, int dirfd, char *name)
{
    char *pseudo, *pseudoD;
    int i, ifd, tmpi;
    char incrBuf[4*sizeof(unsigned long long)+1];
    unsigned long long curIncr, oldIncr, ii;

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

    /* now find the first dead increment */
    for (oldIncr = curIncr - 1; oldIncr > 0; oldIncr--) {
        struct stat sbuf;
        pseudo[2] = 'm';
        sprintf(pseudoD, "/%llu.old", oldIncr);
        if (fstatat(dirfd, pseudo, &sbuf, 0) == 0) {
            if (sbuf.st_mtime <= oldest) {
                fprintf(stderr, "%s %d\n", name, (int) oldIncr);
                /* this is old enough to purge */
                break;
            }
        }
    }

    /* delete all the old increments */
    for (ii = oldIncr; ii > 0; ii--) {
        /* metadata */
        pseudo[2] = 'm';
        sprintf(pseudoD, "/%llu.old", ii);
        unlinkat(dirfd, pseudo, 0);

        /* and content */
        pseudo[2] = 'c';
        sprintf(pseudoD, "/%llu.old", ii);
        unlinkat(dirfd, pseudo, 0);
        sprintf(pseudoD, "/%llu.bsp", ii);
        unlinkat(dirfd, pseudo, 0);
    }

    /* now we may have gotten rid of the file entirely */
    *pseudoD = 0;
    for (i = 0; pseudos[i]; i++) {
        pseudo[2] = pseudos[i];
        if (unlinkat(dirfd, pseudo, AT_REMOVEDIR) != 0) break;
    }
    if (!pseudos[i]) {
        /* completely removed this file, so remove the increment file as well */
        pseudo[2] = 'i';
        unlinkat(dirfd, pseudo, 0);
    }

done:
    close(ifd);
    free(pseudo);
}
