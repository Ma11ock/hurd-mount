/* hurd/libfshelp/mount.c
   mount(2), umount(2), and umount2(2) implementation.

   Written by Ryan Jeffrey <ryan@ryanmj.xyz>.
   Based off of code in hurd/utils/mount.c and hurd/utils/umount.c, Copyright
   Free Software Foundation, Inc.
 */

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

static error_t parse_opt(int key, char *arg, struct argp_state *state);

struct fstab_argp_params fstab_params;

static const struct argp_option argp_opts[] =
{
    {0, 0}
};

static const struct argp_child argp_kids[] =
    { { &fstab_argp, 0,
        "Filesystem selection (if no explicit filesystem arguments given):", 2 },
      { 0 } };

struct argp argp = { argp_opts, parse_opt, NULL, NULL, argp_kids };


/* XXX fix libc */
#undef _PATH_MOUNTED
#define _PATH_MOUNTED "/etc/mtab"

struct mnt_opt_map
{
    unsigned long intopt;
    const char   *stropt;
};

/* For converting mountflags into options for the filesystem driver.  */
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
    { 0, NULL }
};

/* Add a string to an argv-like array of strings. */
static error_t
add_to_argv(char ***out_argv, size_t *out_argc, const char *str)
{
    char  **mnt_argv = *out_argv;
    size_t  mnt_argc = *out_argc;

    /* Fill argv[0] with "" to simulate program call in an actual argv. */
    if(!out_argv || !mnt_argc)
    {
        mnt_argv = malloc(sizeof(char*));
        if(!mnt_argv)
            return ENOMEM;
        mnt_argv[0] = malloc(sizeof(""));
        if(!mnt_argv[0])
            return ENOMEM;
        strcpy(mnt_argv[0], "");
        mnt_argc++;
    }

    mnt_argc++;
    char **check = realloc(mnt_argv, mnt_argc * sizeof(char*));
    if(!check)
        return ENOMEM;
    mnt_argv = check;
    mnt_argv[mnt_argc - 1] = strdup(str);
    if(!mnt_argv[mnt_argc - 1])
        return ENOMEM;

    *out_argc = mnt_argc;
    *out_argv = mnt_argv;
    return 0;
}

static error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    struct fstab_argp_params *params = state->input;
    switch(key)
    {
    case ARGP_KEY_INIT:
        state->child_inputs[0] = params; /* Pass down fstab_argp parser. */
        break;
    }
  return 0;
}

