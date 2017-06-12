/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2005-2008 Csaba Henk <csaba.henk@creo.hu>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#include "fuse_i.h"
#include "fuse_misc.h"
#include "fuse_opt.h"

#include <sys/note.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/user.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <stddef.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <limits.h>
#include <thread.h>
#include <signal.h>
#include <door.h>

#define	MOUNT_VERSION	"0"	/* XXX */

extern int solaris_debug;

/*
 * The Solaris mount_fusefs gets a door.
 */
void
sol_dispatch(void *, char *, size_t, door_desc_t *, uint_t);

static int  solaris_door_fd = -1;
static char door_path[64];
static int did_mount;

/*
 * Solaris door setup.
 */
/* ARGSUSED */
int
fuse_sol_door_create(struct fuse *f)
{
	sigset_t oldmask, tmpmask;
	int door_fd = -1;
	int tmp_fd = -1;
	int rc;

	/*
	 * Create a file for the door, making sure that
	 * only the owner of this process has access.
	 */
	snprintf(door_path, sizeof (door_path),
		 "/tmp/fuse-%d", getpid());
	(void) unlink(door_path);
	tmp_fd = open(door_path, O_RDWR|O_CREAT|O_EXCL, 0600);
	if (tmp_fd < 0) {
		rc = -errno;
		perror(door_path);
		goto errout;
	}
	close(tmp_fd);
	tmp_fd = -1;

	/*
	 * Create the door service threads with signals blocked.
	 * (They inherit from this thread.)
	 */
	sigfillset(&tmpmask);
	sigprocmask(SIG_BLOCK, &tmpmask, &oldmask);

	/* Setup the door service. */
	door_fd = door_create(sol_dispatch, NULL, DOOR_REFUSE_DESC);
	if (door_fd < 0) {
		rc = -errno;
		fprintf(stderr, "door_create failed\n");
		goto errout;
	}
	fdetach(door_path);
	if (fattach(door_fd, door_path) < 0) {
		rc = -errno;
		fprintf(stderr, "fattach failed\n");
		goto errout;
	}
	solaris_door_fd = door_fd;
	rc = 0;

errout:
	return (rc);
}

void
fuse_sol_door_destroy(void)
{
	if (door_path[0]) {
		fdetach(door_path);
		unlink(door_path);
	}
	if (solaris_door_fd != -1) {
		door_revoke(solaris_door_fd);
		solaris_door_fd = -1;
	}
}


#define FUSERMOUNT_PROG		"mount_fusefs"
#define FUSE_DEV_TRUNK		"/dev/fuse"
#define _PATH_DEV		"/dev"
#define	SPECNAMELEN		256

enum {
	KEY_ALLOW_ROOT,
	KEY_RO,
	KEY_HELP,
	KEY_VERSION,
	KEY_KERN
};

struct mount_opts {
	int allow_other;
	int allow_root;
	int ishelp;
	char *kernel_opts;
};

static struct mount_opts sol_mo;

#define FUSE_DUAL_OPT_KEY(templ, key) 				\
	FUSE_OPT_KEY(templ, key), FUSE_OPT_KEY("no" templ, key)

