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

/* For converting mountflags into options for the filesystem driver  */
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

error_t
parse_opt(int key, char *arg, struct argp_state *state)
{
    struct fstab_argp_params *params = state->input;
    switch(key)
    {
    case ARGP_KEY_INIT:
        state->child_inputs[0] = params; /* Pass down fstab_argp parser */
        break;
    }
  return 0;
}

/* Gets mounted filesystem from path (can be relative) and stores into fs.
   Returns a pointer to a fs struct on success,
   Returns NULL and sets `err' on failure  */
static struct fs *
get_mounted_fs(const char *path, error_t *err)
{
    struct fstab  *fstab           = NULL;
    struct fs     *fs              = NULL;
    char          *names           = NULL;
    size_t         names_len       = 0;


    argz_create_sep(path, ',', &names, &names_len);
    if(!names)
    {
        *err = ENOMEM;
        goto end_get_mnted_fs;
    }

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
        *err = errno;
        goto end_get_mnted_fs;
    }

    fs = fstab_find_mount(fstab, path);
    if(!fs)
        *err = errno;

end_get_mnted_fs:
    return fs;
}

/* Perform the mount */
static error_t
do_mount(struct fs *fs, bool remount, char *options,
         size_t options_len, const char *fstype)
{
    puts("Perf mount");
    error_t   err        = 0;
    char     *fsopts     = NULL;
    size_t    fsopts_len = 0;
    fsys_t    mounted;

    /* Check if we can determine if the filesystem is mounted */
    /* TODO this sets errno to EPERM? with strerror giving
       "Operation not permitted", and if root sets it to 1073741830.
       mount(8) has the same bug. mounted is always set to MACH_PORT_NULL */
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
           `fs' file_t, `mounted' is always MACH_PORT_NULL */
        if(mounted == MACH_PORT_NULL)
        {
            err = EBUSY;
            goto end_domount;
        }


        /* Check if the user is just changing the read-write settings */
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

/* Mounts a filesystem */
int
mount(const char *source, const char *target,
      const char *filesystemtype, unsigned long mountflags,
      const void *data)
{
    /* Remount and firmlink are special because they are options for us and
       not the filesystem driver */
    bool                    remount     = false;
    bool                    firmlink    = false;
    error_t                 err         = 0;
    struct fs               *fs         = NULL;
    unsigned long           flags       = 0;
    /* Dynamic copies of `source', `target', and `filesystemtype'
       respectively */
    char                    *device      = NULL;
    char                    *mountpoint  = NULL;
    char                    *fstype      = NULL;
    /* TODO assumes data is a string */
    const char              *datastr     = (data) ? data : "";
    char                    *mnt_ops     = NULL;
    size_t                   mnt_ops_len = 0;
    /* For argp */
    struct fstab_argp_params params;
    char                   **mnt_argv    = malloc(sizeof(char*));
    int                      mnt_argc    = 0;

    if(!mnt_argv)
    {
        err = ENOMEM;
        goto end_mount;
    }
    mnt_argv[0] = strdup("");
    if(!mnt_argv[0])
    {
        err = ENOMEM;
        goto end_mount;
    }
    mnt_argc++;
    /* Separate the per-mountpoint flags */
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
        char         *data_ops     = NULL;
        size_t        data_ops_len = 0;
        char         *argv_ops     = NULL;
        size_t        argv_ops_len = 0;

        inline error_t add_arg_toargv(const char *str)
        {
            mnt_argc++;
            char **check = realloc(mnt_argv, mnt_argc * sizeof(char*));
            if(!check)
            {
                mnt_argc--;
                return ENOMEM;
            }
            mnt_argv = check;
            mnt_argv[mnt_argc - 1] = strdup(str);
            if(!mnt_argv[mnt_argc - 1])
            {
                mnt_argc--;
                return ENOMEM;
            }
            return 0;
        }

        inline error_t add_op_to_str(const char *str)
        {
            size_t str_size = strlen(str);
            if(!argv_ops)
            {
                argv_ops = malloc(str_size);
                if(!argv_ops)
                    return ENOMEM;
            }
            else
            {
                char  *check = realloc(argv_ops, argv_ops_len * sizeof(char)
                                       + sizeof(char));
                if(!check)
                    return ENOMEM;
                argv_ops = check;
                argv_ops[argv_ops_len++] = ',';
            }
            memcpy(argv_ops + argv_ops_len, str, str_size + 1);
            argv_ops_len += str_size;
            return 0;
        }

        if(device)
        {
            err = add_arg_toargv(device);
            if(err)
                goto end_mount;
        }
        err = add_arg_toargv(mountpoint);
        if(err)
            goto end_mount;
        err = add_arg_toargv("-o");
        if(err)
            goto end_mount;

#define ARGZ(call)              \
    err = argz_##call;          \
    if(err)                     \
        goto end_mount;

        add_op_to_str(data);

        /* TODO this assumes that data is a string, which might not be
           correct */
        ARGZ(create_sep(datastr, ',', &data_ops, &data_ops_len));

        {
            /* Remove `bind', `noauto', and `remount' options */
            /* Create mnt_argv array for argp                 */
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
        /* Add OR'd flags to option string */
        for(size_t i = 0;
            mnt_options_maps[i].stropt != NULL;
            i++)
        {
            if(flags & mnt_options_maps[i].intopt)
            {
                ARGZ(add(&mnt_ops, &mnt_ops_len, mnt_options_maps[i].stropt));
                add_op_to_str(mnt_options_maps[i].stropt);
            }
        }

        add_arg_toargv(argv_ops);

#undef ARGZ
    }

    /* Normal arguments and argp */
    {
        static const struct argp_option argp_opts[] =
        {
            {"timeout",   'T',    "MILLISECONDS", 0, "Timeout for translator startup"},
            {"format",    'p',    "mount|fstab|translator", OPTION_ARG_OPTIONAL,
             "Output format for query (no filesystem arguments)"},
            {"options", 'o', "OPTIONS", 0, "A `,' separated list of options"},
            {"readonly", 'r', 0, 0, "Never write to disk or allow opens for writing"},
            {"writable", 'w', 0, 0, "Use normal read/write behavior"},
            {"update", 'u', 0, 0, "Flush any meta-data cached in core"},
            {"remount", 0, 0, OPTION_ALIAS},
            {"verbose", 'v', 0, 0, "Give more detailed information"},
            {"no-mtab", 'n', 0, 0, "Do not update /etc/mtab"},
            {"test-opts", 'O', "OPTIONS", 0,
             "Only mount fstab entries matching the given set of options"},
            {"bind", 'B', 0, 0, "Bind mount, firmlink"},
            {"firmlink", 0, 0, OPTION_ALIAS},
            {"fake", 'f', 0, 0, "Do not actually mount, just pretend"},
            {0, 0}
        };

        static const char doc[] = "Start active filesystem translators";
        static const char args_doc[] = "\
        DEVICE\t(in " _PATH_MNTTAB ")\n\
        DIRECTORY\t(in " _PATH_MNTTAB ")\n\
        [-t TYPE] DEVICE DIRECTORY\n\
        -a";

        static const struct argp_child argp_kids[] =
            { { &fstab_argp, 0,
                "Filesystem selection (if no explicit filesystem arguments given):", 2 },
              { 0 } };

        struct argp argp = { argp_opts, parse_opt, args_doc, doc, argp_kids };
        argp_parse(&argp, mnt_argc, mnt_argv, 0, 0, &params);
    }

    if(device) /* two-argument form */
    {
        struct fstab *fstab = NULL;

        /* TODO This runs slow! */
        fstab = fstab_argp_create(&params, SEARCH_FMTS,
                                  sizeof(SEARCH_FMTS));
        if(!fstab)
        {
            err = EINVAL;
            goto end_mount;
        }

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
    else if(mountpoint) /* One argument form (remount) */
    {
        fs = get_mounted_fs(mountpoint, &err);
        if(err || !fs)
        {
            goto end_mount;
        }
    }

    if(fs)
        err = do_mount(fs, remount, mnt_ops, mnt_ops_len, fstype);

end_mount:
    for(int i = 0; i < mnt_argc; i++)
    {
        free(mnt_argv[i]);
    }
    if(mnt_argv)
        free(mnt_argv);
    if(device)
        free(device);
    if(mountpoint)
        free(mountpoint);
    if(fstype)
        free(fstype);

    if(err) errno = err;
    return err ? -1 : 0;
}

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

/* Unmounts a filesystem */
int
umount(const char *target)
{
    return umount2(target, 0);
}

/* Unmounts a filesystem with options */
int
umount2(const char *target, int flags)
{
    error_t    err  = 0;
    struct fs *fs   = NULL;

    if(!target || (target[0] == '\0'))
    {
        err = EINVAL;
        goto end_umount;
    }

    fs = get_mounted_fs(target, &err);
    if(err)
        goto end_umount;

    err = do_umount(fs, flags);
end_umount:
    if(err) errno = err;
    return err ? -1 : 0;
}
