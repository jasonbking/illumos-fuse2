/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2001-2007  Miklos Szeredi <miklos@szeredi.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB
*/

#include "fuse.h"
#include "fuse_lowlevel.h"

#define FUSE_UNKNOWN_INO 0xffffffff

struct fuse_chan;
struct fuse_ll;

struct fuse_config {
	unsigned int uid;
	unsigned int gid;
	unsigned int  umask;
	double entry_timeout;
	double negative_timeout;
	double attr_timeout;
	double ac_attr_timeout;
	int ac_attr_timeout_set;
	int noforget;
	int debug;
	int hard_remove;
	int use_ino;
	int readdir_ino;
	int set_mode;
	int set_uid;
	int set_gid;
	int direct_io;
	int kernel_cache;
	int auto_cache;
	int intr;
	int intr_signal;
	int help;
	char *modules;
};

struct fuse_fs {
	struct fuse_operations op;	/* user-provided, via fuse_main */
	struct fuse_module *m;
	void *user_data;		/* user-provided, via fuse_main */
	int compat;
	int debug;
};

struct fuse {
	struct fuse_session *se;
	struct node **name_table;
	size_t name_table_size;
	struct node **id_table;
	size_t id_table_size;
	fuse_ino_t ctr;
	unsigned int generation;
	unsigned int hidectr;
	pthread_mutex_t lock;
	struct fuse_config conf;
	int intr_installed;
	struct fuse_fs *fs;
	int nullpath_ok;
	int curr_ticket;
	struct lock_queue_element *lockq;
};

struct fuse_session {
	struct fuse_session_ops op;

	void *data;	/* struct fuse_ll */

	volatile int exited;

	struct fuse_chan *ch;
};

struct fuse_req {
	struct fuse_ll *f;
	uint64_t unique;
	int ctr;
	pthread_mutex_t lock;
	struct fuse_ctx ctx;
	struct fuse_chan *ch;
	int interrupted;
	union {
		struct {
			uint64_t unique;
		} i;
		struct {
			fuse_interrupt_func_t func;
			void *data;
		} ni;
	} u;
	struct fuse_req *next;
	struct fuse_req *prev;
};

struct fuse_dh {
	pthread_mutex_t lock;
	struct fuse *fuse;
	fuse_req_t req;
	char *contents;
	int allocated;
	unsigned len;
	unsigned size;
	unsigned needlen;
	int filled;
	uint64_t fh;
	int error;
	fuse_ino_t nodeid;
#ifdef	__SOLARIS__
	int pathlen;
	char *path;
#endif
};

struct fuse_context_i {
	struct fuse_context ctx;
	fuse_req_t req;
};

struct fuse_ll {
	int debug;
	int allow_root;
	int atomic_o_trunc;
	int posix_locks;
	int no_remote_lock;
	int big_writes;
#ifdef	__SOLARIS__
	/* Not doing "lowlevel" stuff */
#else
	struct fuse_lowlevel_ops op;
#endif
	int got_init;
	struct cuse_data *cuse_data;
	void *userdata;		/* struct fuse */
	uid_t owner;
	struct fuse_conn_info conn;
	struct fuse_req list;
	struct fuse_req interrupts;
	pthread_mutex_t lock;
	int got_destroy;
};

struct fuse_cmd {
	char *buf;
	size_t buflen;
	struct fuse_chan *ch;
};

struct fuse *fuse_new_common(struct fuse_chan *ch, struct fuse_args *args,
			     const struct fuse_operations *op,
			     size_t op_size, void *user_data, int compat);

int fuse_sync_compat_args(struct fuse_args *args);

struct fuse_chan *fuse_kern_chan_new(int fd);

struct fuse_session *fuse_lowlevel_new_common(struct fuse_args *args,
					const struct fuse_lowlevel_ops *op,
					size_t op_size, void *userdata);

#if defined(__SOLARIS__)
/* Solaris uses these instead of the "lowlevel" stuff. */
struct fuse_chan *fuse_sol_chan_new(int fd);
struct fuse_session *fuse_solaris_new_common(struct fuse_args *args,
					void *userdata);
void fuse_sol_door_destroy(void);
int fuse_sol_mount1(const char *mountpoint, struct fuse_args *args);
int fuse_sol_mount2(const char *mountpoint, struct fuse *f);
void fuse_sol_unmount(const char *mountpoint, int fd);
int fuse_fill_dir(void *dh_, const char *name, const struct stat *statp,
		  off_t off);

#endif	/* __SOLARIS__ */

struct fuse_context_i *fuse_get_context_internal(void);

void fuse_kern_unmount_compat22(const char *mountpoint);
void fuse_kern_unmount(const char *mountpoint, int fd);
int fuse_kern_mount(const char *mountpoint, struct fuse_args *args);

int fuse_send_reply_iov_nofree(fuse_req_t req, int error, struct iovec *iov,
			       int count);
void fuse_free_req(fuse_req_t req);


struct fuse *fuse_setup_common(int argc, char *argv[],
			       const struct fuse_operations *op,
			       size_t op_size,
			       char **mountpoint,
			       int *multithreaded,
			       int *fd,
			       void *user_data,
			       int compat);

void cuse_lowlevel_init(fuse_req_t req, fuse_ino_t nodeide, const void *inarg);
