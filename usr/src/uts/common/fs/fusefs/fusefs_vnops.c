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
 * Copyright 2012 Nexenta Systems, Inc.  All rights reserved.
 */

/*
 * Vnode operations for fusefs
 * (from NFS)
 */

#include <sys/systm.h>
#include <sys/cred.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/filio.h>
#include <sys/fs_subr.h>
#include <sys/errno.h>
#include <sys/dirent.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/uio.h>
#include <sys/kmem.h>
#include <sys/cmn_err.h>
#include <sys/vfs_opreg.h>
#include <sys/policy.h>

#include "fusefs.h"
#include "fusefs_calls.h"
#include "fusefs_node.h"
#include "fusefs_subr.h"

/*
 * Turning this on causes nodes to be created in the cache
 * during directory listings, normally avoiding a second
 * up-call to get attributes just after a readdir.
 */
int fusefs_fastlookup = 1;

/* local static function defines */

static int	fusefslookup_cache(vnode_t *, char *, int, vnode_t **,
			cred_t *);
static int	fusefslookup(vnode_t *dvp, char *nm, vnode_t **vpp, cred_t *cr,
			int cache_ok, caller_context_t *);
static int	fusefsrename(vnode_t *odvp, char *onm, vnode_t *ndvp, char *nnm,
			cred_t *cr, caller_context_t *);
static int	fusefssetattr(vnode_t *, struct vattr *, int, cred_t *);
static int	fusefs_accessx(void *, int, cred_t *);
static int	fusefs_readvdir(vnode_t *vp, uio_t *uio, cred_t *cr, int *eofp,
			caller_context_t *);
static void	fusefs_rele_fid(fusenode_t *);

/*
 * These are the vnode ops routines which implement the vnode interface to
 * the networked file system.  These routines just take their parameters,
 * make them look networkish by putting the right info into interface structs,
 * and then calling the appropriate remote routine(s) to do the work.
 *
 * Note on directory name lookup cacheing:  If we detect a stale fhandle,
 * we purge the directory cache relative to that vnode.  This way, the
 * user won't get burned by the cache repeatedly.  See <fusefs/fusenode.h> for
 * more details on fusenode locking.
 */

static int	fusefs_open(vnode_t **, int, cred_t *, caller_context_t *);
static int	fusefs_close(vnode_t *, int, int, offset_t, cred_t *,
			caller_context_t *);
static int	fusefs_read(vnode_t *, struct uio *, int, cred_t *,
			caller_context_t *);
static int	fusefs_write(vnode_t *, struct uio *, int, cred_t *,
			caller_context_t *);
static int	fusefs_getattr(vnode_t *, struct vattr *, int, cred_t *,
			caller_context_t *);
static int	fusefs_setattr(vnode_t *, struct vattr *, int, cred_t *,
			caller_context_t *);
static int	fusefs_access(vnode_t *, int, int, cred_t *,
			caller_context_t *);
static int	fusefs_fsync(vnode_t *, int, cred_t *, caller_context_t *);
static void	fusefs_inactive(vnode_t *, cred_t *, caller_context_t *);
static int	fusefs_lookup(vnode_t *, char *, vnode_t **, struct pathname *,
			int, vnode_t *, cred_t *, caller_context_t *,
			int *, pathname_t *);
static int	fusefs_create(vnode_t *, char *, struct vattr *, enum vcexcl,
			int, vnode_t **, cred_t *, int, caller_context_t *,
			vsecattr_t *);
static int	fusefs_remove(vnode_t *, char *, cred_t *, caller_context_t *,
			int);
static int	fusefs_rename(vnode_t *, char *, vnode_t *, char *, cred_t *,
			caller_context_t *, int);
static int	fusefs_mkdir(vnode_t *, char *, struct vattr *, vnode_t **,
			cred_t *, caller_context_t *, int, vsecattr_t *);
static int	fusefs_rmdir(vnode_t *, char *, vnode_t *, cred_t *,
			caller_context_t *, int);
static int	fusefs_readdir(vnode_t *, struct uio *, cred_t *, int *,
			caller_context_t *, int);
static int	fusefs_rwlock(vnode_t *, int, caller_context_t *);
static void	fusefs_rwunlock(vnode_t *, int, caller_context_t *);
static int	fusefs_seek(vnode_t *, offset_t, offset_t *,
			caller_context_t *);
static int	fusefs_frlock(vnode_t *, int, struct flock64 *, int, offset_t,
			struct flk_callback *, cred_t *, caller_context_t *);
static int	fusefs_space(vnode_t *, int, struct flock64 *, int, offset_t,
			cred_t *, caller_context_t *);
static int	fusefs_pathconf(vnode_t *, int, ulong_t *, cred_t *,
			caller_context_t *);
static int	fusefs_shrlock(vnode_t *, int, struct shrlock *, int, cred_t *,
			caller_context_t *);

/* Dummy function to use until correct function is ported in */
int noop_vnodeop() {
	return (0);
}

struct vnodeops *fusefs_vnodeops = NULL;

/*
 * Most unimplemented ops will return ENOSYS because of fs_nosys().
 * The only ops where that won't work are ACCESS (due to open(2)
 * failures) and ... (anything else left?)
 */
const fs_operation_def_t fusefs_vnodeops_template[] = {
	{ VOPNAME_OPEN,		{ .vop_open = fusefs_open } },
	{ VOPNAME_CLOSE,	{ .vop_close = fusefs_close } },
	{ VOPNAME_READ,		{ .vop_read = fusefs_read } },
	{ VOPNAME_WRITE,	{ .vop_write = fusefs_write } },
	{ VOPNAME_IOCTL,	{ .error = fs_nosys } }, /* fusefs_ioctl, */
	{ VOPNAME_GETATTR,	{ .vop_getattr = fusefs_getattr } },
	{ VOPNAME_SETATTR,	{ .vop_setattr = fusefs_setattr } },
	{ VOPNAME_ACCESS,	{ .vop_access = fusefs_access } },
	{ VOPNAME_LOOKUP,	{ .vop_lookup = fusefs_lookup } },
	{ VOPNAME_CREATE,	{ .vop_create = fusefs_create } },
	{ VOPNAME_REMOVE,	{ .vop_remove = fusefs_remove } },
	{ VOPNAME_LINK,		{ .error = fs_nosys } }, /* fusefs_link, */
	{ VOPNAME_RENAME,	{ .vop_rename = fusefs_rename } },
	{ VOPNAME_MKDIR,	{ .vop_mkdir = fusefs_mkdir } },
	{ VOPNAME_RMDIR,	{ .vop_rmdir = fusefs_rmdir } },
	{ VOPNAME_READDIR,	{ .vop_readdir = fusefs_readdir } },
	{ VOPNAME_SYMLINK,	{ .error = fs_nosys } }, /* fusefs_symlink, */
	{ VOPNAME_READLINK,	{ .error = fs_nosys } }, /* fusefs_readlink, */
	{ VOPNAME_FSYNC,	{ .vop_fsync = fusefs_fsync } },
	{ VOPNAME_INACTIVE,	{ .vop_inactive = fusefs_inactive } },
	{ VOPNAME_FID,		{ .error = fs_nosys } }, /* fusefs_fid, */
	{ VOPNAME_RWLOCK,	{ .vop_rwlock = fusefs_rwlock } },
	{ VOPNAME_RWUNLOCK,	{ .vop_rwunlock = fusefs_rwunlock } },
	{ VOPNAME_SEEK,		{ .vop_seek = fusefs_seek } },
	{ VOPNAME_FRLOCK,	{ .vop_frlock = fusefs_frlock } },
	{ VOPNAME_SPACE,	{ .vop_space = fusefs_space } },
	{ VOPNAME_REALVP,	{ .error = fs_nosys } }, /* fusefs_realvp, */
	{ VOPNAME_GETPAGE,	{ .error = fs_nosys } }, /* fusefs_getpage, */
	{ VOPNAME_PUTPAGE,	{ .error = fs_nosys } }, /* fusefs_putpage, */
	{ VOPNAME_MAP,		{ .error = fs_nosys } }, /* fusefs_map, */
	{ VOPNAME_ADDMAP,	{ .error = fs_nosys } }, /* fusefs_addmap, */
	{ VOPNAME_DELMAP,	{ .error = fs_nosys } }, /* fusefs_delmap, */
	{ VOPNAME_DUMP,		{ .error = fs_nosys } }, /* fusefs_dump, */
	{ VOPNAME_PATHCONF,	{ .vop_pathconf = fusefs_pathconf } },
	{ VOPNAME_PAGEIO,	{ .error = fs_nosys } }, /* fusefs_pageio, */
	{ VOPNAME_SETSECATTR,	{ .error = fs_nosys } },
	{ VOPNAME_GETSECATTR,	{ .error = noop_vnodeop } },
	{ VOPNAME_SHRLOCK,	{ .vop_shrlock = fusefs_shrlock } },
	{ NULL, NULL }
};

