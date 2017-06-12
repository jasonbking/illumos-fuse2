/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2010 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 *
 *	Copyright (c) 1983,1984,1985,1986,1987,1988,1989  AT&T.
 *	All rights reserved.
 *
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 */

/*
 * VFS operations for fusefs
 * (from NFS)
 */

#include <sys/systm.h>
#include <sys/cred.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <sys/vnode.h>
#include <sys/fs_subr.h>
#include <sys/sysmacros.h>
#include <sys/kmem.h>
#include <sys/mkdev.h>
#include <sys/mount.h>
#include <sys/statvfs.h>
#include <sys/errno.h>
#include <sys/debug.h>
#include <sys/cmn_err.h>
#include <sys/modctl.h>
#include <sys/policy.h>
#include <sys/atomic.h>
#include <sys/zone.h>
#include <sys/vfs_opreg.h>
#include <sys/mntent.h>
#include <sys/priv.h>
#include <sys/tsol/label.h>
#include <sys/tsol/tndb.h>
#include <inet/ip.h>

#include "fusefs.h"
#include "fusefs_calls.h"
#include "fusefs_node.h"
#include "fusefs_subr.h"

/*
 * Local functions definitions.
 */
int		fusefsinit(int fstyp, char *name);
void		fusefsfini();

/*
 * FUSEFS Mount options table for MS_OPTIONSTR
 * Note: These are not all the options.
 * Some options come in via MS_DATA.
 * Others are generic (see vfs.c)
 */
static char *intr_cancel[] = { MNTOPT_NOINTR, NULL };
static char *nointr_cancel[] = { MNTOPT_INTR, NULL };

static mntopt_t mntopts[] = {
/*
 *	option name		cancel option	default arg	flags
 *		ufs arg flag
 */
	{ MNTOPT_INTR,		intr_cancel,	NULL,	MO_DEFAULT, 0 },
	{ MNTOPT_NOINTR,	nointr_cancel,	NULL,	0,	0 },
};

static mntopts_t fusefs_mntopts = {
	sizeof (mntopts) / sizeof (mntopt_t),
	mntopts
};

static const char fs_type_name[FSTYPSZ] = "fusefs";

static vfsdef_t vfw = {
	VFSDEF_VERSION,
	(char *)fs_type_name,
	fusefsinit,		/* init routine */
	VSW_HASPROTO|VSW_NOTZONESAFE,	/* flags */
	&fusefs_mntopts			/* mount options table prototype */
};

static struct modlfs modlfs = {
	&mod_fsops,
	"FUSE filesystem",
	&vfw
};

static struct modlinkage modlinkage = {
	MODREV_1, (void *)&modlfs, NULL
};

/*
 * Mutex to protect the following variables:
 *	  fusefs_major
 *	  fusefs_minor
 */
extern	kmutex_t	fusefs_minor_lock;
extern	int		fusefs_major;
extern	int		fusefs_minor;

/*
 * Prevent unloads while we have mounts
 */
uint32_t	fusefs_mountcount;

/*
 * fusefs vfs operations.
 */
static int	fusefs_mount(vfs_t *, vnode_t *, struct mounta *, cred_t *);
static int	fusefs_unmount(vfs_t *, int, cred_t *);
static int	fusefs_root(vfs_t *, vnode_t **);
static int	fusefs_statvfs(vfs_t *, statvfs64_t *);
static int	fusefs_sync(vfs_t *, short, cred_t *);
static void	fusefs_freevfs(vfs_t *);

/*
 * Module loading
 */

/*
 * This routine is invoked automatically when the kernel module
 * containing this routine is loaded.  This allows module specific
 * initialization to be done when the module is loaded.
 */
int
_init(void)
{
	int		error;

	fusefs_mountcount = 0;

	/*
	 * NFS calls these two in _clntinit
	 * Easier to follow this way.
	 */
	if ((error = fusefs_subrinit()) != 0) {
		cmn_err(CE_WARN, "_init: fusefs_subrinit failed");
		return (error);
	}

	if ((error = fusefs_vfsinit()) != 0) {
		cmn_err(CE_WARN, "_init: fusefs_vfsinit failed");
		fusefs_subrfini();
		return (error);
	}

	if ((error = fusefs_clntinit()) != 0) {
		cmn_err(CE_WARN, "_init: fusefs_clntinit failed");
		fusefs_vfsfini();
		fusefs_subrfini();
		return (error);
	}

	error = mod_install((struct modlinkage *)&modlinkage);
	return (error);
}

