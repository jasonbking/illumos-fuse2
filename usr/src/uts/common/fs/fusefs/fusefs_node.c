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
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 */

/*
 * Node hash implementation initially borrowed from NFS (nfs_subr.c)
 * but then heavily modified. It's no longer an array of hash lists,
 * but an AVL tree per mount point.  More on this below.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/bitmap.h>
#include <sys/dnlc.h>
#include <sys/kmem.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>

#include "fusefs.h"
#include "fusefs_node.h"
#include "fusefs_subr.h"

/*
 * The AVL trees (now per-mount) allow finding an fusefs node by its
 * full remote path name.  It also allows easy traversal of all nodes
 * below (path wise) any given node.  A reader/writer lock for each
 * (per mount) AVL tree is used to control access and to synchronize
 * lookups, additions, and deletions from that AVL tree.
 *
 * Previously, this code use a global array of hash chains, each with
 * its own rwlock.  A few struct members, functions, and comments may
 * still refer to a "hash", and those should all now be considered to
 * refer to the per-mount AVL tree that replaced the old hash chains.
 * (i.e. member fmi_hash_lk, function sn_hashfind, etc.)
 *
 * The fusenode freelist is organized as a doubly linked list with
 * a head pointer.  Additions and deletions are synchronized via
 * a single mutex.
 *
 * In order to add an fusenode to the free list, it must be linked into
 * the mount's AVL tree and the exclusive lock for the AVL must be held.
 * If an fusenode is not linked into the AVL tree, then it is destroyed
 * because it represents no valuable information that can be reused
 * about the file.  The exclusive lock for the AVL tree must be held
 * in order to prevent a lookup in the AVL tree from finding the
 * fusenode and using it and assuming that the fusenode is not on the
 * freelist.  The lookup in the AVL tree will have the AVL tree lock
 * held, either exclusive or shared.
 *
 * The vnode reference count for each fusenode is not allowed to drop
 * below 1.  This prevents external entities, such as the VM
 * subsystem, from acquiring references to vnodes already on the
 * freelist and then trying to place them back on the freelist
 * when their reference is released.  This means that the when an
 * fusenode is looked up in the AVL tree, then either the fusenode
 * is removed from the freelist and that reference is tranfered to
 * the new reference or the vnode reference count must be incremented
 * accordingly.  The mutex for the freelist must be held in order to
 * accurately test to see if the fusenode is on the freelist or not.
 * The AVL tree lock might be held shared and it is possible that
 * two different threads may race to remove the fusenode from the
 * freelist.  This race can be resolved by holding the mutex for the
 * freelist.  Please note that the mutex for the freelist does not
 * need to held if the fusenode is not on the freelist.  It can not be
 * placed on the freelist due to the requirement that the thread
 * putting the fusenode on the freelist must hold the exclusive lock
 * for the AVL tree and the thread doing the lookup in the AVL tree
 * is holding either a shared or exclusive lock for the AVL tree.
 *
 * The lock ordering is:
 *
 *	AVL tree lock -> vnode lock
 *	AVL tree lock -> freelist lock
 */

static kmutex_t fusefreelist_lock;
static fusenode_t *fusefreelist = NULL;
static ulong_t	fusenodenew = 0;
long	nfusenode = 0;

static struct kmem_cache *fusenode_cache;

/*
 * Mutex to protect the following variables:
 *	fusefs_major
 *	fusefs_minor
 */
kmutex_t fusefs_minor_lock;
int fusefs_major;
int fusefs_minor;

/* See fusefs_node_findcreate() */
fusefattr_t fusefs_fattr0;

/*
 * Local functions.
 * SN for Fuse Node
 */
static void sn_rmfree(fusenode_t *);
static void sn_inactive(fusenode_t *);
static void sn_addhash_locked(fusenode_t *, avl_index_t);
static void sn_rmhash_locked(fusenode_t *);
static void sn_destroy_node(fusenode_t *);
void fusefs_kmem_reclaim(void *cdrarg);

static fusenode_t *
sn_hashfind(fusemntinfo_t *, const char *, int, avl_index_t *);

static fusenode_t *
make_fusenode(fusemntinfo_t *, const char *, int, int *);

/*
 * Free the resources associated with an fusenode.
 * Note: This is different from fusefs_inactive
 *
 * NFS: nfs_subr.c:rinactive
 */
