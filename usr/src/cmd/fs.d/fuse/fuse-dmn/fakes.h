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

#include <sys/fs/fuse_ktypes.h>

int fake_init(uint_t want, uint32_t *ret_opts);
int fake_statvfs(struct fuse_statvfs *);
int fake_fgetattr(uint64_t, struct fuse_stat *);
int fake_getattr(const char *, struct fuse_stat *);

int fake_opendir(const char *path, uint64_t *ret_fd);
int fake_closedir(uint64_t fid);
int fake_readdir(uint64_t fid, off64_t off, int *eof_flag,
		struct fuse_stat *, struct fuse_dirent *);

int fake_open(const char *path, int oflags, uint64_t *ret_fd);
int fake_close(uint64_t fid);
int fake_read(uint64_t fid, off64_t off, uint_t len,
		void *data, uint_t *ret_len);
int fake_write(uint64_t fid, off64_t off, uint_t len,
		void *data, uint_t *ret_len);
int fake_flush(uint64_t);
