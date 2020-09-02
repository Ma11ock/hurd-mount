/* TODO remount does not work yet, further test mount(8) remounting */
/* test mount(2) and umount(2) behaviour on Linux */
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

/* XXX fix libc */
#undef _PATH_MOUNTED
#define _PATH_MOUNTED "/etc/mtab"


struct mnt_opt_map
{
    unsigned long intopt;
    const char   *stropt;
};

static const struct mnt_opt_map mnt_options_maps[] =
{
    { MS_RDONLY,        "ro" },
    { MS_NOATIME,       "noatime" },
    { MS_NODIRATIME,    "nodiratime" },
    { MS_RELATIME,      "relatime" },
    { MS_NOEXEC,        "noexec" },
    { MS_NOSUID,        "nosuid" },
    { MS_STRICTATIME,   "strictatime" },
    { MS_SYNCHRONOUS,   "sync" },
    { MS_NOSUID,        "nosuid" },
};

/* Determing options and pass options string, no more flags or data here */
/* Perform the mount */
static error_t do_mount(struct fs *fs, bool remount, char *options,
                        size_t options_len, const char *fstype)
{
    error_t   err        = 0;
    char     *fsopts     = NULL;
    size_t    fsopts_len = 0;
    fsys_t    mounted;

    /* Check if we can determine if the filesystem is mounted */
    /* TODO this sets errno to EPERM, and if root sets it to 1073741830 */
    err = fs_fsys(fs, &mounted);
    if(err)
        goto end_domount;

#define ARGZ(call)              \
    err = argz_##call;          \
    if(err)                     \
        goto end_domount;

    {
        if(fs->mntent.mnt_opts)
        {
            ARGZ(add_sep(&options, &options_len, fs->mntent.mnt_opts, ','));
        }
    }

    if(remount)
    {
        /* Check if the user is just changing the read-write settings */
        if(options && (options_len == 2))
        {
            if(strcmp(options, "rw") == 0)
            {
                err = fs_set_readonly(fs, FALSE);
                goto end_domount;
            }
            else if(strcmp(options, "ro") == 0)
            {
                err = fs_set_readonly(fs, TRUE);
                goto end_domount;
            }
        }

        if(mounted == MACH_PORT_NULL)
        {
            err = EBUSY;
            goto end_domount;
        }

        err = fsys_set_options(mounted, options, options_len, 0);
        if(err)
            goto end_domount;
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
            err = EBUSY;
            goto end_domount;
        }

        fs->mntent.mnt_type = strdup(fstype);
        if(!fs->mntent.mnt_type)
        {
            err = ENOMEM;
            goto end_domount;
        }

        err = fs_type(fs, &type);
        if(err)
            goto end_domount;
        if(type->program == NULL)
        {
            err = EFTYPE;
            goto end_domount;
        }

        /* Convert the list of options into a list of switch arguments.  */
        for(char *tmp = options; tmp; tmp = argz_next(options, options_len, tmp))
        {
            if(*tmp == '-') /* Allow letter opts `-o -r,-E', BSD style.  */
            {
                ARGZ(add(&fsopts, &fsopts_len, tmp));
            }
            /*  Prepend `--' to the option to make a long option switch,
                e.g. `--ro' or `--rsize=1024'.  */
            else if((strcmp(tmp, "defaults") != 0) && (strlen(tmp) != 0) &&
                    (strcmp(tmp, "loop") != 0) && (strcmp(tmp, "exec") != 0))

            {
                size_t tmparg_len = strlen(tmp) + 3;
                char *tmparg = malloc(tmparg_len);
                if(!tmparg)
                {
                    err = ENOMEM;
                    goto end_domount;
                }
                tmparg[tmparg_len - 1] = '\0';

                tmparg[0] = tmparg[1] = '-';
                memcpy(&tmparg[2], tmp, tmparg_len - 2);
                ARGZ(add(&fsopts, &fsopts_len, tmparg));
                free(tmparg);
            }
        }

        /* Stick the translator program name in front of the option switches.  */
        ARGZ(insert(&fsopts, &fsopts_len, fsopts, type->program));
        /* Now stick the device name on the end as the last argument.  */
        ARGZ(add(&fsopts, &fsopts_len, fs->mntent.mnt_fsname));

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
        {
            err = open_err;
            goto end_domount;
        }
        else if(err)
            goto end_domount;
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

end_domount:
    if(fsopts)
        free(fsopts);
    return err;
}


