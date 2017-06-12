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
 * Copyright 2012 Nexenta Systems, Inc.  All rights reserved.
 */

#ifndef _FS_FUSEFS_FUSE_DOOR_H_
#define	_FS_FUSEFS_FUSE_DOOR_H_

#include <sys/param.h>
#include <sys/time.h>
#include <sys/fs/fuse_ktypes.h>

/*
 * Door call arg/ret formats used by fuse-dmn and libfuse
 *
 * Careful modifying these.  Make sure offsets and sizes
 * work correctly for both 32/64 bit.  See ioc_check.ref
 */

/* Maximum I/O size per FUSE read/write call. */
#define	FUSE_MAX_IOSIZE	MAXBSIZE	/* 8K for now */

/* Op. codes. */
typedef enum {			/* arg type, ret type */
	FUSE_OP_INIT = 1,	/* generic, generic */
	FUSE_OP_DESTROY,	/* generic, generic */
	FUSE_OP_STATVFS,	/* generic, statvfs */

	FUSE_OP_FGETATTR,	/* fid, getattr */
	FUSE_OP_GETATTR,	/* path, getattr */

	FUSE_OP_OPENDIR,	/* path, fid */
	FUSE_OP_CLOSEDIR,	/* fid, generic */
	FUSE_OP_READDIR,	/* read, readdir */

	FUSE_OP_OPEN,		/* path, fid */
	FUSE_OP_CLOSE,		/* fid, generic */
	FUSE_OP_READ,		/* read, read */
	FUSE_OP_WRITE,		/* write, write */
	FUSE_OP_FLUSH,		/* fid, generic */

	FUSE_OP_CREATE,		/* path, fid */
	FUSE_OP_FTRUNC,		/* ftrunc, generic */
	FUSE_OP_UTIMES,		/* utimes, generic */
	FUSE_OP_CHMOD,		/* path, generic */
	FUSE_OP_CHOWN,		/* chown, generic */
	FUSE_OP_DELETE,		/* path, generic */
	FUSE_OP_RENAME,		/* path2, generic */
	FUSE_OP_MKDIR,		/* path, generic */
	FUSE_OP_RMDIR,		/* path, generic */
} fuse_opcode_t;

/* For ops that don't send data. */
struct fuse_generic_arg {
	int32_t arg_opcode;
	uint32_t arg_flags;
};

/* For ops that don't return data. */
struct fuse_generic_ret {
	int32_t ret_err;
	uint32_t ret_flags;
};

/* XXX fuse_init_ret? */

/* Calls with a FID (fstat, read, write) */
struct fuse_fid_arg {
	uint32_t arg_opcode;
	uint32_t arg_flags;
	uint64_t arg_fid;
};

/* Calls the return a FID (open, opendir) */
struct fuse_fid_ret {
	uint32_t ret_err;
	uint32_t ret_flags;
	uint64_t ret_fid;
};


struct fuse_statvfs_ret {
	uint32_t ret_err;
	uint32_t ret_flags;
	struct fuse_statvfs ret_stvfs;
};

struct fuse_getattr_ret {
	uint32_t ret_err;
	uint32_t ret_flags;
	struct fuse_stat ret_st;
};

/* For ops that send one path name (and one or two scalars). */
struct fuse_path_arg {
	uint32_t arg_opcode;
	uint32_t arg_val[2];
	uint32_t arg_pathlen;
	char arg_path[MAXPATHLEN];
};

/* For ops that send two path names. */
struct fuse_path2_arg {
	uint32_t arg_opcode;
	uint32_t arg_flags;

	uint32_t arg_p1len;
	uint32_t arg_p2len;
	char arg_path1[MAXPATHLEN];
	char arg_path2[MAXPATHLEN];
};

struct fuse_readdir_ret {
	uint32_t ret_err;
	uint32_t ret_flags;	/* EOF flag */

	struct fuse_stat ret_st;
	struct fuse_dirent ret_de;
	char ret_path[MAXNAMELEN]; /* ret_de.d_name */
};

struct fuse_read_arg {
	uint32_t arg_opcode;
	uint32_t arg_flags;

	uint64_t arg_fid;
	off64_t arg_offset;
	uint32_t arg_length;
	/* FUSE wants the path here too. */
	uint32_t arg_pathlen;
	char arg_path[MAXPATHLEN];
};

struct fuse_read_ret {
	uint32_t ret_err;
	uint32_t ret_length;
	char ret_data[FUSE_MAX_IOSIZE];
};

struct fuse_write_arg {
	uint32_t arg_opcode;
	uint32_t arg_flags;

	uint64_t arg_fid;
	off64_t arg_offset;
	uint32_t arg_length;
	/* FUSE wants the path here too. */
	uint32_t arg_pathlen;
	char arg_path[MAXPATHLEN];
	char arg_data[FUSE_MAX_IOSIZE];
};

struct fuse_write_ret {
	uint32_t ret_err;
	uint32_t ret_length;
};

struct fuse_ftrunc_arg {
	uint32_t arg_opcode;
	uint32_t arg_flags;
	uint64_t arg_fid;	/* non-zero for ftruncate */
	off64_t arg_offset;
	uint32_t arg__pad;
	uint32_t arg_pathlen;
	char arg_path[MAXPATHLEN];
};

struct fuse_utimes_arg {
	uint32_t arg_opcode;
	uint32_t arg_flags;
	uint64_t arg_atime;
	uint64_t arg_mtime;
	uint32_t arg_atime_ns;
	uint32_t arg_mtime_ns;
	uint32_t arg__pad;
	uint32_t arg_pathlen;
	char arg_path[MAXPATHLEN];
};

#endif /* !_FS_FUSEFS_FUSE_DOOR_H_ */
