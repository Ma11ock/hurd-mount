#include "../sutils/fstab.h"
#include <errno.h>
#include <error.h>
#include <sys/mount.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <hurd/fsys.h>
#include <hurd/fshelp.h>
#include <hurd/paths.h>
#include <mntent.h>
#include <string.h>
#include <stdbool.h>

#define MNT_NOSUID  0x01
#define MNT_NODEV   0x02
#define MNT_NOEXEC  0x04
#define MNT_NOATIME 0x08
#define MNT_NODIRATIME  0x10
#define MNT_RELATIME    0x20
#define MNT_READONLY    0x40    /* does the user want this to be r/o? */

#define MNT_SHRINKABLE  0x100
#define MNT_WRITE_HOLD  0x200

#define MNT_SHARED  0x1000  /* if the vfsmount is a shared mount */
#define MNT_UNBINDABLE  0x2000  /* if the vfsmount is a unbindable mount */


/*
 * MNT_SHARED_MASK is the set of flags that should be cleared when a
 * mount becomes shared.  Currently, this is only the flag that says a
 * mount cannot be bind mounted, since this is how we create a mount
 * that shares events with another mount.  If you add a new MNT_*
 * flag, consider how it interacts with shared mounts.
 */
#define MNT_SHARED_MASK (MNT_UNBINDABLE)
#define MNT_USER_SETTABLE_MASK  (MNT_NOSUID | MNT_NODEV | MNT_NOEXEC \
                 | MNT_NOATIME | MNT_NODIRATIME | MNT_RELATIME \
                 | MNT_READONLY)
#define MNT_ATIME_MASK (MNT_NOATIME | MNT_NODIRATIME | MNT_RELATIME )

#define MNT_INTERNAL_FLAGS (MNT_SHARED | MNT_WRITE_HOLD | MNT_INTERNAL | \
                MNT_DOOMED | MNT_SYNC_UMOUNT | MNT_MARKED | \
                MNT_CURSOR)

#define MNT_INTERNAL    0x4000

#define MNT_LOCK_ATIME      0x040000
#define MNT_LOCK_NOEXEC     0x080000
#define MNT_LOCK_NOSUID     0x100000
#define MNT_LOCK_NODEV      0x200000
#define MNT_LOCK_READONLY   0x400000
#define MNT_LOCKED      0x800000
#define MNT_DOOMED      0x1000000
#define MNT_SYNC_UMOUNT     0x2000000
#define MNT_MARKED      0x4000000
#define MNT_UMOUNT      0x8000000
#define MNT_CURSOR      0x10000000


#define SEARCH_FMTS _HURD "%sfs\0" _HURD "%s"

struct mnt_opt_map
{
    unsigned long intopt;
    const char   *stropt;
};

static struct mnt_opt_map mnt_options_maps[] =
{
    (struct mnt_opt_map){ MNT_READONLY,     "ro" },
    (struct mnt_opt_map){ MNT_NOATIME,       "noatime" },
    (struct mnt_opt_map){ MNT_NODIRATIME,    "nodiratime" },
    (struct mnt_opt_map){ MNT_RELATIME,      "relatime" },
};

/*  Appends value to argz_vec-like string, creates it if it does not exist
    Returns true on success,
    Returns false on memory allocation error */
static bool mnt_append_option(char **vecin, size_t *vecsiz, const char *op)
{
    size_t cursize = *vecsiz;
    char *vec = *vecin;
    size_t opsize = strlen(op) + 1;

    if(vec == NULL)
    {
        cursize = opsize;
        vec = strdup(op);
        if(!vec)
            return false;
    }
    else
    {
        cursize += opsize;
        char *tmp = realloc(vec, cursize * sizeof(*vec));
        if(!tmp)
        {
            free(vec);
            return false;
        }

        vec = tmp;
        memcpy(vec + *vecsiz, op, opsize);
    }

    *vecin = vec;
    *vecsiz = cursize;
    return true;
}

/*  Remove an option for a vector */
static bool mnt_remove_option(char **vecin, size_t *vecsiz, const char *op)
{
    size_t cursize = *vecsiz;
    char *vec = *vecin;

    if(vec == NULL)
        return true;
    else
    {
        char  *new_vec  = NULL;
        size_t new_size = 0;

        /* Write the strings to the new buffer */
        for(char *pos = vec; *pos; pos += strlen(pos) + 1)
        {
            if(strcmp(pos, op) != 0)
            {
                if(!mnt_append_option(&new_vec, &new_size, op))
                    return false;
            }
        }

        free(vec);
        vec = new_vec;
        cursize = new_size;
    }

    *vecsiz = cursize;
    *vecin = vec;
    return true;
}


#include <stdio.h>