static void
sn_inactive(fusenode_t *np)
{
	cred_t		*oldcr;
	char 		*orpath;
	int		orplen;

	/*
	 * Flush and invalidate all pages (todo)
	 * Free any held credentials and caches...
	 * etc.  (See NFS code)
	 */
	mutex_enter(&np->r_statelock);

	oldcr = np->r_cred;
	np->r_cred = NULL;

	orpath = np->n_rpath;
	orplen = np->n_rplen;
	np->n_rpath = NULL;
	np->n_rplen = 0;

	mutex_exit(&np->r_statelock);

	if (oldcr != NULL)
		crfree(oldcr);

	if (orpath != NULL)
		kmem_free(orpath, orplen + 1);
}

/*
 * Find and optionally create an fusenode for the passed
 * mountinfo, directory, separator, and name.  If the
 * desired fusenode already exists, return a reference.
 * If the file attributes pointer is non-null, the node
 * is created if necessary and linked into the AVL tree.
 *
 * Callers that need a node created but don't have the
 * real attributes pass fusefs_fattr0 to force creation.
 *
 * Note: make_fusenode() may upgrade the "hash" lock to exclusive.
 *
 * NFS: nfs_subr.c:makenfsnode
 */
fusenode_t *
fusefs_node_findcreate(
	fusemntinfo_t *mi,
	const char *dirnm,
	int dirlen,
	const char *name,
	int nmlen,
	char sep,
	fusefattr_t *fap)
{
	char tmpbuf[MAXNAMELEN];
	size_t rpalloc;
	char *p, *rpath;
	int rplen;
	fusenode_t *np;
	vnode_t *vp;
	int newnode;

	/*
	 * Build the search string, either in tmpbuf or
	 * in allocated memory if larger than tmpbuf.
	 */
	rplen = dirlen;
	if (sep != '\0')
		rplen++;
	rplen += nmlen;
	if (rplen < sizeof (tmpbuf)) {
		/* use tmpbuf */
		rpalloc = 0;
		rpath = tmpbuf;
	} else {
		rpalloc = rplen + 1;
		rpath = kmem_alloc(rpalloc, KM_SLEEP);
	}
	p = rpath;
	bcopy(dirnm, p, dirlen);
	p += dirlen;
	if (sep != '\0')
		*p++ = sep;
	if (name != NULL) {
		bcopy(name, p, nmlen);
		p += nmlen;
	}
	ASSERT(p == rpath + rplen);

	/*
	 * Find or create a node with this path.
	 */
	rw_enter(&mi->fmi_hash_lk, RW_READER);
	if (fap == NULL)
		np = sn_hashfind(mi, rpath, rplen, NULL);
	else
		np = make_fusenode(mi, rpath, rplen, &newnode);
	rw_exit(&mi->fmi_hash_lk);

	if (rpalloc)
		kmem_free(rpath, rpalloc);

	if (fap == NULL) {
		/*
		 * Caller is "just looking" (no create)
		 * so np may or may not be NULL here.
		 * Either way, we're done.
		 */
		return (np);
	}

	/*
	 * We should have a node, possibly created.
	 * Do we have (real) attributes to apply?
	 */
	ASSERT(np != NULL);
	if (fap == &fusefs_fattr0)
		return (np);

	/*
	 * Apply the given attributes to this node,
	 * dealing with any cache impact, etc.
	 */
	vp = FUSETOV(np);
	if (!newnode) {
		/*
		 * Found an existing node.
		 * Maybe purge caches...
		 */
		fusefs_cache_check(vp, fap);
	}
	fusefs_attrcache_fa(vp, fap);

	/*
	 * Note NFS sets vp->v_type here, assuming it
	 * can never change for the life of a node.
	 * We allow v_type to change, and set it in
	 * fusefs_attrcache().  Also: mode, uid, gid
	 */
	return (np);
}

/*
 * NFS: nfs_subr.c:rtablehash
 * We use fusefs_hash().
 */

/*
 * Find or create an fusenode.
 * NFS: nfs_subr.c:make_rnode
 */