/* ARGSUSED */
static int
fusefs_open(vnode_t **vpp, int flag, cred_t *cr, caller_context_t *ct)
{
	fusenode_t	*np;
	vnode_t		*vp;
	uint64_t	fid, oldfid;
	int		rights;
	int		oldgenid;
	fusemntinfo_t	*fmi;
	fusefs_ssn_t	*ssp;
	cred_t		*oldcr;
	int		tmperror;
	int		error = 0;

	vp = *vpp;
	np = VTOFUSE(vp);
	fmi = VTOFMI(vp);
	ssp = fmi->fmi_ssn;

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	if (vp->v_type != VREG && vp->v_type != VDIR) { /* XXX VLNK? */
		FUSEFS_DEBUG("open eacces vtype=%d\n", vp->v_type);
		return (EACCES);
	}

	/*
	 * Get exclusive access to n_fid and related stuff.
	 * No returns after this until out.
	 */
	if (fusefs_rw_enter_sig(&np->r_lkserlock, RW_WRITER, FUSEINTR(vp)))
		return (EINTR);

	/*
	 * Keep track of the vnode type at first open.
	 * It may change later, and we need close to do
	 * cleanup for the type we opened.  Also deny
	 * open of new types until old type is closed.
	 */
	if (np->n_ovtype == VNON) {
		ASSERT(np->n_fidrefs == 0);
	} else if (np->n_ovtype != vp->v_type) {
		FUSEFS_DEBUG("open n_ovtype=%d v_type=%d\n",
		    np->n_ovtype, vp->v_type);
		error = EACCES;
		goto out;
	}

	/*
	 * If caller specified O_TRUNC/FTRUNC, then be sure to set
	 * FWRITE (to drive successful setattr(size=0) after open)
	 */
	if (flag & FTRUNC)
		flag |= FWRITE;

	/*
	 * If we already have it open, and the FID is still valid,
	 * check whether the rights are sufficient for FID reuse.
	 */
	if (np->n_fidrefs > 0 &&
	    np->n_ssgenid == ssp->ss_genid) {
		int upgrade = 0;

		if ((flag & FWRITE) &&
		    !(np->n_rights & FWRITE))
			upgrade = 1;
		if ((flag & FREAD) &&
		    !(np->n_rights & FREAD))
			upgrade = 1;
		if (!upgrade) {
			/*
			 *  the existing open is good enough
			 */
			np->n_fidrefs++;
			goto have_fid;
		}
	}
	rights = np->n_fidrefs ? np->n_rights : 0;

	/*
	 * we always ask for at least READ
	 */
	rights |= FREAD;
	if ((flag & FWRITE) != 0)
		rights |= FWRITE;

	if (vp->v_type == VDIR) {
		rights = FREAD;
		error = fusefs_call_opendir(ssp,
		    np->n_rplen, np->n_rpath, &fid);
	} else {
		error = fusefs_call_open(ssp,
		    np->n_rplen, np->n_rpath, rights, &fid);
	}
	if (error)
		goto out;

	/*
	 * We have a new FID and access rights.
	 */
	oldfid = np->n_fid;
	oldgenid = np->n_ssgenid;
	np->n_fid = fid;
	np->n_ssgenid = ssp->ss_genid;
	np->n_rights = rights;
	np->n_fidrefs++;
	if (np->n_fidrefs > 1 &&
	    oldgenid == ssp->ss_genid) {
		/*
		 * We already had it open (presumably because
		 * it was open with insufficient rights.)
		 * Close old wire-open.
		 */
		if (vp->v_type == VDIR)
			tmperror = fusefs_call_closedir(ssp, oldfid);
		else
			tmperror = fusefs_call_close(ssp, oldfid);
		if (tmperror)
			FUSEFS_DEBUG("error %d closing %s\n",
			    tmperror, np->n_rpath);
	}

	/*
	 * This thread did the open.
	 * Save our credentials too.
	 */
	mutex_enter(&np->r_statelock);
	oldcr = np->r_cred;
	np->r_cred = cr;
	crhold(cr);
	if (oldcr)
		crfree(oldcr);
	mutex_exit(&np->r_statelock);

have_fid:
	/*
	 * Keep track of the vnode type at first open.
	 * (see comments above)
	 */
	if (np->n_ovtype == VNON)
		np->n_ovtype = vp->v_type;

out:
	fusefs_rw_exit(&np->r_lkserlock);
	return (error);
}

/*ARGSUSED*/
static int
fusefs_close(vnode_t *vp, int flag, int count, offset_t offset, cred_t *cr,
	caller_context_t *ct)
{
	fusenode_t	*np;
	fusemntinfo_t	*fmi;

	np = VTOFUSE(vp);
	fmi = VTOFMI(vp);

	/*
	 * Don't "bail out" for VFS_UNMOUNTED here,
	 * as we want to do cleanup, etc.
	 */

	/*
	 * zone_enter(2) prevents processes from changing zones with our files
	 * open; if we happen to get here from the wrong zone we can't do
	 * anything over the wire.
	 */
	if (fmi->fmi_zone != curproc->p_zone) {
		/*
		 * We could attempt to clean up locks, except we're sure
		 * that the current process didn't acquire any locks on
		 * the file: any attempt to lock a file belong to another zone
		 * will fail, and one can't lock an FUSEFS file and then change
		 * zones, as that fails too.
		 *
		 * Returning an error here is the sane thing to do.  A
		 * subsequent call to VN_RELE() which translates to a
		 * fusefs_inactive() will clean up state: if the zone of the
		 * vnode's origin is still alive and kicking, an async worker
		 * thread will handle the request (from the correct zone), and
		 * everything (minus the final fusefs_getattr_otw() call) should
		 * be OK. If the zone is going away fusefs_async_inactive() will
		 * throw away cached pages inline.
		 */
		return (EIO);
	}

	/*
	 * If we are using local locking for this filesystem, then
	 * release all of the SYSV style record locks.  Otherwise,
	 * we are doing network locking and we need to release all
	 * of the network locks.  All of the locks held by this
	 * process on this file are released no matter what the
	 * incoming reference count is.
	 */
	if (fmi->fmi_flags & FMI_LLOCK) {
		pid_t pid = ddi_get_pid();
		cleanlocks(vp, pid, 0);
		cleanshares(vp, pid);
	}

	/*
	 * This (passed in) count is the ref. count from the
	 * user's file_t before the closef call (fio.c).
	 * We only care when the reference goes away.
	 */
	if (count > 1)
		return (0);

	/*
	 * Decrement the reference count for the FID
	 * and possibly do the OtW close.
	 *
	 * Exclusive lock for modifying n_fid stuff.
	 * Don't want this one ever interruptible.
	 */
	(void) fusefs_rw_enter_sig(&np->r_lkserlock, RW_WRITER, 0);

	fusefs_rele_fid(np);

	fusefs_rw_exit(&np->r_lkserlock);

	return (0);
}

/*
 * Helper for fusefs_close.  Decrement the reference count
 * for an FUSE-level file or directory ID, and when the last
 * reference for the fid goes away, do the OtW close.
 * Also called in fusefs_inactive (defensive cleanup).
 */
static void
fusefs_rele_fid(fusenode_t *np)
{
	fusefs_ssn_t	*ssp;
	cred_t		*oldcr;
	uint64_t	ofid;
	int		error;

	ssp = np->n_mount->fmi_ssn;
	error = 0;

	/* Make sure we serialize for n_dirseq use. */
	ASSERT(fusefs_rw_lock_held(&np->r_lkserlock, RW_WRITER));

	ASSERT(np->n_fidrefs > 0);
	if (--np->n_fidrefs)
		return;

	/*
	 * Note that vp->v_type may change if a remote node
	 * is deleted and recreated as a different type, and
	 * our getattr may change v_type accordingly.
	 * Now use n_ovtype to keep track of the v_type
	 * we had during open (see comments above).
	 */
	ofid = np->n_fid;
	np->n_fid = FUSE_FID_UNUSED;
	if (ofid != FUSE_FID_UNUSED &&
	    np->n_ssgenid == ssp->ss_genid) {
		if (np->n_ovtype == VDIR)
			error = fusefs_call_closedir(ssp, ofid);
		else
			error = fusefs_call_close(ssp, ofid);
		if (error) {
			FUSEFS_DEBUG("error %d closing %s\n",
			    error, np->n_rpath);
		}
	}

	/* Allow next open to use any v_type. */
	np->n_ovtype = VNON;

	/*
	 * Other "last close" stuff.
	 */
	mutex_enter(&np->r_statelock);
	if (np->n_flag & NATTRCHANGED)
		fusefs_attrcache_rm_locked(np);
	oldcr = np->r_cred;
	np->r_cred = NULL;
	mutex_exit(&np->r_statelock);
	if (oldcr != NULL)
		crfree(oldcr);
}

/* ARGSUSED */
static int
fusefs_read(vnode_t *vp, struct uio *uiop, int ioflag, cred_t *cr,
	caller_context_t *ct)
{
	struct vattr	va;
	fusenode_t	*np;
	fusemntinfo_t	*fmi;
	fusefs_ssn_t	*ssp;
	offset_t	endoff;
	ssize_t		past_eof;
	ssize_t		save_resid;
	uint32_t	len, rlen;
	uint32_t	maxlen;
	int		error = 0;

	np = VTOFUSE(vp);
	fmi = VTOFMI(vp);
	ssp = fmi->fmi_ssn;

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	ASSERT(fusefs_rw_lock_held(&np->r_rwlock, RW_READER));

	if (vp->v_type != VREG)
		return (EISDIR);

	if (uiop->uio_resid == 0)
		return (0);

	/*
	 * Like NFS3, just check for 63-bit overflow.
	 * Our FUSE layer takes care to return EFBIG
	 * when it has to fallback to a 32-bit call.
	 */
	endoff = uiop->uio_loffset + uiop->uio_resid;
	if (uiop->uio_loffset < 0 || endoff < 0)
		return (EINVAL);

	/* get vnode attributes from server */
	va.va_mask = AT_SIZE | AT_MTIME;
	if (error = fusefsgetattr(vp, &va, cr))
		return (error);

	/* Update mtime with mtime from server here? */

	/* if offset is beyond EOF, read nothing */
	if (uiop->uio_loffset >= va.va_size)
		return (0);

	/*
	 * Limit the read to the remaining file size.
	 * Do this by temporarily reducing uio_resid
	 * by the amount the lies beyoned the EOF.
	 */
	if (endoff > va.va_size) {
		past_eof = (ssize_t)(endoff - va.va_size);
		uiop->uio_resid -= past_eof;
	} else
		past_eof = 0;

	/* Shared lock for n_fid use in fuse_call_read */
	if (fusefs_rw_enter_sig(&np->r_lkserlock, RW_READER, FUSEINTR(vp)))
		return (EINTR);

	/* Make sure fid is valid. */
	if (np->n_fidrefs == 0 ||
	    np->n_fid == FUSE_FID_UNUSED ||
	    np->n_ssgenid != ssp->ss_genid) {
		error = ESTALE;
		goto serlk_out;
	}

	/*
	 * Do the I/O in maxlen chunks.
	 */
	maxlen = ssp->ss_max_iosize;
	save_resid = uiop->uio_resid;
	while (uiop->uio_resid > 0) {
		/* Lint: uio_resid may be 64-bits */
		rlen = len = (uint32_t)MIN(maxlen, uiop->uio_resid);
		error = fusefs_call_read(ssp,
		    np->n_fid, &rlen, uiop,
		    np->n_rplen, np->n_rpath);

		/*
		 * Note: the above called uio_update, so
		 * not doing that here as one might expect.
		 *
		 * Quit the loop either on error, or if we
		 * transferred less then requested.
		 */
		if (error || (rlen < len))
			break;
	}
	if (error && (save_resid != uiop->uio_resid)) {
		/*
		 * Stopped on an error after having
		 * successfully transferred data.
		 * Suppress this error.
		 */
		FUSEFS_DEBUG("error %d suppressed\n", error);
		error = 0;
	}

serlk_out:
	fusefs_rw_exit(&np->r_lkserlock);

	/* undo adjustment of resid */
	uiop->uio_resid += past_eof;

	return (error);
}


