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
 * Calls up to the FUSE process.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/door.h>
#include <sys/vnode.h>
#include <sys/dirent.h>
#include <sys/uio.h>
#include <sys/sunddi.h>
#include <sys/sysmacros.h>
#include <sys/cmn_err.h>
#include <sys/sdt.h>

#include <sys/fs/fuse_door.h>

#include "fusefs.h"
#include "fusefs_calls.h"
#include "fusefs_node.h"
#include "fusefs_subr.h"

uint_t fusefs_genid = 0;

int
fusefs_ssn_create(int doorfd, fusefs_ssn_t **ret_ssn)
{
	fusefs_ssn_t *ssn;
	door_handle_t dh;
	int err;

	dh = door_ki_lookup(doorfd);
	if (dh == NULL)
		return (EINVAL);

	ssn = kmem_zalloc(sizeof (*ssn), KM_SLEEP);
	ssn->ss_door_handle = dh;
	ssn->ss_genid = atomic_inc_uint_nv(&fusefs_genid);
	ssn->ss_max_iosize = FUSE_MAX_IOSIZE;

	/*
	 * Get attributes of this FUSE library program.
	 */
	err = fusefs_call_init(ssn, 0);
	if (err) {
		FUSEFS_DEBUG("fusefs_call_init error %d\n", err);
		fusefs_ssn_rele(ssn);
		return (err);
	}

	*ret_ssn = ssn;
	return (0);
}

#if 0	/* XXX: not used for now */
void
fusefs_ssn_hold(fusefs_ssn_t *ssn)
{
}
#endif

void
fusefs_ssn_kill(fusefs_ssn_t *ssn)
{
	door_arg_t da;
	struct fuse_generic_arg arg;
	struct fuse_generic_ret ret;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_DESTROY;
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	(void) door_ki_upcall(ssn->ss_door_handle, &da);
}

void
fusefs_ssn_rele(fusefs_ssn_t *ssn)
{
	if (ssn->ss_door_handle != NULL)
		door_ki_rele(ssn->ss_door_handle);
	kmem_free(ssn, sizeof (*ssn));
}

/*
 * Up-calls to the FUSE daemon.
 */

int
fusefs_call_init(fusefs_ssn_t *ssn, int want)
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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	ssn->ss_opts = ret.ret_flags;
	/* XXX - todo: other option stuff */

	return (0);
}


int
fusefs_call_statvfs(fusefs_ssn_t *ssn, statvfs64_t *stv)
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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
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

int
fusefs_call_fgetattr(fusefs_ssn_t *ssn,
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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*fap = ret.ret_st;	/* XXX (stat64) */

	return (0);
}

int
fusefs_call_getattr(fusefs_ssn_t *ssn,
	int rplen, const char *rpath,
	fusefattr_t *fap)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_getattr_ret ret;
	int rc;

	if (rplen >= MAXPATHLEN)
		return (ENAMETOOLONG);

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_GETATTR;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen);
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*fap = ret.ret_st;	/* XXX (stat64) */
	return (0);
}

int
fusefs_call_getattr2(fusefs_ssn_t *ssn,
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

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*fap = ret.ret_st;	/* XXX (stat64) */
	return (0);
}

int
fusefs_call_opendir(fusefs_ssn_t *ssn,
	int rplen, const char *rpath, uint64_t *ret_fid)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_fid_ret ret;
	int rc;

	if (rplen >= MAXPATHLEN)
		return (ENAMETOOLONG);

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_OPENDIR;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen);
	memset(&ret, 0, sizeof (ret));

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*ret_fid = ret.ret_fid;
	return (0);
}

int
fusefs_call_closedir(fusefs_ssn_t *ssn, uint64_t fid)
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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_readdir(fusefs_ssn_t *ssn, uint64_t fid, int offset,
	fusefattr_t *fap, dirent64_t *de, int *eofp)
{
	door_arg_t da;
	struct fuse_read_arg arg;
	struct fuse_readdir_ret *retp;
	uint32_t nmlen;
	int rc;

	memset(&arg, 0, sizeof (arg));
	arg.arg_opcode = FUSE_OP_READDIR;
	arg.arg_fid = fid;
	arg.arg_offset = offset;

	retp = kmem_alloc(sizeof (*retp), KM_SLEEP);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)&arg;
	da.data_size = sizeof (arg);
	da.rbuf = (void *) retp;
	da.rsize = sizeof (*retp);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
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
	/* Convert (*de = retp->ret_de) */
	de->d_ino = retp->ret_de.d_ino;
	de->d_off = retp->ret_de.d_off;
	de->d_reclen = (uint16_t)nmlen;

	/*
	 * Copy the whole d_name field which actually
	 * extends into the ret_name field following.
	 */
	memcpy(de->d_name, retp->ret_de.d_name, nmlen + 1);

	if (retp->ret_flags & 1)
		*eofp = 1;

