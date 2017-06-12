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
 * FUSE deamon door call dispatch.
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/dirent.h>
#include <sys/note.h>
#include <sys/fs/fuse_door.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <synch.h>
#include <time.h>
#include <unistd.h>

#include <door.h>
#include <thread.h>

#include "fuse_dmn.h"
#include "fakes.h"

/*ARGSUSED*/
static void
do_init(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_generic_arg *arg = vargp;
	struct fuse_generic_ret ret;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_init(arg->arg_flags, &ret.ret_flags);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/*ARGSUSED*/
static void
do_statfs(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_statvfs_ret ret;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_statvfs(&ret.ret_stvfs);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_fgetattr(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_fid_arg *arg = vargp;
	struct fuse_getattr_ret ret;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_fgetattr(arg->arg_fid, &ret.ret_st);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_getattr(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_path_arg *arg = vargp;
	struct fuse_getattr_ret ret;

	if (argsz < sizeof (*arg))
		return;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_getattr(arg->arg_path, &ret.ret_st);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_opendir(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_path_arg *arg = vargp;
	struct fuse_fid_ret ret;
	int rc;

	if (argsz < sizeof (*arg))
		return;

	memset(&ret, 0, sizeof (ret));
	rc = fake_opendir(arg->arg_path, &ret.ret_fid);
	ret.ret_err = rc;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_closedir(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_fid_arg *arg = vargp;
	struct fuse_generic_ret ret;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_closedir(arg->arg_fid);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_readdir(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_read_arg *arg = vargp;
	struct fuse_readdir_ret ret;
	int at_eof = 0;

	if (argsz < sizeof (*arg))
		return;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_readdir(arg->arg_fid, arg->arg_offset,
	    &at_eof, &ret.ret_st, &ret.ret_de);
	if (at_eof)
		ret.ret_flags |= 1;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_open(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_path_arg *arg = vargp;
	struct fuse_fid_ret ret;
	int rc;

	if (argsz < sizeof (*arg))
		return;

	memset(&ret, 0, sizeof (ret));
	rc = fake_open(arg->arg_path, arg->arg_val[0], &ret.ret_fid);
	ret.ret_err = rc;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_close(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_fid_arg *arg = vargp;
	struct fuse_generic_ret ret;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_close(arg->arg_fid);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_read(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_read_arg *arg = vargp;
	struct fuse_read_ret ret;

	if (argsz < sizeof (*arg))
		return;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_read(arg->arg_fid,
	    arg->arg_offset, arg->arg_length,
	    &ret.ret_data, &ret.ret_length);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_write(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_write_arg *arg = vargp;
	struct fuse_write_ret ret;

	if (argsz < sizeof (*arg))
		return;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_write(arg->arg_fid,
	    arg->arg_offset, arg->arg_length,
	    arg->arg_data, &ret.ret_length);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

static void
do_flush(void *cookie, void *vargp, size_t argsz)
{
	struct fuse_fid_arg *arg = vargp;
	struct fuse_generic_ret ret;

	memset(&ret, 0, sizeof (ret));
	ret.ret_err = fake_flush(arg->arg_fid);
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}


/*ARGSUSED*/
void
dmn_dispatch(void *cookie, char *cargp, size_t argsz,
    door_desc_t *dp, uint_t n_desc)
{
	void *vargp = cargp;
	struct fuse_generic_arg *argp;
	struct fuse_generic_ret ret;
	int rc;

	/*
	 * Allow a NULL arg call to check if the
	 * deamon is running.  Just return zero.
	 */
	if (vargp == NULL) {
		rc = 0;
		goto out;
	}

	/*
	 * Decode the op. code and dispatch.
	 * These return only on errors.
	 */
	argp = vargp;
	if (argsz < sizeof (*argp)) {
		rc = EINVAL;
		goto out;
	}
	memset(&ret, 0, sizeof (ret));
	rc = ENOSYS;
	switch (argp->arg_opcode) {

	case FUSE_OP_INIT:
		do_init(cookie, vargp, argsz);
		break;
	case FUSE_OP_STATVFS:
		do_statfs(cookie, vargp, argsz);
		break;

	case FUSE_OP_FGETATTR:
		do_fgetattr(cookie, vargp, argsz);
		break;
	case FUSE_OP_GETATTR:
		do_getattr(cookie, vargp, argsz);
		break;

	case FUSE_OP_OPENDIR:
		do_opendir(cookie, vargp, argsz);
		break;
	case FUSE_OP_CLOSEDIR:
		do_closedir(cookie, vargp, argsz);
		break;
	case FUSE_OP_READDIR:
		do_readdir(cookie, vargp, argsz);
		break;

	case FUSE_OP_OPEN:
		do_open(cookie, vargp, argsz);
		break;
	case FUSE_OP_CLOSE:
		do_close(cookie, vargp, argsz);
		break;
	case FUSE_OP_READ:
		do_read(cookie, vargp, argsz);
		break;
	case FUSE_OP_WRITE:
		do_write(cookie, vargp, argsz);
		break;
	case FUSE_OP_FLUSH:
		do_flush(cookie, vargp, argsz);
		break;

	default:
		fprintf(stderr, "dmn_dispatch, unimpl. op %d\n",
		    argp->arg_opcode);
		rc = ENOSYS;
		break;
	}

out:
	ret.ret_err = rc;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}