/* Mounts a filesystem */
int mount(const char *source, const char *target,
          const char *filesystemtype, unsigned long mountflags,
          const void *data)
{
    /* Remount and firmlink are special because they are options for us and
       not the filesystem driver */
    bool          remount      = false;
    bool          firmlink     = false;
    error_t       err          = 0;
    struct fstab *fstab        = NULL;
    struct fs    *fs           = NULL;
    unsigned long flags        = 0;
    char         *device       = NULL;
    char         *mountpoint   = NULL;
    char         *fstype       = NULL;
    /* TODO assumes data is a string */
    const char   *datastr      = (data) ? data : "";

    /* Separate the per-mountpoint flags */
    if(mountflags & MS_BIND)
        firmlink = true;
    if(mountflags & MS_REMOUNT)
    	remount = true;
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

    /* Check for mount options in data */
    if(strstr(datastr, "remount"))
        remount = true;
    if(strstr(datastr, "bind"))
        firmlink = true;

    if(!filesystemtype || (filesystemtype[0] == '\0'))
    {
        /* Ignore fstype if performing a remount */
        if(!remount)
        {
            err = EINVAL;
            goto end_mount;
        }

        fstype = strdup("auto");
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
        struct fstab_argp_params fstab_params =
        {
            fstab_path: device,
            program_search_fmts: NULL,
            program_search_fmts_len: 0,
            do_all: 0,
            types: NULL,
            types_len: 0,
            exclude: NULL,
            exclude_len: 0,
            names: NULL,
            names_len: 0
        };

        /* TODO This runs slow! */
        fstab = fstab_argp_create(&fstab_params, SEARCH_FMTS, sizeof(SEARCH_FMTS));
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
        char         *data_ops     = NULL;
        size_t        data_ops_len = 0;
        char         *mnt_ops      = NULL;
        size_t        mnt_ops_len  = 0;

#define ARGZ(call)              \
    err = argz_##call;          \
    if(err)                     \
        goto end_mount;

        /* TODO this assumes that data is a string, which might not be correct */
        ARGZ(create_sep(datastr, ',', &data_ops, &data_ops_len));

        {
            /* Remove `bind', `noauto', and `remount' options */
            for(char *curstr = data_ops; curstr;
                curstr = argz_next(data_ops, data_ops_len, curstr))
            {
                if((strcmp(curstr, MNTOPT_NOAUTO) != 0) &&
                   (strcmp(curstr, "bind") != 0)        &&
                   (strcmp(curstr, "remount") != 0))
                {
                    ARGZ(add(&mnt_ops, &mnt_ops_len, curstr));
                }
            }

        }
        /* Add OR'd flags to option string */
        for(size_t i = 0; i < sizeof(mnt_options_maps) / sizeof(struct mnt_opt_map);
            i++)
        {
            if(flags & mnt_options_maps[i].intopt)
            {
                ARGZ(add(&mnt_ops, &mnt_ops_len, mnt_options_maps[i].stropt));
            }
        }

#undef ARGZ

        if(fs != NULL)
            err = do_mount(fs, remount, mnt_ops, mnt_ops_len, fstype);

        if(data_ops)
            free(data_ops);
        if(mnt_ops)
            free(mnt_ops);
    }
end_mount:
    if(device)
        free(device);
    if(mountpoint)
        free(mountpoint);
    if(fstype)
        free(fstype);

    if(err) errno = err;
    return err ? -1 : 0;
}

static error_t do_umount(struct fs *fs, int goaway_flags)
{

    error_t err = 0;
    file_t  node;

    node = file_name_lookup(fs->mntent.mnt_dir, O_NOTRANS, 0666);
    if(node == MACH_PORT_NULL)
    {
        goto end_doumount;
    }

    err = file_set_translator(node, 0, FS_TRANS_SET, goaway_flags, NULL,
                              0, MACH_PORT_NULL, MACH_MSG_TYPE_COPY_SEND);

    if(err)
        goto end_doumount;



    if((fs->mntent.mnt_fsname[0] != '\0') &&
       (strcmp(fs->mntent.mnt_fsname, "none") != 0))
    {
        file_t source = file_name_lookup(fs->mntent.mnt_fsname, O_NOTRANS, 0666);
        if(source == MACH_PORT_NULL)
            goto end_doumount;

        err = file_set_translator(source, 0, FS_TRANS_SET, goaway_flags,
                                  NULL, 0, MACH_PORT_NULL,
                                  MACH_MSG_TYPE_COPY_SEND);

        if(!(goaway_flags & FSYS_GOAWAY_FORCE))
            err = 0;
        if(err)
            goto end_doumount;

        mach_port_deallocate(mach_task_self(), source);
        return 0;
    }

end_doumount:
    return err ? -1 : 0;
}

/* Unmounts a filesystem */
int umount(const char *target)
{
    return umount2(target, 0);
}

/* Unmounts a filesystem with options */
int umount2(const char *target, int flags)
{
    error_t       err             = 0;
    struct fstab *fstab           = NULL;
    struct fs    *fs              = NULL;
    char         *mountpoint_full = NULL;

    if(!target || (target[0] == '\0'))
    {
        err = EINVAL;
        goto end_umount;
    }

    mountpoint_full = realpath(target, NULL);
    if(!mountpoint_full)
        goto end_umount;

    struct fstab_argp_params fstab_params =
    {
        fstab_path: _PATH_MOUNTED,
        program_search_fmts: NULL,
        program_search_fmts_len: 0,
        do_all: 0,
        types: NULL,
        types_len: 0,
        exclude: NULL,
        exclude_len: 0,
        names: NULL,
        names_len: 0
    };

    fstab = fstab_argp_create(&fstab_params, NULL, 0);
    if(!fstab)
    {
        err = ENOMEM;
        goto end_umount;
    }

    fs = fstab_find_mount(fstab, mountpoint_full);
    if(!fs)
    {
        err = ENOMEM;
        goto end_umount;
    }

    err |= do_umount(fs, flags);

end_umount:
    if(mountpoint_full)
        free(mountpoint_full);
    if(err) errno = err;
    return err ? -1 : 0;
}