static fusenode_t *
make_fusenode(
	fusemntinfo_t *mi,
	const char *rpath,
	int rplen,
	int *newnode)
{
	fusenode_t *np;
	fusenode_t *tnp;
	vnode_t *vp;
	vfs_t *vfsp;
	avl_index_t where;
	char *new_rpath = NULL;

	ASSERT(RW_READ_HELD(&mi->fmi_hash_lk));
	vfsp = mi->fmi_vfsp;

start:
	np = sn_hashfind(mi, rpath, rplen, NULL);
	if (np != NULL) {
		*newnode = 0;
		return (np);
	}

	/* Note: will retake this lock below. */
	rw_exit(&mi->fmi_hash_lk);

	/*
	 * see if we can find something on the freelist
	 */
	mutex_enter(&fusefreelist_lock);
	if (fusefreelist != NULL && fusenodenew >= nfusenode) {
		np = fusefreelist;
		sn_rmfree(np);
		mutex_exit(&fusefreelist_lock);

		vp = FUSETOV(np);

		if (np->r_flags & RHASHED) {
			fusemntinfo_t *tmp_mi = np->n_mount;
			ASSERT(tmp_mi != NULL);
			rw_enter(&tmp_mi->fmi_hash_lk, RW_WRITER);
			mutex_enter(&vp->v_lock);
			if (vp->v_count > 1) {
				vp->v_count--;
				mutex_exit(&vp->v_lock);
				rw_exit(&tmp_mi->fmi_hash_lk);
				/* start over */
				rw_enter(&mi->fmi_hash_lk, RW_READER);
				goto start;
			}
			mutex_exit(&vp->v_lock);
			sn_rmhash_locked(np);
			rw_exit(&tmp_mi->fmi_hash_lk);
		}

		sn_inactive(np);

		mutex_enter(&vp->v_lock);
		if (vp->v_count > 1) {
			vp->v_count--;
			mutex_exit(&vp->v_lock);
			rw_enter(&mi->fmi_hash_lk, RW_READER);
			goto start;
		}
		mutex_exit(&vp->v_lock);
		vn_invalid(vp);
		/*
		 * destroy old locks before bzero'ing and
		 * recreating the locks below.
		 */
		fusefs_rw_destroy(&np->r_rwlock);
		fusefs_rw_destroy(&np->r_lkserlock);
		mutex_destroy(&np->r_statelock);
		cv_destroy(&np->r_cv);
		/*
		 * Make sure that if fusenode is recycled then
		 * VFS count is decremented properly before
		 * reuse.
		 */
		VFS_RELE(vp->v_vfsp);
		vn_reinit(vp);
	} else {
		/*
		 * allocate and initialize a new fusenode
		 */
		vnode_t *new_vp;

		mutex_exit(&fusefreelist_lock);

		np = kmem_cache_alloc(fusenode_cache, KM_SLEEP);
		new_vp = vn_alloc(KM_SLEEP);

		atomic_add_long((ulong_t *)&fusenodenew, 1);
		vp = new_vp;
	}

	/*
	 * Allocate and copy the rpath we'll need below.
	 */
	new_rpath = kmem_alloc(rplen + 1, KM_SLEEP);
	bcopy(rpath, new_rpath, rplen);
	new_rpath[rplen] = '\0';

	/* Initialize fusenode_t */
	bzero(np, sizeof (*np));

	fusefs_rw_init(&np->r_rwlock, NULL, RW_DEFAULT, NULL);
	fusefs_rw_init(&np->r_lkserlock, NULL, RW_DEFAULT, NULL);
	mutex_init(&np->r_statelock, NULL, MUTEX_DEFAULT, NULL);
	cv_init(&np->r_cv, NULL, CV_DEFAULT, NULL);
	/* cv_init(&np->r_commit.c_cv, NULL, CV_DEFAULT, NULL); */

	np->r_vnode = vp;
	np->n_mount = mi;

	np->n_fid = FUSE_FID_UNUSED;
	/* Leave attributes "stale." */

	/* Now fill in the vnode. */
	vn_setops(vp, fusefs_vnodeops);
	vp->v_data = (caddr_t)np;
	VFS_HOLD(vfsp);
	vp->v_vfsp = vfsp;
	vp->v_type = VNON;

	/*
	 * We entered with mi->fmi_hash_lk held (reader).
	 * Retake it now, (as the writer).
	 * Will return with it held.
	 */
	rw_enter(&mi->fmi_hash_lk, RW_WRITER);

	/*
	 * There is a race condition where someone else
	 * may alloc the fusenode while no locks are held,
	 * so check again and recover if found.
	 */
	tnp = sn_hashfind(mi, rpath, rplen, &where);
	if (tnp != NULL) {
		/*
		 * Lost the race.  Put the node we were building
		 * on the free list and return the one we found.
		 */
		rw_exit(&mi->fmi_hash_lk);
		kmem_free(new_rpath, rplen + 1);
		fusefs_addfree(np);
		rw_enter(&mi->fmi_hash_lk, RW_READER);
		*newnode = 0;
		return (tnp);
	}

	/*
	 * Hash search identifies nodes by the remote path
	 * (n_rpath) so fill that in now, before linking
	 * this node into the node cache (AVL tree).
	 */
	np->n_rpath = new_rpath;
	np->n_rplen = rplen;

	/* fake inode number: hash of the full path name */
	np->n_ino = fusefs_gethash(new_rpath, rplen);

	sn_addhash_locked(np, where);
	*newnode = 1;
	return (np);
}

