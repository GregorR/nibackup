NiBackup
========

NiBackup (notification-based incremental backup) is a system created by Gregor
Richards for continuous, incremental backup. It watches the filesystem with
fanotify to continuously back up files, as well as performing complete backups
at preset intervals. The entire history of every file is saved with reverse
binary diffs.

NiBackup is based on the (Linux-specific) fanotify interface, which is in turn
an improvement in some ways on the inotify interface. This means it detects
changes *as they happen* on the filesystem, and keeps continuous, incremental
backups. Unlike inotify, fanotify monitors the entire filesystem.

NiBackup is currently in a very experimental stage. I'm using it for my own
backup, and making it publicly available, but it needs a lot of work to make it
more usable. At this time I don't expect the archive format to change, so
future versions should be compatible, but will have an improved UI and fixes.


Using NiBackup
==============

NiBackup consists of three tools: nibackup (the daemon), nibackup-purge and
nibackup-restore.

nibackup is the daemon. Running it is very simple:
`sudo nibackup <directory to back up> <backup directory>`.
It will start with a full synchronization, then use fanotify to watch for
future changes. Additional options are available to control wait times, ignore
dotfiles, etc. Because fanotify is wonky and incomplete, it also performs
another full synchronization every six hours, also configurable.

nibackup-purge purges old data from a backup.
`nibackup-purge <backup> <age>`
deletes all unused backup increments older than `age` seconds. If `age` is 0,
all old data will be removed, leaving only the current status backed up.  The
`-n` option is also supported to show what would be deleted without deleting
it.

nibackup-restore restores files from a backup.


Technical
=========

NiBackup is fairly simple. Every file in the source is mirrored by several
files in the destination, each starting with "ni":

* nii: The increment file. Indicates how many increments this file has been
       backed up.
* nim: The metadata directory. For each increment, represents the file
       metadata.
* nic: The file content for regular files, or link target for symlinks, for
       each increment. The newest increment is stored plain, named ?.new. Older
       increments are either stored as bsdiff patches (.bsp) or, if that fails,
       complete (.old).
* nid: The directory content, for directories.
