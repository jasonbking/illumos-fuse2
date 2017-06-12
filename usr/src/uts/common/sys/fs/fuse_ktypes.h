/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 */

#ifndef _FS_FUSEFS_FUSE_KTYPES_H_
#define	_FS_FUSEFS_FUSE_KTYPES_H_

/*
 * We need representations of stat, statvfs, dirent that are
 * invariant across 32-bit and 64-bit kernels and userland.
 * Sort the members by decreasing alignment, and pad to a
 * multiple of the largest alignment (8 bytes).
 */

/* See sys/statvfs.h */
struct fuse_statvfs {
	uint64_t	f_blocks;	/* total blocks of f_frsize */
	uint64_t	f_bfree;	/* total free blocks of f_frsize */
	uint64_t	f_bavail;	/* free blocks avail to non-superuser */
	uint64_t	f_files;	/* total # of file nodes (inodes) */
	uint64_t	f_ffree;	/* total # of free file nodes */
	uint64_t	f_favail;	/* free nodes avail to non-superuser */
	uint32_t	f_bsize;	/* preferred file system block size */
	uint32_t	f_frsize;	/* fundamental file system block size */
	uint32_t	f_namemax;	/* maximum file name length */
	uint32_t	f_flag;		/* bit-mask of flags */
	/* Intentionally ommit: fsid, basetype, fstr */
};

/* See sys/stat.h */
struct fuse_stat {
	uint64_t	st_ino;
	uint64_t	st_size;
	uint64_t	st_blocks;
	uint64_t	st_atime_sec;
	uint64_t	st_mtime_sec;
	uint64_t	st_ctime_sec;
	uint32_t	st_atime_ns;
	uint32_t	st_mtime_ns;
	uint32_t	st_ctime_ns;
	uint32_t	st_mode;
	uint32_t	st_nlink;
	uint32_t	st_uid;
	uint32_t	st_gid;
	uint32_t	st_rdev;
	uint32_t	st_blksize;
	uint32_t	st__pad;
	/* Intentionally ommit: dev */
};

/* See sys/dirent.h */
struct fuse_dirent {
	uint64_t	d_ino;
	uint64_t	d_off;	/* next valid offset */
	uint32_t	d_nmlen;
	char		d_name[4];
};

#endif /* !_FS_FUSEFS_FUSE_KTYPES_H_ */