/* Perform the mount */
static error_t do_mount(struct fs *fs, bool remount, unsigned long flags,
                        const char *fstype, const char *data)
{
    error_t   err        = 0;
    char     *fsopts     = NULL;
    char     *mntops     = NULL;
    size_t    mntops_len = 0;
    size_t    fsopts_len = 0;
    fsys_t    mounted;
    const char *delim = ",";

    puts("We got to do_mount");

    /* Check if we can determine if the filesystem is mounted */
    err = fs_fsys(fs, &mounted);
    if(err)
        return err;

    if(fs->mntent.mnt_opts)
    {
        /*  TODO test, i cannot figure out if mnt_opts is from fstab (we should ignore)
            or if it is simply the filesystem's mount options (not ignore) */
        char  *token      = strtok(fs->mntent.mnt_opts, delim);
        char  *token_vec  = NULL;
        size_t tvec_size  = 0;

        while(token)
        {
            if(!mnt_append_option(&token_vec, &tvec_size, token))
                return ENOMEM;

            token = strtok(NULL, delim);
        }

    }

    /* Creates a vector of options to pass to the translator */
    for(size_t i = 0;
        i < sizeof(mnt_options_maps) / sizeof(struct mnt_opt_map);
        i++)
        if(flags & mnt_options_maps[i].intopt)
            if(!mnt_append_option(&fsopts, &fsopts_len, mnt_options_maps[i].stropt))
                return ENOMEM;

    /*  Remove the `noauto' and `bind' options, since they're for us not the
        filesystem.  */
    for(char *tstr = fsopts; tstr; tstr += strlen(tstr) + 1)
    {
        if(strcmp(tstr, MNTOPT_NOAUTO) == 0)
            if(!mnt_remove_option(&fsopts, &fsopts_len, MNTOPT_NOAUTO))
                return ENOMEM;
        if(strcmp(tstr, "bind") == 0)
        {
            fs->mntent.mnt_type = strdup("firmlink");
            if(!fs->mntent.mnt_type)
                return ENOMEM;
            if(!mnt_remove_option(&fsopts, &fsopts_len, "bind"))
                return ENOMEM;
        }
    }

    if(remount)
    {
        if(mounted == MACH_PORT_NULL)
            return EBUSY;

        err = fsys_set_options(mounted, fsopts, fsopts_len, 0);
        if(err)
            return err;
    }
    else
    {
        /*  Error during file lookup; we use this to avoid duplicating error
            messages.  */
        error_t open_err = 0;
        /* The control port for any active translator we start up.  */
        fsys_t active_control;
        file_t node;
        struct fstype *type = NULL;

        /* The callback to start_translator opens NODE as a side effect.  */
        error_t open_node(int flags,
                          mach_port_t *underlying,
                          mach_msg_type_name_t *underlying_type,
                          task_t task, void *cookie)
        {
            node = file_name_lookup (fs->mntent.mnt_dir, flags | O_NOTRANS, 0666);
            if (node == MACH_PORT_NULL)
            {
                open_err = errno;
                return open_err;
            }

            *underlying = node;
            *underlying_type = MACH_MSG_TYPE_COPY_SEND;

            return 0;
        }

        if(mounted != MACH_PORT_NULL)
        {
            return EBUSY;
        }

        fs->mntent.mnt_type = strdup(fstype);
        if(!fs->mntent.mnt_type)
        {
            return ENOMEM;
        }

        err = fs_type(fs, &type);
        if(err)
            return err;
        if(type->program == NULL)
            return EFTYPE;

        /* Stick the translator program name in front of the option switches.  */
        {
            char  *tmpbuf     = strdup(type->program);
            if(!tmpbuf)
                return ENOMEM;
            size_t tmpbuf_len = strlen(tmpbuf) + 1;

            for(char *tstr = fsopts; tstr; tstr += strlen(tstr) + 1)
            {
                if(!mnt_append_option(&tmpbuf, &tmpbuf_len, tstr))
                    return ENOMEM;
            }
            /*  Now stick the device name on the end as the last argument.  */
            if(!mnt_append_option(&tmpbuf, &tmpbuf_len, fs->mntent.mnt_fsname))
                return ENOMEM;

            free(fsopts);
            fsopts = tmpbuf;
            fsopts_len = tmpbuf_len;

        }

        {
            mach_port_t ports[INIT_PORT_MAX];
            mach_port_t fds[STDERR_FILENO + 1];
            int ints[INIT_INT_MAX];
            int i;

            for(i = 0; i < INIT_PORT_MAX; i++)
                ports[i] = MACH_PORT_NULL;
            for(i = 0; i < STDERR_FILENO + 1; i++)
                fds[i] = MACH_PORT_NULL;
            memset(ints, 0, INIT_INT_MAX * sizeof(int));

            ports[INIT_PORT_CWDIR] = getcwdir();
            ports[INIT_PORT_CRDIR] = getcrdir();
            ports[INIT_PORT_AUTH] = getauth();

            err = fshelp_start_translator_long(open_node, NULL,
                                               fsopts, fsopts, fsopts_len,
                                               fds, MACH_MSG_TYPE_COPY_SEND,
                                               STDERR_FILENO + 1,
                                               ports, MACH_MSG_TYPE_COPY_SEND,
                                               INIT_PORT_MAX,
                                               ints, INIT_INT_MAX,
                                               geteuid(),
                                               0, &active_control);
            for(i = 0; i < INIT_PORT_MAX; i++)
                mach_port_deallocate (mach_task_self(), ports[i]);
            for(i = 0; i <= STDERR_FILENO; i++)
                mach_port_deallocate (mach_task_self(), fds[i]);
        }

        if(open_err)
            return open_err;
        else if(err)
            return err;
        else
        {
            err = file_set_translator(node, 0, FS_TRANS_SET | FS_TRANS_EXCL, 0,
                                      0, 0, active_control,
                                      MACH_MSG_TYPE_COPY_SEND);
            if(err)
                fsys_goaway(active_control, FSYS_GOAWAY_FORCE);
            mach_port_deallocate(mach_task_self(), active_control);
        }
    }

/*
    if(fsopts)
        free(fsopts);
*/
    return err;
}

