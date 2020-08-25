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

#define SEARCH_FMTS _HURD "%sfs\0" _HURD "%s"
#define DEFAULT_FSTYPE  "auto"

static const char *fstype = DEFAULT_FSTYPE;
static int fake = 0;

struct mnt_opt_map
{
    unsigned long intopt;
    const char   *stropt;
};

static struct mnt_options_maps[] =
{
    (struct mnt_opt_map){ MS_RDONLY,     "ro" },
    (struct mnt_opt_map){ MS_NOATIME,    "noatime" },
    (struct mnt_opt_map){ MSNODIRATIME,  "nodiratime" },
    (struct mnt_opt_map){ MS_RELATIME,   "relatime" },
};

/* C-string handling function
   On success returns the number of chars appended to the options string
   On failure returns 0 */
static size_t mnt_append_option(char **str, const char *op)
{
    char *opstr   = *str;
    size_t opsize = strlen(op) + 1;
    size_t curstrsize;

    if(opstr == NULL)
    {
        opstr = malloc(opsize);
        if(!opstr)
            return 0;
        memcpy(opstr, op, opsize);
    }
    else
    {
        curstrsize = strlen(opstr);
        opstr = realloc(opstr, curstrsize + opsize);
        if(!opstr)
            return 0;
        memcpy(opstr + curstrsize, opstr, opsize);
    }

    return opsize;
}

#include <stdio.h>

/* Perform the mount */
static error_t do_mount(struct fs *fs, int remount, unsigned long flags,
                        const void *data)
{
    error_t err;
    char   *fsopts;
    char   *o;
    size_t  fsopts_len;
    fsys_t  mounted;

    /* Check if filesystem is already mounted */
    err = fs_fsys(fs, &mounted);
    if(err)
    {
        return EBUSY;
    }

    /* TODO eval arguments */

    /* END TODO */
    if(remount)
    {
        if(mounted == MACH_PORT_NULL)
        {
            return EBUSY;
        }
    }

    fsopts_len = 0;
    fsopts = NULL;

    for(size_t i = 0;
        i < sizeof(mnt_options_maps) / sizeof(struct mnt_opt_map);
        i++)
    {
        size_t tmp;
        if(mountflags & mnt_options_maps[i].intopt)
        {
            tmp = mnt_append_option(&fsopts, mnt_options_maps[i].stropt);
            if(!tmp)
                return ENOMEM;
            fsopts_len += tmp;
        }
    }

    printf("Opts are: %s\n", fsopts);

}

// TODO error to errno
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

    /* Abs path? Security? TODO */

    /* Separate the per-mountpoint flags */
    if(mountflags & MS_BIND) /* TODO test */
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


int umount(const char *target)
{
    return 0;
}

int umount2(const char *target, int flags)
{
    return 0;
}