/* ARGSUSED */
static int
fusefs_write(vnode_t *vp, struct uio *uiop, int ioflag, cred_t *cr,
	caller_context_t *ct)
{
	struct vattr	va;
	fusenode_t	*np;
	fusemntinfo_t	*fmi;
	fusefs_ssn_t	*ssp;
	offset_t	endoff, limit;
	ssize_t		past_limit;
	ssize_t		save_resid;
	uint32_t	len, rlen;
	uint32_t	maxlen;
	int		error = 0;

	np = VTOFUSE(vp);
	fmi = VTOFMI(vp);
	ssp = fmi->fmi_ssn;

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	ASSERT(fusefs_rw_lock_held(&np->r_rwlock, RW_WRITER));

	if (vp->v_type != VREG)
		return (EISDIR);

	if (uiop->uio_resid == 0)
		return (0);

	/*
	 * Handle ioflag bits: (FAPPEND|FSYNC|FDSYNC)
	 */
	if (ioflag & (FAPPEND | FSYNC)) {
		fusefs_attrcache_remove(np);
		/* XXX: fusefs_vinvalbuf? */
	}
	if (ioflag & FAPPEND) {
		/*
		 * File size can be changed by another client
		 */
		va.va_mask = AT_SIZE;
		if (error = fusefsgetattr(vp, &va, cr))
			return (error);
		uiop->uio_loffset = va.va_size;
	}

	/*
	 * Like NFS3, just check for 63-bit overflow.
	 */
	endoff = uiop->uio_loffset + uiop->uio_resid;
	if (uiop->uio_loffset < 0 || endoff < 0)
		return (EINVAL);

	/*
	 * Check to make sure that the process will not exceed
	 * its limit on file size.  It is okay to write up to
	 * the limit, but not beyond.  Thus, the write which
	 * reaches the limit will be short and the next write
	 * will return an error.
	 *
	 * So if we're starting at or beyond the limit, EFBIG.
	 * Otherwise, temporarily reduce resid to the amount
	 * the falls after the limit.
	 */
	limit = uiop->uio_llimit;
	if (limit == RLIM64_INFINITY || limit > MAXOFFSET_T)
		limit = MAXOFFSET_T;
	if (uiop->uio_loffset >= limit)
		return (EFBIG);
	if (endoff > limit) {
		past_limit = (ssize_t)(endoff - limit);
		uiop->uio_resid -= past_limit;
	} else
		past_limit = 0;

	/* Shared lock for n_fid use in fusefs_call_write */
	if (fusefs_rw_enter_sig(&np->r_lkserlock, RW_READER, FUSEINTR(vp)))
		return (EINTR);

	/* Make sure fid is valid. */
	if (np->n_fidrefs == 0 ||
	    np->n_fid == FUSE_FID_UNUSED ||
	    np->n_ssgenid != ssp->ss_genid) {
		error = ESTALE;
		goto serlk_out;
	}

	/*
	 * Do the I/O in maxlen chunks.
	 */
	maxlen = ssp->ss_max_iosize;
	save_resid = uiop->uio_resid;
	while (uiop->uio_resid > 0) {
		/* Lint: uio_resid may be 64-bits */
		rlen = len = (uint32_t)MIN(maxlen, uiop->uio_resid);
		error = fusefs_call_write(ssp,
		    np->n_fid, &rlen, uiop,
		    np->n_rplen, np->n_rpath);
		/*
		 * Note: the above called uio_update, so
		 * not doing that here as one might expect.
		 *
		 * Quit the loop either on error, or if we
		 * transferred less then requested.
		 */
		if (error || (rlen < len))
			break;
	}
	if (error && (save_resid != uiop->uio_resid)) {
		/*
		 * Stopped on an error after having
		 * successfully transferred data.
		 * Suppress this error.
		 */
		FUSEFS_DEBUG("error %d suppressed\n", error);
		error = 0;
	}

	if (error == 0) {
		/*
		 * Update local notion of file size and
		 * invalidate cached attributes.
		 *
		 * NFS:
		 * Mark the attribute cache as timed out and
		 * set RWRITEATTR to indicate that the file
		 * was modified with a WRITE operation and
		 * that the attributes can not be trusted.
		 */
		mutex_enter(&np->r_statelock);
		fusefs_attrcache_rm_locked(np);
		/* XXX: np->r_flags |= RWRITEATTR; ? */
		np->n_flag |= NATTRCHANGED;
		if (uiop->uio_loffset > (offset_t)np->r_size)
			np->r_size = (len_t)uiop->uio_loffset;
		mutex_exit(&np->r_statelock);

		if (ioflag & (FSYNC|FDSYNC)) {
			/* Don't error the I/O if this fails. */
			(void) fusefs_call_flush(ssp, np->n_fid);
		}
	}

serlk_out:
	fusefs_rw_exit(&np->r_lkserlock);

	/* undo adjustment of resid */
	uiop->uio_resid += past_limit;

	return (error);
}


/*
 * Return either cached or remote attributes. If get remote attr
 * use them to check and invalidate caches, then cache the new attributes.
 */