/*
 * fusefs_addfree
 * Put an fusenode on the free list, or destroy it immediately
 * if it offers no value were it to be reclaimed later.  Also
 * destroy immediately when we have too many fusenodes, etc.
 *
 * Normally called by fusefs_inactive, but also
 * called in here during cleanup operations.
 *
 * NFS: nfs_subr.c:rp_addfree
 */
void
fusefs_addfree(fusenode_t *np)
{
	vnode_t *vp;
	struct vfs *vfsp;
	fusemntinfo_t *mi;

	ASSERT(np->r_freef == NULL && np->r_freeb == NULL);

	vp = FUSETOV(np);
	ASSERT(vp->v_count >= 1);

	vfsp = vp->v_vfsp;
	mi = VFTOFMI(vfsp);

	/*
	 * If there are no more references to this fusenode and:
	 * we have too many fusenodes allocated, or if the node
	 * is no longer accessible via the AVL tree (!RHASHED),
	 * or an i/o error occurred while writing to the file,
	 * or it's part of an unmounted FS, then try to destroy
	 * it instead of putting it on the fusenode freelist.
	 */
	if (np->r_count == 0 && (
	    (np->r_flags & RHASHED) == 0 ||
	    (np->r_error != 0) ||
	    (vfsp->vfs_flag & VFS_UNMOUNTED) ||
	    (fusenodenew > nfusenode))) {

		/* Try to destroy this node. */

		if (np->r_flags & RHASHED) {
			rw_enter(&mi->fmi_hash_lk, RW_WRITER);
			mutex_enter(&vp->v_lock);
			if (vp->v_count > 1) {
				vp->v_count--;
				mutex_exit(&vp->v_lock);
				rw_exit(&mi->fmi_hash_lk);
				return;
				/*
				 * Will get another call later,
				 * via fusefs_inactive.
				 */
			}
			mutex_exit(&vp->v_lock);
			sn_rmhash_locked(np);
			rw_exit(&mi->fmi_hash_lk);
		}

		sn_inactive(np);

		/*
		 * Recheck the vnode reference count.  We need to
		 * make sure that another reference has not been
		 * acquired while we were not holding v_lock.  The
		 * fusenode is not in the fusenode "hash" AVL tree, so
		 * the only way for a reference to have been acquired
		 * is for a VOP_PUTPAGE because the fusenode was marked
		 * with RDIRTY or for a modified page.  This vnode
		 * reference may have been acquired before our call
		 * to sn_inactive.  The i/o may have been completed,
		 * thus allowing sn_inactive to complete, but the
		 * reference to the vnode may not have been released
		 * yet.  In any case, the fusenode can not be destroyed
		 * until the other references to this vnode have been
		 * released.  The other references will take care of
		 * either destroying the fusenode or placing it on the
		 * fusenode freelist.  If there are no other references,
		 * then the fusenode may be safely destroyed.
		 */
		mutex_enter(&vp->v_lock);
		if (vp->v_count > 1) {
			vp->v_count--;
			mutex_exit(&vp->v_lock);
			return;
		}
		mutex_exit(&vp->v_lock);

		sn_destroy_node(np);
		return;
	}

	/*
	 * Lock the AVL tree and then recheck the reference count
	 * to ensure that no other threads have acquired a reference
	 * to indicate that the fusenode should not be placed on the
	 * freelist.  If another reference has been acquired, then
	 * just release this one and let the other thread complete
	 * the processing of adding this fusenode to the freelist.
	 */
	rw_enter(&mi->fmi_hash_lk, RW_WRITER);

	mutex_enter(&vp->v_lock);
	if (vp->v_count > 1) {
		vp->v_count--;
		mutex_exit(&vp->v_lock);
		rw_exit(&mi->fmi_hash_lk);
		return;
	}
	mutex_exit(&vp->v_lock);

	/*
	 * Put this node on the free list.
	 */
	mutex_enter(&fusefreelist_lock);
	if (fusefreelist == NULL) {
		np->r_freef = np;
		np->r_freeb = np;
		fusefreelist = np;
	} else {
		np->r_freef = fusefreelist;
		np->r_freeb = fusefreelist->r_freeb;
		fusefreelist->r_freeb->r_freef = np;
		fusefreelist->r_freeb = np;
	}
	mutex_exit(&fusefreelist_lock);

	rw_exit(&mi->fmi_hash_lk);
}

/*
 * Remove an fusenode from the free list.
 *
 * The caller must be holding fusefreelist_lock and the fusenode
 * must be on the freelist.
 *
 * NFS: nfs_subr.c:rp_rmfree
 */
