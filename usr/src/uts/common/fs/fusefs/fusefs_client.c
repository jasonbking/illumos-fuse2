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

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/thread.h>
#include <sys/t_lock.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/vfs.h>
#include <sys/errno.h>
#include <sys/buf.h>
#include <sys/stat.h>
#include <sys/cred.h>
#include <sys/kmem.h>
#include <sys/debug.h>
#include <sys/vmsystm.h>
#include <sys/flock.h>
#include <sys/share.h>
#include <sys/cmn_err.h>
#include <sys/tiuser.h>
#include <sys/sysmacros.h>
#include <sys/callb.h>
#include <sys/kstat.h>
#include <sys/signal.h>
#include <sys/list.h>
#include <sys/mode.h>
#include <sys/zone.h>

#include "fusefs.h"
#include "fusefs_calls.h"
#include "fusefs_node.h"
#include "fusefs_subr.h"

#include <vm/hat.h>
#include <vm/as.h>
#include <vm/page.h>
#include <vm/pvn.h>
#include <vm/seg.h>
#include <vm/seg_map.h>
#include <vm/seg_vn.h>

static int fusefs_getattr_cache(vnode_t *, fusefattr_t *);
static void fattr_to_vattr(vnode_t *, fusefattr_t *, vattr_t *);

/*
 * The following code provide zone support in order to perform an action
 * for each fusefs mount in a zone.  This is also where we would add
 * per-zone globals and kernel threads for the fusefs module (since
 * they must be terminated by the shutdown callback).
 */

struct fmi_globals {
	kmutex_t	smg_lock;  /* lock protecting smg_list */
	list_t		smg_list;  /* list of FUSEFS mounts in zone */
	boolean_t	smg_destructor_called;
};
typedef struct fmi_globals fmi_globals_t;

static zone_key_t fmi_list_key;

/*
 * Attributes caching:
 *
 * Attributes are cached in the fusenode in struct vattr form.
 * There is a time associated with the cached attributes (r_attrtime)
 * which tells whether the attributes are valid. The time is initialized
 * to the difference between current time and the modify time of the vnode
 * when new attributes are cached. This allows the attributes for
 * files that have changed recently to be timed out sooner than for files
 * that have not changed for a long time. There are minimum and maximum
 * timeout values that can be set per mount point.
 */

/*
 * Validate caches by checking cached attributes. If they have timed out
 * get the attributes from the server and compare mtimes. If mtimes are
 * different purge all caches for this vnode.
 */
int
fusefs_validate_caches(
	struct vnode *vp,
	cred_t *cr)
{
	struct vattr va;

	va.va_mask = AT_SIZE;
	return (fusefsgetattr(vp, &va, cr));
}

/*
 * Purge all of the various data caches.
 */
/*ARGSUSED*/
void
fusefs_purge_caches(struct vnode *vp)
{
#if 0	/* not yet: mmap support */
	/*
	 * NFS: Purge the DNLC for this vp,
	 * Clear any readdir state bits,
	 * the readlink response cache, ...
	 */
	fusenode_t *np = VTOFUSE(vp);

	/*
	 * Flush the page cache.
	 */
	if (vn_has_cached_data(vp)) {
		(void) VOP_PUTPAGE(vp, (u_offset_t)0, 0, B_INVAL, cr, NULL);
	}
#endif	/* not yet */
}

/*
 * Check the attribute cache to see if the new attributes match
 * those cached.  If they do, the various `data' caches are
 * considered to be good.  Otherwise, purge the cached data.
 */
void
fusefs_cache_check(
	struct vnode *vp,
	fusefattr_t *fap)
{
	fusenode_t *np;
	int purge_data = 0;

	np = VTOFUSE(vp);
	mutex_enter(&np->r_statelock);

	/*
	 * Compare with NFS macro: CACHE_VALID
	 * If the mtime or size has changed,
	 * purge cached data.
	 */
	if (np->r_attr.st_mtime_sec != fap->st_mtime_sec ||
	    np->r_attr.st_mtime_ns  != fap->st_mtime_ns)
		purge_data = 1;
	if (np->r_attr.st_size != fap->st_size)
		purge_data = 1;

	mutex_exit(&np->r_statelock);

	if (purge_data)
		fusefs_purge_caches(vp);
}

/*
 * Set attributes cache for given vnode using vnode attributes.
 * From NFS: nfs_attrcache_va
 */
#if 0 	/* not yet (not sure if we need this) */
void
fusefs_attrcache_va(vnode_t *vp, vattr_t *vap)
{
	fusefattr_t fa;

	vattr_to_fattr(vp, vap, &fa);
	fusefs_attrcache_fa(vp, &fa);
}
#endif

/*
 * Set attributes cache for given vnode using FUSE fattr
 * and update the attribute cache timeout.
 *
 * From NFS: nfs_attrcache, nfs_attrcache_va
 */
