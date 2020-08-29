
#include <argp.h>
#include <argz.h>
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

#define SEARCH_FMTS _HURD "%sfs\0" _HURD "%s"

struct mnt_opt_map
{
    unsigned long intopt;
    const char   *stropt;
};

static struct mnt_opt_map mnt_options_maps[] =
{
    (struct mnt_opt_map){ MS_RDONLY,        "ro" },
    (struct mnt_opt_map){ MS_NOATIME,       "noatime" },
    (struct mnt_opt_map){ MS_NODIRATIME,    "nodiratime" },
    (struct mnt_opt_map){ MS_RELATIME,      "relatime" },
    (struct mnt_opt_map){ MS_NOEXEC,        "noexec" },
    (struct mnt_opt_map){ MS_NOSUID,        "nosuid" },
    (struct mnt_opt_map){ MS_STRICTATIME,   "strictatime" },
    (struct mnt_opt_map){ MS_SYNCHRONOUS,   "sync" },
    (struct mnt_opt_map){ MS_NOSUID,        "nosuid" },
};

#include <stdio.h>

/* Determing options and pass options string, no more flags or data here */
/* Perform the mount */
static error_t do_mount(struct fs *fs, bool remount, char *options,
                        size_t options_len ,const char *fstype)
{
    error_t   err         = 0;
//    char     *fsopts      = NULL;
//    size_t    fsopts_len  = 0;
    fsys_t    mounted;


    /* Check if we can determine if the filesystem is mounted */
    err = fs_fsys(fs, &mounted);
    if(err)
        return err;

#define ARGZ(call)              \
    err = argz_##call;          \
    if(err)                     \
        return err;

    {
        if(fs->mntent.mnt_opts)
        {
            ARGZ(add_sep(&options, &options_len, fs->mntent.mnt_opts, ','));

            /* Remove `bind' and  `noauto' options */
            for(char *curstr = options; curstr;
                curstr = argz_next(options, options_len, curstr))
            {
                if(strcmp(curstr, MNTOPT_NOAUTO) == 0)
                {
                    argz_delete(&options, &options_len, MNTOPT_NOAUTO);
                }
                else if(strcmp(curstr, "bind") == 0)
                {
                    fs->mntent.mnt_type = strdup("firmlink");
                    if(!fs->mntent.mnt_type)
                    {
                        return ENOMEM;
                    }
                    argz_delete(&options, &options_len, "bind");
                }
            }
        }
    }

    /* TODO convert the list of options into a list of switch arguments? */

    if(remount)
    {
        if(mounted == MACH_PORT_NULL)
            return EBUSY;

        err = fsys_set_options(mounted, options, options_len, 0);
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
        ARGZ(insert(&options, &options_len, options, type->program));
        ARGZ(add(&options, &options_len, fs->mntent.mnt_fsname));

#undef ARGZ

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
                                               options, options, options_len,
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
    bool          remount     = false;
    bool          firmlink    = false;
    error_t       err         = 0;
    struct fstab *fstab       = NULL;
    struct fs    *fs          = NULL;
    unsigned long flags       = 0;
    char         *device      = NULL;
    char         *mountpoint  = NULL;
    char         *fstype      = NULL;

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
        flags |= MS_RELATIME;

    /* TODO better system for keeping track of atime values */
    /* Separate the per-mountpoint flags */
    if (mountflags & MS_NOSUID)
        flags |= MS_NOSUID;
    if (mountflags & MS_NODEV)
        flags |= MS_NODEV;
    if (mountflags & MS_NOEXEC)
        flags |= MS_NOEXEC;
    if (mountflags & MS_NOATIME)
        flags |= MS_NOATIME;
    if (mountflags & MS_NODIRATIME)
        flags |= MS_NODIRATIME;
    if (mountflags & MS_STRICTATIME)
        flags &= ~(MS_RELATIME | MS_NOATIME);
    if (mountflags & MS_RDONLY)
        flags |= MS_RDONLY;


    if(!filesystemtype || (filesystemtype[0] == '\0'))
    {
        /* Ignore fstype if performing a remount */
        if(!remount)
        {
            err = EINVAL;
            goto end_mount;
        }
        fstype = strdup("auto"); /* Maybe? */
        if(!fstype)
        {
            err = ENOMEM;
            goto end_mount;
        }
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
            if(fstype)
                free(fstype);
            goto end_mount;
        }
    }

    if(!source || (source[0] == '\0'))
    {
        /* Ignore source if performing a remount */
        if(!remount)
        {
            err = EINVAL;
            goto end_mount;
        }
    }
    else
    {
        device = strdup(source);
        if(!device)
        {
            err = ENOMEM;
            if(fstype)
                free(fstype);
            free(mountpoint);
            goto end_mount;
        }
    }

    {
        struct fstab_argp_params psz =
        {
            fstab_path: device,
            program_search_fmts: SEARCH_FMTS,
            program_search_fmts_len: sizeof(SEARCH_FMTS),
            do_all: 0,
            types: NULL,
            types_len: 0,
            exclude: NULL,
            exclude_len: 0,
            names: NULL,
            names_len: 0
        };

        fstab = fstab_argp_create(&psz, SEARCH_FMTS, sizeof(SEARCH_FMTS));
        if(!fstab)
        {
            err = EINVAL;
            goto end_mount;
        }
    }

    if(device) /* two-argument form */
    {
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

    }
    else if(mountpoint) /* One argument form */
    {
        struct mntent m =
        {
            mnt_fsname: mountpoint, /*  Since we cannot know the device using
                                        mountpoint here leads to more helpful
                                        error messages */
            mnt_dir: mountpoint,
            mnt_type: fstype, /* TODO determine fstype? can this be NULL? */
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
    }

    {
        char  *data_ops       = NULL;
        size_t data_ops_len   = 0;

#define ARGZ(call)              \
    err = argz_##call;          \
    if(err)                     \
        goto end_mount;

        /* TODO this assumes that data is a string, which might not be correct */
        ARGZ(create_sep((char*)data, ',', &data_ops, &data_ops_len));

        for(size_t i = 0; i < sizeof(mnt_options_maps) / sizeof(struct mnt_opt_map);
            i++)
        {
            if(flags & mnt_options_maps[i].intopt)
            {
                ARGZ(add(&data_ops, &data_ops_len, mnt_options_maps[i].stropt));
            }
        }

#undef ARGZ

        if(fs != NULL)
            err = do_mount(fs, remount, data_ops, data_ops_len, fstype);

        if(data_ops)
            free(data_ops);
    }

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