out:
	kmem_free(retp, sizeof (*retp));
	return (rc);
}

int
fusefs_call_open(fusefs_ssn_t *ssn,
	int rplen, const char *rpath, int oflags, uint64_t *ret_fid)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_fid_ret ret;
	int rc;

	if (rplen >= MAXPATHLEN)
		return (ENAMETOOLONG);

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*ret_fid = ret.ret_fid;
	return (0);
}

int
fusefs_call_close(fusefs_ssn_t *ssn, uint64_t fid)
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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_read(fusefs_ssn_t *ssn,
	uint64_t fid, uint32_t *rlen, uio_t *uiop,
	int rplen, const char *rpath)
{
	door_arg_t da;
	struct fuse_read_arg *argp;
	struct fuse_read_ret *retp;
	int allocsize, rc;

	/* Sanity check */
	if (*rlen > FUSE_MAX_IOSIZE)
		return (EFAULT);

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);
	argp->arg_opcode = FUSE_OP_READ;
	argp->arg_fid = fid;
	argp->arg_offset = uiop->uio_loffset;
	argp->arg_length = *rlen;

	/* Fuse wants the pathname here too. */
	if (rplen > (MAXPATHLEN - 1))
		rplen = MAXPATHLEN - 1;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen+1);

	/* XXX: Later, make allocsize dynamic. */
	allocsize = sizeof (*retp);
	retp = kmem_alloc(allocsize, KM_SLEEP);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) retp;
	da.rsize = allocsize;

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
	if (rc == 0)
		rc = retp->ret_err;
	if (rc != 0)
		goto out;

	/* Length of data we read. */
	*rlen = retp->ret_length;
	rc = uiomove(retp->ret_data, *rlen, UIO_READ, uiop);

out:
	kmem_free(retp, allocsize);
	kmem_free(argp, sizeof (*argp));
	return (rc);
}

int
fusefs_call_write(fusefs_ssn_t *ssn,
	uint64_t fid, uint32_t *rlen, uio_t *uiop,
	int rplen, const char *rpath)
{
	door_arg_t da;
	struct fuse_write_arg *argp;
	struct fuse_write_ret ret;
	int allocsize, rc;

	/* Sanity check */
	if (*rlen > FUSE_MAX_IOSIZE)
		return (EFAULT);

	/* XXX: Later, make allocsize dynamic. */
	allocsize = sizeof (*argp);
	argp = kmem_alloc(allocsize, KM_SLEEP);

	argp->arg_opcode = FUSE_OP_WRITE;
	argp->arg_fid = fid;
	argp->arg_offset = uiop->uio_loffset;
	argp->arg_length = *rlen;

	/* Fuse wants the pathname here too. */
	if (rplen > (MAXPATHLEN - 1))
		rplen = MAXPATHLEN - 1;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen+1);

	/* XXX: Undo changes to uiop on error? */
	rc = uiomove(argp->arg_data, *rlen, UIO_WRITE, uiop);
	if (rc != 0)
		goto out;

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = allocsize;
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
	if (rc == 0)
		rc = ret.ret_err;
	if (rc != 0)
		goto out;

	/* Length of data we read. */
	*rlen = ret.ret_length;

out:
	kmem_free(argp, allocsize);
	return (rc);
}

int
fusefs_call_flush(fusefs_ssn_t *ssn, uint64_t fid)
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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_create(fusefs_ssn_t *ssn,
	int dnlen, const char *dname,
	int cnlen, const char *cname,
	int mode, uint64_t *ret_fid)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_fid_ret ret;
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

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_CREATE;
	argp->arg_pathlen = plen;
	argp->arg_val[0] = mode;

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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	*ret_fid = ret.ret_fid;
	return (0);
}