/* ARGSUSED */
static int
fusefs_getattr(vnode_t *vp, struct vattr *vap, int flags, cred_t *cr,
	caller_context_t *ct)
{
	fusenode_t *np;
	fusemntinfo_t *fmi;

	fmi = VTOFMI(vp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	/*
	 * If it has been specified that the return value will
	 * just be used as a hint, and we are only being asked
	 * for size, fsid or rdevid, then return the client's
	 * notion of these values without checking to make sure
	 * that the attribute cache is up to date.
	 * The whole point is to avoid an over the wire GETATTR
	 * call.
	 */
	np = VTOFUSE(vp);
	if (flags & ATTR_HINT) {
		if (vap->va_mask ==
		    (vap->va_mask & (AT_SIZE | AT_FSID | AT_RDEV))) {
			mutex_enter(&np->r_statelock);
			if (vap->va_mask | AT_SIZE)
				vap->va_size = np->r_size;
			if (vap->va_mask | AT_FSID)
				vap->va_fsid = vp->v_vfsp->vfs_dev;
			if (vap->va_mask | AT_RDEV)
				vap->va_rdev = vp->v_rdev;
			mutex_exit(&np->r_statelock);
			return (0);
		}
	}

	return (fusefsgetattr(vp, vap, cr));
}

/* fusefsgetattr() in fusefs_client.c */

/*ARGSUSED4*/
static int
fusefs_setattr(vnode_t *vp, struct vattr *vap, int flags, cred_t *cr,
		caller_context_t *ct)
{
	vfs_t		*vfsp;
	fusemntinfo_t	*fmi;
	int		error;
	uint_t		mask;
	struct vattr	oldva;

	vfsp = vp->v_vfsp;
	fmi = VFTOFMI(vfsp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	mask = vap->va_mask;
	if (mask & AT_NOSET)
		return (EINVAL);

	if (vfsp->vfs_flag & VFS_RDONLY)
		return (EROFS);

	/*
	 * This is a _local_ access check so that only those
	 * allowed by the mount options can set attributes.
	 * Note that the file UID+GID can be different from
	 * the mount owner, and we need to check the _mount_
	 * owner here.  See _access_rwx
	 */
	bzero(&oldva, sizeof (oldva));
	oldva.va_mask = AT_TYPE | AT_MODE;
	error = fusefsgetattr(vp, &oldva, cr);
	if (error)
		return (error);
	oldva.va_mask |= AT_UID | AT_GID;
	oldva.va_uid = fmi->fmi_uid;
	oldva.va_gid = fmi->fmi_gid;

	error = secpolicy_vnode_setattr(cr, vp, vap, &oldva, flags,
	    fusefs_accessx, vp);
	if (error)
		return (error);

	if (mask & (AT_UID | AT_GID)) {
		FUSEFS_DEBUG("Can't set UID/GID on %s",
		    VTOFUSE(vp)->n_rpath);
		/*
		 * It might be more correct to return the
		 * error here, but that causes complaints
		 * when root extracts a cpio archive, etc.
		 * So ignore this error, and go ahead with
		 * the rest of the setattr work.
		 */
	}

	return (fusefssetattr(vp, vap, flags, cr));
}

/* ARGSUSED */
static int
fusefssetattr(vnode_t *vp, struct vattr *vap, int flags, cred_t *cr)
{
	fusenode_t	*np;
	fusemntinfo_t	*fmi;
	fusefs_ssn_t	*ssp;
	uint_t		mask;
	int		modified = 0;
	int		error = 0;
	struct timespec	*atime = NULL;
	struct timespec	*mtime = NULL;
	uid_t		uid = (uid_t)-1;
	gid_t		gid = (gid_t)-1;

	np = VTOFUSE(vp);
	fmi = VTOFMI(vp);
	ssp = fmi->fmi_ssn;
	mask = vap->va_mask;

	ASSERT(curproc->p_zone == VTOFMI(vp)->fmi_zone);

	/*
	 * If our caller is trying to set multiple attributes, they
	 * can make no assumption about what order they are done in.
	 * Here we try to do them in order of decreasing likelihood
	 * of failure, just to minimize the chance we'll wind up
	 * with a partially complete request.
	 */

	/* Shared lock for (possible) n_fid use. */
	if (fusefs_rw_enter_sig(&np->r_lkserlock, RW_READER, FUSEINTR(vp)))
		return (EINTR);

	if (mask & AT_SIZE) {
		/*
		 * If the new file size is less than what the client sees as
		 * the file size, then just change the size and invalidate
		 * the pages.
		 * I am commenting this code at present because the function
		 * fusefs_putapage() is not yet implemented.
		 */

		/*
		 * Set the file size to vap->va_size.
		 * Use the FID if we have one with VWRITE.
		 */
		if ((np->n_fidrefs > 0) &&
		    (np->n_fid != FUSE_FID_UNUSED) &&
		    (np->n_rights & FWRITE) &&
		    (np->n_ssgenid == ssp->ss_genid))
			error = fusefs_call_ftruncate(ssp,
			    np->n_fid, vap->va_size,
			    np->n_rplen, np->n_rpath);
		else
			error = EBADF;
		if (error == EBADF) {
			error = fusefs_call_ftruncate(ssp,
			    0, vap->va_size,
			    np->n_rplen, np->n_rpath);
		}
		if (error) {
			FUSEFS_DEBUG("setsize error %d file %s\n",
			    error, np->n_rpath);
		} else {
			mutex_enter(&np->r_statelock);
			np->r_size = vap->va_size;
			np->n_flag |= NATTRCHANGED;
			mutex_exit(&np->r_statelock);
			modified = 1;
		}
	}

	if (mask & AT_ATIME)
		atime = &vap->va_atime;
	if (mask & AT_MTIME)
		mtime = &vap->va_mtime;
	if (mask & (AT_ATIME | AT_MTIME)) {
		error = fusefs_call_utimes(ssp,
		    np->n_rplen, np->n_rpath,
		    atime, mtime);
		if (error)
			goto out;
		modified = 1;
	}

	if (mask & AT_MODE) {
		error = fusefs_call_chmod(ssp,
		    np->n_rplen, np->n_rpath,
		    vap->va_mode);
		if (error)
			goto out;
		modified = 1;
	}

	if (mask & AT_UID)
		uid = vap->va_uid;
	if (mask & AT_GID)
		gid = vap->va_gid;
	if (mask & (AT_UID | AT_GID)) {
		error = fusefs_call_chown(ssp,
		    np->n_rplen, np->n_rpath,
		    uid, gid);
		if (error)
			goto out;
		modified = 1;
	}

out:
	if (modified) {
		/*
		 * Invalidate attribute cache in case the server
		 * doesn't set exactly the attributes we asked.
		 */
		fusefs_attrcache_remove(np);
	}

	fusefs_rw_exit(&np->r_lkserlock);

	return (error);
}

/*
 * fusefs_access_rwx()
 * Common function for fusefs_access, etc.
 *
 * The security model implemented by the FS is unusual
 * due to the current "single user mounts" restriction:
 * All access under a given mount point uses the CIFS
 * credentials established by the owner of the mount.
 *
 * Most access checking is handled by the CIFS server,
 * but we need sufficient Unix access checks here to
 * prevent other local Unix users from having access
 * to objects under this mount that the uid/gid/mode
 * settings in the mount would not allow.
 *
 * With this model, there is a case where we need the
 * ability to do an access check before we have the
 * vnode for an object.  This function takes advantage
 * of the fact that the uid/gid/mode is per mount, and
 * avoids the need for a vnode.
 *
 * We still (sort of) need a vnode when we call
 * secpolicy_vnode_access, but that only uses
 * the vtype field, so we can use a pair of fake
 * vnodes that have only v_type filled in.
 */
static int
fusefs_access_rwx(vfs_t *vfsp, int vtype, int mode, cred_t *cr)
{
	/* See the secpolicy call below. */
	static const vnode_t tmpl_vdir = { .v_type = VDIR };
	static const vnode_t tmpl_vreg = { .v_type = VREG };
	vattr_t		va;
	vnode_t		*tvp;
	struct fusemntinfo *fmi = VFTOFMI(vfsp);
	int shift = 0;

	/*
	 * Build our (fabricated) vnode attributes.
	 */
	bzero(&va, sizeof (va));
	va.va_mask = AT_TYPE | AT_MODE | AT_UID | AT_GID;
	va.va_type = vtype;
	va.va_mode = (vtype == VDIR) ?
	    fmi->fmi_dmode : fmi->fmi_fmode;
	va.va_uid = fmi->fmi_uid;
	va.va_gid = fmi->fmi_gid;

	/*
	 * Disallow write attempts on read-only file systems,
	 * unless the file is a device or fifo node.  Note:
	 * Inline vn_is_readonly and IS_DEVVP here because
	 * we may not have a vnode ptr.  Original expr. was:
	 * (mode & VWRITE) && vn_is_readonly(vp) && !IS_DEVVP(vp))
	 */
	if ((mode & VWRITE) &&
	    (vfsp->vfs_flag & VFS_RDONLY) &&
	    !(vtype == VCHR || vtype == VBLK || vtype == VFIFO))
		return (EROFS);

	/*
	 * Disallow attempts to access mandatory lock files.
	 * Similarly, expand MANDLOCK here.
	 * XXX: not sure we need this.
	 */
	if ((mode & (VWRITE | VREAD | VEXEC)) &&
	    va.va_type == VREG && MANDMODE(va.va_mode))
		return (EACCES);

	/*
	 * Access check is based on only
	 * one of owner, group, public.
	 * If not owner, then check group.
	 * If not a member of the group,
	 * then check public access.
	 */
	if (crgetuid(cr) != va.va_uid) {
		shift += 3;
		if (!groupmember(va.va_gid, cr))
			shift += 3;
	}

	/*
	 * We need a vnode for secpolicy_vnode_access,
	 * but the only thing it looks at is v_type,
	 * so pass one of the templates above.
	 */
	tvp = (va.va_type == VDIR) ?
	    (vnode_t *)&tmpl_vdir :
	    (vnode_t *)&tmpl_vreg;

	return (secpolicy_vnode_access2(cr, tvp, va.va_uid,
	    va.va_mode << shift, mode));
}

/*
 * See fusefs_setattr
 */
static int
fusefs_accessx(void *arg, int mode, cred_t *cr)
{
	vnode_t *vp = arg;
	/*
	 * Note: The caller has checked the current zone,
	 * the FMI_DEAD and VFS_UNMOUNTED flags, etc.
	 * XXX: Call FUSE:access here?
	 */
	return (fusefs_access_rwx(vp->v_vfsp, vp->v_type, mode, cr));
}

/* ARGSUSED */
static int
fusefs_access(vnode_t *vp, int mode, int flags, cred_t *cr,
	caller_context_t *ct)
{
	vfs_t		*vfsp;
	fusemntinfo_t	*fmi;

	vfsp = vp->v_vfsp;
	fmi = VFTOFMI(vfsp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	return (fusefs_access_rwx(vfsp, vp->v_type, mode, cr));
}


/*
 * Flush local dirty pages to stable storage on the server.
 *
 * If FNODSYNC is specified, then there is nothing to do because
 * metadata changes are not cached on the client before being
 * sent to the server.
 */
/* ARGSUSED */
static int
fusefs_fsync(vnode_t *vp, int syncflag, cred_t *cr, caller_context_t *ct)
{
	fusemntinfo_t	*fmi;
	fusenode_t 	*np;
	int		error = 0;

	np = VTOFUSE(vp);
	fmi = VTOFMI(vp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	if ((syncflag & FNODSYNC) || IS_SWAPVP(vp))
		return (0);

	if ((syncflag & (FSYNC|FDSYNC)) == 0)
		return (0);

	/* Shared lock for n_fid use in _flush */
	if (fusefs_rw_enter_sig(&np->r_lkserlock, RW_READER, FUSEINTR(vp)))
		return (EINTR);

	if (np->n_fidrefs > 0)
		error = fusefs_call_flush(fmi->fmi_ssn, np->n_fid);

	fusefs_rw_exit(&np->r_lkserlock);

	return (error);
}

/*
 * Last reference to vnode went away.
 */
/* ARGSUSED */
static void
fusefs_inactive(vnode_t *vp, cred_t *cr, caller_context_t *ct)
{
	fusenode_t	*np;

	/*
	 * Don't "bail out" for VFS_UNMOUNTED here,
	 * as we want to do cleanup, etc.
	 * See also pcfs_inactive
	 */

	np = VTOFUSE(vp);

	/*
	 * If this is coming from the wrong zone, we let someone in the right
	 * zone take care of it asynchronously.  We can get here due to
	 * VN_RELE() being called from pageout() or fsflush().  This call may
	 * potentially turn into an expensive no-op if, for instance, v_count
	 * gets incremented in the meantime, but it's still correct.
	 */

	/*
	 * Defend against the possibility that higher-level callers
	 * might not correctly balance open and close calls.  If we
	 * get here with open references remaining, it means there
	 * was a missing VOP_CLOSE somewhere.  If that happens, do
	 * the close here so we don't "leak" FIDs on the server.
	 *
	 * Exclusive lock for modifying n_fid stuff.
	 * Don't want this one ever interruptible.
	 */
	(void) fusefs_rw_enter_sig(&np->r_lkserlock, RW_WRITER, 0);

	if (np->n_fidrefs != 0) {
		FUSEFS_DEBUG("open file: refs %d fid 0x%lx path %s\n",
		    np->n_fidrefs, (long)np->n_fid, np->n_rpath);
		/* Force last close. */
		np->n_fidrefs = 1;
		fusefs_rele_fid(np);
	}

	fusefs_rw_exit(&np->r_lkserlock);

	fusefs_addfree(np);
}

/*
 * Remote file system operations having to do with directory manipulation.
 */
/* ARGSUSED */
static int
fusefs_lookup(vnode_t *dvp, char *nm, vnode_t **vpp, struct pathname *pnp,
	int flags, vnode_t *rdir, cred_t *cr, caller_context_t *ct,
	int *direntflags, pathname_t *realpnp)
{
	vfs_t		*vfs;
	fusemntinfo_t	*fmi;
	fusenode_t	*dnp;
	int		error;

	vfs = dvp->v_vfsp;
	fmi = VFTOFMI(vfs);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EPERM);

	if (fmi->fmi_flags & FMI_DEAD || vfs->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	dnp = VTOFUSE(dvp);

	/* No extended attributes. */
	if (flags & LOOKUP_XATTR)
		return (EINVAL);

	if (fusefs_rw_enter_sig(&dnp->r_rwlock, RW_READER, FUSEINTR(dvp)))
		return (EINTR);

	error = fusefslookup(dvp, nm, vpp, cr, 1, ct);

	fusefs_rw_exit(&dnp->r_rwlock);

	return (error);
}

/* ARGSUSED */
static int
fusefslookup(vnode_t *dvp, char *nm, vnode_t **vpp, cred_t *cr,
	int cache_ok, caller_context_t *ct)
{
	int		error;
	int		supplen; /* supported length */
	vnode_t		*vp;
	fusenode_t	*np;
	fusenode_t	*dnp;
	fusemntinfo_t	*fmi;
	/* struct fuse_vc	*vcp; */
	const char	*name = (const char *)nm;
	int 		nmlen = strlen(nm);
	int 		rplen;
	fusefattr_t fa;

	fmi = VTOFMI(dvp);
	dnp = VTOFUSE(dvp);

	ASSERT(curproc->p_zone == fmi->fmi_zone);

	supplen = FUSE_MAXFNAMELEN;

	/*
	 * RWlock must be held, either reader or writer.
	 */
	ASSERT(dnp->r_rwlock.count != 0);

	/*
	 * If lookup is for "", just return dvp.
	 * No need to perform any access checks.
	 */
	if (nmlen == 0) {
		VN_HOLD(dvp);
		*vpp = dvp;
		return (0);
	}

	/*
	 * Can't do lookups in non-directories.
	 */
	if (dvp->v_type != VDIR)
		return (ENOTDIR);

	/*
	 * Need search permission in the directory.
	 */
	error = fusefs_access(dvp, VEXEC, 0, cr, ct);
	if (error)
		return (error);

	/*
	 * If lookup is for ".", just return dvp.
	 * Access check was done above.
	 */
	if (nmlen == 1 && name[0] == '.') {
		VN_HOLD(dvp);
		*vpp = dvp;
		return (0);
	}

	/*
	 * Now some sanity checks on the name.
	 * First check the length.
	 */
	if (nmlen > supplen)
		return (ENAMETOOLONG);

	/*
	 * Special handling for lookup of ".."
	 *
	 * We keep full pathnames (as seen on the server)
	 * so we can just trim off the last component to
	 * get the full pathname of the parent.  Note:
	 * We don't actually copy and modify, but just
	 * compute the trimmed length and pass that with
	 * the current dir path (not null terminated).
	 *
	 * We don't go over-the-wire to get attributes
	 * for ".." because we know it's a directory,
	 * and we can just leave the rest "stale"
	 * until someone does a getattr.
	 */
	if (nmlen == 2 && name[0] == '.' && name[1] == '.') {
		if (dvp->v_flag & VROOT) {
			/*
			 * Already at the root.  This can happen
			 * with directory listings at the root,
			 * which lookup "." and ".." to get the
			 * inode numbers.  Let ".." be the same
			 * as "." in the FS root.
			 */
			VN_HOLD(dvp);
			*vpp = dvp;
			return (0);
		}

		/*
		 * Find the parent path length.
		 */
		rplen = dnp->n_rplen;
		ASSERT(rplen > 0);
		while (--rplen >= 0) {
			if (dnp->n_rpath[rplen] == '/')
				break;
		}
		if (rplen <= 0) {
			/* Found our way to the root. */
			vp = FUSETOV(fmi->fmi_root);
			VN_HOLD(vp);
			*vpp = vp;
			return (0);
		}
		np = fusefs_node_findcreate(fmi,
		    dnp->n_rpath, rplen, NULL, 0, 0,
		    &fusefs_fattr0); /* force create */
		ASSERT(np != NULL);
		vp = FUSETOV(np);
		vp->v_type = VDIR;

		/* Success! */
		*vpp = vp;
		return (0);
	}

	/*
	 * Normal lookup of a name under this directory.
	 * Note we handled "", ".", ".." above.
	 */
	if (cache_ok) {
		/*
		 * The caller indicated that it's OK to use a
		 * cached result for this lookup, so try to
		 * reclaim a node from the fusefs node cache.
		 */
		error = fusefslookup_cache(dvp, nm, nmlen, &vp, cr);
		if (error)
			return (error);
		if (vp != NULL) {
			/* hold taken in lookup_cache */
			*vpp = vp;
			return (0);
		}
	}

	/*
	 * OK, go over-the-wire to get the attributes,
	 * then create the node.
	 */
	error = fusefs_call_getattr2(fmi->fmi_ssn,
	    dnp->n_rplen, dnp->n_rpath,
	    nmlen, name, &fa);
	if (error == ENOTDIR) {
		/*
		 * Lookup failed because this directory was
		 * removed or renamed by another client.
		 * Remove any cached attributes under it.
		 */
		fusefs_attrcache_remove(dnp);
		fusefs_attrcache_prune(dnp);
	}
	if (error)
		goto out;

	error = fusefs_nget(dvp, name, nmlen, &fa, &vp);
	if (error)
		goto out;

	/* Success! */
	*vpp = vp;

out:

	return (error);
}

/*
 * fusefslookup_cache
 *
 * Try to reclaim a node from the fusefs node cache.
 * Some statistics for DEBUG.
 *
 * This mechanism lets us avoid many of the five (or more)
 * OtW lookup calls per file seen with "ls -l" if we search
 * the fusefs node cache for recently inactive(ated) nodes.
 */
#ifdef DEBUG
int fusefs_lookup_cache_calls = 0;
int fusefs_lookup_cache_error = 0;
int fusefs_lookup_cache_miss = 0;
int fusefs_lookup_cache_stale = 0;
int fusefs_lookup_cache_hits = 0;
#endif /* DEBUG */

/* ARGSUSED */
static int
fusefslookup_cache(vnode_t *dvp, char *nm, int nmlen,
	vnode_t **vpp, cred_t *cr)
{
	struct vattr va;
	fusenode_t *dnp;
	fusenode_t *np;
	vnode_t *vp;
	int error;
	char sep;

	dnp = VTOFUSE(dvp);
	*vpp = NULL;

#ifdef DEBUG
	fusefs_lookup_cache_calls++;
#endif

	/*
	 * First make sure we can get attributes for the
	 * directory.  Cached attributes are OK here.
	 * If we removed or renamed the directory, this
	 * will return ENOENT.  If someone else removed
	 * this directory or file, we'll find out when we
	 * try to open or get attributes.
	 */
	va.va_mask = AT_TYPE | AT_MODE;
	error = fusefsgetattr(dvp, &va, cr);
	if (error) {
#ifdef DEBUG
		fusefs_lookup_cache_error++;
#endif
		return (error);
	}

	/*
	 * Passing NULL fusefattr here so we will
	 * just look, not create.
	 */
	sep = FUSEFS_DNP_SEP(dnp);
	np = fusefs_node_findcreate(dnp->n_mount,
	    dnp->n_rpath, dnp->n_rplen,
	    nm, nmlen, sep, NULL);
	if (np == NULL) {
#ifdef DEBUG
		fusefs_lookup_cache_miss++;
#endif
		return (0);
	}

	/*
	 * Found it.  Attributes still valid?
	 */
	vp = FUSETOV(np);
	if (np->r_attrtime <= gethrtime()) {
		/* stale */
#ifdef DEBUG
		fusefs_lookup_cache_stale++;
#endif
		VN_RELE(vp);
		return (0);
	}

	/*
	 * Success!
	 * Caller gets hold from fusefs_node_findcreate
	 */
#ifdef DEBUG
	fusefs_lookup_cache_hits++;
#endif
	*vpp = vp;
	return (0);
}

/* ARGSUSED */
static int
fusefs_create(vnode_t *dvp, char *nm, struct vattr *va, enum vcexcl exclusive,
	int mode, vnode_t **vpp, cred_t *cr, int lfaware, caller_context_t *ct,
	vsecattr_t *vsecp)
{
	int		error, cerror;
	vfs_t		*vfsp;
	vnode_t		*vp;
	fusenode_t	*dnp;
	fusemntinfo_t	*fmi;
	struct vattr	vattr;
	fusefattr_t	fa;
	const char *name = (const char *)nm;
	int		nmlen = strlen(nm);
	uint64_t	fid;

	vfsp = dvp->v_vfsp;
	fmi = VFTOFMI(vfsp);
	dnp = VTOFUSE(dvp);
	vp = NULL;

	if (curproc->p_zone != fmi->fmi_zone)
		return (EPERM);

	if (fmi->fmi_flags & FMI_DEAD || vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	/*
	 * Note: this may break mknod(2) calls to create a directory,
	 * but that's obscure use.  Some other filesystems do this.
	 * XXX: Later, redirect VDIR type here to _mkdir.
	 */
	if (va->va_type != VREG)
		return (EINVAL);

	/*
	 * If the pathname is "", just use dvp, no checks.
	 * Do this outside of the rwlock (like zfs).
	 */
	if (nmlen == 0) {
		VN_HOLD(dvp);
		*vpp = dvp;
		return (0);
	}

	/* Don't allow "." or ".." through here. */
	if ((nmlen == 1 && name[0] == '.') ||
	    (nmlen == 2 && name[0] == '.' && name[1] == '.'))
		return (EISDIR);

	/*
	 * We make a copy of the attributes because the caller does not
	 * expect us to change what va points to.
	 */
	vattr = *va;

	if (fusefs_rw_enter_sig(&dnp->r_rwlock, RW_WRITER, FUSEINTR(dvp)))
		return (EINTR);

	/*
	 * NFS needs to go over the wire, just to be sure whether the
	 * file exists or not.  Using a cached result is dangerous in
	 * this case when making a decision regarding existence.
	 *
	 * The FUSE protocol does NOT really need to go OTW here
	 * thanks to the expressive NTCREATE disposition values.
	 * Unfortunately, to do Unix access checks correctly,
	 * we need to know if the object already exists.
	 * When the object does not exist, we need VWRITE on
	 * the directory.  Note: fusefslookup() checks VEXEC.
	 */
	error = fusefslookup(dvp, nm, &vp, cr, 0, ct);
	if (error == 0) {
		/*
		 * The file already exists.  Error?
		 * NB: have a hold from fusefslookup
		 */
		if (exclusive == EXCL) {
			error = EEXIST;
			VN_RELE(vp);
			goto out;
		}
		/*
		 * Verify requested access.
		 */
		error = fusefs_access(vp, mode, 0, cr, ct);
		if (error) {
			VN_RELE(vp);
			goto out;
		}

		/*
		 * Truncate (if requested).
		 */
		if ((vattr.va_mask & AT_SIZE) && vattr.va_size == 0) {
			vattr.va_mask = AT_SIZE;
			error = fusefssetattr(vp, &vattr, 0, cr);
			if (error) {
				VN_RELE(vp);
				goto out;
			}
		}
		/* Success! */
#ifdef NOT_YET
		vnevent_create(vp, ct);
#endif
		*vpp = vp;
		goto out;
	}

	/*
	 * The file did not exist.  Need VWRITE in the directory.
	 */
	error = fusefs_access(dvp, VWRITE, 0, cr, ct);
	if (error)
		goto out;

	/*
	 * Now things get tricky.  We also need to check the
	 * requested open mode against the file we may create.
	 * See comments at fusefs_access_rwx
	 */
	error = fusefs_access_rwx(vfsp, VREG, mode, cr);
	if (error)
		goto out;

	/*
	 * Create (or open) a new child node.
	 * Cannot be "." and ".." now.
	 */
	error = fusefs_call_create(fmi->fmi_ssn,
	    dnp->n_rplen, dnp->n_rpath,
	    nmlen, name, mode, &fid);
	if (error)
		goto out;

	cerror = fusefs_call_close(fmi->fmi_ssn, fid);
	if (cerror)
		FUSEFS_DEBUG("error %d closing %s/%s\n",
		    cerror, dnp->n_rpath, name);

	/* Modified the directory. */
	fusefs_attr_touchdir(dnp);

	/*
	 * Get attributes we want for creating the node.
	 */
	error = fusefs_call_getattr2(fmi->fmi_ssn,
	    dnp->n_rplen, dnp->n_rpath,
	    nmlen, name, &fa);
	if (error)
		goto out;

	/* Create the node */
	error = fusefs_nget(dvp, name, nmlen, &fa, &vp);
	if (error)
		goto out;

	/* XXX invalidate pages if we truncated? */

	/* Success! */
	*vpp = vp;
	error = 0;

out:
	fusefs_rw_exit(&dnp->r_rwlock);
	return (error);
}

/* ARGSUSED */
static int
fusefs_remove(vnode_t *dvp, char *nm, cred_t *cr, caller_context_t *ct,
	int flags)
{
	int		error;
	vnode_t		*vp;
	fusenode_t	*np;
	fusenode_t	*dnp;
	/* enum fusefsstat status; */
	fusemntinfo_t	*fmi;

	fmi = VTOFMI(dvp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EPERM);

	if (fmi->fmi_flags & FMI_DEAD || dvp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	dnp = VTOFUSE(dvp);
	if (fusefs_rw_enter_sig(&dnp->r_rwlock, RW_WRITER, FUSEINTR(dvp)))
		return (EINTR);

	/*
	 * Verify access to the dirctory.
	 */
	error = fusefs_access(dvp, VWRITE|VEXEC, 0, cr, ct);
	if (error)
		goto out;

	/* Need the victim vnode. */
	error = fusefslookup(dvp, nm, &vp, cr, 0, ct);
	if (error)
		goto out;

	/* Never allow link/unlink directories on CIFS. */
	if (vp->v_type == VDIR) {
		VN_RELE(vp);
		error = EPERM;
		goto out;
	}

	/*
	 * Now we have the real reference count on the vnode
	 * Do we have the file open?
	 */
	np = VTOFUSE(vp);
	mutex_enter(&np->r_statelock);
	if ((vp->v_count > 1) && (np->n_fidrefs > 0)) {
		/*
		 * NFS does a rename on remove here.
		 * Do that for FUSE too?
		 */
		mutex_exit(&np->r_statelock);
		error = EBUSY;
	} else {
		fusefs_attrcache_rm_locked(np);
		mutex_exit(&np->r_statelock);

		error = fusefs_call_delete(fmi->fmi_ssn,
		    np->n_rplen, np->n_rpath);

		/*
		 * If the file should no longer exist, discard
		 * any cached attributes under this node, and
		 * remove it from the AVL tree.  If success,
		 * also mark directoy modified.
		 */
		switch (error) {
		case 0:
			/* Modified the directory. */
			fusefs_attr_touchdir(dnp);
			/* FALLTHROUGH */
		case ENOENT:
			fusefs_attrcache_prune(np);
			fusefs_rmhash(np);
			break;
		}
	}

	VN_RELE(vp);

out:
	fusefs_rw_exit(&dnp->r_rwlock);

	return (error);
}

/* ARGSUSED */
static int
fusefs_rename(vnode_t *odvp, char *onm, vnode_t *ndvp, char *nnm, cred_t *cr,
	caller_context_t *ct, int flags)
{
	/* vnode_t		*realvp; */

	if (curproc->p_zone != VTOFMI(odvp)->fmi_zone ||
	    curproc->p_zone != VTOFMI(ndvp)->fmi_zone)
		return (EPERM);

	if (VTOFMI(odvp)->fmi_flags & FMI_DEAD ||
	    VTOFMI(ndvp)->fmi_flags & FMI_DEAD ||
	    odvp->v_vfsp->vfs_flag & VFS_UNMOUNTED ||
	    ndvp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	return (fusefsrename(odvp, onm, ndvp, nnm, cr, ct));
}

/*
 * fusefsrename does the real work of renaming in FUSEFS
 */
/* ARGSUSED */
static int
fusefsrename(vnode_t *odvp, char *onm, vnode_t *ndvp, char *nnm, cred_t *cr,
	caller_context_t *ct)
{
	fusemntinfo_t	*fmi = VTOFMI(odvp);
	vnode_t		*nvp = NULL;
	vnode_t		*ovp = NULL;
	fusenode_t	*onp;
	fusenode_t	*nnp;
	fusenode_t	*odnp;
	fusenode_t	*ndnp;
	int		error;
	int		nvp_locked = 0;

	ASSERT(curproc->p_zone == fmi->fmi_zone);

	if (strcmp(onm, ".") == 0 || strcmp(onm, "..") == 0 ||
	    strcmp(nnm, ".") == 0 || strcmp(nnm, "..") == 0)
		return (EINVAL);

	/*
	 * Check that everything is on the same filesystem.
	 * vn_rename checks the fsid's, but in case we don't
	 * fill those in correctly, check here too.
	 */
	if (odvp->v_vfsp != ndvp->v_vfsp)
		return (EXDEV);

	odnp = VTOFUSE(odvp);
	ndnp = VTOFUSE(ndvp);

	/*
	 * Avoid deadlock here on old vs new directory nodes
	 * by always taking the locks in order of address.
	 * The order is arbitrary, but must be consistent.
	 */
	if (odnp < ndnp) {
		if (fusefs_rw_enter_sig(&odnp->r_rwlock, RW_WRITER,
		    FUSEINTR(odvp)))
			return (EINTR);
		if (fusefs_rw_enter_sig(&ndnp->r_rwlock, RW_WRITER,
		    FUSEINTR(ndvp))) {
			fusefs_rw_exit(&odnp->r_rwlock);
			return (EINTR);
		}
	} else {
		if (fusefs_rw_enter_sig(&ndnp->r_rwlock, RW_WRITER,
		    FUSEINTR(ndvp)))
			return (EINTR);
		if (fusefs_rw_enter_sig(&odnp->r_rwlock, RW_WRITER,
		    FUSEINTR(odvp))) {
			fusefs_rw_exit(&ndnp->r_rwlock);
			return (EINTR);
		}
	}

	/*
	 * No returns after this point (goto out)
	 */

	/*
	 * Need write access on source and target.
	 * Server takes care of most checks.
	 */
	error = fusefs_access(odvp, VWRITE|VEXEC, 0, cr, ct);
	if (error)
		goto out;
	if (odvp != ndvp) {
		error = fusefs_access(ndvp, VWRITE, 0, cr, ct);
		if (error)
			goto out;
	}

	/*
	 * Lookup the source name.  Must already exist.
	 */
	error = fusefslookup(odvp, onm, &ovp, cr, 0, ct);
	if (error)
		goto out;

	/*
	 * Lookup the target file.  If it exists, it needs to be
	 * checked to see whether it is a mount point and whether
	 * it is active (open).
	 */
	error = fusefslookup(ndvp, nnm, &nvp, cr, 0, ct);
	if (!error) {
		/*
		 * Target (nvp) already exists.  Check that it
		 * has the same type as the source.  The server
		 * will check this also, (and more reliably) but
		 * this lets us return the correct error codes.
		 */
		if (ovp->v_type == VDIR) {
			if (nvp->v_type != VDIR) {
				error = ENOTDIR;
				goto out;
			}
		} else {
			if (nvp->v_type == VDIR) {
				error = EISDIR;
				goto out;
			}
		}

		/*
		 * POSIX dictates that when the source and target
		 * entries refer to the same file object, rename
		 * must do nothing and exit without error.
		 */
		if (ovp == nvp) {
			error = 0;
			goto out;
		}

		/*
		 * Also must ensure the target is not a mount point,
		 * and keep mount/umount away until we're done.
		 */
		if (vn_vfsrlock(nvp)) {
			error = EBUSY;
			goto out;
		}
		nvp_locked = 1;
		if (vn_mountedvfs(nvp) != NULL) {
			error = EBUSY;
			goto out;
		}

		/*
		 * CIFS gives a SHARING_VIOLATION error when
		 * trying to rename onto an exising object,
		 * so try to remove the target first.
		 * (Only for files, not directories.)
		 */
		if (nvp->v_type == VDIR) {
			error = EEXIST;
			goto out;
		}

		/*
		 * Nodes that are "not active" here have v_count=2
		 * because vn_renameat (our caller) did a lookup on
		 * both the source and target before this call.
		 * Otherwise this similar to fusefs_remove.
		 */
		nnp = VTOFUSE(nvp);
		mutex_enter(&nnp->r_statelock);
		if ((nvp->v_count > 2) && (nnp->n_fidrefs > 0)) {
			/*
			 * The target file exists, is not the same as
			 * the source file, and is active.  Other FS
			 * implementations unlink the target here.
			 * For FUSE, we don't assume we can remove an
			 * open file.  Return an error instead.
			 */
			mutex_exit(&nnp->r_statelock);
			error = EBUSY;
			goto out;
		}

		/*
		 * Target file is not active. Try to remove it.
		 */
		fusefs_attrcache_rm_locked(nnp);
		mutex_exit(&nnp->r_statelock);

		error = fusefs_call_delete(fmi->fmi_ssn,
		    nnp->n_rplen, nnp->n_rpath);

		/*
		 * Similar to fusefs_remove
		 */
		switch (error) {
		case 0:
		case ENOENT:
		case ENOTDIR:
			fusefs_attrcache_prune(nnp);
			break;
		}

		if (error)
			goto out;
		/*
		 * OK, removed the target file.  Continue as if
		 * lookup target had failed (nvp == NULL).
		 */
		vn_vfsunlock(nvp);
		nvp_locked = 0;
		VN_RELE(nvp);
		nvp = NULL;
	} /* nvp */

	onp = VTOFUSE(ovp);
	fusefs_attrcache_remove(onp);

	error = fusefs_call_rename(fmi->fmi_ssn,
	    onp->n_rplen, onp->n_rpath,
	    nnp->n_rplen, nnp->n_rpath,
	    strlen(nnm), nnm);

	/*
	 * If the old name should no longer exist,
	 * discard any cached attributes under it.
	 */
	if (error == 0)
		fusefs_attrcache_prune(onp);

out:
	if (nvp) {
		if (nvp_locked)
			vn_vfsunlock(nvp);
		VN_RELE(nvp);
	}
	if (ovp)
		VN_RELE(ovp);

	fusefs_rw_exit(&odnp->r_rwlock);
	fusefs_rw_exit(&ndnp->r_rwlock);

	return (error);
}

/* ARGSUSED */
static int
fusefs_mkdir(vnode_t *dvp, char *nm, struct vattr *va, vnode_t **vpp,
	cred_t *cr, caller_context_t *ct, int flags, vsecattr_t *vsecp)
{
	vnode_t		*vp;
	struct fusenode	*dnp = VTOFUSE(dvp);
	struct fusemntinfo *fmi = VTOFMI(dvp);
	fusefattr_t	fa;
	const char		*name = (const char *) nm;
	int		nmlen = strlen(name);
	int		error;

	if (curproc->p_zone != fmi->fmi_zone)
		return (EPERM);

	if (fmi->fmi_flags & FMI_DEAD || dvp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	if ((nmlen == 1 && name[0] == '.') ||
	    (nmlen == 2 && name[0] == '.' && name[1] == '.'))
		return (EEXIST);

	if (fusefs_rw_enter_sig(&dnp->r_rwlock, RW_WRITER, FUSEINTR(dvp)))
		return (EINTR);

	/*
	 * Require write access in the containing directory.
	 */
	error = fusefs_access(dvp, VWRITE, 0, cr, ct);
	if (error)
		goto out;

	error = fusefs_call_mkdir(fmi->fmi_ssn,
	    dnp->n_rplen, dnp->n_rpath,
	    nmlen, name);
	if (error)
		goto out;

	/* Modified the directory. */
	fusefs_attr_touchdir(dnp);

	error = fusefs_call_getattr2(fmi->fmi_ssn,
	    dnp->n_rplen, dnp->n_rpath,
	    nmlen, name, &fa);
	if (error)
		goto out;

	error = fusefs_nget(dvp, name, nmlen, &fa, &vp);
	if (error)
		goto out;

	/* Success! */
	*vpp = vp;
	error = 0;
out:
	fusefs_rw_exit(&dnp->r_rwlock);
	return (error);
}

/* ARGSUSED */
static int
fusefs_rmdir(vnode_t *dvp, char *nm, vnode_t *cdir, cred_t *cr,
	caller_context_t *ct, int flags)
{
	vnode_t		*vp = NULL;
	int		vp_locked = 0;
	struct fusemntinfo *fmi = VTOFMI(dvp);
	struct fusenode	*dnp = VTOFUSE(dvp);
	struct fusenode	*np;
	int		error;

	if (curproc->p_zone != fmi->fmi_zone)
		return (EPERM);

	if (fmi->fmi_flags & FMI_DEAD || dvp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	if (fusefs_rw_enter_sig(&dnp->r_rwlock, RW_WRITER, FUSEINTR(dvp)))
		return (EINTR);

	/*
	 * Require w/x access in the containing directory.
	 * Server handles all other access checks.
	 */
	error = fusefs_access(dvp, VEXEC|VWRITE, 0, cr, ct);
	if (error)
		goto out;

	/*
	 * First lookup the entry to be removed.
	 */
	error = fusefslookup(dvp, nm, &vp, cr, 0, ct);
	if (error)
		goto out;
	np = VTOFUSE(vp);

	/*
	 * Disallow rmdir of "." or current dir, or the FS root.
	 * Also make sure it's a directory, not a mount point,
	 * and lock to keep mount/umount away until we're done.
	 */
	if ((vp == dvp) || (vp == cdir) || (vp->v_flag & VROOT)) {
		error = EINVAL;
		goto out;
	}
	if (vp->v_type != VDIR) {
		error = ENOTDIR;
		goto out;
	}
	if (vn_vfsrlock(vp)) {
		error = EBUSY;
		goto out;
	}
	vp_locked = 1;
	if (vn_mountedvfs(vp) != NULL) {
		error = EBUSY;
		goto out;
	}

	fusefs_attrcache_remove(np);
	error = fusefs_call_rmdir(fmi->fmi_ssn,
	    np->n_rplen, np->n_rpath);

	/*
	 * Similar to fusefs_remove
	 */
	switch (error) {
	case 0:
		/* Modified the directory. */
		fusefs_attr_touchdir(dnp);
		/* FALLTHROUGH */
	case ENOENT:
	case ENOTDIR:
		fusefs_attrcache_prune(np);
		fusefs_rmhash(np);
		break;
	}

out:
	if (vp) {
		if (vp_locked)
			vn_vfsunlock(vp);
		VN_RELE(vp);
	}
	fusefs_rw_exit(&dnp->r_rwlock);

	return (error);
}


/* ARGSUSED */
static int
fusefs_readdir(vnode_t *vp, struct uio *uiop, cred_t *cr, int *eofp,
	caller_context_t *ct, int flags)
{
	struct fusenode	*np = VTOFUSE(vp);
	int		error = 0;
	fusemntinfo_t	*fmi;

	fmi = VTOFMI(vp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	/*
	 * Require read access in the directory.
	 */
	error = fusefs_access(vp, VREAD, 0, cr, ct);
	if (error)
		return (error);

	ASSERT(fusefs_rw_lock_held(&np->r_rwlock, RW_READER));

	/*
	 * I am serializing the entire readdir opreation
	 * now since we have not yet implemented readdir
	 * cache. This fix needs to be revisited once
	 * we implement readdir cache.
	 */
	if (fusefs_rw_enter_sig(&np->r_lkserlock, RW_WRITER, FUSEINTR(vp)))
		return (EINTR);

	error = fusefs_readvdir(vp, uiop, cr, eofp, ct);

	fusefs_rw_exit(&np->r_lkserlock);

	return (error);
}

/* ARGSUSED */
static int
fusefs_readvdir(vnode_t *vp, uio_t *uio, cred_t *cr, int *eofp,
	caller_context_t *ct)
{
	/* Largest possible dirent size. */
	static const size_t dbufsiz = DIRENT64_RECLEN(FUSE_MAXFNAMELEN);
	fusefattr_t	fa;
	fusenode_t	*np = VTOFUSE(vp);
	fusemntinfo_t	*fmi = VTOFMI(vp);
	vnode_t		*newvp;
	struct dirent64 *dp;
	int		offset; /* yes, 32 bits */
	int		eof, error, nmlen;
	unsigned short	reclen;

	ASSERT(curproc->p_zone == fmi->fmi_zone);

	/* Make sure we serialize for n_dirseq use. */
	ASSERT(fusefs_rw_lock_held(&np->r_lkserlock, RW_WRITER));

	/*
	 * Make sure we've been opened.
	 */
	if (np->n_fidrefs == 0 ||
	    np->n_fid == FUSE_FID_UNUSED)
		return (EBADF);

	/* Special case an EOF offset (see below). */
	if (uio->uio_loffset == INT32_MAX)
		return (0);

	/* Check for overflow of (32-bit) directory offset. */
	if (uio->uio_loffset < 0 || uio->uio_loffset > INT32_MAX ||
	    (uio->uio_loffset + uio->uio_resid) > INT32_MAX)
		return (EINVAL);

	/* Require space for at least one dirent. */
	if (uio->uio_resid < dbufsiz)
		return (EINVAL);

	FUSEFS_DEBUG("dirname='%s'\n", np->n_rpath);
	dp = kmem_alloc(dbufsiz, KM_SLEEP);

	offset = uio->uio_offset;
	FUSEFS_DEBUG("in: offset=%d, resid=%d\n",
	    (int)uio->uio_offset, (int)uio->uio_resid);
	eof = error = 0;

	/*
	 * While there's room in the caller's buffer:
	 *	get a directory entry from FUSE,
	 *	convert to a dirent, copyout.
	 * We stop when there is no longer room for a
	 * maximum sized dirent because we must decide
	 * before we know anything about the next entry.
	 */
	while (!eof && uio->uio_resid >= dbufsiz) {
		error = fusefs_call_readdir(fmi->fmi_ssn,
		    np->n_fid, offset,
		    &fa, dp, &eof);
		if (error != 0)
			break;

		FUSEFS_DEBUG("ent=%s nxtoff=%d",
		    dp->d_name, (int)dp->d_off);

		/*
		 * Note: Computing reclen depends on alignment, etc.
		 * so the door svc puts nmlen in d_reclen and this
		 * uses it to compute the correct d_reclen.
		 */
		nmlen = dp->d_reclen;
		reclen = DIRENT64_RECLEN(nmlen);
		if (reclen > dbufsiz) {
			error = EIO;
			break;
		}
		dp->d_reclen = reclen;

		/*
		 * We don't get the stat info with . or ..
		 * but fusefslookup can get it for us.
		 */
		newvp = NULL;
		if (dp->d_name[0] == '.' && (nmlen == 1 ||
		    dp->d_name[1] == '.' && nmlen == 2)) {
			/* name is "." or ".." */
			(void) fusefslookup(vp,
			    dp->d_name, &newvp, cr, 1, ct);
		} else if (fusefs_fastlookup) {
			(void) fusefs_nget(vp,
			    dp->d_name, nmlen, &fa, &newvp);
		}
		if (newvp != NULL) {
			dp->d_ino = VTOFUSE(newvp)->n_ino;
			VN_RELE(newvp);
			newvp = NULL;
		} else {
			dp->d_ino = fusefs_getino(np, dp->d_name, nmlen);
		}

		/*
		 * What's the next offset?  Can't be zero,
		 * so we'll replace that with INT32_MAX.
		 */
		offset = dp->d_off;
		if (offset == 0)
			eof = 1;

		/*
		 * We want d_off == zero on the last entry.
		 * Also set the special EOF offset.
		 */
		if (eof) {
			dp->d_off = 0;
			offset = INT32_MAX;
		}

		error = uiomove(dp, dp->d_reclen, UIO_READ, uio);
		if (error)
			break;

		/*
		 * Note that uiomove updates uio_offset,
		 * but we want what was in d_off.
		 */
		uio->uio_offset = offset;
	}
	if (eofp)
		*eofp = eof;

	FUSEFS_DEBUG("out: offset=%d, resid=%d\n",
	    (int)uio->uio_offset, (int)uio->uio_resid);

	kmem_free(dp, dbufsiz);
	return (error);
}


/*
 * The pair of functions VOP_RWLOCK, VOP_RWUNLOCK
 * are optional functions that are called by:
 *    getdents, before/after VOP_READDIR
 *    pread, before/after ... VOP_READ
 *    pwrite, before/after ... VOP_WRITE
 *    (other places)
 *
 * Careful here: None of the above check for any
 * error returns from VOP_RWLOCK / VOP_RWUNLOCK!
 * In fact, the return value from _rwlock is NOT
 * an error code, but V_WRITELOCK_TRUE / _FALSE.
 *
 * Therefore, it's up to _this_ code to make sure
 * the lock state remains balanced, which means
 * we can't "bail out" on interrupts, etc.
 */

/* ARGSUSED2 */
static int
fusefs_rwlock(vnode_t *vp, int write_lock, caller_context_t *ctp)
{
	fusenode_t	*np = VTOFUSE(vp);

	if (!write_lock) {
		(void) fusefs_rw_enter_sig(&np->r_rwlock, RW_READER, FALSE);
		return (V_WRITELOCK_FALSE);
	}


	(void) fusefs_rw_enter_sig(&np->r_rwlock, RW_WRITER, FALSE);
	return (V_WRITELOCK_TRUE);
}

/* ARGSUSED */
static void
fusefs_rwunlock(vnode_t *vp, int write_lock, caller_context_t *ctp)
{
	fusenode_t	*np = VTOFUSE(vp);

	fusefs_rw_exit(&np->r_rwlock);
}


/* ARGSUSED */
static int
fusefs_seek(vnode_t *vp, offset_t ooff, offset_t *noffp, caller_context_t *ct)
{
	fusemntinfo_t	*fmi;

	fmi = VTOFMI(vp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EPERM);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	/*
	 * Because we stuff the readdir cookie into the offset field
	 * someone may attempt to do an lseek with the cookie which
	 * we want to succeed.
	 */
	if (vp->v_type == VDIR)
		return (0);

	/* Like NFS3, just check for 63-bit overflow. */
	if (*noffp < 0)
		return (EINVAL);

	return (0);
}


static int
fusefs_frlock(vnode_t *vp, int cmd, struct flock64 *bfp, int flag,
	offset_t offset, struct flk_callback *flk_cbp, cred_t *cr,
	caller_context_t *ct)
{
	if (curproc->p_zone != VTOFMI(vp)->fmi_zone)
		return (EIO);

	if (VTOFMI(vp)->fmi_flags & FMI_LLOCK)
		return (fs_frlock(vp, cmd, bfp, flag, offset, flk_cbp, cr, ct));
	else {
		/* XXX: Call FUSE lock stuff? */
		return (ENOSYS);
	}
}

/*
 * Free storage space associated with the specified vnode.  The portion
 * to be freed is specified by bfp->l_start and bfp->l_len (already
 * normalized to a "whence" of 0).
 *
 * Called by fcntl(fd, F_FREESP, lkp) for libc:ftruncate, etc.
 */
/* ARGSUSED */
static int
fusefs_space(vnode_t *vp, int cmd, struct flock64 *bfp, int flag,
	offset_t offset, cred_t *cr, caller_context_t *ct)
{
	int		error;
	fusemntinfo_t	*fmi;

	fmi = VTOFMI(vp);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	/* Caller (fcntl) has checked v_type */
	ASSERT(vp->v_type == VREG);
	if (cmd != F_FREESP)
		return (EINVAL);

	/*
	 * Like NFS3, no 32-bit offset checks here.
	 * Our FUSE layer takes care to return EFBIG
	 * when it has to fallback to a 32-bit call.
	 */

	error = convoff(vp, bfp, 0, offset);
	if (!error) {
		ASSERT(bfp->l_start >= 0);
		if (bfp->l_len == 0) {
			struct vattr va;

			/*
			 * ftruncate should not change the ctime and
			 * mtime if we truncate the file to its
			 * previous size.
			 */
			va.va_mask = AT_SIZE;
			error = fusefsgetattr(vp, &va, cr);
			if (error || va.va_size == bfp->l_start)
				return (error);
			va.va_mask = AT_SIZE;
			va.va_size = bfp->l_start;
			error = fusefssetattr(vp, &va, 0, cr);
		} else
			error = EINVAL;
	}

	return (error);
}

/* ARGSUSED */
static int
fusefs_pathconf(vnode_t *vp, int cmd, ulong_t *valp, cred_t *cr,
	caller_context_t *ct)
{
	vfs_t *vfs;
	fusemntinfo_t *fmi;

	vfs = vp->v_vfsp;
	fmi = VFTOFMI(vfs);

	if (curproc->p_zone != fmi->fmi_zone)
		return (EIO);

	if (fmi->fmi_flags & FMI_DEAD || vp->v_vfsp->vfs_flag & VFS_UNMOUNTED)
		return (EIO);

	switch (cmd) {
	case _PC_FILESIZEBITS:
		if (fmi->fmi_flags & FMI_LARGEF)
			*valp = 64;
		else
			*valp = 32;
		break;

	case _PC_LINK_MAX:
		/* We only ever report one link to an object */
		*valp = 1;
		break;

	case _PC_SYMLINK_MAX:	/* No symlinks until we do Unix extensions */
		*valp = 0;
		break;

	case _PC_TIMESTAMP_RESOLUTION:
		/*
		 * XXX: Conservative, 1 microsecond time.
		 * (1000 nanoseconds).
		 */
		*valp = 1000L;
		break;

	default:
		return (fs_pathconf(vp, cmd, valp, cr, ct));
	}
	return (0);
}


/*
 * XXX
 * This op should eventually support PSARC 2007/268.
 */
static int
fusefs_shrlock(vnode_t *vp, int cmd, struct shrlock *shr, int flag, cred_t *cr,
	caller_context_t *ct)
{
	if (curproc->p_zone != VTOFMI(vp)->fmi_zone)
		return (EIO);

	if (VTOFMI(vp)->fmi_flags & FMI_LLOCK)
		return (fs_shrlock(vp, cmd, shr, flag, cr, ct));
	else {
		/* XXX: Call FUSE lock stuff? */
		return (ENOSYS);
	}
}
