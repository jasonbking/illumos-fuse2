/*
 * FUSE: Filesystem in Userspace
 * Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 *
 * This program can be distributed under the terms of the GNU LGPLv2.
 * See the file COPYING.LIB
 */

#include "fuse_i.h"
/* Instead of "fuse_kernel.h" we have... */
#include <sys/fs/fuse_door.h>	/* Solaris doors */
#include "fuse_opt.h"
#include "fuse_misc.h"
#include "fuse_common_compat.h"
#include "fuse_lowlevel_compat.h"

#include <sys/file.h>
#include <sys/fcntl.h>
#include <sys/note.h>

#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <door.h>
#include <thread.h>
#include <signal.h>

/* XXX: Version number of this interface */
#define	FUSE_KERNEL_VERSION 7

/* XXX: Minor version number of this interface */
#define	FUSE_KERNEL_MINOR_VERSION 12

#define	PARAM(inarg) (((char *)(inarg)) + sizeof(*(inarg)))
#define	OFFSET_MAX 0x7fffffffffffffffLL

#define	FUSE_NAME_OFFSET offsetof(struct fuse_dirent, d_name)
#define	FUSE_DIRENT_ALIGN(x) (((x) + sizeof(uint64_t) - 1) & \
				~(sizeof(uint64_t) - 1))
#define	FUSE_DIRENT_SIZE(d) \
	FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + (d)->d_nmlen)

typedef struct fuse_ll sol_ll_t;

struct fuse_pollhandle {
	uint64_t kh;
	struct fuse_chan *ch;
	struct fuse_ll *f;
};


/*
 * There's only one of each of these, and they're
 * needed in our door service.
 */
static struct fuse		*solaris_fuse;
static struct fuse_session	*solaris_se;
static struct fuse_ll		*solaris_ll;
int solaris_debug = 0;

static void convert_stat(const struct stat *stbuf,
	struct fuse_stat *kst)
{
	kst->st_ino	= (stbuf->st_ino == 0xffffffff) ? 0 : stbuf->st_ino;
	kst->st_size	= stbuf->st_size;
	kst->st_blocks	= stbuf->st_blocks;

	kst->st_atime_sec = stbuf->st_atim.tv_sec;
	kst->st_atime_ns  = stbuf->st_atim.tv_nsec;

	kst->st_mtime_sec = stbuf->st_mtim.tv_sec;
	kst->st_mtime_ns  = stbuf->st_mtim.tv_nsec;

	kst->st_ctime_sec = stbuf->st_ctim.tv_sec;
	kst->st_ctime_ns  = stbuf->st_ctim.tv_nsec;

	kst->st_mode	= stbuf->st_mode;
	kst->st_nlink	= stbuf->st_nlink;
	kst->st_uid	= stbuf->st_uid;
	kst->st_gid	= stbuf->st_gid;
	kst->st_rdev	= stbuf->st_rdev;
	kst->st_blksize = stbuf->st_blksize;
}

static void list_init_req(struct fuse_req *req)
{
	req->next = req;
	req->prev = req;
}

/* ARGSUSED */
int fuse_reply_iov(fuse_req_t req, const struct iovec *iov, int count)
{
	return -ENOSYS;
}

size_t fuse_dirent_size(size_t namelen)
{
	return FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + namelen);
}

char *fuse_add_dirent(char *buf, const char *name, const struct stat *stbuf,
		      off_t off)
{
	unsigned namelen = strlen(name);
	unsigned entlen = FUSE_NAME_OFFSET + namelen;
	unsigned entsize = fuse_dirent_size(namelen);
	unsigned padlen = entsize - entlen;
	/* LINTED: alignment */
	struct fuse_dirent *dirent = (struct fuse_dirent *)buf;

	dirent->d_ino = stbuf->st_ino;
	dirent->d_off = off;
	dirent->d_nmlen = namelen;
	/* XXX: dirent->type = (stbuf->st_mode & 0170000) >> 12; */
	strncpy(dirent->d_name, name, namelen);
	if (padlen)
		memset(buf + entlen, 0, padlen);

	return buf + entsize;
}

size_t fuse_add_direntry(fuse_req_t req, char *buf, size_t bufsize,
			 const char *name, const struct stat *stbuf, off_t off)
{
	size_t entsize;

	(void) req;
	entsize = fuse_dirent_size(strlen(name));
	if (entsize <= bufsize && buf)
		fuse_add_dirent(buf, name, stbuf, off);
	return entsize;
}

static void convert_statvfs(const struct statvfs *stbuf,
			   struct fuse_statvfs *kstatfs)
{
	kstatfs->f_blocks	= stbuf->f_blocks;
	kstatfs->f_bfree	= stbuf->f_bfree;
	kstatfs->f_bavail	= stbuf->f_bavail;
	kstatfs->f_files	= stbuf->f_files;
	kstatfs->f_ffree	= stbuf->f_ffree;
	kstatfs->f_favail	= stbuf->f_favail;
	kstatfs->f_bsize	= stbuf->f_bsize;
	kstatfs->f_frsize	= stbuf->f_frsize;
	kstatfs->f_namemax	= stbuf->f_namemax;
	kstatfs->f_flag		= stbuf->f_flag;
}

/* ARGSUSED */
int fuse_reply_err(fuse_req_t req, int err)
{
	return -ENOSYS;
}

/* ARGSUSED */
void fuse_reply_none(fuse_req_t req)
{
}

static unsigned long calc_timeout_sec(double t)
{
	if (t > (double) ULONG_MAX)
		return ULONG_MAX;
	else if (t < 0.0)
		return 0;
	else
		return (unsigned long) t;
}



