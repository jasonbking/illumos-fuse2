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

#ifndef _FS_FUSEFS_FUSEFS_CALLS_H_
#define	_FS_FUSEFS_FUSEFS_CALLS_H_

/*
 * Door calls to the FUSE daemon.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/dirent.h>
#include <sys/fs/fuse_ktypes.h>

typedef struct fuse_ssn {
	int	ss_door_handle;
	int	ss_max_iosize;
	uint32_t	ss_opts;
} fuse_ssn_t;

typedef struct fuse_stat fusefattr_t;

int cli_call_init(fuse_ssn_t *, int);

int cli_call_statvfs(fuse_ssn_t *, struct fuse_statvfs *);

int cli_call_fgetattr(fuse_ssn_t *,
	uint64_t fid, fusefattr_t *);
int cli_call_getattr(fuse_ssn_t *,
	int rplen, const char *rpath,
	fusefattr_t *);
int cli_call_getattr2(fuse_ssn_t *,
	int dnlen, const char *dname,
	int cnlen, const char *cname,
	fusefattr_t *);

int cli_call_opendir(fuse_ssn_t *,
	int rplen, const char *rpath, uint64_t *ret_fid);
int cli_call_closedir(fuse_ssn_t *, uint64_t fid);
int cli_call_readdir(fuse_ssn_t *, uint64_t fid, int offset,
	fusefattr_t *fa, struct fuse_dirent *de, int *eofp);

int cli_call_open(fuse_ssn_t *,
	int rplen, const char *rpath, int oflags, uint64_t *ret_fid);
int cli_call_close(fuse_ssn_t *, uint64_t fid);

int  cli_call_read(fuse_ssn_t *,
	uint64_t fid, off64_t offset, uint32_t length,
	void *rd_data, uint32_t *ret_len);
int  cli_call_write(fuse_ssn_t *,
	uint64_t fid, off64_t offset, uint32_t length,
	void *wr_data, uint32_t *ret_len);

int cli_call_flush(fuse_ssn_t *, uint64_t fid);

int cli_call_create(fuse_ssn_t *,
	int dnlen, const char *dname,
	int cnlen, const char *cname,
	uint64_t *ret_fid);

int cli_call_ftruncate(fuse_ssn_t *, uint64_t fid, u_offset_t off);
int cli_call_truncate(fuse_ssn_t *,
	int rplen, const char *rpath, u_offset_t off);

int cli_call_utimes(fuse_ssn_t *,
	int rplen, const char *rpath,
	timespec_t *atime, timespec_t *mtime);

int cli_call_chmod(fuse_ssn_t *,
	int rplen, const char *rpath, mode_t mode);

int cli_call_chown(fuse_ssn_t *,
	int rplen, const char *rpath, uid_t, gid_t);

int cli_call_delete(fuse_ssn_t *,
	int rplen, const char *rpath);

int cli_call_rename(fuse_ssn_t *,
	int oldplen, const char *oldpath,
	int ndirlen, const char *ndirpath,
	int nnmlen, const char *newname);

int cli_call_mkdir(fuse_ssn_t *,
	int ndirlen, const char *ndirpath,
	int nnmlen, const char *newname);

int cli_call_rmdir(fuse_ssn_t *,
	int rplen, const char *rpath);

/*
 * FUSE session management
 */

int cli_ssn_create(char *, fuse_ssn_t **);
void cli_ssn_kill(fuse_ssn_t *);
void cli_ssn_hold(fuse_ssn_t *);
void cli_ssn_rele(fuse_ssn_t *);

#endif /* !_FS_FUSEFS_FUSEFS_CALLS_H_ */
