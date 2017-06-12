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

/*
 * Test door calls to the FUSE daemon, same interface as used by:
 * Similar to uts/common/fs/fusefs/fusefs_calls.c
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/dirent.h>
#include <sys/fs/fuse_door.h>

#include <door.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <umem.h>

#include "cli_calls.h"


int
cli_ssn_create(char *door_path, fuse_ssn_t **ret_ssn)
{
	fuse_ssn_t *ssn;
	int err, fd;

	fd = open(door_path, O_RDONLY, 0);
	if (fd == -1)
		return (errno);

	ssn = umem_zalloc(sizeof (*ssn), UMEM_NOFAIL);
	ssn->ss_door_handle = fd;
	ssn->ss_max_iosize = FUSE_MAX_IOSIZE;

	/*
	 * Get attributes of this FUSE library program.
	 */
	err = cli_call_init(ssn, 0);
	if (err) {
		fprintf(stderr, "call_init, err %d\n", err);
		cli_ssn_rele(ssn);
		return (err);
	}

	*ret_ssn = ssn;
	return (0);
}

#if 0	/* XXX: not used for now */
void
cli_ssn_hold(fuse_ssn_t *ssn)
{
}
#endif

/*ARGSUSED*/
void
cli_ssn_kill(fuse_ssn_t *ssn)
{
	/* XXX: Prevent further door calls... */
}

void
cli_ssn_rele(fuse_ssn_t *ssn)
{
	close(ssn->ss_door_handle);
	umem_free(ssn, sizeof (*ssn));
}

/*
 * Door calls to the FUSE daemon.
 */

int
cli_call_init(fuse_ssn_t *ssn, int want)
{
	door_arg_t da;
	struct fuse_generic_arg arg;
	struct fuse_generic_ret ret;
	int rc;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_INIT;
	arg.arg_flags = want;
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	ssn->ss_opts = ret.ret_flags;
	/* XXX - todo: other option stuff */

	return (0);
}

int
cli_call_statvfs(fuse_ssn_t *ssn, struct fuse_statvfs *stv)
{
	door_arg_t da;
	struct fuse_generic_arg arg;
	struct fuse_statvfs_ret ret;
	int rc;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_STATVFS;
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);

	if (ret.ret_err != 0)
		return (ret.ret_err);

	/* Convert... */
	stv->f_blocks	= ret.ret_stvfs.f_blocks;
	stv->f_bfree	= ret.ret_stvfs.f_bfree;
	stv->f_bavail	= ret.ret_stvfs.f_bavail;
	stv->f_files	= ret.ret_stvfs.f_files;
	stv->f_ffree	= ret.ret_stvfs.f_ffree;
	stv->f_favail	= ret.ret_stvfs.f_favail;
	stv->f_bsize	= ret.ret_stvfs.f_bsize;
	stv->f_frsize	= ret.ret_stvfs.f_frsize;
	stv->f_namemax	= ret.ret_stvfs.f_namemax;
	stv->f_flag	= ret.ret_stvfs.f_flag;
	/* NB: ignore f_fsid, f_basetype */

	return (0);
}

/*ARGSUSED*/
int
cli_call_fgetattr(fuse_ssn_t *ssn,
	uint64_t fid, fusefattr_t *fap)
{
	door_arg_t da;
	struct fuse_fid_arg arg;
	struct fuse_getattr_ret ret;
	int rc;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_FGETATTR;
	arg.arg_fid = fid;
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);

	if (ret.ret_err != 0)
		return (ret.ret_err);

	*fap = ret.ret_st;	/* XXX (stat64) */

	return (0);
}

int
cli_call_getattr(fuse_ssn_t *ssn,
	int rplen, const char *rpath,
	fusefattr_t *fap)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_getattr_ret ret;
	int rc;

	if (rplen >= MAXPATHLEN)
		return (ENAMETOOLONG);

	argp = umem_zalloc(sizeof (*argp), UMEM_NOFAIL);

	argp->arg_opcode = FUSE_OP_GETATTR;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen);
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);

	umem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*fap = ret.ret_st;	/* XXX (stat64) */
	return (0);
}