/* Mounts a filesystem */
int mount(const char *source, const char *target,
          const char *filesystemtype, unsigned long mountflags,
          const void *data)
{
    bool          remount    = false;
    bool          firmlink   = false;
    error_t       err        = 0;
    struct fstab *fstab      = NULL;
    struct fs    *fs         = NULL;
    unsigned long flags      = 0;
    char         *device;
    char         *mountpoint;
    char         *fstype;

    /* TODO nested func */

    if(!filesystemtype || (filesystemtype[0] == '\0'))
    {
        err = EINVAL;
        goto end_mount;
    }
    else
    {
        fstype = strdup(filesystemtype);
        if(!fstype)
        {
            err = ENOMEM;
            goto end_mount;
        }
    }

    if(!target || (target[0] == '\0'))
    {
        err = EINVAL;
        goto end_mount;
    }
    else
    {
        mountpoint = strdup(target);
        if(!mountpoint)
        {
            err = ENOMEM;
            goto end_mount;
        }
    }

    if(!source || (source[0] == '\0'))
    {
        err = EINVAL;
        goto end_mount;
    }
    else
    {
        device = strdup(source);
        if(!device)
        {
            err = ENOMEM;
            free(mountpoint);
            goto end_mount;
        }
    }


    /* Discard magic */
    if ((mountflags & MS_MGC_MSK) == MS_MGC_VAL)
        mountflags &= ~MS_MGC_MSK;

    /* Separate the per-mountpoint flags */
    if(mountflags & MS_BIND)
        firmlink = true;
    if(mountflags & MS_REMOUNT)
    	remount = true;

    /* Default to relatime unless overriden */
    if (!(mountflags & MS_NOATIME))
        flags |= MNT_RELATIME;

    /* Separate the per-mountpoint flags */
    if (mountflags & MS_NOSUID)
        flags |= MNT_NOSUID;
    if (mountflags & MS_NODEV)
        flags |= MNT_NODEV;
    if (mountflags & MS_NOEXEC)
        flags |= MNT_NOEXEC;
    if (mountflags & MS_NOATIME)
        flags |= MNT_NOATIME;
    if (mountflags & MS_NODIRATIME)
        flags |= MNT_NODIRATIME;
    if (mountflags & MS_STRICTATIME)
        flags &= ~(MNT_RELATIME | MNT_NOATIME);
    if (mountflags & MS_RDONLY)
        flags |= MNT_READONLY;


    struct mntent m =
    {
        mnt_fsname: device,
        mnt_dir: mountpoint,
        mnt_type: fstype,
        mnt_opts: 0,
        mnt_freq: 0,
        mnt_passno: 0
    };
    if(firmlink)
    {
        m.mnt_type = strdup("firmlink");
        if(!m.mnt_type)
        {
            err = ENOMEM;
            goto end_mount;
        }
    }

    err = fstab_add_mntent(fstab, &m, &fs);
    if(err)
        goto end_mount;

    if(fs != NULL)
        err = do_mount(fs, remount, flags, filesystemtype, data);

end_mount:
    if(err) errno = err;
    return err ? -1 : 0;
}


/* Unmounts a filesystem */
int umount(const char *target)
{
    return 0;
}

int umount2(const char *target, int flags)
{
    return 0;
}
