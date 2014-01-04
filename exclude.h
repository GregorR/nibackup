#ifndef EXCLUDE_H
#define EXCLUDE_H

struct NiBackup_;

/* load an exclusion list */
int loadExclusions(struct NiBackup_ *ni, const char *from);

/* Check if this file is excluded. Returns 1 if excluded. */
int excluded(struct NiBackup_ *ni, const char *name);

#endif
