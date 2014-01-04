#ifndef BACKUP_H
#define BACKUP_H

struct NiBackup_;

/* initialize backup structures */
void backupInit(int source);

/* recursively back up everything */
void backupRecursive(struct NiBackup_ *ni);

/* back up this path and all containing directories */
void backupContaining(struct NiBackup_ *ni, char *path);

#endif