/*
 * Free kernel module resources that were allocated in _init
 * and remove the linkage information into the kernel
 */
int
_fini(void)
{
	int	error;

	/*
	 * If a forcedly unmounted instance is still hanging around,
	 * we cannot allow the module to be unloaded because that would
	 * cause panics once the VFS framework decides it's time to call
	 * into VFS_FREEVFS().
	 */
	if (fusefs_mountcount)
		return (EBUSY);

	error = mod_remove(&modlinkage);
	if (error)
		return (error);

	/*
	 * Free the allocated fusenodes, etc.
	 */
	fusefs_clntfini();

	/* NFS calls these two in _clntfini */
	fusefs_vfsfini();
	fusefs_subrfini();

	/*
	 * Free the ops vectors
	 */
	fusefsfini();
	return (0);
}

/*
 * Return information about the module
 */
int
_info(struct modinfo *modinfop)
{
	return (mod_info((struct modlinkage *)&modlinkage, modinfop));
}

/*
 * Initialize the vfs structure
 */

int fusefsfstyp;
vfsops_t *fusefs_vfsops = NULL;

static const fs_operation_def_t fusefs_vfsops_template[] = {
	{ VFSNAME_MOUNT, { .vfs_mount = fusefs_mount } },
	{ VFSNAME_UNMOUNT, { .vfs_unmount = fusefs_unmount } },
	{ VFSNAME_ROOT,	{ .vfs_root = fusefs_root } },
	{ VFSNAME_STATVFS, { .vfs_statvfs = fusefs_statvfs } },
	{ VFSNAME_SYNC,	{ .vfs_sync = fusefs_sync } },
	{ VFSNAME_VGET,	{ .error = fs_nosys } },
	{ VFSNAME_MOUNTROOT, { .error = fs_nosys } },
	{ VFSNAME_FREEVFS, { .vfs_freevfs = fusefs_freevfs } },
	{ NULL, NULL }
};

int
fusefsinit(int fstyp, char *name)
{
	int		error;

	error = vfs_setfsops(fstyp, fusefs_vfsops_template, &fusefs_vfsops);
	if (error != 0) {
		zcmn_err(GLOBAL_ZONEID, CE_WARN,
		    "fusefsinit: bad vfs ops template");
		return (error);
	}

	error = vn_make_ops(name, fusefs_vnodeops_template, &fusefs_vnodeops);
	if (error != 0) {
		(void) vfs_freevfsops_by_type(fstyp);
		zcmn_err(GLOBAL_ZONEID, CE_WARN,
		    "fusefsinit: bad vnode ops template");
		return (error);
	}

	fusefsfstyp = fstyp;

	return (0);
}

void
fusefsfini()
{
	if (fusefs_vfsops) {
		(void) vfs_freevfsops_by_type(fusefsfstyp);
		fusefs_vfsops = NULL;
	}
	if (fusefs_vnodeops) {
		vn_freevnodeops(fusefs_vnodeops);
		fusefs_vnodeops = NULL;
	}
}

void
fusefs_free_fmi(fusemntinfo_t *fmi)
{
	if (fmi == NULL)
		return;

	if (fmi->fmi_zone != NULL)
		zone_rele(fmi->fmi_zone);

	if (fmi->fmi_ssn != NULL)
		fusefs_ssn_rele(fmi->fmi_ssn);

	avl_destroy(&fmi->fmi_hash_avl);
	rw_destroy(&fmi->fmi_hash_lk);
	cv_destroy(&fmi->fmi_statvfs_cv);
	mutex_destroy(&fmi->fmi_lock);

	kmem_free(fmi, sizeof (fusemntinfo_t));
}

/*
 * fusefs mount vfsop
 * Set up mount info record and attach it to vfs struct.
 */