void
fusefs_attrcache_fa(vnode_t *vp, fusefattr_t *fap)
{
	fusenode_t *np;
	fusemntinfo_t *fmi;
	hrtime_t delta, now;
	u_offset_t newsize;
	vtype_t	 vtype, oldvt;

	np = VTOFUSE(vp);
	fmi = VTOFMI(vp);
	vtype = IFTOVT(fap->st_mode);

	mutex_enter(&np->r_statelock);
	now = gethrtime();

	/*
	 * Delta is the number of nanoseconds that we will
	 * cache the attributes of the file.  It is based on
	 * the number of nanoseconds since the last time that
	 * we detected a change.  The assumption is that files
	 * that changed recently are likely to change again.
	 * There is a minimum and a maximum for regular files
	 * and for directories which is enforced though.
	 *
	 * Using the time since last change was detected
	 * eliminates direct comparison or calculation
	 * using mixed client and server times.  FUSEFS
	 * does not make any assumptions regarding the
	 * client and server clocks being synchronized.
	 */
	if (fap->st_mtime_sec != np->r_attr.st_mtime_sec ||
	    fap->st_mtime_ns  != np->r_attr.st_mtime_ns ||
	    fap->st_size	  != np->r_attr.st_size)
		np->r_mtime = now;

	if ((fmi->fmi_flags & FMI_NOAC) || (vp->v_flag & VNOCACHE))
		delta = 0;
	else {
		delta = now - np->r_mtime;
		if (vtype == VDIR) {
			if (delta < fmi->fmi_acdirmin)
				delta = fmi->fmi_acdirmin;
			else if (delta > fmi->fmi_acdirmax)
				delta = fmi->fmi_acdirmax;
		} else {
			if (delta < fmi->fmi_acregmin)
				delta = fmi->fmi_acregmin;
			else if (delta > fmi->fmi_acregmax)
				delta = fmi->fmi_acregmax;
		}
	}

	np->r_attrtime = now + delta;
	np->r_attr = *fap;
	oldvt = vp->v_type;
	vp->v_type = vtype;

	/*
	 * Shall we update r_size? (local notion of size)
	 *
	 * The real criteria for updating r_size should be:
	 * if the file has grown on the server, or if
	 * the client has not modified the file.
	 */
	newsize = fap->st_size;

	if (np->r_size != newsize) {
#if 0	/* not yet: mmap support */
		if (!vn_has_cached_data(vp) || ...)
			/* XXX: See NFS page cache code. */
#endif	/* not yet */
		/* OK to set the size. */
		np->r_size = newsize;
	}

	/* NFS: np->r_flags &= ~RWRITEATTR; */
	np->n_flag &= ~NATTRCHANGED;

	mutex_exit(&np->r_statelock);

	if (oldvt != vtype) {
		FUSEFS_DEBUG("vtype change %d to %d\n", oldvt, vtype);
	}
}

/*
 * Fill in attribute from the cache.
 *
 * If valid, copy to *fap and return zero,
 * otherwise return an error.
 *
 * From NFS: nfs_getattr_cache()
 */
int
fusefs_getattr_cache(vnode_t *vp, fusefattr_t *fap)
{
	fusenode_t *np;
	int error;

	np = VTOFUSE(vp);

	mutex_enter(&np->r_statelock);
	if (gethrtime() >= np->r_attrtime) {
		/* cache expired */
		error = ENOENT;
	} else {
		/* cache is valid */
		*fap = np->r_attr;
		error = 0;
	}
	mutex_exit(&np->r_statelock);

	return (error);
}

/*
 * Get attributes over-the-wire and update attributes cache
 * if no error occurred in the over-the-wire operation.
 * Return 0 if successful, otherwise error.
 * From NFS: nfs_getattr_otw
 */
int
fusefs_getattr_otw(vnode_t *vp, fusefattr_t *fap, cred_t *cr)
{
	_NOTE(ARGUNUSED(cr));
	fusemntinfo_t	*fmi;
	fusefs_ssn_t	*ssp;
	struct fusenode	*np;
	int error;

	np = VTOFUSE(vp);
	fmi = VTOFMI(vp);
	ssp = fmi->fmi_ssn;

	bzero(fap, sizeof (fap));

	/* Shared lock for (possible) n_fid use. */
	if (fusefs_rw_enter_sig(&np->r_lkserlock, RW_READER, FUSEINTR(vp)))
		return (EINTR);

	/* Try to use the FID if we have one. */
	if ((np->n_fidrefs > 0) &&
	    (np->n_fid != FUSE_FID_UNUSED) &&
	    (np->n_ssgenid == ssp->ss_genid))
		error = fusefs_call_fgetattr(
		    ssp, np->n_fid, fap);
	else
		error = ENOSYS;
	if (error == ENOSYS)
		error = fusefs_call_getattr(
		    ssp, np->n_rplen, np->n_rpath, fap);

	fusefs_rw_exit(&np->r_lkserlock);

	if (error) {
		/* NFS had: PURGE_STALE_FH(error, vp, cr) */
		fusefs_attrcache_remove(np);
		if (error == ENOENT || error == ENOTDIR) {
			/*
			 * Getattr failed because the object was
			 * removed or renamed by another client.
			 * Remove any cached attributes under it.
			 */
			fusefs_attrcache_prune(np);
		}
		return (error);
	}

	/*
	 * NFS: nfs_cache_fattr(vap, fa, vap, t, cr);
	 * which did: fattr_to_vattr, nfs_attr_cache.
	 * We cache the fattr form, so just do the
	 * cache check and store the attributes.
	 */
	fusefs_cache_check(vp, fap);
	fusefs_attrcache_fa(vp, fap);

	return (0);
}

