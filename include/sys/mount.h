#ifndef _SYS_MOUNT_H
#define _SYS_MOUNT_H

#include <features.h>
#include <hurd/fsys.h>

#define MS_RDONLY       1         /* Mount readonly. */
#define MS_NOSUID       2         /* Ignore suid and sgid bits. */
#define MS_NODEV        4         /* Disallow access to device special files. */
#define MS_NOEXEC       8         /* Disallow Program execution. */
#define MS_SYNCHRONOUS  16        /* Writes are synced at once. */
#define MS_REMOUNT      32        /* Alter flags of mounted fs. */
#define MS_MANDLOCK     64        /* Allow mandatory locks on the fs. */
#define MS_DIRSYNC      128       /* Directory modifications are synchronous. */
#define MS_NOATIME      1024      /* Do not update the access time.*/
#define MS_NODIRATIME   2048      /* Do not update directory access times. */
#define MS_BIND         4096      /* Bind directory to different place. */
#define MS_MOVE         8192      /**/
#define MS_REC          16384     /**/
#define MS_SILENT       32768     /**/
#define MS_POSIXACL     1 << 16   /* VFS does not apply umask. */
#define MS_UNBINDABLE   1 << 17   /* Change to unbindable. */
#define MS_PRIVATE      1 << 18   /* Change to private. */
#define MS_SLAVE        1 << 19   /* Change to slave. */
#define MS_SHARED       1 << 20   /* Change to shared. */
#define MS_RELATIME     1 << 21   /* Update atime relative to mtime/ctime. */
#define MS_KERNMOUNT    1 << 22   /* This is a kern_mount call. */
#define MS_I_VERSION    1 << 23   /* Update inode I_version field. */
#define MS_STRICTATIME  1 << 24   /* Always perform atime updates. */
#define MS_LAZYTIME     1 << 25   /* Update the on-disk [acm]times lazily. */
#define MS_ACTIVE       1 << 30   /**/
#define MS_NOUSER       1 << 31   /**/

/* Flags that can be altered by MS_REMOUNT  */
#define MS_RMT_MASK (MS_RDONLY | MS_SYNCHRONOUS | MS_MANDLOCK | MS_I_VERSION \
             | MS_LAZYTIME)


/* Possible value for FLAGS parameter of `umount2'.  */
/* See `hurd_types.h' for more */
#define MNT_FORCE FSYS_GOAWAY_FORCE
#define MNT_DETACH 2                 /* Ignored */
#define MNT_EXPIRE 4                 /* Ignored */
#define UMOUNT_NOFOLLOW 8            /* Ignored */
#define UMOUNT_NOSYNC FSYS_GOAWAY_NOSYNC

__BEGIN_DECLS

/* Mount The filesystem to target. */
extern int mount(const char *__source, const char *__target,
                 const char *__filesystemtype, unsigned long __mountflags,
                 const void *__data) __THROW;

/* Unmount the filesystem. */
extern int umount(const char *__target) __THROW;

/* Unmount the filesystem with flags. */
extern int umount2(const char *__target, int __flags) __THROW;

__END_DECLS
#endif /* _SYS_MOUNT_H */