static int
fusefs_mount(vfs_t *vfsp, vnode_t *mvp, struct mounta *uap, cred_t *cr)
{
	char		*data = uap->dataptr;
	int		error;
	fusenode_t 	*rtnp = NULL;	/* root of this fs */
	fusemntinfo_t 	*fmi = NULL;
	dev_t 		fusefs_dev;
	int 		version;
	int 		doorfd;
	zone_t		*zone = curproc->p_zone;
	zone_t		*mntzone = NULL;
	fusefs_ssn_t	*ssp = NULL;
	int		flags, sec;

	STRUCT_DECL(fusefs_args, args);		/* fusefs mount arguments */

	if ((error = secpolicy_fs_mount(cr, mvp, vfsp)) != 0)
		return (error);

	if (mvp->v_type != VDIR)
		return (ENOTDIR);

	/*
	 * get arguments
	 *
	 * uap->datalen might be different from sizeof (args)
	 * in a compatible situation.
	 */
	STRUCT_INIT(args, get_udatamodel());
	bzero(STRUCT_BUF(args), SIZEOF_STRUCT(fusefs_args, DATAMODEL_NATIVE));
	if (copyin(data, STRUCT_BUF(args), MIN(uap->datalen,
	    SIZEOF_STRUCT(fusefs_args, DATAMODEL_NATIVE))))
		return (EFAULT);

	/*
	 * Check mount program version
	 */
	version = STRUCT_FGET(args, version);
	if (version != FUSEFS_VERSION) {
		cmn_err(CE_WARN, "mount version mismatch:"
		    " kernel=%d, mount=%d\n",
		    FUSEFS_VERSION, version);
		return (EINVAL);
	}

	/*
	 * Deal with re-mount requests.
	 */
	if (uap->flags & MS_REMOUNT) {
		cmn_err(CE_WARN, "MS_REMOUNT not implemented");
		return (ENOTSUP);
	}

	/*
	 * Check for busy
	 */
	mutex_enter(&mvp->v_lock);
	if (!(uap->flags & MS_OVERLAY) &&
	    (mvp->v_count != 1 || (mvp->v_flag & VROOT))) {
		mutex_exit(&mvp->v_lock);
		return (EBUSY);
	}
	mutex_exit(&mvp->v_lock);

	/*
	 * Build a service object from the passed FD.
	 * It is returned with a "ref" (hold) for us.
	 * Release this hold: at errout below, or in
	 * fusefs_freevfs().
	 */
	doorfd = STRUCT_FGET(args, doorfd);
	error = fusefs_ssn_create(doorfd, &ssp);
	if (error) {
		cmn_err(CE_WARN, "invalid door handle %d (%d)\n",
		    doorfd, error);
		return (error);
	}

	/*
	 * Use "goto errout" from here on.
	 * See: ssp, fmi, rtnp, mntzone
	 */

	/*
	 * Determine the zone we're being mounted into.
	 */
	zone_hold(mntzone = zone);		/* start with this assumption */
	if (getzoneid() == GLOBAL_ZONEID) {
		zone_rele(mntzone);
		mntzone = zone_find_by_path(refstr_value(vfsp->vfs_mntpt));
		ASSERT(mntzone != NULL);
		if (mntzone != zone) {
			error = EBUSY;
			goto errout;
		}
	}

	/*
	 * Stop the mount from going any further if the zone is going away.
	 */
	if (zone_status_get(mntzone) >= ZONE_IS_SHUTTING_DOWN) {
		error = EBUSY;
		goto errout;
	}

	/*
	 * XXX: Trusted Extensions stuff?
	 */

	/* Prevent unload. */
	atomic_inc_32(&fusefs_mountcount);

	/*
	 * Create a mount record and link it to the vfs struct.
	 * No more possiblities for errors from here on.
	 * Tear-down of this stuff is in fusefs_free_fmi()
	 *
	 * Compare with NFS: nfsrootvp()
	 */
	fmi = kmem_zalloc(sizeof (*fmi), KM_SLEEP);

	mutex_init(&fmi->fmi_lock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&fmi->fmi_statvfs_cv, NULL, CV_DEFAULT, NULL);

	rw_init(&fmi->fmi_hash_lk, NULL, RW_DEFAULT, NULL);
	fusefs_init_hash_avl(&fmi->fmi_hash_avl);

	fmi->fmi_ssn = ssp;
	ssp = NULL;
	fmi->fmi_zone = mntzone;
	mntzone = NULL;

	/*
	 * Initialize option defaults
	 */
	fmi->fmi_flags	= FMI_LLOCK | FMI_LARGEF;
	fmi->fmi_acregmin = SEC2HR(FUSEFS_ACREGMIN);
	fmi->fmi_acregmax = SEC2HR(FUSEFS_ACREGMAX);
	fmi->fmi_acdirmin = SEC2HR(FUSEFS_ACDIRMIN);
	fmi->fmi_acdirmax = SEC2HR(FUSEFS_ACDIRMAX);

	/*
	 * All "generic" mount options have already been
	 * handled in vfs.c:domount() - see mntopts stuff.
	 * Query generic options using vfs_optionisset().
	 */
	if (vfs_optionisset(vfsp, MNTOPT_INTR, NULL))
		fmi->fmi_flags |= FMI_INT;

	/*
	 * Get the mount options that come in as fusefs_args,
	 * starting with args.flags (FUSEFS_MF_xxx)
	 */
	flags = STRUCT_FGET(args, flags);
	fmi->fmi_uid 	= STRUCT_FGET(args, uid);
	fmi->fmi_gid 	= STRUCT_FGET(args, gid);
	fmi->fmi_fmode	= STRUCT_FGET(args, file_mode) & 0777;
	fmi->fmi_dmode	= STRUCT_FGET(args, dir_mode) & 0777;

	/*
	 * Hande the FUSEFS_MF_xxx flags.
	 */
	if (flags & FUSEFS_MF_NOAC)
		fmi->fmi_flags |= FMI_NOAC;
	if (flags & FUSEFS_MF_ACREGMIN) {
		sec = STRUCT_FGET(args, acregmin);
		if (sec < 0 || sec > FUSEFS_ACMINMAX)
			sec = FUSEFS_ACMINMAX;
		fmi->fmi_acregmin = SEC2HR(sec);
	}
	if (flags & FUSEFS_MF_ACREGMAX) {
		sec = STRUCT_FGET(args, acregmax);
		if (sec < 0 || sec > FUSEFS_ACMAXMAX)
			sec = FUSEFS_ACMAXMAX;
		fmi->fmi_acregmax = SEC2HR(sec);
	}
	if (flags & FUSEFS_MF_ACDIRMIN) {
		sec = STRUCT_FGET(args, acdirmin);
		if (sec < 0 || sec > FUSEFS_ACMINMAX)
			sec = FUSEFS_ACMINMAX;
		fmi->fmi_acdirmin = SEC2HR(sec);
	}
	if (flags & FUSEFS_MF_ACDIRMAX) {
		sec = STRUCT_FGET(args, acdirmax);
		if (sec < 0 || sec > FUSEFS_ACMAXMAX)
			sec = FUSEFS_ACMAXMAX;
		fmi->fmi_acdirmax = SEC2HR(sec);
	}

#if 0
	/*
	 * XXX - Todo: Enable or disable options based on
	 * the data return by fusefs_call_init
	 */

	/*
	 * We enable XATTR by default (via fusefs_mntopts)
	 * but if the share does not support named streams,
	 * force the NOXATTR option (also clears XATTR).
	 * Caller will set or clear VFS_XATTR after this.
	 */
	if ((fmi->fmi_opts & FILE_NAMED_STREAMS) == 0)
		vfs_setmntopt(vfsp, MNTOPT_NOXATTR, NULL, 0);
#endif	/* XXX */

	/*
	 * Assign a unique device id to the mount
	 */
	mutex_enter(&fusefs_minor_lock);
	do {
		fusefs_minor = (fusefs_minor + 1) & MAXMIN32;
		fusefs_dev = makedevice(fusefs_major, fusefs_minor);
	} while (vfs_devismounted(fusefs_dev));
	mutex_exit(&fusefs_minor_lock);

	vfsp->vfs_dev	= fusefs_dev;
	vfs_make_fsid(&vfsp->vfs_fsid, fusefs_dev, fusefsfstyp);
	vfsp->vfs_data	= (caddr_t)fmi;
	vfsp->vfs_fstype = fusefsfstyp;
	vfsp->vfs_bsize = MAXBSIZE;
	vfsp->vfs_bcount = 0;

	fmi->fmi_vfsp	= vfsp;
	fusefs_zonelist_add(fmi);	/* undo in fusefs_freevfs */

	/*
	 * Create the root vnode, which we need in unmount
	 * for the call to fusefs_check_table(), etc.
	 * Release this hold in fusefs_unmount.
	 */
	rtnp = fusefs_node_findcreate(fmi, "/", 1, NULL, 0, 0,
	    &fusefs_fattr0);
	ASSERT(rtnp != NULL);
	rtnp->r_vnode->v_type = VDIR;
	rtnp->r_vnode->v_flag |= VROOT;
	fmi->fmi_root = rtnp;

	/*
	 * NFS does other stuff here too:
	 *   async worker threads
	 *   init kstats
	 *
	 * End of code from NFS nfsrootvp()
	 */
	return (0);

errout:
	vfsp->vfs_data = NULL;
	if (fmi != NULL)
		fusefs_free_fmi(fmi);

	if (mntzone != NULL)
		zone_rele(mntzone);

	if (ssp != NULL)
		fusefs_ssn_rele(ssp);

	return (error);
}