/*
 * Return either cached or remote attributes. If get remote attr
 * use them to check and invalidate caches, then cache the new attributes.
 *
 * From NFS: nfsgetattr()
 */
int
fusefsgetattr(vnode_t *vp, struct vattr *vap, cred_t *cr)
{
	fusefattr_t fa;
	fusemntinfo_t *fmi;
	int error;

	fmi = VTOFMI(vp);

	ASSERT(curproc->p_zone == fmi->fmi_zone);

	/*
	 * If we've got cached attributes, just use them;
	 * otherwise go to the server to get attributes,
	 * which will update the cache in the process.
	 */
	error = fusefs_getattr_cache(vp, &fa);
	if (error)
		error = fusefs_getattr_otw(vp, &fa, cr);
	if (error)
		return (error);

	/*
	 * Re. client's view of the file size, see:
	 * fusefs_attrcache_fa, fusefs_getattr_otw
	 */
	fattr_to_vattr(vp, &fa, vap);

	return (0);
}


/*
 * Convert FUSE over the wire attributes to vnode form.
 * Returns 0 for success, error if failed (overflow, etc).
 * From NFS: nattr_to_vattr()
 */
static void
fattr_to_vattr(vnode_t *vp, fusefattr_t *fa, vattr_t *vap)
{
	struct fusenode *np = VTOFUSE(vp);

	vap->va_mask = AT_ALL;

	vap->va_type = IFTOVT(fa->st_mode);
	vap->va_mode = fa->st_mode & ~S_IFMT;
	vap->va_uid = fa->st_uid;
	vap->va_gid = fa->st_gid;
	vap->va_fsid = vp->v_vfsp->vfs_dev;
	vap->va_nodeid = np->n_ino; /* don't trust: fa->st_ino; */
	vap->va_nlink = fa->st_nlink;

	/* Careful: see ... */
	vap->va_size = (u_offset_t)fa->st_size;

	/*
	 * Times.  Note, already converted from NT to
	 * Unix form (in the unmarshalling code).
	 */
	vap->va_atime.tv_sec  = fa->st_atime_sec;
	vap->va_atime.tv_nsec = fa->st_atime_ns;
	vap->va_mtime.tv_sec  = fa->st_mtime_sec;
	vap->va_mtime.tv_nsec = fa->st_mtime_ns;
	vap->va_ctime.tv_sec  = fa->st_ctime_sec;
	vap->va_ctime.tv_nsec = fa->st_ctime_ns;

	/*
	 * FUSE does not provide reliable values for:
	 * st_dev, st_blksize, so make 'em up.
	 * va_nblocks is 512 byte blocks.
	 */
	vap->va_rdev = vp->v_rdev;
	vap->va_blksize = MAXBSIZE;
	vap->va_nblocks = fa->st_blocks;
	vap->va_seq = 0;
}


/*
 * Update the local notion of the mtime of some directory.
 * See comments re. r_mtime in fusefs_node.h
 */
void
fusefs_attr_touchdir(struct fusenode *dnp)
{

	mutex_enter(&dnp->r_statelock);

	/*
	 * Now that we keep the client's notion of mtime
	 * separately from the server, this is easy.
	 */
	dnp->r_mtime = gethrtime();

	/*
	 * Invalidate the cache, so that we go to the wire
	 * to check that the server doesn't have a better
	 * timestamp next time we care.
	 */
	fusefs_attrcache_rm_locked(dnp);
	mutex_exit(&dnp->r_statelock);
}

void
fusefs_attrcache_remove(struct fusenode *np)
{
	mutex_enter(&np->r_statelock);
	/* fusefs_attrcache_rm_locked(np); */
	np->r_attrtime = gethrtime();
	mutex_exit(&np->r_statelock);
}