static void
sn_rmfree(fusenode_t *np)
{

	ASSERT(MUTEX_HELD(&fusefreelist_lock));
	ASSERT(np->r_freef != NULL && np->r_freeb != NULL);

	if (np == fusefreelist) {
		fusefreelist = np->r_freef;
		if (np == fusefreelist)
			fusefreelist = NULL;
	}

	np->r_freeb->r_freef = np->r_freef;
	np->r_freef->r_freeb = np->r_freeb;

	np->r_freef = np->r_freeb = NULL;
}

/*
 * Put an fusenode in the "hash" AVL tree.
 *
 * The caller must be hold the rwlock as writer.
 *
 * NFS: nfs_subr.c:rp_addhash
 */
static void
sn_addhash_locked(fusenode_t *np, avl_index_t where)
{
	fusemntinfo_t *mi = np->n_mount;

	ASSERT(RW_WRITE_HELD(&mi->fmi_hash_lk));
	ASSERT(!(np->r_flags & RHASHED));

	avl_insert(&mi->fmi_hash_avl, np, where);

	mutex_enter(&np->r_statelock);
	np->r_flags |= RHASHED;
	mutex_exit(&np->r_statelock);
}

/*
 * Remove an fusenode from the "hash" AVL tree.
 *
 * The caller must hold the rwlock as writer.
 *
 * NFS: nfs_subr.c:rp_rmhash_locked
 */
static void
sn_rmhash_locked(fusenode_t *np)
{
	fusemntinfo_t *mi = np->n_mount;

	ASSERT(RW_WRITE_HELD(&mi->fmi_hash_lk));
	ASSERT(np->r_flags & RHASHED);

	avl_remove(&mi->fmi_hash_avl, np);

	mutex_enter(&np->r_statelock);
	np->r_flags &= ~RHASHED;
	mutex_exit(&np->r_statelock);
}

/*
 * Remove an fusenode from the "hash" AVL tree.
 *
 * The caller must not be holding the rwlock.
 */
void
fusefs_rmhash(fusenode_t *np)
{
	fusemntinfo_t *mi = np->n_mount;

	rw_enter(&mi->fmi_hash_lk, RW_WRITER);
	sn_rmhash_locked(np);
	rw_exit(&mi->fmi_hash_lk);
}

/*
 * Lookup an fusenode by remote pathname
 *
 * The caller must be holding the AVL rwlock, either shared or exclusive.
 *
 * NFS: nfs_subr.c:rfind
 */
static fusenode_t *
sn_hashfind(
	fusemntinfo_t *mi,
	const char *rpath,
	int rplen,
	avl_index_t *pwhere) /* optional */
{
	fusefs_node_hdr_t nhdr;
	fusenode_t *np;
	vnode_t *vp;

	ASSERT(RW_LOCK_HELD(&mi->fmi_hash_lk));

	bzero(&nhdr, sizeof (nhdr));
	nhdr.hdr_n_rpath = (char *)rpath;
	nhdr.hdr_n_rplen = rplen;

	/* See fusefs_node_cmp below. */
	np = avl_find(&mi->fmi_hash_avl, &nhdr, pwhere);

	if (np == NULL)
		return (NULL);

	/*
	 * Found it in the "hash" AVL tree.
	 * Remove from free list, if necessary.
	 */
	vp = FUSETOV(np);
	if (np->r_freef != NULL) {
		mutex_enter(&fusefreelist_lock);
		/*
		 * If the fusenode is on the freelist,
		 * then remove it and use that reference
		 * as the new reference.  Otherwise,
		 * need to increment the reference count.
		 */
		if (np->r_freef != NULL) {
			sn_rmfree(np);
			mutex_exit(&fusefreelist_lock);
		} else {
			mutex_exit(&fusefreelist_lock);
			VN_HOLD(vp);
		}
	} else
		VN_HOLD(vp);

	return (np);
}

static int
fusefs_node_cmp(const void *va, const void *vb)
{
	const fusefs_node_hdr_t *a = va;
	const fusefs_node_hdr_t *b = vb;
	int clen, diff;

	/*
	 * Same semantics as strcmp, but does not
	 * assume the strings are null terminated.
	 */
	clen = (a->hdr_n_rplen < b->hdr_n_rplen) ?
	    a->hdr_n_rplen : b->hdr_n_rplen;
	diff = strncmp(a->hdr_n_rpath, b->hdr_n_rpath, clen);
	if (diff < 0)
		return (-1);
	if (diff > 0)
		return (1);
	/* they match through clen */
	if (b->hdr_n_rplen > clen)
		return (-1);
	if (a->hdr_n_rplen > clen)
		return (1);
	return (0);
}

