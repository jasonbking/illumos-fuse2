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

#ifndef	_FUSEFS_FUSEFS_H
#define	_FUSEFS_FUSEFS_H

/*
 * FS-specific VFS structures for fusefs.
 * (per-mount stuff, etc.)
 */

#include <sys/param.h>
#include <sys/fstyp.h>
#include <sys/avl.h>
#include <sys/list.h>
#include <sys/t_lock.h>
#include <sys/vfs.h>
#include <sys/vfs_opreg.h>
#include <sys/fs/fusefs_mount.h>

/*
 * Path component length
 *
 * The generic fs code uses MAXNAMELEN to represent
 * what the largest component length is, but note:
 * that length DOES include the terminating NULL.
 * FUSE_MAXFNAMELEN does NOT include the NULL.
 */
#define	FUSE_MAXFNAMELEN		(MAXNAMELEN-1)	/* 255 */

/*
 * SM_MAX_STATFSTIME is the maximum time to cache statvfs data. Since this
 * should be a fast call on the server, the time the data cached is short.
 * That lets the cache handle bursts of statvfs() requests without generating
 * lots of network traffic.
 */
#define	SM_MAX_STATFSTIME 2

/* Mask values for fusemount structure sm_status field */
#define	SM_STATUS_STATFS_BUSY 0x00000001 /* statvfs is in progress */
#define	SM_STATUS_STATFS_WANT 0x00000002 /* statvfs wakeup is wanted */
#define	SM_STATUS_TIMEO 0x00000004 /* this mount is not responding */
#define	SM_STATUS_DEAD	0x00000010 /* connection gone - unmount this */

extern const struct fs_operation_def	fusefs_vnodeops_template[];
extern struct vnodeops			*fusefs_vnodeops;

struct fusenode; /* fusefs_node.h */

/*
 * Options we get from the libfuse init call.
 * Just flags for now...
 */
struct fusefs_ssn {
	/* XXX: hold count, call count, semaphore, state... */
	void		*ss_door_handle;
	int		ss_genid;	/* generation ID */
	uint_t		ss_max_iosize;
	uint32_t	ss_opts;
};
typedef struct fusefs_ssn fusefs_ssn_t;

/*
 * The values for fmi_flags (from nfs_clnt.h)
 */
#define	FMI_INT		0x04		/* interrupts allowed */
#define	FMI_NOAC	0x10		/* don't cache attributes */
#define	FMI_LLOCK	0x80		/* local locking only */
#define	FMI_LARGEF	0x100		/* has large files */
#define	FMI_DEAD	0x200000	/* mount has been terminated */

/*
 * These are the attributes we can get from libfuse.
 */
typedef struct fuse_stat fusefattr_t;

/*
 * Corresponds to NFS: struct mntinfo
 */
typedef struct fusemntinfo {
	struct vfs		*fmi_vfsp;	/* mount back pointer to vfs */
	struct fusenode		*fmi_root;	/* the root node */
	struct fusefs_ssn	*fmi_ssn;	/* our service proc. */
	kmutex_t		fmi_lock;	/* mutex for flags, etc. */
	uint32_t		fmi_flags;	/* NFS-derived flag bits */
	uint32_t		fmi_status;	/* status bits for this mount */
	hrtime_t		fmi_statfstime;	/* sm_statvfsbuf cache time */
	statvfs64_t		fmi_statvfsbuf;	/* cached statvfs data */
	kcondvar_t		fmi_statvfs_cv;

	/*
	 * The fusefs node cache for this mount.
	 * Named "hash" for historical reasons.
	 * See fusefs_node.h for details.
	 */
	avl_tree_t		fmi_hash_avl;
	krwlock_t		fmi_hash_lk;

	/*
	 * Kstat statistics
	 */
	struct kstat    *fmi_io_kstats;
	struct kstat    *fmi_ro_kstats;

	/*
	 * Zones support.
	 */
	struct zone		*fmi_zone;	/* Zone mounted in */
	list_node_t		fmi_zone_node;	/* Link to per-zone fmi list */
	/* Lock for the list is: fmi_globals_t -> smg_lock */

	/*
	 * Stuff copied or derived from the mount args
	 */
	uid_t		fmi_uid;		/* user id */
	gid_t		fmi_gid;		/* group id */
	mode_t		fmi_fmode;		/* mode for files */
	mode_t		fmi_dmode;		/* mode for dirs */

	hrtime_t	fmi_acregmin;	/* min time to hold cached file attr */
	hrtime_t	fmi_acregmax;	/* max time to hold cached file attr */
	hrtime_t	fmi_acdirmin;	/* min time to hold cached dir attr */
	hrtime_t	fmi_acdirmax;	/* max time to hold cached dir attr */
} fusemntinfo_t;

/*
 * Attribute cache timeout defaults (in seconds).
 */
#define	FUSEFS_ACREGMIN	3	/* min secs to hold cached file attr */
#define	FUSEFS_ACREGMAX	60	/* max secs to hold cached file attr */
#define	FUSEFS_ACDIRMIN	30	/* min secs to hold cached dir attr */
#define	FUSEFS_ACDIRMAX	60	/* max secs to hold cached dir attr */
/* and limits for the mount options */
#define	FUSEFS_ACMINMAX	600	/* 10 min. is longest min timeout */
#define	FUSEFS_ACMAXMAX	3600	/* 1 hr is longest max timeout */

/*
 * High-res time is nanoseconds.
 */
#define	SEC2HR(sec)	((sec) * (hrtime_t)NANOSEC)

/*
 * vnode pointer to mount info
 */
#define	VTOFMI(vp)	((fusemntinfo_t *)(((vp)->v_vfsp)->vfs_data))
#define	VFTOFMI(vfsp)	((fusemntinfo_t *)((vfsp)->vfs_data))
#define	FUSEINTR(vp)	(VTOFMI(vp)->fmi_flags & FMI_INT)

#endif	/* _FUSEFS_FUSEFS_H */
