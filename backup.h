#ifndef BACKUP_H
#define BACKUP_H

struct NiBackup_;

/* initialize backup structures */
void backupInit(int source);

/* recursively back up this path */
void backupRecursive(struct NiBackup_ *ni, int source, int destDir);

/* back up this path and all containing directories */
void backupContaining(struct NiBackup_ *ni, char *path);

#endif