/*
 * Setup the "hash" AVL tree used for our node cache.
 * See: fusefs_mount, fusefs_destroy_table.
 */
void
fusefs_init_hash_avl(avl_tree_t *avl)
{
	avl_create(avl, fusefs_node_cmp, sizeof (fusenode_t),
	    offsetof(fusenode_t, r_avl_node));
}

/*
 * Invalidate the cached attributes for all nodes "under" the
 * passed-in node.  Note: the passed-in node is NOT affected by
 * this call.  This is used both for files under some directory
 * after the directory is deleted or renamed, and for extended
 * attribute files (named streams) under a plain file after that
 * file is renamed or deleted.
 *
 * Do this by walking the AVL tree starting at the passed in node,
 * and continuing while the visited nodes have a path prefix matching
 * the entire path of the passed-in node, and a separator just after
 * that matching path prefix.  Watch out for cases where the AVL tree
 * order may not exactly match the order of an FS walk, i.e.
 * consider this sequence:
 *	"foo"		(directory)
 *	"foo bar"	(name containing a space)
 *	"foo/bar"
 * The walk needs to skip "foo bar" and keep going until it finds
 * something that doesn't match the "foo" name prefix.
 */
void
fusefs_attrcache_prune(fusenode_t *top_np)
{
	fusemntinfo_t *mi;
	fusenode_t *np;
	char *rpath;
	int rplen;

	mi = top_np->n_mount;
	rw_enter(&mi->fmi_hash_lk, RW_READER);

	np = top_np;
	rpath = top_np->n_rpath;
	rplen = top_np->n_rplen;
	for (;;) {
		np = avl_walk(&mi->fmi_hash_avl, np, AVL_AFTER);
		if (np == NULL)
			break;
		if (np->n_rplen < rplen)
			break;
		if (0 != strncmp(np->n_rpath, rpath, rplen))
			break;
		if (np->n_rplen > rplen && (
		    np->n_rpath[rplen] == ':' ||
		    np->n_rpath[rplen] == '/'))
			fusefs_attrcache_remove(np);
	}

	rw_exit(&mi->fmi_hash_lk);
}

#ifdef FUSE_VNODE_DEBUG
int fusefs_check_table_debug = 1;
#else /* FUSE_VNODE_DEBUG */
int fusefs_check_table_debug = 0;
#endif /* FUSE_VNODE_DEBUG */


/*
 * Return 1 if there is a active vnode belonging to this vfs in the
 * fusenode cache.
 *
 * Several of these checks are done without holding the usual
 * locks.  This is safe because destroy_fusetable(), fusefs_addfree(),
 * etc. will redo the necessary checks before actually destroying
 * any fusenodes.
 *
 * NFS: nfs_subr.c:check_rtable
 *
 * Debugging changes here relative to NFS.
 * Relatively harmless, so left 'em in.
 */
int
fusefs_check_table(struct vfs *vfsp, fusenode_t *rtnp)
{
	fusemntinfo_t *mi;
	fusenode_t *np;
	vnode_t *vp;
	int busycnt = 0;

	mi = VFTOFMI(vfsp);
	rw_enter(&mi->fmi_hash_lk, RW_READER);
	for (np = avl_first(&mi->fmi_hash_avl); np != NULL;
	    np = avl_walk(&mi->fmi_hash_avl, np, AVL_AFTER)) {

		if (np == rtnp)
			continue; /* skip the root */
		vp = FUSETOV(np);

		/* Now the 'busy' checks: */
		/* Not on the free list? */
		if (np->r_freef == NULL) {
			FUSEFS_DEBUG("!r_freef: node=0x%p, rpath=%s\n",
			    (void *)np, np->n_rpath);
			busycnt++;
		}

		/* Has dirty pages? */
		if (vn_has_cached_data(vp) &&
		    (np->r_flags & RDIRTY)) {
			FUSEFS_DEBUG("is dirty: node=0x%p, rpath=%s\n",
			    (void *)np, np->n_rpath);
			busycnt++;
		}

		/* Other refs? (not reflected in v_count) */
		if (np->r_count > 0) {
			FUSEFS_DEBUG("+r_count: node=0x%p, rpath=%s\n",
			    (void *)np, np->n_rpath);
			busycnt++;
		}

		if (busycnt && !fusefs_check_table_debug)
			break;

	}
	rw_exit(&mi->fmi_hash_lk);

	return (busycnt);
}