/* See fusefs_node.h */
#undef fusefs_attrcache_rm_locked
void
fusefs_attrcache_rm_locked(struct fusenode *np)
{
	ASSERT(MUTEX_HELD(&np->r_statelock));
	np->r_attrtime = gethrtime();
}


/*
 * FUSE Client initialization and cleanup.
 * Much of it is per-zone now.
 */


/* ARGSUSED */
static void *
fusefs_zone_init(zoneid_t zoneid)
{
	fmi_globals_t *smg;

	smg = kmem_alloc(sizeof (*smg), KM_SLEEP);
	mutex_init(&smg->smg_lock, NULL, MUTEX_DEFAULT, NULL);
	list_create(&smg->smg_list, sizeof (fusemntinfo_t),
	    offsetof(fusemntinfo_t, fmi_zone_node));
	smg->smg_destructor_called = B_FALSE;
	return (smg);
}

/*
 * Callback routine to tell all FUSEFS mounts in the zone to stop creating new
 * threads.  Existing threads should exit.
 */
/* ARGSUSED */
static void
fusefs_zone_shutdown(zoneid_t zoneid, void *data)
{
	fmi_globals_t *smg = data;
	fusemntinfo_t *fmi;

	ASSERT(smg != NULL);
again:
	mutex_enter(&smg->smg_lock);
	for (fmi = list_head(&smg->smg_list); fmi != NULL;
	    fmi = list_next(&smg->smg_list, fmi)) {

		/*
		 * If we've done the shutdown work for this FS, skip.
		 * Once we go off the end of the list, we're done.
		 */
		if (fmi->fmi_flags & FMI_DEAD)
			continue;

		/*
		 * We will do work, so not done.  Get a hold on the FS.
		 */
		VFS_HOLD(fmi->fmi_vfsp);

		mutex_enter(&fmi->fmi_lock);
		fmi->fmi_flags |= FMI_DEAD;
		mutex_exit(&fmi->fmi_lock);

		/*
		 * Drop lock and release FS, which may change list, then repeat.
		 * We're done when every mi has been done or the list is empty.
		 */
		mutex_exit(&smg->smg_lock);
		VFS_RELE(fmi->fmi_vfsp);
		goto again;
	}
	mutex_exit(&smg->smg_lock);
}

static void
fusefs_zone_free_globals(fmi_globals_t *smg)
{
	list_destroy(&smg->smg_list);	/* makes sure the list is empty */
	mutex_destroy(&smg->smg_lock);
	kmem_free(smg, sizeof (*smg));

}

/* ARGSUSED */
static void
fusefs_zone_destroy(zoneid_t zoneid, void *data)
{
	fmi_globals_t *smg = data;

	ASSERT(smg != NULL);
	mutex_enter(&smg->smg_lock);
	if (list_head(&smg->smg_list) != NULL) {
		/* Still waiting for VFS_FREEVFS() */
		smg->smg_destructor_called = B_TRUE;
		mutex_exit(&smg->smg_lock);
		return;
	}
	fusefs_zone_free_globals(smg);
}

/*
 * Add an FUSEFS mount to the per-zone list of FUSEFS mounts.
 */
void
fusefs_zonelist_add(fusemntinfo_t *fmi)
{
	fmi_globals_t *smg;

	smg = zone_getspecific(fmi_list_key, fmi->fmi_zone);
	mutex_enter(&smg->smg_lock);
	list_insert_head(&smg->smg_list, fmi);
	mutex_exit(&smg->smg_lock);
}

/*
 * Remove an FUSEFS mount from the per-zone list of FUSEFS mounts.
 */
void
fusefs_zonelist_remove(fusemntinfo_t *fmi)
{
	fmi_globals_t *smg;

	smg = zone_getspecific(fmi_list_key, fmi->fmi_zone);
	mutex_enter(&smg->smg_lock);
	list_remove(&smg->smg_list, fmi);
	/*
	 * We can be called asynchronously by VFS_FREEVFS() after the zone
	 * shutdown/destroy callbacks have executed; if so, clean up the zone's
	 * fmi_globals.
	 */
	if (list_head(&smg->smg_list) == NULL &&
	    smg->smg_destructor_called == B_TRUE) {
		fusefs_zone_free_globals(smg);
		return;
	}
	mutex_exit(&smg->smg_lock);
}


/*
 * FUSEFS Client initialization routine.  This routine should only be called
 * once.  It performs the following tasks:
 *      - Initalize all global locks
 *      - Call sub-initialization routines (localize access to variables)
 */
int
fusefs_clntinit(void)
{

	zone_key_create(&fmi_list_key, fusefs_zone_init, fusefs_zone_shutdown,
	    fusefs_zone_destroy);
	return (0);
}

/*
 * This routine is called when the modunload is called. This will cleanup
 * the previously allocated/initialized nodes.
 */
void
fusefs_clntfini(void)
{
	(void) zone_key_delete(fmi_list_key);
}
