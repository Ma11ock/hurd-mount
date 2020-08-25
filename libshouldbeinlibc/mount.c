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
#define DEFAULT_FSTYPE  "auto"

static const char *fstype = DEFAULT_FSTYPE;
static int fake = 0;

struct mnt_opt_map
{
    unsigned long intopt;
    const char   *stropt;
};

static struct mnt_opt_map mnt_options_maps[] =
{
    (struct mnt_opt_map){ MS_RDONLY,     "ro" },
    (struct mnt_opt_map){ MS_NOATIME,    "noatime" },
    (struct mnt_opt_map){ MS_NODIRATIME, "nodiratime" },
    (struct mnt_opt_map){ MS_RELATIME,   "relatime" },
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

/* Perform the mount */
static error_t do_mount(struct fs *fs, int remount, unsigned long flags,
                        const void *data)
{
    error_t   err        = 0;
    char     *fsopts     = NULL;
    char     *o          = NULL;
    char     *mntops     = NULL;
    size_t    mntops_len = 0;
    size_t    fsopts_len = 0;
    fsys_t    mounted;

    /* Check if filesystem is already mounted */
    err = fs_fsys(fs, &mounted);
    if(err)
        return err;


    for(size_t i = 0;
        i < sizeof(mnt_options_maps) / sizeof(struct mnt_opt_map);
        i++)
        if(flags & mnt_options_maps[i].intopt)
            if(!mnt_append_option(&fsopts, &fsopts_len, mnt_options_maps[i].stropt))
                return ENOMEM;


    if(remount)
    {
        if(mounted == MACH_PORT_NULL)
            return EBUSY;

        err = fsys_set_options(mounted, *fsopts, fsopts_len, 0);
        if(err)
            return err;
    }
    else
    {
        error_t open_err = 0;
        /* The control port for any active translator we start up.  */
        fsys_t active_control;
        file_t node;
        struct fstype *type = NULL;

        /* The callback to start_translator opens NODE as a side effect.  */
        error_t open_node (int flags,
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

        

    }

    if(fsopts)
        free(fsopts);
}

/* Mounts a filesystem */
int mount(const char *source, const char *target,
          const char *filesystemtype, unsigned long mountflags,
          const void *data)
{
    unsigned int  remount   = 0;
    unsigned int  firmlink  = 0;
    error_t       err       = 0;
    struct fstab *fstab     = NULL;
    struct fs    *fs        = NULL;

    /* Discard magic */
    if ((mountflags & MS_MGC_MSK) == MS_MGC_VAL)
        mountflags &= ~MS_MGC_MSK;

    /* Separate the per-mountpoint flags */
    if(mountflags & MS_BIND)
        firmlink = 1;
    if(mountflags & MS_REMOUNT)
    	remount = 1;

    if(source)
    {
	    struct mntent m =
	    {
	        mnt_fsname: source,
	        mnt_dir: target,
	        mnt_type: filesystemtype,
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
        if(err) goto end_mount;

    }
    else if(target && remount) /* One argument remount */
    {
        struct mntent m =
        {
            mnt_fsname: target, /* since we cannot know the device,
                   		           using mountpoint here leads to more
                                   helpful error messages              */
            mnt_type: filesystemtype,
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
        if(err) goto end_mount;
    }
    else if(source) /* One-argument form */
    {
        fs = fstab_find(fstab, source);
        if(!fs)
        {
            err = EINVAL;
            goto end_mount;
        }
    }
    else
    	fs = NULL;

    if(fs != NULL)
        err = do_mount(fs, remount, mountflags, data);

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
