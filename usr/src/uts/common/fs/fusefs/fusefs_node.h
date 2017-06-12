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

#ifndef _FS_FUSEFS_NODE_H_
#define	_FS_FUSEFS_NODE_H_

/*
 * Much code copied into here from Sun NFS.
 * Compare with nfs_clnt.h
 */

#include <sys/avl.h>
#include <sys/list.h>
#include <sys/fs/fuse_ktypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A homegrown reader/writer lock implementation.  It addresses
 * two requirements not addressed by the system primitives.  They
 * are that the `enter" operation is optionally interruptible and
 * that that they can be re`enter'ed by writers without deadlock.
 */
typedef struct fusefs_rwlock {
	int count;
	int waiters;
	kthread_t *owner;
	kmutex_t lock;
	kcondvar_t cv;
} fusefs_rwlock_t;

/*
 * The format of the fusefs node header, which contains the
 * fields used to link nodes in the AVL tree, and those
 * fields needed by the AVL node comparison functions.
 * It's a separate struct so we can call avl_find with
 * this relatively small struct as a stack local.
 *
 * The AVL tree is mntinfo.fmi_hash_avl,
 * and its lock is mntinfo.fmi_hash_lk.
 */
typedef struct fusefs_node_hdr {
	/*
	 * Our linkage in the node cache AVL tree.
	 */
	avl_node_t	hdr_avl_node;

	/*
	 * Identity of this node:  The full path name,
	 * in server form, relative to the share root.
	 */
	char		*hdr_n_rpath;
	int		hdr_n_rplen;
} fusefs_node_hdr_t;

/*
 * Below is the FUSEFS-specific representation of a "node".
 * Fields starting with "r_" came from NFS struct "rnode"
 * and fields starting with "n_" were added for FUSE.
 * We have avoided renaming fields so we would not cause
 * excessive changes in the code using this struct.
 *
 * Now using an AVL tree instead of hash lists, but kept the
 * "hash" in some member names and functions to reduce churn.
 * One AVL tree per mount replaces the global hash buckets.
 *
 * Notes carried over from the NFS code:
 *
 * The fusenode is the "inode" for remote files.  It contains all the
 * information necessary to handle remote file on the client side.
 *
 * Note on file sizes:  we keep two file sizes in the fusenode: the size
 * according to the client (r_size) and the size according to the server
 * (r_attr.va_size).  They can differ because we modify r_size during a
 * write system call (fusefs_rdwr), before the write request goes over the
 * wire (before the file is actually modified on the server).  If an OTW
 * request occurs before the cached data is written to the server the file
 * size returned from the server (r_attr.va_size) may not match r_size.
 * r_size is the one we use, in general.  r_attr.va_size is only used to
 * determine whether or not our cached data is valid.
 *
 * Each fusenode has 3 locks associated with it (not including the fusenode
 * "hash" AVL tree and free list locks):
 *
 *	r_rwlock:	Serializes fusefs_write and fusefs_setattr requests
 *			and allows fusefs_read requests to proceed in parallel.
 *			Serializes reads/updates to directories.
 *
 *	r_lkserlock:	Serializes lock requests with map, write, and
 *			readahead operations.
 *
 *	r_statelock:	Protects all fields in the fusenode except for
 *			those listed below.  This lock is intented
 *			to be held for relatively short periods of
 *			time (not accross entire putpage operations,
 *			for example).
 *
 * The following members are protected by the mutex fusefreelist_lock:
 *	r_freef
 *	r_freeb
 *
 * The following members are protected by the AVL tree rwlock:
 *	r_avl_node	(r__hdr.hdr_avl_node)
 *
 * Note: r_modaddr is only accessed when the r_statelock mutex is held.
 *	Its value is also controlled via r_rwlock.  It is assumed that
 *	there will be only 1 writer active at a time, so it safe to
 *	set r_modaddr and release r_statelock as long as the r_rwlock
 *	writer lock is held.
 *
 * 64-bit offsets: the code formerly assumed that atomic reads of
 * r_size were safe and reliable; on 32-bit architectures, this is
 * not true since an intervening bus cycle from another processor
 * could update half of the size field.  The r_statelock must now
 * be held whenever any kind of access of r_size is made.
 *
 * Lock ordering:
 * 	r_rwlock > r_lkserlock > r_statelock
 */

