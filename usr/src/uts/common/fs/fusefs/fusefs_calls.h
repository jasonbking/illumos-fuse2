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

#ifndef _FS_FUSEFS_FUSEFS_CALLS_H_
#define	_FS_FUSEFS_FUSEFS_CALLS_H_

/*
 * Up-calls to the FUSE daemon.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/dirent.h>
#include <sys/fs/fuse_ktypes.h>

int fusefs_call_init(fusefs_ssn_t *, int);

int fusefs_call_statvfs(fusefs_ssn_t *, statvfs64_t *);

int fusefs_call_fgetattr(fusefs_ssn_t *,
	uint64_t fid, fusefattr_t *);
int fusefs_call_getattr(fusefs_ssn_t *,
	int rplen, const char *rpath,
	fusefattr_t *);
int fusefs_call_getattr2(fusefs_ssn_t *,
	int dnlen, const char *dname,
	int cnlen, const char *cname,
	fusefattr_t *);

int fusefs_call_opendir(fusefs_ssn_t *,
	int rplen, const char *rpath, uint64_t *ret_fid);
int fusefs_call_closedir(fusefs_ssn_t *, uint64_t fid);
int fusefs_call_readdir(fusefs_ssn_t *, uint64_t fid, int offset,
	fusefattr_t *fa, dirent64_t *de, int *eofp);

int fusefs_call_open(fusefs_ssn_t *,
	int rplen, const char *rpath, int oflags, uint64_t *ret_fid);
int fusefs_call_close(fusefs_ssn_t *, uint64_t fid);

int  fusefs_call_read(fusefs_ssn_t *,
	uint64_t fid, uint32_t *rlen, uio_t *uiop,
	int rplen, const char *rpath);
int  fusefs_call_write(fusefs_ssn_t *,
	uint64_t fid, uint32_t *wlen, uio_t *uiop,
	int rplen, const char *rpath);

int fusefs_call_flush(fusefs_ssn_t *, uint64_t fid);

/* Modify operations */

int fusefs_call_create(fusefs_ssn_t *,
	int dnlen, const char *dname,
	int cnlen, const char *cname,
	int mode, uint64_t *ret_fid);

int fusefs_call_ftruncate(fusefs_ssn_t *,
	uint64_t fid, u_offset_t off,
	int rplen, const char *rpath);

int fusefs_call_utimes(fusefs_ssn_t *,
	int rplen, const char *rpath,
	timespec_t *atime, timespec_t *mtime);

int fusefs_call_chmod(fusefs_ssn_t *,
	int rplen, const char *rpath, mode_t mode);

int fusefs_call_chown(fusefs_ssn_t *,
	int rplen, const char *rpath, uid_t, gid_t);

int fusefs_call_delete(fusefs_ssn_t *,
	int rplen, const char *rpath);

int fusefs_call_rename(fusefs_ssn_t *,
	int oldplen, const char *oldpath,
	int ndirlen, const char *ndirpath,
	int nnmlen, const char *newname);

int fusefs_call_mkdir(fusefs_ssn_t *,
	int ndirlen, const char *ndirpath,
	int nnmlen, const char *newname);

int fusefs_call_rmdir(fusefs_ssn_t *,
	int rplen, const char *rpath);

/*
 * FUSE session management
 */

int fusefs_ssn_create(int, fusefs_ssn_t **);
void fusefs_ssn_kill(fusefs_ssn_t *);
void fusefs_ssn_hold(fusefs_ssn_t *);
void fusefs_ssn_rele(fusefs_ssn_t *);

#endif /* !_FS_FUSEFS_FUSEFS_CALLS_H_ */