/*
 * Destroy inactive vnodes from the AVL tree which belong to this
 * vfs.  It is essential that we destroy all inactive vnodes during a
 * forced unmount as well as during a normal unmount.
 *
 * NFS: nfs_subr.c:destroy_rtable
 *
 * In here, we're normally destrying all or most of the AVL tree,
 * so the natural choice is to use avl_destroy_nodes.  However,
 * there may be a few busy nodes that should remain in the AVL
 * tree when we're done.  The solution: use a temporary tree to
 * hold the busy nodes until we're done destroying the old tree,
 * then copy the temporary tree over the (now emtpy) real tree.
 */
void
fusefs_destroy_table(struct vfs *vfsp)
{
	avl_tree_t tmp_avl;
	fusemntinfo_t *mi;
	fusenode_t *np;
	fusenode_t *rlist;
	void *v;

	mi = VFTOFMI(vfsp);
	rlist = NULL;
	fusefs_init_hash_avl(&tmp_avl);

	rw_enter(&mi->fmi_hash_lk, RW_WRITER);
	v = NULL;
	while ((np = avl_destroy_nodes(&mi->fmi_hash_avl, &v)) != NULL) {

		mutex_enter(&fusefreelist_lock);
		if (np->r_freef == NULL) {
			/*
			 * Busy node (not on the free list).
			 * Will keep in the final AVL tree.
			 */
			mutex_exit(&fusefreelist_lock);
			avl_add(&tmp_avl, np);
		} else {
			/*
			 * It's on the free list.  Remove and
			 * arrange for it to be destroyed.
			 */
			sn_rmfree(np);
			mutex_exit(&fusefreelist_lock);

			/*
			 * Last part of sn_rmhash_locked().
			 * NB: avl_destroy_nodes has already
			 * removed this from the "hash" AVL.
			 */
			mutex_enter(&np->r_statelock);
			np->r_flags &= ~RHASHED;
			mutex_exit(&np->r_statelock);

			/*
			 * Add to the list of nodes to destroy.
			 * Borrowing avl_child[0] for this list.
			 */
			np->r_avl_node.avl_child[0] =
			    (struct avl_node *)rlist;
			rlist = np;
		}
	}
	avl_destroy(&mi->fmi_hash_avl);

	/*
	 * Replace the (now destroyed) "hash" AVL with the
	 * temporary AVL, which restores the busy nodes.
	 */
	mi->fmi_hash_avl = tmp_avl;
	rw_exit(&mi->fmi_hash_lk);

	/*
	 * Now destroy the nodes on our temporary list (rlist).
	 * This call to fusefs_addfree will end up destroying the
	 * fusenode, but in a safe way with the appropriate set
	 * of checks done.
	 */
	while ((np = rlist) != NULL) {
		rlist = (fusenode_t *)np->r_avl_node.avl_child[0];
		fusefs_addfree(np);
	}
}

/*
 * This routine destroys all the resources associated with the fusenode
 * and then the fusenode itself.  Note: sn_inactive has been called.
 *
 * NFS: nfs_subr.c:destroy_rnode
 */
static void
sn_destroy_node(fusenode_t *np)
{
	vnode_t *vp;
	vfs_t *vfsp;

	vp = FUSETOV(np);
	vfsp = vp->v_vfsp;

	ASSERT(vp->v_count == 1);
	ASSERT(np->r_count == 0);
	ASSERT(np->r_mapcnt == 0);
	ASSERT(np->r_cred == NULL);
	ASSERT(np->n_rpath == NULL);
	ASSERT(!(np->r_flags & RHASHED));
	ASSERT(np->r_freef == NULL && np->r_freeb == NULL);
	atomic_add_long((ulong_t *)&fusenodenew, -1);
	vn_invalid(vp);
	vn_free(vp);
	kmem_cache_free(fusenode_cache, np);
	VFS_RELE(vfsp);
}

/*
 * Flush all vnodes in this (or every) vfs.
 * Used by nfs_sync and by nfs_unmount.
 */
/*ARGSUSED*/
void
fusefs_rflush(struct vfs *vfsp, cred_t *cr)
{
	/* Todo: mmap support. */
}

/*
 * fusefs_nget()
 *
 * Find or create a node under some directory node.
 */