int
cli_call_getattr2(fuse_ssn_t *ssn,
	int dnlen, const char *dname,
	int cnlen, const char *cname,
	fusefattr_t *fap)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_getattr_ret ret;
	char *p;
	int plen, rc;

	/*
	 * Add one '/' and a null, except when we're
	 * starting at the root dir, then just a null.
	 */
	plen = dnlen + cnlen + 2;
	if (dnlen == 1)
		--plen;
	if (plen >= MAXPATHLEN)
		return (ENAMETOOLONG);

	argp = umem_zalloc(sizeof (*argp), UMEM_NOFAIL);

	argp->arg_opcode = FUSE_OP_GETATTR;
	argp->arg_pathlen = plen;

	/* Fill in path from two parts. */
	p = argp->arg_path;
	memcpy(p, dname, dnlen);
	p += dnlen;
	if (dnlen > 1)
		*p++ = '/';
	memcpy(p, cname, cnlen);

	memset(&ret, 0, sizeof (ret));
	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);

	umem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);

	if (ret.ret_err != 0)
		return (ret.ret_err);

	*fap = ret.ret_st;	/* XXX (stat64) */
	return (0);
}

/*ARGSUSED*/
int
cli_call_opendir(fuse_ssn_t *ssn,
	int rplen, const char *rpath, uint64_t *ret_fid)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_fid_ret ret;
	int rc;

	if (rplen >= MAXPATHLEN)
		return (ENAMETOOLONG);

	argp = umem_zalloc(sizeof (*argp), UMEM_NOFAIL);

	argp->arg_opcode = FUSE_OP_OPENDIR;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen);
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);

	umem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*ret_fid = ret.ret_fid;
	return (0);
}

int
cli_call_closedir(fuse_ssn_t *ssn, uint64_t fid)
{
	door_arg_t da;
	struct fuse_fid_arg arg;
	struct fuse_generic_ret ret;
	int rc;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_CLOSEDIR;
	arg.arg_fid = fid;
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
cli_call_readdir(fuse_ssn_t *ssn, uint64_t fid, int offset,
	fusefattr_t *fap, struct fuse_dirent *de, int *eofp)
{
	door_arg_t da;
	struct fuse_read_arg arg;
	struct fuse_readdir_ret *retp;
	int nmlen, rc;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_READDIR;
	arg.arg_fid = fid;
	arg.arg_offset = offset;

	retp = umem_alloc(sizeof (*retp), UMEM_NOFAIL);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) retp;
	da.rsize = sizeof (*retp);

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc == 0)
		rc = retp->ret_err;
	if (rc != 0)
		goto out;

	/*
	 * Note: Computing reclen depends on alignment, etc.
	 * so the door svc puts nmlen in d_reclen and the
	 * caller uses that to compute d_reclen.  Also
	 * sanity check nmlen vs. size of ret_path.
	 */
	nmlen = retp->ret_de.d_nmlen;
	if (nmlen >= MAXNAMELEN) {
		rc = EIO;
		goto out;
	}

	*fap = retp->ret_st;
	*de = retp->ret_de;
	/*
	 * Copy the whole d_name field which actually
	 * extends into the ret_name field following.
	 */
	memcpy(de->d_name, retp->ret_de.d_name, nmlen + 1);

	if (retp->ret_flags & 1)
		*eofp = 1;

out:
	umem_free(retp, sizeof (*retp));
	return (rc);
}

/*ARGSUSED*/
int
cli_call_open(fuse_ssn_t *ssn,
	int rplen, const char *rpath, int oflags, uint64_t *ret_fid)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_fid_ret ret;
	int rc;

	if (rplen >= MAXPATHLEN)
		return (ENAMETOOLONG);

	argp = umem_zalloc(sizeof (*argp), UMEM_NOFAIL);

	argp->arg_opcode = FUSE_OP_OPEN;
	argp->arg_val[0] = oflags;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen);
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);

	umem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*ret_fid = ret.ret_fid;
	return (0);
}