/*
 * vfs operations
 */
static int
fusefs_unmount(vfs_t *vfsp, int flag, cred_t *cr)
{
	fusemntinfo_t	*fmi;
	fusenode_t	*rtnp;

	fmi = VFTOFMI(vfsp);

	if (secpolicy_fs_unmount(cr, vfsp) != 0)
		return (EPERM);

	if ((flag & MS_FORCE) == 0) {
		fusefs_rflush(vfsp, cr);

		/*
		 * If there are any active vnodes on this file system,
		 * (other than the root vnode) then the file system is
		 * busy and can't be umounted.
		 */
		if (fusefs_check_table(vfsp, fmi->fmi_root))
			return (EBUSY);

		/*
		 * We normally hold a ref to the root vnode, so
		 * check for references beyond the one we expect:
		 *   fusemntinfo_t -> fmi_root
		 * Note that NFS does not hold the root vnode.
		 */
		if (fmi->fmi_root &&
		    fmi->fmi_root->r_vnode->v_count > 1)
			return (EBUSY);
	}

	/*
	 * common code for both forced and non-forced
	 *
	 * Setting VFS_UNMOUNTED prevents new operations.
	 * Operations already underway may continue,
	 * but not for long.
	 */
	vfsp->vfs_flag |= VFS_UNMOUNTED;

	/*
	 * Shutdown any outstanding I/O requests on this session.
	 * XXX: Maybe release the door too?
	 */
	fusefs_ssn_kill(fmi->fmi_ssn);

	/*
	 * If we hold the root VP (and we normally do)
	 * then it's safe to release it now.
	 */
	if (fmi->fmi_root) {
		rtnp = fmi->fmi_root;
		fmi->fmi_root = NULL;
		VN_RELE(rtnp->r_vnode);	/* release root vnode */
	}

	/*
	 * Remove all nodes from the node hash tables.
	 * This (indirectly) calls: fusefs_addfree, fuseinactive,
	 * which will try to flush dirty pages, etc. so
	 * don't destroy the underlying share just yet.
	 *
	 * Also, with a forced unmount, some nodes may
	 * remain active, and those will get cleaned up
	 * after their last vn_rele.
	 */
	fusefs_destroy_table(vfsp);

	/*
	 * Delete our kstats...
	 *
	 * Doing it here, rather than waiting until
	 * fusefs_freevfs so these are not visible
	 * after the unmount.
	 */
	if (fmi->fmi_io_kstats) {
		kstat_delete(fmi->fmi_io_kstats);
		fmi->fmi_io_kstats = NULL;
	}
	if (fmi->fmi_ro_kstats) {
		kstat_delete(fmi->fmi_ro_kstats);
		fmi->fmi_ro_kstats = NULL;
	}

	/*
	 * The rest happens in fusefs_freevfs()
	 */
	return (0);
}