int
fusefs_nget(vnode_t *dvp, const char *name, int nmlen,
	fusefattr_t *fap, vnode_t **vpp)
{
	struct fusenode *dnp = VTOFUSE(dvp);
	struct fusenode *np;
	vnode_t *vp;
	char sep;

	ASSERT(fap != NULL);
	*vpp = NULL;

	/* Don't allow "" or "." or ".." here. */
	if (nmlen == 0 || (nmlen == 1 && name[0] == '.') ||
	    (nmlen == 2 && name[0] == '.' && name[1] == '.')) {
		return (EINVAL);
	}
	sep = FUSEFS_DNP_SEP(dnp);

	/* Find or create the node. */
	np = fusefs_node_findcreate(dnp->n_mount,
	    dnp->n_rpath, dnp->n_rplen,
	    name, nmlen, sep, fap);

	/*
	 * We should have np now, because we passed
	 * fap != NULL to fusefs_node_findcreate.
	 */
	ASSERT(np != NULL);
	vp = FUSETOV(np);

	/*
	 * Files in an XATTR dir are also XATTR.
	 */
	if (dnp->n_flag & N_XATTR) {
		mutex_enter(&np->r_statelock);
		np->n_flag |= N_XATTR;
		mutex_exit(&np->r_statelock);
	}

	*vpp = vp;

	return (0);
}

/* access cache */
/* client handles */

/*
 * initialize resources that are used by fusefs_subr.c
 * this is called from the _init() routine (by the way of fusefs_clntinit())
 *
 * NFS: nfs_subr.c:nfs_subrinit
 */
int
fusefs_subrinit(void)
{
	ulong_t nfusenode_max;

	/*
	 * Allocate and initialize the fusenode cache
	 */
	if (nfusenode <= 0)
		nfusenode = ncsize; /* dnlc.h */
	nfusenode_max = (ulong_t)((kmem_maxavail() >> 2) /
	    sizeof (struct fusenode));
	if (nfusenode > nfusenode_max || (nfusenode == 0 && ncsize == 0)) {
		zcmn_err(GLOBAL_ZONEID, CE_NOTE,
		    "setting nfusenode to max value of %ld", nfusenode_max);
		nfusenode = nfusenode_max;
	}

	fusenode_cache = kmem_cache_create("fusenode_cache",
	    sizeof (fusenode_t), 0, NULL, NULL,
	    fusefs_kmem_reclaim, NULL, NULL, 0);

	/*
	 * Initialize the various mutexes and reader/writer locks
	 */
	mutex_init(&fusefreelist_lock, NULL, MUTEX_DEFAULT, NULL);
	mutex_init(&fusefs_minor_lock, NULL, MUTEX_DEFAULT, NULL);

	/*
	 * Assign unique major number for all fusefs mounts
	 */
	if ((fusefs_major = getudev()) == -1) {
		zcmn_err(GLOBAL_ZONEID, CE_WARN,
		    "fusefs: init: can't get unique device number");
		fusefs_major = 0;
	}
	fusefs_minor = 0;

	return (0);
}

/*
 * free fusefs hash table, etc.
 * NFS: nfs_subr.c:nfs_subrfini
 */
void
fusefs_subrfini(void)
{

	/*
	 * Destroy the fusenode cache
	 */
	kmem_cache_destroy(fusenode_cache);

	/*
	 * Destroy the various mutexes and reader/writer locks
	 */
	mutex_destroy(&fusefreelist_lock);
	mutex_destroy(&fusefs_minor_lock);
}

/* rddir_cache ? */

/*
 * Support functions for fusefs_kmem_reclaim
 */

static void
fusefs_node_reclaim(void)
{
	fusemntinfo_t *mi;
	fusenode_t *np;
	vnode_t *vp;

	mutex_enter(&fusefreelist_lock);
	while ((np = fusefreelist) != NULL) {
		sn_rmfree(np);
		mutex_exit(&fusefreelist_lock);
		if (np->r_flags & RHASHED) {
			vp = FUSETOV(np);
			mi = np->n_mount;
			rw_enter(&mi->fmi_hash_lk, RW_WRITER);
			mutex_enter(&vp->v_lock);
			if (vp->v_count > 1) {
				vp->v_count--;
				mutex_exit(&vp->v_lock);
				rw_exit(&mi->fmi_hash_lk);
				mutex_enter(&fusefreelist_lock);
				continue;
			}
			mutex_exit(&vp->v_lock);
			sn_rmhash_locked(np);
			rw_exit(&mi->fmi_hash_lk);
		}
		/*
		 * This call to fusefs_addfree will end up destroying the
		 * fusenode, but in a safe way with the appropriate set
		 * of checks done.
		 */
		fusefs_addfree(np);
		mutex_enter(&fusefreelist_lock);
	}
	mutex_exit(&fusefreelist_lock);
}

/*
 * Called by kmem_cache_alloc ask us if we could
 * "Please give back some memory!"
 *
 * Todo: dump nodes from the free list?
 */
/*ARGSUSED*/
void
fusefs_kmem_reclaim(void *cdrarg)
{
	fusefs_node_reclaim();
}

/* nfs failover stuff */
/* nfs_rw_xxx - see fusefs_rwlock.c */