/* Perform the mount. */
static error_t
do_mount(struct fs *fs, bool remount, char *options,
         size_t options_len, const char *fstype)
{
    error_t   err        = 0;
    char     *fsopts     = NULL;
    size_t    fsopts_len = 0;
    fsys_t    mounted;

    /* Check if we can determine if the filesystem is mounted. */
    /* TODO this sets errno to EPERM? with strerror giving
       "Operation not permitted", and if root sets it to 1073741830.
       mount(8) has the same bug. mounted is always set to MACH_PORT_NULL. */
    err = fs_fsys(fs, &mounted);
    if(err)
        goto end_domount;

#define ARGZ(call)              \
    err = argz_##call;          \
    if(err)                     \
        goto end_domount;

    /* Convert the list of options into a list of switch arguments.  */
    for(char *tmp = options; tmp; tmp = argz_next(options, options_len, tmp))
    {
        if(*tmp == '-') /* Allow letter opts `-o -r,-E', BSD style.  */
        {
            ARGZ(add(&fsopts, &fsopts_len, tmp));
        }
        /*  Prepend `--' to the option to make a long option switch,
            e.g. `--ro' or `--rsize=1024'.  */
        else if((strcmp(tmp, "defaults") != 0) && (strlen(tmp) != 0)
                && (strcmp(tmp, "loop") != 0) && (strcmp(tmp, "exec") != 0))

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

    if(fs->mntent.mnt_opts)
    {
        ARGZ(add_sep(&options, &options_len, fs->mntent.mnt_opts, ','));
    }

    if(remount && fsopts)
    {
        /* TODO remounting does not work, errorstr returns
           'operation not supported' on errno when performed on
           `fs' file_t, `mounted' is always MACH_PORT_NULL. */
        if(mounted == MACH_PORT_NULL)
        {
            err = EBUSY;
            goto end_domount;
        }


        /* Check if the user is just changing the read-write settings. */
        if(strcmp(fsopts, "--rw") == 0)
            err = fs_set_readonly(fs, FALSE);
        else if(strcmp(fsopts, "--ro") == 0)
            err = fs_set_readonly(fs, TRUE);
        else
            err = fsys_set_options(mounted, fsopts, fsopts_len, 0);
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
            node = file_name_lookup (fs->mntent.mnt_dir,
                                     flags | O_NOTRANS, 0666);
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


        /* Stick the translator program name in front of the option
           switches.  */
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
                mach_port_deallocate(mach_task_self(), ports[i]);
            for(i = 0; i <= STDERR_FILENO; i++)
                mach_port_deallocate(mach_task_self(), fds[i]);
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

/* Mounts a filesystem. */
int
mount(const char *source, const char *target,
      const char *filesystemtype, unsigned long mountflags,
      const void *data)
{
    /* Remount and firmlink are special because they are options for us and
       not the filesystem driver. */
    bool                    remount     = false;
    bool                    firmlink    = false;
    error_t                 err         = 0;
    struct fs               *fs         = NULL;
    unsigned long           flags       = 0;
    /* Dynamic copies of `source', `target', and `filesystemtype'
       respectively. */
    char                    *device      = NULL;
    char                    *mountpoint  = NULL;
    char                    *fstype      = NULL;
    /* TODO assumes data is a string. */
    const char              *datastr     = (data) ? data : "";
    char                    *mnt_ops     = NULL;
    size_t                   mnt_ops_len = 0;
    /* For argp. */
    struct fstab            *fstab       = NULL;


    /* Separate the per-mountpoint flags. */
    if(mountflags & MS_BIND)
        firmlink = true;
    if(mountflags & MS_REMOUNT)
        remount = true;
    if(mountflags & MS_NOSUID)
        flags |= MS_NOSUID;
    if(mountflags & MS_NODEV)
        flags |= MS_NODEV;
    if(mountflags & MS_NOEXEC)
        flags |= MS_NOEXEC;
    if(mountflags & MS_NOATIME)
        flags |= MS_NOATIME;
    if(mountflags & MS_NODIRATIME)
        flags |= MS_NODIRATIME;
    if(mountflags & MS_STRICTATIME)
        flags &= ~(MS_RELATIME | MS_NOATIME);
    if(mountflags & MS_RDONLY)
        flags |= MS_RDONLY;
    if(mountflags & MS_RELATIME)
        flags |= MS_RELATIME;

    /* Check for mount options in data. */
    if(strstr(datastr, "remount"))
        remount = true;
    if(strstr(datastr, "bind"))
        firmlink = true;

    if(!filesystemtype || (filesystemtype[0] == '\0'))
    {
        /* Ignore fstype if performing a remount. */
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
        /* Ignore source if performing a remount. */
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
        char        *data_ops     = NULL;
        size_t       data_ops_len = 0;
        char       **mnt_argv     = NULL;
        size_t       mnt_argc     = 0;

        if(device)
        {
            err = add_to_argv(&mnt_argv, &mnt_argc, device);
            if(err)
                goto end_mount;
        }
        err = add_to_argv(&mnt_argv, &mnt_argc, mountpoint);
        if(err)
            goto end_mount;
#define ARGZ(call)              \
    err = argz_##call;          \
    if(err)                     \
        goto end_mount;


        /* TODO this assumes that data is a string, which might not be
           correct. */
        ARGZ(create_sep(datastr, ',', &data_ops, &data_ops_len));

        {
            /* Remove `bind', `noauto', and `remount' options. */
            /* Create mnt_argv array for argp.                 */
            for(char *curstr = data_ops; curstr;
                curstr = argz_next(data_ops, data_ops_len, curstr))
            {
                if((strcmp(curstr, MNTOPT_NOAUTO) != 0)
                   && (strcmp(curstr, "bind") != 0)
                   && (strcmp(curstr, "remount") != 0))
                {
                    ARGZ(add(&mnt_ops, &mnt_ops_len, curstr));
                }
            }

        }
        /* Add OR'd flags to option string. */
        for(size_t i = 0;
            mnt_options_maps[i].stropt != NULL;
            i++)
        {
            if(flags & mnt_options_maps[i].intopt)
            {
                ARGZ(add(&mnt_ops, &mnt_ops_len, mnt_options_maps[i].stropt));
            }
        }

#undef ARGZ

        argp_parse(&argp, mnt_argc, mnt_argv, 0, 0, &fstab_params);

        for(size_t i = 0; i < mnt_argc; i++)
        {
            free(mnt_argv[i]);
        }
        free(mnt_argv);
    }

    fstab = fstab_argp_create(&fstab_params, SEARCH_FMTS,
                              sizeof(SEARCH_FMTS));
    if(!fstab)
    {
        err = EINVAL;
        goto end_mount;
    }

    struct mntent m =
    {
        mnt_fsname: (device) ? device : mountpoint, /* Since we cannot
                                                    know the device (in a
                                                    remount), using mountpoint
                                                    here leads to more helpful
                                                    error messages. */
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


    if(fs)
        err = do_mount(fs, remount, mnt_ops, mnt_ops_len, fstype);

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

/* Perform the unmount. */
static error_t
do_umount(struct fs *fs, int goaway_flags)
{

    error_t err  = 0;
    file_t  node = file_name_lookup(fs->mntent.mnt_dir, O_NOTRANS, 0666);
    if(node == MACH_PORT_NULL)
    {
        goto end_doumount;
    }

    err = file_set_translator(node, 0, FS_TRANS_SET, goaway_flags, NULL,
                              0, MACH_PORT_NULL, MACH_MSG_TYPE_COPY_SEND);

    if(!err && ((fs->mntent.mnt_fsname[0] != '\0')
                && (strcmp(fs->mntent.mnt_fsname, "none") != 0)))
    {
        file_t source = file_name_lookup(fs->mntent.mnt_fsname, O_NOTRANS,
                                         0666);
        if(source == MACH_PORT_NULL)
            goto end_doumount;

        err = file_set_translator(source, 0, FS_TRANS_SET, goaway_flags,
                                  NULL, 0, MACH_PORT_NULL,
                                  MACH_MSG_TYPE_COPY_SEND);


        mach_port_deallocate(mach_task_self(), source);

        if(!(goaway_flags & FSYS_GOAWAY_FORCE))
            err = 0;
        if(err)
            goto end_doumount;

    }

end_doumount:
    mach_port_deallocate(mach_task_self(), node);
    return err;
}

/* Unmounts a filesystem. */
int
umount(const char *target)
{
    return umount2(target, 0);
}

/* Unmounts a filesystem with options. */
int
umount2(const char *target, int flags)
{
    error_t        err             = 0;
    struct fs     *fs              = NULL;
    struct fstab  *fstab           = NULL;

    memset(&fstab_params, 0, sizeof(fstab_params));

    if(!target || (target[0] == '\0'))
    {
        err = EINVAL;
        goto end_umount;
    }

    {
        char **mnt_argv = NULL;
        size_t mnt_argc = 0;

        err = add_to_argv(&mnt_argv, &mnt_argc, target);
        if(err)
            goto end_umount;

        err = argp_parse(&argp, mnt_argc, mnt_argv, 0, 0, &fstab_params);
        if(err)
            goto end_umount;

        for(size_t i = 0; i < mnt_argc; i++)
        {
            free(mnt_argv[i]);
        }
        free(mnt_argv);
    }
    /* Read the mtab file by default.  */
    if (! fstab_params.fstab_path)
        fstab_params.fstab_path = _PATH_MOUNTED;

    fstab = fstab_argp_create(&fstab_params, NULL, 0);
    if(!fstab)
    {
        err = errno;
        goto end_umount;
    }

    fs = fstab_find_mount(fstab, target);
    if(!fs)
        err = errno;
    if(err)
        goto end_umount;

    err = do_umount(fs, flags);
end_umount:
    if(err) errno = err;
    return err ? -1 : 0;
}