static const struct fuse_opt fuse_mount_opts[] = {
	{ "allow_other", offsetof(struct mount_opts, allow_other), 1 },
	{ "allow_root", offsetof(struct mount_opts, allow_root), 1 },
	FUSE_OPT_KEY("allow_root",		KEY_ALLOW_ROOT),
	FUSE_OPT_KEY("-r",			KEY_RO),
	FUSE_OPT_KEY("-h",			KEY_HELP),
	FUSE_OPT_KEY("--help",			KEY_HELP),
	FUSE_OPT_KEY("-V",			KEY_VERSION),
	FUSE_OPT_KEY("--version",		KEY_VERSION),
	/* standard FreeBSD mount options */
	FUSE_DUAL_OPT_KEY("dev",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("async",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("atime",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("dev",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("exec",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("suid",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("symfollow",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("rdonly",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("sync",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("union",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("userquota",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("groupquota",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("clusterr",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("clusterw",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("suiddir",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("snapshot",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("multilabel",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("acls",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("force",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("update",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("ro",			KEY_KERN),
	FUSE_DUAL_OPT_KEY("rw",			KEY_KERN),
	FUSE_DUAL_OPT_KEY("auto",		KEY_KERN),
	/* options supported under both Linux and FBSD */
	FUSE_DUAL_OPT_KEY("allow_other",	KEY_KERN),
	FUSE_DUAL_OPT_KEY("default_permissions",KEY_KERN),
	FUSE_OPT_KEY("max_read=",		KEY_KERN),
	FUSE_OPT_KEY("subtype=",		KEY_KERN),
	/* FBSD FUSE specific mount options */
	FUSE_DUAL_OPT_KEY("private",		KEY_KERN),
	FUSE_DUAL_OPT_KEY("neglect_shares",	KEY_KERN),
	FUSE_DUAL_OPT_KEY("push_symlinks_in",	KEY_KERN),
	FUSE_OPT_KEY("nosync_unmount",		KEY_KERN),
	/* stock FBSD mountopt parsing routine lets anything be negated... */
	/*
	 * Linux specific mount options, but let just the mount util
	 * handle them
	 */
	FUSE_OPT_KEY("fsname=",			KEY_KERN),
	FUSE_OPT_KEY("nonempty",		KEY_KERN),
	FUSE_OPT_KEY("large_read",		KEY_KERN),
	FUSE_OPT_END
};

static void mount_help(void)
{
	fprintf(stderr,
"    -o allow_other         allow access to other users\n"
"    -o allow_root          allow access to root\n"
"    -o nonempty            allow mounts over non-empty file/dir\n"
"    -o default_permissions enable permission checking by kernel\n"
"    -o fsname=NAME         set filesystem name\n"
"    -o subtype=NAME        set filesystem type\n"
"    -o large_read          issue large read requests (2.4 only)\n"
"    -o max_read=N          set maximum size of read requests\n"
"\n");
}

static void mount_version(void)
{
	printf("mount_fusefs version: %s", MOUNT_VERSION);
}

static int fuse_mount_opt_proc(void *data, const char *arg, int key,
			       struct fuse_args *outargs)
{
	struct mount_opts *mo = data;

	switch (key) {
	case KEY_ALLOW_ROOT:
		if (fuse_opt_add_opt(&mo->kernel_opts, "allow_other") == -1 ||
		    fuse_opt_add_arg(outargs, "-oallow_root") == -1)
			return -1;
		return 0;

	case KEY_RO:
		arg = "ro";
		/* fall through */

	case KEY_KERN:
		return fuse_opt_add_opt(&mo->kernel_opts, arg);

	case KEY_HELP:
		mount_help();
		mo->ishelp = 1;
		break;

	case KEY_VERSION:
		mount_version();
		mo->ishelp = 1;
		break;
	}
	return 1;
}

void fuse_unmount_compat22(const char *mountpoint)
{
	fuse_kern_unmount(mountpoint, -1);
}

void fuse_kern_unmount(const char *mountpoint, int fd)
{
	_NOTE(ARGUNUSED(fd));
	const char *argv[4];
	pid_t pid;

	/*
	 * The actual mount doesn't happen until _mount2,
	 * and this is called during cleanup where that
	 * may not have happened, don't try to unmount
	 * unless _mount2 has succeeded.
	 */
	if (did_mount == 0)
		return;

	argv[0] = "/usr/sbin/umount";
	argv[1] = "-f";
	argv[2] = mountpoint;
	argv[3] = NULL;

	pid = fork();

	if (pid == -1)
		return;

	if (pid == 0) {
		execvp(argv[0], (char **) argv);
		exit(1);
	}

	waitpid(pid, NULL, 0);
}


static int fuse_mount_core(const char *mountpoint, const char *opts)
{
	pid_t pid, cpid;
	int status;
	char *no_mount;

	no_mount = getenv("FUSE_NO_MOUNT");
	if (mountpoint == NULL || no_mount || solaris_debug)
		fprintf(stderr, "door-path: %s\n", door_path);
	if (mountpoint == NULL || no_mount)
		return 0;

	if (door_path[0] == '\0') {
		fprintf(stderr, "NULL door_path\n");
		return -ENOENT;
	}

	if (solaris_debug)
		fprintf(stderr, "opts=%s\n", opts);

	pid = fork();
	cpid = pid;

	if (pid == -1) {
		perror("fuse: fork() failed");
		return -errno;
	}

	if (pid == 0) {
		const char *argv[32];
		int a = 0;

		argv[a++] = "/usr/sbin/mount";
		argv[a++] = "-F";
		argv[a++] = "fusefs";
		if (opts) {
			argv[a++] = "-o";
			argv[a++] = opts;
		}
		argv[a++] = door_path;
		argv[a++] = mountpoint;
		argv[a++] = NULL;
		execvp(argv[0], (char **) argv);
		perror("fuse: failed to exec mount program");
		exit(1);
	}

	if (waitpid(cpid, &status, 0) == -1 || WEXITSTATUS(status) != 0) {
		perror("fuse: failed to mount file system");
		return -1;
	}
	did_mount = 1;

	return 0;
}

/*
 * In Solaris, we setup the fuse server (back-end) first,
 * and then do the mount with the door created by setup.
 * This is called when libfuse thinks we should mount.
 * See fuse_sol_moun2() for the real mount.
 *
 * We need to parse the mount options here, because that
 * modifies the args, removing mount stuff, and leaving
 * only generic FUSE args.  (Sigh... side effects...)
 * So this now just parses and saves the args.
 * It does NOT actually do the mount yet.
 */
int fuse_sol_mount1(const char *mountpoint, struct fuse_args *args)
{
	_NOTE(ARGUNUSED(mountpoint));
	int res = -1;

	memset(&sol_mo, 0, sizeof(sol_mo));
	/* mount util should not try to spawn the daemon */
	setenv("MOUNT_FUSEFS_SAFE", "1", 1);
	/* to notify the mount util it's called from lib */
	setenv("MOUNT_FUSEFS_CALL_BY_LIB", "1", 1);

	if (args &&
	    fuse_opt_parse(args, &sol_mo, fuse_mount_opts, fuse_mount_opt_proc) == -1)
		return -1;

	if (solaris_debug)
		printf("sol_mount1: kernel_opts=%s\n", sol_mo.kernel_opts);

	if (sol_mo.allow_other && sol_mo.allow_root) {
		fprintf(stderr, "fuse: 'allow_other' and 'allow_root' options are mutually exclusive\n");
		return -1;
	}
	if (sol_mo.ishelp)
		return 0;

#if 0	/* XXX: Will do the actual mount later... */
	res = fuse_mount_core(mountpoint, sol_mo.kernel_opts);
#else
	res = 0;
#endif
	/* Will free(sol_mo.kernel_opts) later too. */

	return res;
}

/*
 * Startup the door service and do the actual mount.
 */
int fuse_sol_mount2(const char *mountpoint, struct fuse *f)
{
	int rc;

	/*
	 * XXX: Maybe call back-end INIT here?
	 */
	rc = fuse_sol_door_create(f);
	rc = fuse_mount_core(mountpoint, sol_mo.kernel_opts);

	return (rc);
}

/* XXX: don't support this. */
/* ARGSUSED */
int fuse_kern_mount(const char *mountpoint, struct fuse_args *args)
{
	return (-1);
}

/* XXX: or this. */
/* ARGSUSED */
int fuse_mount_compat22(const char *mountpoint, const char *opts)
{
	return (-1);
}

FUSE_SYMVER(".symver fuse_unmount_compat22,fuse_unmount@FUSE_2.2");