int
cli_call_close(fuse_ssn_t *ssn, uint64_t fid)
{
	door_arg_t da;
	struct fuse_fid_arg arg;
	struct fuse_generic_ret ret;
	int rc;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_CLOSE;
	arg.arg_fid = fid;
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
cli_call_read(fuse_ssn_t *ssn,
	uint64_t fid, off64_t offset, uint32_t length,
	void *rd_data, uint32_t *ret_len)
{
	door_arg_t da;
	struct fuse_read_arg arg;
	struct fuse_read_ret *retp;
	int allocsize, rc;

	/* Sanity check */
	if (length > FUSE_MAX_IOSIZE)
		return (EFAULT);

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_READ;
	arg.arg_fid = fid;
	arg.arg_offset = offset;
	arg.arg_length = length;

	/* XXX: Later, make allocsize dynamic. */
	allocsize = sizeof (*retp);
	retp = umem_alloc(allocsize, UMEM_NOFAIL);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) retp;
	da.rsize = allocsize;

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc == 0)
		rc = retp->ret_err;
	if (rc != 0)
		goto out;

	/* Length of data we read. */
	*ret_len = retp->ret_length;
	memcpy(rd_data, retp->ret_data, *ret_len);

out:
	umem_free(retp, allocsize);
	return (rc);
}

/*ARGSUSED*/
int
cli_call_write(fuse_ssn_t *ssn,
	uint64_t fid, off64_t offset, uint32_t length,
	void *wr_data, uint32_t *ret_len)
{
	door_arg_t da;
	struct fuse_write_arg *argp;
	struct fuse_write_ret ret;
	int allocsize, rc;

	/* Sanity check */
	if (length > FUSE_MAX_IOSIZE)
		return (EFAULT);

	/* XXX: Later, make allocsize dynamic. */
	allocsize = sizeof (*argp);
	argp = umem_alloc(allocsize, UMEM_NOFAIL);

	argp->arg_opcode = FUSE_OP_WRITE;
	argp->arg_fid = fid;
	argp->arg_offset = offset;
	argp->arg_length = length;

	memcpy(argp->arg_data, wr_data, length);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = allocsize;
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc == 0)
		rc = ret.ret_err;
	if (rc != 0)
		goto out;

	/* Length of data we wrote. */
	*ret_len = ret.ret_length;

out:
	umem_free(argp, allocsize);
	return (rc);
}

int
cli_call_flush(fuse_ssn_t *ssn, uint64_t fid)
{
	door_arg_t da;
	struct fuse_fid_arg arg;
	struct fuse_generic_ret ret;
	int rc;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_FLUSH;
	arg.arg_fid = fid;
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_call(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

/* XXX: Modify operations - not yet. */

/*ARGSUSED*/
int
cli_call_create(fuse_ssn_t *ssn,
	int dnlen, const char *dname,
	int cnlen, const char *cname,
	uint64_t *ret_fid)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_ftruncate(fuse_ssn_t *ssn, uint64_t fid, u_offset_t off)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_truncate(fuse_ssn_t *ssn,
	int rplen, const char *rpath, u_offset_t off)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_utimes(fuse_ssn_t *ssn,
	int rplen, const char *rpath,
	timespec_t *atime, timespec_t *mtime)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_chmod(fuse_ssn_t *ssn,
	int rplen, const char *rpath, mode_t mode)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_chown(fuse_ssn_t *ssn,
	int rplen, const char *rpath, uid_t uid, gid_t gid)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_delete(fuse_ssn_t *ssn,
	int rplen, const char *rpath)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_rename(fuse_ssn_t *ssn,
	int oldplen, const char *oldpath,
	int ndirlen, const char *ndirpath,
	int nnmlen, const char *newname)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_mkdir(fuse_ssn_t *ssn,
	int ndirlen, const char *ndirpath,
	int nnmlen, const char *newname)
{
	return (EACCES);
}

/*ARGSUSED*/
int
cli_call_rmdir(fuse_ssn_t *ssn,
	int rplen, const char *rpath)
{
	return (EACCES);
}