#if 0	/* XXX - see do_open */
static void fill_open(struct fuse_open_out *arg,
		      const struct fuse_file_info *f)
{
	arg->fh = f->fh;
	if (f->direct_io)
		arg->open_flags |= FOPEN_DIRECT_IO;
	if (f->keep_cache)
		arg->open_flags |= FOPEN_KEEP_CACHE;
	if (f->nonseekable)
		arg->open_flags |= FOPEN_NONSEEKABLE;
}
#endif	/* XXX */

/* ARGSUSED */
int fuse_reply_entry(fuse_req_t req, const struct fuse_entry_param *e)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_create(fuse_req_t req, const struct fuse_entry_param *e,
		      const struct fuse_file_info *f)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_attr(fuse_req_t req, const struct stat *attr,
		    double attr_timeout)
{
	return (-ENOSYS);
}

/* ARGSUSED */
int fuse_reply_readlink(fuse_req_t req, const char *linkname)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_open(fuse_req_t req, const struct fuse_file_info *f)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_write(fuse_req_t req, size_t count)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_buf(fuse_req_t req, const char *buf, size_t size)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_statfs(fuse_req_t req, const struct statvfs *stbuf)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_xattr(fuse_req_t req, size_t count)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_lock(fuse_req_t req, struct flock *lock)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_bmap(fuse_req_t req, uint64_t idx)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_ioctl_retry(fuse_req_t req,
			   const struct iovec *in_iov, size_t in_count,
			   const struct iovec *out_iov, size_t out_count)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_ioctl(fuse_req_t req, int result, const void *buf, size_t size)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_ioctl_iov(fuse_req_t req, int result, const struct iovec *iov,
			 int count)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_poll(fuse_req_t req, unsigned revents)
{
	return -ENOSYS;
}

/*
 * Solaris equivalent of fuse_lib_init()
 */
void
sol_lib_init(void *data, struct fuse_conn_info *conn)
{
	struct fuse *f = (struct fuse *) data;
	struct fuse_context_i *c = fuse_get_context_internal();
	memset(c, 0, sizeof(*c));
	c->ctx.fuse = f;
	fuse_fs_init(f->fs, conn);
}

/*
 * Solaris equivalent of fuse_lib_destroy()
 */
static void
sol_lib_destroy(void *data)
{
	struct fuse *f = (struct fuse *) data;
	struct fuse_context_i *c = fuse_get_context_internal();

	memset(c, 0, sizeof(*c));
	c->ctx.fuse = f;
	fuse_fs_destroy(f->fs);
	f->fs = NULL;
}

/*
 * "Stock" fuse had all the "lowlevel" operations here:
 *
 * do_lookup, do_forget, do_getattr, do_setattr, do_access,
 * do_readlink, do_mknod, do_mkdir, do_unlink, do_rmdir,
 * do_symlink, do_rename, do_link, do_create, do_open,
 * do_read, do_write, do_flush, do_release, do_fsync,
 * do_opendir, do_readdir, do_releasedir, do_fsyncdir,
 * do_statfs, do_setxattr, do_getxattr, do_listxattr,
 * do_removexattr, do_getlk, do_setlk, do_setlkw,
 * do_bmap, do_ioctl, do_poll, do_init, do_destroy
 *
 * In this implementation, we instead have handler functions
 * for the set of door calls we use.  They are intentionally
 * very close to the functions in struct fuse_operations.
 * See fuse_door.h for the list of door call operations
 * used by the Solaris fusefs.
 */



/*
 * These all take a sol_ll_t * first arg, which is the
 * Solaris replacement for struct fuse_ll.  Otherwise,
 * they're similar in spirit to the fuse_ll_ops.
 */

