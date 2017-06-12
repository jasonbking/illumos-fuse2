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

#ifndef	_FUSEFS_MOUNT_H
#define	_FUSEFS_MOUNT_H

/*
 * This file defines the interface used by mount_fusefs.
 * Bump the version if fusefs_args changes.
 */

#define	FUSEFS_VERSION	1
#define	FUSEFS_VFSNAME	"fusefs"

/* Values for fusefs_args.flags */
#define	FUSEFS_MF_SOFT		0x0001
#define	FUSEFS_MF_INTR		0x0002
#define	FUSEFS_MF_NOAC		0x0004
#define	FUSEFS_MF_ACREGMIN	0x0100	/* set min secs for file attr cache */
#define	FUSEFS_MF_ACREGMAX	0x0200	/* set max secs for file attr cache */
#define	FUSEFS_MF_ACDIRMIN	0x0400	/* set min secs for dir attr cache */
#define	FUSEFS_MF_ACDIRMAX	0x0800	/* set max secs for dir attr cache */

/* Layout of the mount control block for an fuse file system. */
struct fusefs_args {
	int		version;		/* fusefs mount version */
	int		doorfd;			/* file descriptor */
	uint_t		flags;			/* FUSEFS_MF_ flags */
	uid_t		uid;			/* octal user id */
	gid_t		gid;			/* octal group id */
	mode_t		file_mode;		/* octal srwx for files */
	mode_t		dir_mode;		/* octal srwx for dirs */
	int		acregmin;		/* attr cache file min secs */
	int		acregmax;		/* attr cache file max secs */
	int		acdirmin;		/* attr cache dir min secs */
	int		acdirmax;		/* attr cache dir max secs */
};

#ifdef _SYSCALL32

/* Layout of the mount control block for an fuse file system. */
struct fusefs_args32 {
	int32_t		version;		/* fusefs mount version */
	int32_t		doorfd;			/* file descriptor */
	uint32_t	flags;			/* FUSEFS_MF_ flags */
	uid32_t		uid;			/* octal user id */
	gid32_t		gid;			/* octal group id */
	mode32_t	file_mode;		/* octal srwx for files */
	mode32_t	dir_mode;		/* octal srwx for dirs */
	int32_t		acregmin;		/* attr cache file min secs */
	int32_t		acregmax;		/* attr cache file max secs */
	int32_t		acdirmin;		/* attr cache dir min secs */
	int32_t		acdirmax;		/* attr cache dir max secs */
};

#endif /* _SYSCALL32 */
#endif	/* _FUSEFS_MOUNT_H */