typedef struct fusenode {
	/* Our linkage in the node cache AVL tree (see above). */
	fusefs_node_hdr_t	r__hdr;

	/* short-hand names for r__hdr members */
#define	r_avl_node	r__hdr.hdr_avl_node
#define	n_rpath		r__hdr.hdr_n_rpath
#define	n_rplen		r__hdr.hdr_n_rplen

	fusemntinfo_t	*n_mount;	/* VFS data */
	vnode_t		*r_vnode;	/* associated vnode */

	/*
	 * Linkage in fusefreelist, for reclaiming nodes.
	 * Lock for the free list is: fusefreelist_lock
	 */
	struct fusenode	*r_freef;	/* free list forward pointer */
	struct fusenode	*r_freeb;	/* free list back pointer */

	fusefs_rwlock_t	r_rwlock;	/* serialize write/setattr requests */
	fusefs_rwlock_t	r_lkserlock;	/* serialize lock with other ops */
	kmutex_t	r_statelock;	/* protect (most) fusenode fields */

	/*
	 * File handle, directory search handle,
	 * and reference counts for them, etc.
	 * Lock for these is: r_lkserlock
	 */
	uint64_t	n_fid;		/* file handle */
	int		n_fidrefs;
	enum vtype	n_ovtype;	/* vnode type opened */
	int		n_rights;	/* granted rights */
	int		n_ssgenid;	/* gereration no. (remount) */

	/*
	 * Misc. bookkeeping
	 */
	cred_t		*r_cred;	/* current credentials */
	u_offset_t	r_nextr;	/* next read offset (read-ahead) */
	long		r_mapcnt;	/* count of mmapped pages */
	uint_t		r_count;	/* # of refs not reflect in v_count */
	uint_t		r_awcount;	/* # of outstanding async write */
	uint_t		r_gcount;	/* getattrs waiting to flush pages */
	uint_t		r_flags;	/* flags, see below */
	uint32_t	n_flag;		/* NXXX flags below */
	uint_t		r_error;	/* async write error */
	kcondvar_t	r_cv;		/* condvar for blocked threads */
	kthread_t	*r_serial;	/* id of purging thread */
	list_t		r_indelmap;	/* list of delmap callers */

	/*
	 * Attributes: local, and as last seen on the server.
	 * See notes above re: r_size vs r_attr.va_size, etc.
	 */
	fusefattr_t	r_attr;		/* attributes from the server */
	hrtime_t	r_attrtime;	/* time attributes become invalid */
	hrtime_t	r_mtime;	/* client time file last modified */
	len_t		r_size;		/* client's view of file size */
	/*
	 * Other attributes, not carried in smbfattr_t
	 */
	u_longlong_t	n_ino;
} fusenode_t;

/* Invalid n_fid value. */
#define	FUSE_FID_UNUSED	0xFFFFFFFFFFFFFFFF

/*
 * Flag bits in: fusenode_t .n_flag
 */
#define	NMODIFIED	0x00004 /* bogus, until async IO implemented */
#define	NGOTIDS		0x00020
#define	NFLUSHWIRE	0x01000
#define	NATTRCHANGED	0x02000 /* kill cached attributes at close */
#define	N_XATTR 	0x10000 /* extended attribute (dir or file) */

/*
 * Flag bits in: fusenode_t .r_flags
 */
#define	RREADDIRPLUS	0x1	/* issue a READDIRPLUS instead of READDIR */
#define	RDIRTY		0x2	/* dirty pages from write operation */
#define	RSTALE		0x4	/* file handle is stale */
#define	RMODINPROGRESS	0x8	/* page modification happening */
#define	RTRUNCATE	0x10	/* truncating, don't commit */
#define	RHAVEVERF	0x20	/* have a write verifier to compare against */
#define	RCOMMIT		0x40	/* commit in progress */
#define	RCOMMITWAIT	0x80	/* someone is waiting to do a commit */
#define	RHASHED		0x100	/* fusenode is in the "hash" AVL tree */
#define	ROUTOFSPACE	0x200	/* an out of space error has happened */
#define	RDIRECTIO	0x400	/* bypass the buffer cache */
#define	RLOOKUP		0x800	/* a lookup has been performed */
#define	RWRITEATTR	0x1000	/* attributes came from WRITE */
#define	RINDNLCPURGE	0x2000	/* in the process of purging DNLC references */
#define	RDELMAPLIST	0x4000	/* delmap callers tracking for as callback */

/*
 * Convert between vnode and fusenode
 */
#define	VTOFUSE(vp)	((fusenode_t *)((vp)->v_data))
#define	FUSETOV(np)	((np)->r_vnode)

/*
 * A macro to compute the separator that should be used for
 * names under some directory.
 */
#define	FUSEFS_DNP_SEP(dnp) \
	((dnp->n_rplen > 1) ? '/' : '\0')

#ifdef __cplusplus
}
#endif

#endif /* _FS_FUSEFS_NODE_H_ */