/* FUSE_OP_INIT */
static void
do_init(sol_ll_t *f, void *vargp, size_t argsz)
{
	_NOTE(ARGUNUSED(argsz));
	struct fuse_generic_arg *arg = vargp;
	struct fuse_generic_ret ret = {0};
	size_t bufsize = fuse_chan_bufsize(solaris_se->ch);
	size_t arg_max_readahead = FUSE_MAX_IOSIZE; /* XXX */

	if (f->debug) {
		fprintf(stderr, "INIT: (solaris doors)\n");
		fprintf(stderr, "flags=0x%08x\n", arg->arg_flags);
		fprintf(stderr, "max_readahead=0x%08x\n",
			arg_max_readahead);
	}
	/*
	 * We're not using the FUSE kernel protocol.
	 * (using solaris doors instead)
	 */
	f->conn.proto_major = FUSE_KERNEL_VERSION;
	f->conn.proto_minor = FUSE_KERNEL_MINOR_VERSION;
	f->conn.capable = 0;
	f->conn.want = 0;

#if 0	/* XXX - needed? */
	if (f->conn.async_read)
		f->conn.async_read = arg->arg_flags & FUSE_ASYNC_READ;
	if (arg_max_readahead < f->conn.max_readahead)
		f->conn.max_readahead = arg_max_readahead;
	if (arg->arg_flags & FUSE_ASYNC_READ)
		f->conn.capable |= FUSE_CAP_ASYNC_READ;
	if (arg->arg_flags & FUSE_POSIX_LOCKS)
		f->conn.capable |= FUSE_CAP_POSIX_LOCKS;
	if (arg->arg_flags & FUSE_ATOMIC_O_TRUNC)
		f->conn.capable |= FUSE_CAP_ATOMIC_O_TRUNC;
	if (arg->arg_flags & FUSE_EXPORT_SUPPORT)
		f->conn.capable |= FUSE_CAP_EXPORT_SUPPORT;
	if (arg->arg_flags & FUSE_BIG_WRITES)
		f->conn.capable |= FUSE_CAP_BIG_WRITES;
	if (arg->arg_flags & FUSE_DONT_MASK)
		f->conn.capable |= FUSE_CAP_DONT_MASK;

	if (f->atomic_o_trunc)
		f->conn.want |= FUSE_CAP_ATOMIC_O_TRUNC;
	if (f->posix_locks && !f->no_remote_lock)
		f->conn.want |= FUSE_CAP_POSIX_LOCKS;
	if (f->big_writes)
		f->conn.want |= FUSE_CAP_BIG_WRITES;
	/* XXX: conn->want |= FUSE_CAP_EXPORT_SUPPORT; */

	if (bufsize < FUSE_MIN_READ_BUFFER) {
		fprintf(stderr, "fuse: warning: buffer size too small: %zu\n",
			bufsize);
		bufsize = FUSE_MIN_READ_BUFFER;
	}
#endif	/* XXX */

	bufsize -= 4096;
	if (bufsize < f->conn.max_write)
		f->conn.max_write = bufsize;

	f->got_init = 1;
	sol_lib_init(f->userdata, &f->conn);

#if 0	/* XXX - needed? */
	if (f->conn.async_read || (f->conn.want & FUSE_CAP_ASYNC_READ))
		ret.ret_flags |= FUSE_ASYNC_READ;
	if (f->conn.want & FUSE_CAP_POSIX_LOCKS)
		ret.ret_flags |= FUSE_POSIX_LOCKS;
	if (f->conn.want & FUSE_CAP_ATOMIC_O_TRUNC)
		ret.ret_flags |= FUSE_ATOMIC_O_TRUNC;
	if (f->conn.want & FUSE_CAP_EXPORT_SUPPORT)
		ret.ret_flags |= FUSE_EXPORT_SUPPORT;
	if (f->conn.want & FUSE_CAP_BIG_WRITES)
		ret.ret_flags |= FUSE_BIG_WRITES;
	if (f->conn.want & FUSE_CAP_DONT_MASK)
		ret.ret_flags |= FUSE_DONT_MASK;
#endif	/* XXX */

	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_DESTROY */
static void
do_destroy(sol_ll_t *ll, void *vargp, size_t argsz)
{
	_NOTE(ARGUNUSED(vargp, argsz));
	struct fuse_generic_ret ret = {0};

	if (ll->debug)
		fprintf(stderr, "got destroy request\n");
	ll->got_destroy = 1;

	/* Make sure door calls stop. */
	fuse_sol_door_destroy();
	sol_lib_destroy(ll->userdata);

	/* Make the sigwait quit (soon). */
	alarm(1);

	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_STATVFS */
static void
do_statfs(sol_ll_t *ll, void *vargp, size_t argsz)
{
	_NOTE(ARGUNUSED(vargp, argsz));
	struct fuse *f = ll->userdata;
	struct fuse_statvfs_ret ret = { 0 };
	struct statvfs stvfs;
	int err;

	/* XXX: Always the root - OK? */
	memset(&stvfs, 0, sizeof(stvfs));
	err = fuse_fs_statfs(f->fs, "/", &stvfs);
	if (err == 0) {
		convert_statvfs(&stvfs, &ret.ret_stvfs);
	}

	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_FGETATTR */
static void
do_fgetattr(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_fid_arg *arg = vargp;
	struct fuse_getattr_ret ret = {0};
	struct fuse_file_info fi;
	struct stat st;
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	memset(&fi, 0, sizeof (fi));
	fi.fh = arg->arg_fid;
	fi.fh_old = fi.fh;

	/* See fuse_lib_getattr */
	memset(&st, 0, sizeof(st));
	err = fuse_fs_fgetattr(f->fs, NULL, &st, &fi);
	if (err == 0) {
		convert_stat(&st, &ret.ret_st);
	}

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_GETATTR */
static void
do_getattr(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_getattr_ret ret = { 0 };
	struct stat st;
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	/* See fuse_lib_getattr */
	memset(&st, 0, sizeof(st));
	err = fuse_fs_getattr(f->fs, arg->arg_path, &st);
	if (err == 0) {
		convert_stat(&st, &ret.ret_st);
	}

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_OPENDIR */
static void
do_opendir(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_fid_ret ret = { 0 };
	struct fuse_file_info fi;
	struct fuse_dh *dh = NULL;
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	/* See fuse_lib_opendir */
	dh = malloc(sizeof(*dh));
	if (dh == NULL) {
		err = -ENOMEM;
		goto out;
	}
	memset(dh, 0, sizeof(struct fuse_dh));
	dh->fuse = f;
	dh->contents = NULL;
	dh->len = 0;
	dh->filled = 0;
	dh->nodeid = 0;	/* XXX: ino? */
	fuse_mutex_init(&dh->lock);

	/* Save the path for readdir */
	dh->path = malloc(arg->arg_pathlen + 1);
	if (dh->path == NULL) {
		err = -ENOMEM;
		goto out;
	}
	dh->pathlen = arg->arg_pathlen;
	memcpy(dh->path, arg->arg_path, dh->pathlen + 1);

	memset(&fi, 0, sizeof(fi));
	fi.flags = O_RDONLY;

	err = fuse_fs_opendir(f->fs, arg->arg_path, &fi);
	if (err == 0) {
		dh->fh = fi.fh;
		ret.ret_fid = (uintptr_t) dh;
	}

 out:
	if (err) {
		if (dh) {
			pthread_mutex_destroy(&dh->lock);
			free(dh->path);
			free(dh);
		}
	}
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_CLOSEDIR */
static void
do_closedir(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_fid_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	struct fuse_file_info fi;
	struct fuse_dh *dh;
	int err = 0;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	/* See fuse_lib_releasedir */
	dh = (struct fuse_dh *) (uintptr_t) arg->arg_fid;
	memset(&fi, 0, sizeof(fi));
	fi.fh = dh->fh;
	fi.fh_old = fi.fh;

	fuse_fs_releasedir(f->fs, "-", &fi);
	pthread_mutex_destroy(&dh->lock);
	free(dh->contents);
	free(dh);

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_READDIR */
static void
do_readdir(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_read_arg *arg = vargp;
	struct fuse_readdir_ret ret = { 0 };
	struct fuse_file_info fi = { 0 };
	struct stat st = { 0 };
	struct fuse_dh *dh;
	struct fuse_dirent *de;
	char *fmt, *p, *path = NULL;
	size_t off, size;
	int err = 0;
	int nmlen, pathlen;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out2;
	}

	/* See fuse_lib_readdir, get_dirhandle */
	dh = (struct fuse_dh *) (uintptr_t) arg->arg_fid;
	fi.fh = dh->fh;
	fi.fh_old = fi.fh;

	off = (size_t) arg->arg_offset;
	size = FUSE_MAX_IOSIZE;

	pthread_mutex_lock(&dh->lock);
	/* According to SUS, directory contents need to be refreshed on
	   rewinddir() */
	if (off == 0)
		dh->filled = 0;

	if (!dh->filled) {
		/* err = readdir_fill(f, req, ino, size, off, dh, &fi); */
		dh->len = 0;
		dh->error = 0;
		dh->needlen = size;
		dh->filled = 1;
		/* dh->req = req; XXX */
		err = fuse_fs_readdir(f->fs, dh->path, dh,
			fuse_fill_dir, arg->arg_offset, &fi);
		if (err)
			goto out;
	}
	if (dh->filled) {
		if (off < dh->len) {
			if (off + size > dh->len)
				size = dh->len - off;
		} else
			size = 0;
	} else {
		size = dh->len;
		off = 0;
	}

	/* EOF check */
	if (size == 0) {
		err = ENOSPC;	/* EOF */
		goto out;
	}

	/* fuse_reply_buf(req, dh->contents + off, size); */
	/* LINTED: alignment */
	de = (struct fuse_dirent *) (dh->contents + off);
	/* XXX nmlen = strnlen(de->d_name, 255); */
	nmlen = de->d_nmlen;

	if (de->d_name[0] == '.' && (nmlen == 1 ||
	    de->d_name[1] == '.' && nmlen == 2)) {
		/* Don't get stat for . or .. */
	} else {
		/* Need the full path for stat */
		pathlen = dh->pathlen + nmlen + 2;
		path = malloc(pathlen);
		if (path == NULL) {
			err = -ENOMEM;
			goto out;
		}
		p = path;
		memcpy(p, dh->path, dh->pathlen);
		p += dh->pathlen;
		if (dh->pathlen > 1)
			*p++ = '/';
		memcpy(p, de->d_name, de->d_nmlen);
		p += de->d_nmlen;
		*p = '\0';

		/* Get the stat for this dirent */
		err = fuse_fs_getattr(f->fs, path, &st);
		if (err)
			goto out;
		convert_stat(&st, &ret.ret_st);
	}

	/*
	 * Copy+convert the dirent.
	 * Yes, reclen is nmlen here.
	 */
	size = fuse_dirent_size(nmlen);
	ret.ret_de.d_ino = st.st_ino;
	ret.ret_de.d_off = de->d_off;
	ret.ret_de.d_nmlen = de->d_nmlen;
	p = ret.ret_de.d_name;
	memcpy(p, de->d_name, de->d_nmlen);
	p += de->d_nmlen;
	*p = '\0';

	if (de->d_off >= dh->len) {
		ret.ret_flags |= 1; /* EOF */
	}

out:
	if (path)
		free(path);
	pthread_mutex_unlock(&dh->lock);

out2:
	if (ll->debug)
		fprintf(stderr, "readdir, err=%d\n", err);
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_OPEN */
static void
do_open(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_fid_ret ret = { 0 };
	struct fuse_file_info fi;
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	memset(&fi, 0, sizeof(fi));
	/* arg_flags are sys/file.h FREAD, FWRITE, etc */
	if (arg->arg_val[0] & FWRITE)
		fi.flags = O_RDWR;
	else
		fi.flags = O_RDONLY;

	err = fuse_fs_open(f->fs, arg->arg_path, &fi);
	if (err == 0) {
		ret.ret_fid = fi.fh;
	}

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_CLOSE */
static void
do_close(sol_ll_t *ll, void *vargp, size_t argsz)
{
	_NOTE(ARGUNUSED(argsz));
	struct fuse *f = ll->userdata;
	struct fuse_fid_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	struct fuse_file_info fi;
	const char *path = "-"; /* XXX - OK? */

	memset(&fi, 0, sizeof(fi));
	fi.fh = arg->arg_fid;
	fi.fh_old = fi.fh;

#if 0	/* XXX needed?  FSYNC? */
	if (arg->arg_flags & FUSE_RELEASE_FLUSH) {
		fuse_fs_flush(f->fs, path, &fi);
	}
#endif	/* XXX */
	fuse_fs_release(f->fs, path, &fi);

	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_READ */
static void
do_read(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_read_arg *arg = vargp;
	struct fuse_read_ret ret = { 0 };
	struct fuse_file_info fi;
	int err, res;

	if (argsz < sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	memset(&fi, 0, sizeof(fi));
	fi.fh = arg->arg_fid;
	fi.fh_old = fi.fh;

	res = fuse_fs_read(f->fs, arg->arg_path,
		ret.ret_data, arg->arg_length,
		arg->arg_offset, &fi);
	if (res < 0) {
		err = res;
		goto out;
	}
	ret.ret_length = res;
	err = 0;

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_WRITE */
static void
do_write(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_write_arg *arg = vargp;
	struct fuse_write_ret ret = { 0 };
	struct fuse_file_info fi;
	int err, res;

	if (argsz < sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	memset(&fi, 0, sizeof(fi));
	fi.fh = arg->arg_fid;
	fi.fh_old = fi.fh;


	res = fuse_fs_write(f->fs, arg->arg_path,
		arg->arg_data, arg->arg_length,
		arg->arg_offset, &fi);
	if (res < 0) {
		err = res;
		goto out;
	}
	ret.ret_length = res;
	err = 0;

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_FLUSH */
static void
do_flush(sol_ll_t *ll, void *vargp, size_t argsz)
{
	_NOTE(ARGUNUSED(argsz));
	struct fuse *f = ll->userdata;
	struct fuse_fid_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	struct fuse_file_info fi;
	const char *path = "-"; /* XXX - OK? */

	if (argsz != sizeof (*arg))
		goto out;

	memset(&fi, 0, sizeof(fi));
	fi.fh = arg->arg_fid;
	fi.fh_old = fi.fh;
	fuse_fs_flush(f->fs, path, &fi);

out:
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_CREATE */
static void
do_create(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_fid_ret ret = { 0 };
	struct fuse_file_info fi;
	mode_t mode;
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	/*
	 * arg_val[0] is requested access (open mode)
	 * here we want file type + mode.
	 */
	mode = arg->arg_val[0] | S_IFREG | S_IRUSR;
	memset(&fi, 0, sizeof(fi));
	err = fuse_fs_create(f->fs, arg->arg_path, mode, &fi);
	if (err == 0) {
		ret.ret_fid = fi.fh;
		goto out;
	}

	if (err != -ENOSYS)
		goto out;

	/*
	 * OK, _create gave ENOSYS.  Try mknod+open
	 */
	memset(&fi, 0, sizeof(fi));
	err = fuse_fs_mknod(f->fs, arg->arg_path, mode, 0);
	if (err == 0) {
		err = fuse_fs_open(f->fs, arg->arg_path, &fi);
		if (err == 0)
			goto out;
		(void) fuse_fs_unlink(f->fs, arg->arg_path);
	}

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_FTRUNC */
static void
do_ftruncate(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_ftrunc_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	struct fuse_file_info fi;
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	if (arg->arg_fid != 0) {
		memset(&fi, 0, sizeof(fi));
		fi.fh = arg->arg_fid;
		fi.fh_old = fi.fh;

		err = fuse_fs_ftruncate(f->fs, arg->arg_path,
					arg->arg_offset, &fi);
	} else {
		err = fuse_fs_truncate(f->fs, arg->arg_path,
					arg->arg_offset);
	}

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_UTIMES */
static void
do_utimes(sol_ll_t *ll, void *vargp, size_t argsz)
{
	_NOTE(ARGUNUSED(argsz));
	struct fuse *f = ll->userdata;
	struct fuse_utimes_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	struct timespec tv[2]; /* atime, mtime */
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	memset(tv, 0, sizeof (tv));
	tv[0].tv_sec = arg->arg_atime;
	tv[0].tv_nsec = arg->arg_atime_ns;
	tv[1].tv_sec = arg->arg_mtime;
	tv[1].tv_nsec = arg->arg_mtime_ns;

	err = fuse_fs_utimens(f->fs, arg->arg_path, tv);

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_CHMOD */
static void
do_chmod(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	/* arg_val[0] is the mode */
	err = fuse_fs_chmod(f->fs, arg->arg_path, arg->arg_val[0]);

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_CHOWN */
static void
do_chown(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	err = fuse_fs_chown(f->fs, arg->arg_path,
	    arg->arg_val[0], arg->arg_val[1]);

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_DELETE */
static void
do_delete(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	err = fuse_fs_unlink(f->fs, arg->arg_path);

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_RENAME */
static void
do_rename(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path2_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}
	err = fuse_fs_rename(f->fs, arg->arg_path1, arg->arg_path2);

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_MKDIR */
static void
do_mkdir(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}

	/* XXX: get the mode... */
	err = fuse_fs_mkdir(f->fs, arg->arg_path, 0700);

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/* FUSE_OP_RMDIR */
static void
do_rmdir(sol_ll_t *ll, void *vargp, size_t argsz)
{
	struct fuse *f = ll->userdata;
	struct fuse_path_arg *arg = vargp;
	struct fuse_generic_ret ret = { 0 };
	int err;

	if (argsz != sizeof (*arg)) {
		err = -EINVAL;
		goto out;
	}
	err = fuse_fs_rmdir(f->fs, arg->arg_path);

out:
	ret.ret_err = -err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}

/*ARGSUSED*/
void
sol_dispatch(void *door_cookie, char *cargp, size_t argsz,
    void *dp, uint_t n_desc)
{
	void *vargp = cargp;
	struct fuse_generic_arg *argp = vargp;
	struct fuse_generic_ret ret = { 0 };
	sol_ll_t *ll = solaris_ll;
	int err = 0;

	/*
	 * Allow a NULL arg call to check if the
	 * deamon is running.  Just return zero.
	 */
	if (vargp == NULL) {
		err = 0;
		goto out;
	}

	/* XXX: context setup? */
	if (ll == NULL || ll->got_destroy) {
		err = ESRCH;
		goto out;
	}

	/*
	 * Decode the op. code and dispatch.	
	 * These return only on errors.
	 * They all get a struct fuse_ll
	 */
	if (argsz < sizeof (*argp)) {
		err = EINVAL;
		goto out;
	}
	memset(&ret, 0, sizeof (ret));

	switch (argp->arg_opcode) {

	/*
	 * Misc and VFS operations
	 */

	case FUSE_OP_INIT:
		do_init(ll, vargp, argsz);
		break;

	case FUSE_OP_DESTROY:
		do_destroy(ll, vargp, argsz);
		break;

	case FUSE_OP_STATVFS:
		do_statfs(ll, vargp, argsz);
		break;

	case FUSE_OP_FGETATTR:
		do_fgetattr(ll, vargp, argsz);
		break;

	/*
	 * Non-modify operations
	 */

	case FUSE_OP_GETATTR:
		do_getattr(ll, vargp, argsz);
		break;

	case FUSE_OP_OPENDIR:
		do_opendir(ll, vargp, argsz);
		break;

	case FUSE_OP_CLOSEDIR:
		do_closedir(ll, vargp, argsz);
		break;

	case FUSE_OP_READDIR:
		do_readdir(ll, vargp, argsz);
		break;

	case FUSE_OP_OPEN:
		do_open(ll, vargp, argsz);
		break;

	case FUSE_OP_CLOSE:
		do_close(ll, vargp, argsz);
		break;

	case FUSE_OP_READ:
		do_read(ll, vargp, argsz);
		break;

	/*
	 * Modify operations
	 */
	case FUSE_OP_WRITE:
		do_write(ll, vargp, argsz);
		break;

	case FUSE_OP_FLUSH:
		do_flush(ll, vargp, argsz);
		break;

	case FUSE_OP_CREATE:
		do_create(ll, vargp, argsz);
		break;

	case FUSE_OP_FTRUNC:
		do_ftruncate(ll, vargp, argsz);
		break;

	case FUSE_OP_UTIMES:
		do_utimes(ll, vargp, argsz);
		break;

	case FUSE_OP_CHMOD:
		do_chmod(ll, vargp, argsz);
		break;

	case FUSE_OP_CHOWN:
		do_chown(ll, vargp, argsz);
		break;

	case FUSE_OP_DELETE:
		do_delete(ll, vargp, argsz);
		break;

	case FUSE_OP_RENAME:
		do_rename(ll, vargp, argsz);
		break;

	case FUSE_OP_MKDIR:
		do_mkdir(ll, vargp, argsz);
		break;

	case FUSE_OP_RMDIR:
		do_rmdir(ll, vargp, argsz);
		break;

	default:
		fprintf(stderr, "sol_dispatch, unimpl. op %d\n",
			argp->arg_opcode);
		err = ENOSYS;
		break;
	}

	/* XXX: context cleanup? */

out:
	ret.ret_err = err;
	door_return((void *)&ret, sizeof (ret), NULL, 0);
}


void fuse_pollhandle_destroy(struct fuse_pollhandle *ph)
{
	free(ph);
}

/* ARGSUSED */
int fuse_lowlevel_notify_poll(struct fuse_pollhandle *ph)
{
	return 0;
}

/* ARGSUSED */
int fuse_lowlevel_notify_inval_inode(struct fuse_chan *ch, fuse_ino_t ino,
                                     off_t off, off_t len)
{
	return 0;
}

/* ARGSUSED */
int fuse_lowlevel_notify_inval_entry(struct fuse_chan *ch, fuse_ino_t parent,
                                     const char *name, size_t namelen)
{
	return 0;
}

/* ARGSUSED */
void *fuse_req_userdata(fuse_req_t req)
{
	return req->f->userdata;
}

const struct fuse_ctx *fuse_req_ctx(fuse_req_t req)
{
	return &req->ctx;
}

/*
 * The size of fuse_ctx got extended, so need to be careful about
 * incompatibility (i.e. a new binary cannot work with an old
 * library).
 */
const struct fuse_ctx *fuse_req_ctx_compat24(fuse_req_t req);
const struct fuse_ctx *fuse_req_ctx_compat24(fuse_req_t req)
{
	return fuse_req_ctx(req);
}
FUSE_SYMVER(".symver fuse_req_ctx_compat24,fuse_req_ctx@FUSE_2.4");


void fuse_req_interrupt_func(fuse_req_t req, fuse_interrupt_func_t func,
			     void *data)
{
	pthread_mutex_lock(&req->lock);
	pthread_mutex_lock(&req->f->lock);
	req->u.ni.func = func;
	req->u.ni.data = data;
	pthread_mutex_unlock(&req->f->lock);
	if (req->interrupted && func)
		func(req, data);
	pthread_mutex_unlock(&req->lock);
}

int fuse_req_interrupted(fuse_req_t req)
{
	int interrupted;

	pthread_mutex_lock(&req->f->lock);
	interrupted = req->interrupted;
	pthread_mutex_unlock(&req->f->lock);

	return interrupted;
}


/* ARGSUSED */
static void fuse_sol_process(void *data, const char *buf, size_t len,
			    struct fuse_chan *ch)
{
#if 0	/* XXX */
	struct fuse_ll *f = (struct fuse_ll *) data;
	int err;

	/*
	 * XXX: Todo - decode door args,
	 * dispatch, door_return()
	 */

	req = (struct fuse_req *) calloc(1, sizeof(struct fuse_req));
	if (req == NULL) {
		fprintf(stderr, "fuse: failed to allocate request\n");
		return;
	}

	req->f = f;
	req->unique = in->unique;
	req->ctx.uid = in->uid;
	req->ctx.gid = in->gid;
	req->ctx.pid = in->pid;
	req->ch = ch;
	req->ctr = 1;
	list_init_req(req);
	fuse_mutex_init(&req->lock);

	err = EIO;
	if (!f->got_init) {
		enum fuse_opcode expected;

		expected = f->cuse_data ? CUSE_INIT : FUSE_INIT;
		if (in->opcode != expected)
			goto reply_err;
	} else if (in->opcode == FUSE_INIT || in->opcode == CUSE_INIT)
		goto reply_err;


	err = ENOSYS;

	/* XXX: switch (in->opcode) ... */
	/* fuse_sol_ops[in->opcode].func(req, in->nodeid, inarg); */
	return;

 reply_err:
	/* XXX fuse_reply_err(req, err); */
#endif	/* XXX */
	return;
}

enum {
	KEY_HELP,
	KEY_VERSION,
};

static struct fuse_opt fuse_sol_opts[] = {
	{ "debug", offsetof(struct fuse_ll, debug), 1 },
	{ "-d", offsetof(struct fuse_ll, debug), 1 },
	{ "allow_root", offsetof(struct fuse_ll, allow_root), 1 },
	{ "max_write=%u", offsetof(struct fuse_ll, conn.max_write), 0 },
	{ "max_readahead=%u", offsetof(struct fuse_ll, conn.max_readahead), 0 },
	{ "async_read", offsetof(struct fuse_ll, conn.async_read), 1 },
	{ "sync_read", offsetof(struct fuse_ll, conn.async_read), 0 },
	{ "atomic_o_trunc", offsetof(struct fuse_ll, atomic_o_trunc), 1},
	{ "no_remote_lock", offsetof(struct fuse_ll, no_remote_lock), 1},
	{ "big_writes", offsetof(struct fuse_ll, big_writes), 1},
	FUSE_OPT_KEY("max_read=", FUSE_OPT_KEY_DISCARD),
	FUSE_OPT_KEY("-h", KEY_HELP),
	FUSE_OPT_KEY("--help", KEY_HELP),
	FUSE_OPT_KEY("-V", KEY_VERSION),
	FUSE_OPT_KEY("--version", KEY_VERSION),
	FUSE_OPT_END
};

static void fuse_sol_version(void)
{
	fprintf(stderr, "using FUSE kernel interface version %i.%i\n",
		FUSE_KERNEL_VERSION, FUSE_KERNEL_MINOR_VERSION);
}

static void fuse_sol_help(void)
{
	fprintf(stderr,
"    -o max_write=N         set maximum size of write requests\n"
"    -o max_readahead=N     set maximum readahead\n"
"    -o async_read          perform reads asynchronously (default)\n"
"    -o sync_read           perform reads synchronously\n"
"    -o atomic_o_trunc      enable atomic open+truncate support\n"
"    -o big_writes          enable larger than 4kB writes\n"
"    -o no_remote_lock      disable remote file locking\n");
}

static int fuse_sol_opt_proc(void *data, const char *arg, int key,
			    struct fuse_args *outargs)
{
	(void) data; (void) outargs;

	switch (key) {
	case KEY_HELP:
		fuse_sol_help();
		break;

	case KEY_VERSION:
		fuse_sol_version();
		break;

	default:
		fprintf(stderr, "fuse: unknown option `%s'\n", arg);
	}

	return -1;
}

int fuse_lowlevel_is_lib_option(const char *opt)
{
	return fuse_opt_match(fuse_sol_opts, opt);
}

static void fuse_sol_destroy(void *data)
{
	struct fuse_ll *ll = (struct fuse_ll *) data;

	/* Make sure door calls stop. */
	fuse_sol_door_destroy();

	if (ll->got_init && !ll->got_destroy) {
		sol_lib_destroy(ll->userdata);
	}

	pthread_mutex_destroy(&ll->lock);
	free(ll->cuse_data);
	free(ll);
}

/*
 * This implementation does not support or use the "lowlevel" stuff.
 * Instead, this just creates a session that will dispatch the fusefs
 * door calls.
 * XXX: Want sol_ll_t here...
 */
struct fuse_session *fuse_solaris_new_common(struct fuse_args *args,
					     void *userdata)
{
	struct fuse_ll *f = NULL;
	struct fuse_session *se = NULL;
	struct fuse_session_ops sop = {
		.process = fuse_sol_process,
		.destroy = fuse_sol_destroy,
	};

	f = (struct fuse_ll *) calloc(1, sizeof(struct fuse_ll));
	if (f == NULL) {
		fprintf(stderr, "fuse: failed to allocate fuse object\n");
		goto errout;
	}

	f->conn.async_read = 1;
	f->conn.max_write = UINT_MAX;
	f->conn.max_readahead = UINT_MAX;
	f->atomic_o_trunc = 0;
	list_init_req(&f->list);
	list_init_req(&f->interrupts);
	fuse_mutex_init(&f->lock);

	if (fuse_opt_parse(args, f, fuse_sol_opts, fuse_sol_opt_proc) == -1)
		goto errout;

	if (f->debug) {
		fprintf(stderr, "FUSE library version: %s\n", PACKAGE_VERSION);
		solaris_debug = 1;
	}

	/* XXX: memcpy(&f->op, op, op_size); */
	f->owner = getuid();
	f->userdata = userdata;		/* struct fuse */

	se = fuse_session_new(&sop, f);
	if (!se)
		goto errout;

	/* See top of file. */
	solaris_fuse = userdata;
	solaris_se = se;
	solaris_ll = f;

	return se;

errout:
	if (f)
		fuse_sol_destroy(f);

	return NULL;
}

/* ARGSUSED */
struct fuse_session *fuse_lowlevel_new_common(struct fuse_args *args,
					      const struct fuse_lowlevel_ops *op,
					      size_t op_size, void *userdata)
{
	fprintf(stderr, "fuse_lowlevel_new is not supported.\n");
	return (NULL);
}

struct fuse_session *fuse_lowlevel_new(struct fuse_args *args,
				       const struct fuse_lowlevel_ops *op,
				       size_t op_size, void *userdata)
{
	return fuse_lowlevel_new_common(args, op, op_size, userdata);
}

#ifndef linux
/*
 * This is currently not implemented on other than Linux...
 */
/* ARGSUSED */
int fuse_req_getgroups(fuse_req_t req, int size, gid_t list[])
{
	return -ENOSYS;
}
#endif

#if !defined(__FreeBSD__) /* XXX? && !defined(__SOLARIS__) */


/* ARGSUSED */
int fuse_reply_open_compat(fuse_req_t req,
			   const struct fuse_file_info_compat *f)
{
	return -ENOSYS;
}

/* ARGSUSED */
int fuse_reply_statfs_compat(fuse_req_t req, const struct statfs *stbuf)
{
	return -ENOSYS;
}

struct fuse_session *fuse_lowlevel_new_compat(const char *opts,
				const struct fuse_lowlevel_ops_compat *op,
				size_t op_size, void *userdata)
{
	struct fuse_session *se;
	struct fuse_args args = FUSE_ARGS_INIT(0, NULL);

	if (opts &&
	    (fuse_opt_add_arg(&args, "") == -1 ||
	     fuse_opt_add_arg(&args, "-o") == -1 ||
	     fuse_opt_add_arg(&args, opts) == -1)) {
		fuse_opt_free_args(&args);
		return NULL;
	}
	se = fuse_lowlevel_new(&args, (const struct fuse_lowlevel_ops *) op,
			       op_size, userdata);
	fuse_opt_free_args(&args);

	return se;
}

struct fuse_sol_compat_conf {
	unsigned max_read;
	int set_max_read;
};

static const struct fuse_opt fuse_sol_opts_compat[] = {
	{ "max_read=", offsetof(struct fuse_sol_compat_conf, set_max_read), 1 },
	{ "max_read=%u", offsetof(struct fuse_sol_compat_conf, max_read), 0 },
	FUSE_OPT_KEY("max_read=", FUSE_OPT_KEY_KEEP),
	FUSE_OPT_END
};

int fuse_sync_compat_args(struct fuse_args *args)
{
	struct fuse_sol_compat_conf conf;

	memset(&conf, 0, sizeof(conf));
	if (fuse_opt_parse(args, &conf, fuse_sol_opts_compat, NULL) == -1)
		return -1;

	if (fuse_opt_insert_arg(args, 1, "-osync_read"))
		return -1;

	if (conf.set_max_read) {
		char tmpbuf[64];

		sprintf(tmpbuf, "-omax_readahead=%u", conf.max_read);
		if (fuse_opt_insert_arg(args, 1, tmpbuf) == -1)
			return -1;
	}
	return 0;
}

FUSE_SYMVER(".symver fuse_reply_statfs_compat,fuse_reply_statfs@FUSE_2.4");
FUSE_SYMVER(".symver fuse_reply_open_compat,fuse_reply_open@FUSE_2.4");
FUSE_SYMVER(".symver fuse_lowlevel_new_compat,fuse_lowlevel_new@FUSE_2.4");

#else /* __FreeBSD__ || __SOLARIS__ */

int fuse_sync_compat_args(struct fuse_args *args)
{
	(void) args;
	return 0;
}

#endif /* __FreeBSD__ || __SOLARIS__ */

struct fuse_session *fuse_lowlevel_new_compat25(struct fuse_args *args,
				const struct fuse_lowlevel_ops_compat25 *op,
				size_t op_size, void *userdata)
{
	if (fuse_sync_compat_args(args) == -1)
		return NULL;

	return fuse_lowlevel_new_common(args,
					(const struct fuse_lowlevel_ops *) op,
					op_size, userdata);
}

FUSE_SYMVER(".symver fuse_lowlevel_new_compat25,fuse_lowlevel_new@FUSE_2.5");

/* Solaris "session loop" stuff. */

/*
 * This implementation does not use a worker loop.
 * Instead, we use a door - see libdoor(3lib).
 * This (the main thread) just waits for signals.
 */
static int
fuse_session_loop_solaris(struct fuse_session *se)
{
	int res = 0;
	struct fuse_chan *ch = fuse_session_next_chan(se, NULL);
	size_t bufsize = fuse_chan_bufsize(ch);
	sigset_t sigmask;
	int sig;

	sigfillset(&sigmask);

	/* temporary... */
	sigwait(&sigmask, &sig);
	/* XXX: Any special signals? */

out:
	fuse_session_reset(se);
	return (0);
}

int
fuse_session_loop(struct fuse_session *se)
{
	return (fuse_session_loop_solaris(se));
}

int
fuse_session_loop_mt(struct fuse_session *se)
{
	return (fuse_session_loop_solaris(se));
}

/*
 * Solaris "channel" stuff.  We don't use channels,	
 * (the kernel to user message stuff) instead using
 * Solaris doors, but libfuse wants to have one of
 * these objects.  So stub these out.
 */

/* ARGSUSED */
static int fuse_sol_chan_receive(struct fuse_chan **chp, char *buf,
				  size_t size)
{
	return -ENOSYS;
}

/* ARGSUSED */
static int fuse_sol_chan_send(struct fuse_chan *ch, const struct iovec iov[],
			       size_t count)
{
	return 0;
}

/* ARGSUSED */
static void fuse_sol_chan_destroy(struct fuse_chan *ch)
{
	fuse_sol_door_destroy();
}

#define	MIN_BUFSIZE 0x21000

/*
 * XXX: Later, do the door create here?
 */
struct fuse_chan *fuse_sol_chan_new(int fd)
{
	struct fuse_chan_ops op = {
		.receive = fuse_sol_chan_receive,
		.send = fuse_sol_chan_send,
		.destroy = fuse_sol_chan_destroy,
	};
	return fuse_chan_new(&op, fd, FUSE_MAX_IOSIZE, NULL);
}

/* ARGSUSED */
struct fuse_chan *fuse_kern_chan_new(int fd)
{
	fprintf(stderr, "fuse_kern_chan_new is not supported.\n");
	return (NULL);
}
