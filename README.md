NiBackup
========

NiBackup (notification-based incremental backup) is a system created by Gregor
Richards for continuous, incremental backup. "Ni" is not a backronym, but a
happy coincidence.

It watches the filesystem with fanotify to continuously back up files, as well
as performing complete backups at preset intervals. The entire history of every
file is saved with reverse binary diffs using the venerable `bsdiff`.

NiBackup is based on the (Linux-specific) fanotify and inotify interfaces. This
means it detects changes *as they happen* on the filesystem, and keeps
continuous, incremental backups.

NiBackup is currently in a beta stage. I'm using it for my own backup, and
making it publicly available, but there are some improvements yet to be made.
At this time I don't expect the archive format to change, so future versions
should be compatible. I am confident that NiBackup in this state works and will
not destroy data (unless you ask it to).


Using NiBackup
==============

NiBackup consists of three tools: nibackup (the daemon), nibackup-purge and
nibackup-restore.

nibackup is the daemon. Running it is very simple:
`sudo nibackup <directory to back up> <backup directory>`.
It will start with a full synchronization, then use fanotify and inotify to
watch for future changes. Additional options are available to control wait
times, ignore dotfiles, etc. `sudo` is necessary to use fanotify: nibackup will
drop its root privileges as soon as it can.

nibackup-purge purges old data from a backup.
`nibackup-purge -a <age> <backup>`
deletes all unused backup increments older than `age` seconds. If `age` is 0,
all old data will be removed, leaving only the current status backed up.  The
`-n` option is also supported to show what would be deleted without deleting
it.

nibackup-restore restores files from a backup. `nibackup-restore -t <time>
<backup> <target>` restores the backup at time `<time>` (specified as a Unix
timestamp) to `<target>`, `nibackup-restore -a <age> <backup> <target>`
restores the state `<age>` seconds ago. The `-i` option restoers only a
subdirectory or specific file.


Features and Flaws
==================

NiBackup backs up files, directories, symlinks and fifos. Sparse files are
preserved as sparse, but old versions of sparse files can become
unsparse: sparseness is only preserved in the latest version. All old versions
of files are stored with reverse binary diffs, so it is always safe to remove
old versions from the backup.

It does not support hard links (will back them up as separate files) or special
files such as devices, and does not recognize moves as moves.

For continuous backup, NiBackup uses fanotify and inotify. fanotify is very
limited: In particular, it does not not notify on file renames or removes.  To
alleviate this, NiBackup additionally uses inotify watches on 1024
recently-accessed directories.  inotify enforces a restriction on the number of
watches available, so it is insufficient for watching an entire filesystem
itself, but it does support more filesystem changes than fanotify.

NiBackup runs a periodic full synchronization to make up for the lacks of the
notification systems, but as a result, if you recover a backup from an
intermediate time, it may e.g. restore both names of a rename or restore a
file that was actually deleted. Nothing it does is (or can be) atomic, so the
restore state is never guaranteed to be precisely what was on the disk at any
given time, only the state of each file as it existed at some time.


Technical
=========

NiBackup is fairly simple. Every file in the source is mirrored by several
files in the destination, each named "ni", then a one-character type, then the
filename. Increments are numbered from 0 (which is always "nonexistent")
upwards.

* nii: The increment file. Indicates how many increments this file has been
       backed up.
* nim: The metadata directory. For each increment, a file `<increment>.mat`
       represents the file metadata at that increment.  The format is ASCII
       with the following lines: type, mode, uid, gid, size, mtime, ctime.
* nic: Directory containing the file content for regular files, or link target
       for symlinks, for each increment. The newest increment is stored plain
       as `<increment>.dat`. Older increments are either stored as bsdiff
       patches (`<increment>.bsp`) or, if that fails, plain
       (`<increment>.dat`).
* nid: Directory containing backups of every path in the backed up directory.
