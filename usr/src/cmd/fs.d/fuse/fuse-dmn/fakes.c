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
 * Fake back-end for fuse-dmn
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <door.h>
#include <thread.h>

#include "fuse_dmn.h"
#include "fakes.h"

static struct fuse_statvfs fake_vfsattr = {
	.f_bsize = MAXBSIZE,
	.f_frsize = DEV_BSIZE,
	.f_blocks = 10000,
	.f_bfree  =  8000,
	.f_bavail =  6000,
	.f_files  =  2000,
	.f_ffree  =  1800,
	.f_favail =  1600,
};

static struct fuse_stat fake_dir_attrs = {
	.st_mode = S_IFDIR | 0750,
	.st_nlink = 2,
	.st_uid = 65534,
	.st_gid = 1,
	.st_size = 64,
	.st_atime_sec = 0x4c1b97a0,
	.st_mtime_sec = 0x4c1b97a0,
	.st_ctime_sec = 0x4c1b97a0,
	.st_blocks = 1,
};

static struct fuse_stat fake_file_attrs = {
	.st_mode = S_IFREG | 0640,
	.st_nlink = 1,
	.st_uid = 65534,
	.st_gid = 1,
	.st_size = 16,
	.st_atime_sec = 0x4c1b97a8,
	.st_mtime_sec = 0x4c1b97a8,
	.st_ctime_sec = 0x4c1b97a8,
	.st_blocks = 2,
};

#define	NFAKES 4
static char *fake_names[] = {
	".",		/* ino 2 (the root) */
	"..",		/* ino 2 (yes, 2 again) */
	"fake",		/* ino 3 */
	"names",	/* ino 4 */
	NULL
};

int
fake_init(uint_t want, uint32_t *ret_opts)
{
	*ret_opts = want;
	return (0);
}

int
fake_statvfs(struct fuse_statvfs *stvfsp)
{
	/* NB: Does not fill in f_fsid, f_basetype */
	*stvfsp = fake_vfsattr;

	return (0);
}

int
fake_fgetattr(uint64_t fd, struct fuse_stat *stp)
{

	if (fd <= 2) {
		/* directory */
		*stp = fake_dir_attrs;
		stp->st_ino = 2;
	} else {
		/* some file */
		*stp = fake_file_attrs;
		stp->st_ino = fd;
	}
	return (0);
}

int
fake_getattr(const char *path, struct fuse_stat *st)
{
	char *name;
	int i, flen, plen;

	plen = strlen(path);
	if (plen <= 1) /* the root */
		return (fake_fgetattr(2, st));

	for (i = 2; i < NFAKES; i++) {
		name = fake_names[i];
		flen = strlen(name);
		if ((plen - 1) == flen && 0 ==
		    strncmp(path+1, name, flen))
			return (fake_fgetattr(i + 1, st));
	}

	DPRINT("getattr: path=%s, ret ENOENT\n", path);
	return (ENOENT);
}

int
fake_opendir(const char *path, uint64_t *ret_fd)
{
	return (fake_open(path, FREAD, ret_fd));
}

int
fake_closedir(uint64_t fd)
{
	return (fake_close(fd));
}

int
fake_readdir(uint64_t fid, off64_t offset, int *eof_flag,
	struct fuse_stat *st, struct fuse_dirent *de)
{
	int nlen;
	char *name;

	if (fid != 2) {
		DPRINT("readdir: fid=%ld, EBADF\n", (long)fid);
		return (EBADF);
	}
	if (offset < 0 || offset >= NFAKES) {
		DPRINT("readdir: off=%d, EBADF\n", (int)offset);
		return (EINVAL);
	}
	name = fake_names[offset];
	nlen = strlen(name);

	if (offset < 2)
		de->d_ino = 2;
	else
		de->d_ino = offset + 1;
	de->d_off = offset + 1;
	de->d_nmlen = nlen;
	memcpy(de->d_name, name, nlen + 1);

	/* Need attributes too. */
	(void) fake_fgetattr(de->d_ino, st);

	if (offset + 1 == NFAKES)
		*eof_flag = 1;

	return (0);
}


int
fake_open(const char *path, int oflags, uint64_t *ret_fd)
{
	struct fuse_stat st;
	int rc;

	rc = fake_getattr(path, &st);
	if (rc != 0)
		return (rc);
	*ret_fd = st.st_ino;
	return (0);
}

int
fake_close(uint64_t fd)
{
	return (0);
}

int
fake_flush(uint64_t fd)
{
	return (0);
}

int
fake_read(uint64_t fd, off64_t offset, uint_t length,
	void *data, uint_t *ret_len)
{
	static char test[16] = "This is a test.\n";

	if (offset >= 16)
		return (0);
	if (length > 16)
		length = 16;

	(void) memcpy(data, test, length);

	/* How much we moved. */
	*ret_len = length;

	return (0);
}

int
fake_write(uint64_t fd, off64_t offset, uint_t length,
	void *data, uint_t *ret_len)
{
	return (EACCES);
}