/*
 * find root of fusefs
 */
static int
fusefs_root(vfs_t *vfsp, vnode_t **vpp)
{
	fusemntinfo_t	*fmi;
	vnode_t		*vp;

	fmi = VFTOFMI(vfsp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EPERM);

	if (fmi->fmi_flags & FMI_DEAD || vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	/*
	 * The root vp is created in mount and held
	 * until unmount, so this is paranoia.
	 */
	if (fmi->fmi_root == NULL)
		return (EIO);

	/* Just take a reference and return it. */
	vp = FUSETOV(fmi->fmi_root);
	VN_HOLD(vp);
	*vpp = vp;

	return (0);
}

/*
 * Get file system statistics.
 */
static int
fusefs_statvfs(vfs_t *vfsp, statvfs64_t *sbp)
{
	int		error;
	fusemntinfo_t	*fmi = VFTOFMI(vfsp);
	fusefs_ssn_t	*ssp = fmi->fmi_ssn;
	statvfs64_t	stvfs;
	hrtime_t now;

	if (curproc->p_zone != fmi->fmi_zone)
		return (EPERM);

	if (fmi->fmi_flags & FMI_DEAD || vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	mutex_enter(&fmi->fmi_lock);

	/*
	 * Use cached result if still valid.
	 */
recheck:
	now = gethrtime();
	if (now < fmi->fmi_statfstime) {
		error = 0;
		goto cache_hit;
	}

	/*
	 * FS attributes are stale, so someone
	 * needs to do an UP call to get them.
	 * Serialize here so only one thread
	 * does the OTW call.
	 */
	if (fmi->fmi_status & SM_STATUS_STATFS_BUSY) {
		fmi->fmi_status |= SM_STATUS_STATFS_WANT;
		if (!cv_wait_sig(&fmi->fmi_statvfs_cv, &fmi->fmi_lock)) {
			mutex_exit(&fmi->fmi_lock);
			return (EINTR);
		}
		/* Hope status is valid now. */
		goto recheck;
	}
	fmi->fmi_status |= SM_STATUS_STATFS_BUSY;
	mutex_exit(&fmi->fmi_lock);

	/*
	 * Do the OTW call.  Note: lock NOT held.
	 */
	bzero(&stvfs, sizeof (stvfs));
	error = fusefs_call_statvfs(ssp, &stvfs);
	if (error) {
		FUSEFS_DEBUG("statfs error=%d\n", error);
	} else {

		/*
		 * Set a few things the OTW call didn't get.
		 */
		stvfs.f_fsid = (unsigned long)vfsp->vfs_fsid.val[0];
		bcopy(fs_type_name, stvfs.f_basetype, FSTYPSZ);
		stvfs.f_flag	= vf_to_stf(vfsp->vfs_flag);
		stvfs.f_namemax	= FUSE_MAXFNAMELEN;

		/*
		 * Save the result, update lifetime
		 */
		now = gethrtime();
		fmi->fmi_statfstime = now +
		    (SM_MAX_STATFSTIME * (hrtime_t)NANOSEC);
		fmi->fmi_statvfsbuf = stvfs; /* struct assign! */
	}

	mutex_enter(&fmi->fmi_lock);
	if (fmi->fmi_status & SM_STATUS_STATFS_WANT)
		cv_broadcast(&fmi->fmi_statvfs_cv);
	fmi->fmi_status &= ~(SM_STATUS_STATFS_BUSY | SM_STATUS_STATFS_WANT);

	/*
	 * Copy the statvfs data to caller's buf.
	 * Note: struct assignment
	 */
cache_hit:
	if (error == 0)
		*sbp = fmi->fmi_statvfsbuf;
	mutex_exit(&fmi->fmi_lock);
	return (error);
}

static kmutex_t fusefs_syncbusy;

/*
 * Flush dirty fusefs files for file system vfsp.
 * If vfsp == NULL, all fusefs files are flushed.
 */
/*ARGSUSED*/
static int
fusefs_sync(vfs_t *vfsp, short flag, cred_t *cr)
{
	/*
	 * Cross-zone calls are OK here, since this translates to a
	 * VOP_PUTPAGE(B_ASYNC), which gets picked up by the right zone.
	 */
	if (!(flag & SYNC_ATTR) && mutex_tryenter(&fusefs_syncbusy) != 0) {
		fusefs_rflush(vfsp, cr);
		mutex_exit(&fusefs_syncbusy);
	}

	return (0);
}

/*
 * Initialization routine for VFS routines.  Should only be called once
 */
int
fusefs_vfsinit(void)
{
	mutex_init(&fusefs_syncbusy, NULL, MUTEX_DEFAULT, NULL);
	return (0);
}

/*
 * Shutdown routine for VFS routines.  Should only be called once
 */
void
fusefs_vfsfini(void)
{
	mutex_destroy(&fusefs_syncbusy);
}

void
fusefs_freevfs(vfs_t *vfsp)
{
	fusemntinfo_t    *fmi;

	/* free up the resources */
	fmi = VFTOFMI(vfsp);

	/*
	 * By this time we should have already deleted the
	 * fmi kstats in the unmount code.  If they are still around
	 * something is wrong
	 */
	ASSERT(fmi->fmi_io_kstats == NULL);

	fusefs_zonelist_remove(fmi);

	fusefs_free_fmi(fmi);

	/*
	 * Allow _fini() to succeed now, if so desired.
	 */
	atomic_dec_32(&fusefs_mountcount);
}