int
fusefs_call_ftruncate(fusefs_ssn_t *ssn, uint64_t fid, u_offset_t off,
	int rplen, const char *rpath)
{
	door_arg_t da;
	struct fuse_ftrunc_arg *argp;
	struct fuse_generic_ret ret;
	int rc;

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_FTRUNC;
	argp->arg_fid = fid;	/* may be zero */
	argp->arg_offset = off;

	if (rplen > (MAXPATHLEN - 1))
		rplen = MAXPATHLEN - 1;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen+1);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_utimes(fusefs_ssn_t *ssn,
	int rplen, const char *rpath,
	timespec_t *atime, timespec_t *mtime)
{
	door_arg_t da;
	struct fuse_utimes_arg *argp;
	struct fuse_generic_ret ret;
	int rc;

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_UTIMES;
	argp->arg_atime = atime->tv_sec;
	argp->arg_atime_ns = atime->tv_nsec;
	argp->arg_mtime = mtime->tv_sec;
	argp->arg_mtime_ns = mtime->tv_nsec;

	if (rplen > (MAXPATHLEN - 1))
		rplen = MAXPATHLEN - 1;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen+1);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_chmod(fusefs_ssn_t *ssn,
	int rplen, const char *rpath, mode_t mode)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_generic_ret ret;
	int rc;

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_CHMOD;
	argp->arg_val[0] = mode;

	if (rplen > (MAXPATHLEN - 1))
		rplen = MAXPATHLEN - 1;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen+1);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_chown(fusefs_ssn_t *ssn,
	int rplen, const char *rpath, uid_t uid, gid_t gid)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_generic_ret ret;
	int rc;

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_CHOWN;
	argp->arg_val[0] = uid;
	argp->arg_val[1] = gid;

	if (rplen > (MAXPATHLEN - 1))
		rplen = MAXPATHLEN - 1;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen+1);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_delete(fusefs_ssn_t *ssn,
	int rplen, const char *rpath)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_generic_ret ret;
	int rc;

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_DELETE;

	if (rplen > (MAXPATHLEN - 1))
		rplen = MAXPATHLEN - 1;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen+1);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_rename(fusefs_ssn_t *ssn,
	int oldplen, const char *oldpath,
	int dnlen, const char *dname,
	int cnlen, const char *cname)
{
	door_arg_t da;
	struct fuse_path2_arg *argp;
	struct fuse_generic_ret ret;
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
	if (oldplen > (MAXPATHLEN - 1))
		oldplen = MAXPATHLEN - 1;

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);
	argp->arg_opcode = FUSE_OP_RENAME;

	/* Old name */
	argp->arg_p1len = oldplen;
	memcpy(argp->arg_path1, oldpath, oldplen+1);

	/* New name (two parts) */
	argp->arg_p2len = plen;
	p = argp->arg_path2;
	memcpy(p, dname, dnlen);
	p += dnlen;
	if (dnlen > 1)
		*p++ = '/';
	memcpy(p, cname, cnlen);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_mkdir(fusefs_ssn_t *ssn,
	int dnlen, const char *dname,
	int cnlen, const char *cname)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_generic_ret ret;
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

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_MKDIR;
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

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}

int
fusefs_call_rmdir(fusefs_ssn_t *ssn,
	int rplen, const char *rpath)
{
	door_arg_t da;
	struct fuse_path_arg *argp;
	struct fuse_generic_ret ret;
	int rc;

	argp = kmem_zalloc(sizeof (*argp), KM_SLEEP);

	argp->arg_opcode = FUSE_OP_RMDIR;

	if (rplen > (MAXPATHLEN - 1))
		rplen = MAXPATHLEN - 1;
	argp->arg_pathlen = rplen;
	memcpy(argp->arg_path, rpath, rplen+1);

	memset(&da, 0, sizeof (da));
	da.data_ptr = (void *)argp;
	da.data_size = sizeof (*argp);
	da.rbuf = (void *) &ret;
	da.rsize = sizeof (ret);

	rc = door_ki_upcall(ssn->ss_door_handle, &da);

	kmem_free(argp, sizeof (*argp));
	if (rc != 0)
		return (rc);
	if (ret.ret_err != 0)
		return (ret.ret_err);

	return (0);
}
