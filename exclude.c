/*
 * exclude.c: Support for file exclusion
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

#include <errno.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "exclude.h"
#include "nibackup.h"

struct Exclusion_ {
    struct Exclusion_ *next;
    regex_t re;
};
typedef struct Exclusion_ Exclusion;

/* load an exclusion list */
int loadExclusions(NiBackup *ni, const char *from)
{
    char *buf = NULL;
    size_t bufsz, bufused;
    Exclusion *excl = NULL, *nexcl;
    int tmpi;
    int ret = -1;
    FILE *fh = fopen(from, "r");

    if (!fh) goto done;

    bufsz = 1024;
    buf = malloc(bufsz);
    if (!buf) goto done;
    buf[0] = '^';

    excl = NULL;

    /* read line-by-line */
    while (fgets(buf + 1, bufsz - 2, fh)) {
        /* if the line was too long, get more */
        bufused = strlen(buf);
        while (buf[bufused-1] != '\n') {
            bufsz *= 2;
            buf = realloc(buf, bufsz);
            if (!buf) goto done;
            if (!fgets(buf + bufused, bufsz - bufused - 1, fh)) break;
            bufused = strlen(buf);
        }

        /* terminate it */
        if (buf[bufused-1] == '\n') {
            buf[bufused-1] = '$';
        } else {
            buf[bufused] = '$';
            buf[bufused+1] = 0;
        }

        if (!strcmp(buf, "^$")) continue;

        /* prepare our excl */
        nexcl = malloc(sizeof(Exclusion));
        if (!nexcl) goto done;
        nexcl->next = excl;

        /* turn it into a regexp */
        if ((tmpi = regcomp(&nexcl->re, buf, REG_NOSUB))) {
            regerror(tmpi, &nexcl->re, buf, bufsz);
            fprintf(stderr, "Regex error: %s\n", buf);
            free(nexcl);
            errno = EIO;
            goto done;
        }

        excl = nexcl;
    }

    ni->exclusions = excl;
    excl = NULL;
    ret = 0;

done:
    if (fh) fclose(fh);
    free(buf);
    while (excl) {
        nexcl = excl->next;
        free(excl);
        excl = nexcl;
    }
    return ret;
}

/* check if this file is excluded */
int excluded(NiBackup *ni, const char *name)
{
    Exclusion *excl;
    if (ni->noRootDotfiles && name[0] == '.') return 1;
    excl = ni->exclusions;
    while (excl) {
        if (regexec(&excl->re, name, 0, NULL, 0) == 0) return 1;
        excl = excl->next;
    }
    return 0;
}
