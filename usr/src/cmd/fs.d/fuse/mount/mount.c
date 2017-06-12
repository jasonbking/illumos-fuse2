/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 */

/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*	Copyright (c) 1984, 1986, 1987, 1988, 1989 AT&T	*/
/*	  All Rights Reserved  	*/

/*
 * University Copyright- Copyright (c) 1982, 1986, 1988
 * The Regents of the University of California
 * All Rights Reserved
 *
 * University Acknowledgment- Portions of this document are derived from
 * software developed by the University of California, Berkeley, and its
 * contributors.
 */

/*
 * fusefs mount
 * (from NFS)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <unistd.h>
#include <ctype.h>
#include <errno.h>
#include <err.h>
#include <libintl.h>
#include <locale.h>
#include <libscf.h>
#include <priv_utils.h>

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/mount.h>
#include <sys/mntent.h>
#include <sys/mnttab.h>

#include <sys/fs/fusefs_mount.h>

extern char *optarg;
extern int optind;

const char *progname = "mount_fusefs";

static char mount_point[MAXPATHLEN + 1];
static void usage(void);
static int setsubopt(struct fusefs_args *, char *);

static const char * const optlist[] = {

	/* Generic VFS options. */
#define	OPT_RO		0
	MNTOPT_RO,
#define	OPT_RW		1
	MNTOPT_RW,
#define	OPT_SUID 	2
	MNTOPT_SUID,
#define	OPT_NOSUID 	3
	MNTOPT_NOSUID,
#define	OPT_DEVICES	4
	MNTOPT_DEVICES,
#define	OPT_NODEVICES	5
	MNTOPT_NODEVICES,
#define	OPT_SETUID	6
	MNTOPT_SETUID,
#define	OPT_NOSETUID	7
	MNTOPT_NOSETUID,
#define	OPT_EXEC	8
	MNTOPT_EXEC,
#define	OPT_NOEXEC	9
	MNTOPT_NOEXEC,
#define	OPT_XATTR	10
	MNTOPT_XATTR,
#define	OPT_NOXATTR	11
	MNTOPT_NOXATTR,

	/* Sort of generic (from NFS) */
#define	OPT_NOAC	12
	MNTOPT_NOAC,
#define	OPT_ACTIMEO	13
	MNTOPT_ACTIMEO,
#define	OPT_ACREGMIN	14
	MNTOPT_ACREGMIN,
#define	OPT_ACREGMAX	15
	MNTOPT_ACREGMAX,
#define	OPT_ACDIRMIN	16
	MNTOPT_ACDIRMIN,
#define	OPT_ACDIRMAX	17
	MNTOPT_ACDIRMAX,

	/* fusefs-specifis options */
#define	OPT_SUBTYPE	18
	"subtype",
#define	OPT_USER	19
	"user",
#define	OPT_UID		20
	"uid",
#define	OPT_GID		21
	"gid",
#define	OPT_DIRPERMS	22
	"dirperms",
#define	OPT_FILEPERMS	23
	"fileperms",
#define	OPT_NOPROMPT	24
	"noprompt",

	NULL
};

static int Oflg = 0;    /* Overlay mounts */
static int qflg = 0;    /* quiet - don't print warnings on bad options */
static int noprompt = 0;	/* don't prompt for password */

/* Note: fusefs uses _both_ kinds of options. */
static int mntflags = MS_DATA | MS_OPTIONSTR;

#define	EX_OK	0	/* normal */
#define	EX_OPT	1	/* bad options, usage, etc */
#define	EX_MNT	2	/* mount point problems, etc */
#define	RET_ERR	3	/* later errors */

struct fusefs_args mdata;
struct mnttab mnt;

/*
 * Initialize this with "rw" just to have something there,
 * so we don't have to decide whether to add a comma when
 * we strcat another option.  Note the "rw" may be changed
 * to an "ro" by option processing.
 */
char optbuf[MAX_MNTOPT_STR] = "rw";

int
main(int argc, char *argv[])
{
	struct stat st;
	int fd, opt, error, err2;
	static char *fstype = FUSEFS_VFSNAME;
	char *env, *state;

	(void) setlocale(LC_ALL, "");
#if !defined(TEXT_DOMAIN)
#define	TEXT_DOMAIN	"SYS_TEST"
#endif
	(void) textdomain(TEXT_DOMAIN);

	/*
	 * Normal users are allowed to run "mount -F fusefs ..."
	 * to mount on a directory they own.  To allow that, this
	 * program is installed setuid root, and it adds SYS_MOUNT
	 * privilege here (if needed), and then restores the user's
	 * normal privileges.  When root runs this, it's a no-op.
	 */
	if (__init_suid_priv(0, PRIV_SYS_MOUNT, (char *)NULL) < 0) {
		(void) fprintf(stderr,
		    gettext("Insufficient privileges, "
		    "%s must be set-uid root\n"), argv[0]);
		exit(RET_ERR);
	}

	if (argc == 2) {
		if (strcmp(argv[1], "-h") == 0) {
			usage();
		} else if (strcmp(argv[1], "-v") == 0) {
			errx(EX_OK, gettext("version %d"),
			    FUSEFS_VERSION);
		}
	}
	if (argc < 3)
		usage();

#if 0	/* XXX */
	/* Debugging support. */
	if ((env = getenv("FUSEFS_DEBUG")) != NULL) {
		fuse_debug = atoi(env);
		if (fuse_debug < 1)
			fuse_debug = 1;
	}

	error = fuse_lib_init();
	if (error)
		exit(RET_ERR);
#endif

	mnt.mnt_mntopts = optbuf;

	memset(&mdata, 0, sizeof (mdata));
	mdata.version = FUSEFS_VERSION;
	mdata.uid = (uid_t)-1;
	mdata.gid = (gid_t)-1;

	while ((opt = getopt(argc, argv, "ro:Oq")) != -1) {
		switch (opt) {
		case 'O':
			Oflg++;
			break;

		case 'q':
			qflg++;
			break;

		case 'r':
			mntflags |= MS_RDONLY;
			break;

		case 'o': {
			char *nextopt, *comma, *sopt;
			int ret;

			for (sopt = optarg; sopt != NULL; sopt = nextopt) {
				comma = strchr(sopt, ',');
				if (comma) {
					nextopt = comma + 1;
					*comma = '\0';
				} else
					nextopt = NULL;
				ret = setsubopt(&mdata, sopt);
				if (ret != 0)
					exit(EX_OPT);
				/* undo changes to optarg */
				if (comma)
					*comma = ',';
			}
			break;
		}

		case '?':
		default:
			usage();
		}
	}

	if (Oflg)
		mntflags |= MS_OVERLAY;

	if (mntflags & MS_RDONLY) {
		char *p;
		/* convert "rw"->"ro" */
		if (p = strstr(optbuf, "rw")) {
			if (*(p+2) == ',' || *(p+2) == '\0')
				*(p+1) = 'o';
		}
	}

	if (optind + 2 != argc)
		usage();

	mnt.mnt_special = argv[optind];
	mnt.mnt_mountp = argv[optind+1];

	if ((realpath(argv[optind+1], mount_point) == NULL) ||
	    (stat(mount_point, &st) == -1)) {
		err(EX_MNT, gettext("could not find mount point %s"),
		    argv[optind+1]);
	}
	if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		err(EX_MNT, gettext("can't mount on %s"), mount_point);
	}

	/*
	 * Fill in mdata defaults.
	 */
	if (mdata.uid == (uid_t)-1)
		mdata.uid = getuid();
	if (mdata.gid == (gid_t)-1)
		mdata.gid = getgid();
	if (mdata.file_mode == 0)
		mdata.file_mode = S_IRWXU;
	if (mdata.dir_mode == 0) {
		mdata.dir_mode = mdata.file_mode;
		if (mdata.dir_mode & S_IRUSR)
			mdata.dir_mode |= S_IXUSR;
		if (mdata.dir_mode & S_IRGRP)
			mdata.dir_mode |= S_IXGRP;
		if (mdata.dir_mode & S_IROTH)
			mdata.dir_mode |= S_IXOTH;
	}

	/*
	 * The "special" device (just a door fow now)
	 */
	fd = open(mnt.mnt_special, O_RDONLY, 0);
	if (fd == -1) {
		fprintf(stderr, gettext("%s: open failed, %s\n"),
			mnt.mnt_special, strerror(errno));
		exit(RET_ERR);
	}
	mdata.doorfd = fd;

	/*
	 * Have FUSE helper door, now mount.
	 */

	/* Need sys_mount privilege for the mount call. */
	(void) __priv_bracket(PRIV_ON);
	err2 = mount(mnt.mnt_special, mnt.mnt_mountp,
	    mntflags, fstype, &mdata, sizeof (mdata),
	    mnt.mnt_mntopts, MAX_MNTOPT_STR);
	(void) __priv_bracket(PRIV_OFF);

	if (err2 < 0) {
		if (errno != ENOENT) {
			err(EX_MNT, gettext("%: %s"),
			    progname, mnt.mnt_mountp);
		} else {
			struct stat sb;
			if (stat(mnt.mnt_mountp, &sb) < 0 &&
			    errno == ENOENT)
				err(EX_MNT, gettext("%s: %s"),
				    progname, mnt.mnt_mountp);
			else
				err(EX_MNT, gettext("%s: %s"),
				    progname, mnt.mnt_special);
		}
	}

	return (0);
}

#define	bad(val) (val == NULL || !isdigit(*val))

int
setsubopt(struct fusefs_args *mdatap, char *subopt)
{
	char *equals, *optarg;
	struct passwd *pwd;
	struct group *grp;
	long val;
	int rc = EX_OK;
	int index;
	char *p;

	equals = strchr(subopt, '=');
	if (equals) {
		*equals = '\0';
		optarg = equals + 1;
	} else
		optarg = NULL;

	for (index = 0; optlist[index] != NULL; index++) {
		if (strcmp(subopt, optlist[index]) == 0)
			break;
	}

	/*
	 * Note: if the option was unknown, index will
	 * point to the NULL at the end of optlist[],
	 * and we'll take the switch default.
	 */

	switch (index) {

	case OPT_SUID:
	case OPT_NOSUID:
	case OPT_DEVICES:
	case OPT_NODEVICES:
	case OPT_SETUID:
	case OPT_NOSETUID:
	case OPT_EXEC:
	case OPT_NOEXEC:
	case OPT_XATTR:
	case OPT_NOXATTR:
		/*
		 * These options are handled via the
		 * generic option string mechanism.
		 * None of these take an optarg.
		 */
		if (optarg != NULL)
			goto badval;
		(void) strlcat(optbuf, ",", sizeof (optbuf));
		if (strlcat(optbuf, subopt, sizeof (optbuf)) >=
		    sizeof (optbuf)) {
			if (!qflg)
				warnx(gettext("option string too long"));
			rc = EX_OPT;
		}
		break;

	/*
	 * OPT_RO, OPT_RW, are actually generic too,
	 * but we use the mntflags for these, and
	 * then update the options string later.
	 */
	case OPT_RO:
		mntflags |= MS_RDONLY;
		break;
	case OPT_RW:
		mntflags &= ~MS_RDONLY;
		break;

	/*
	 * NFS-derived options for attribute cache
	 * handling (disable, set min/max timeouts)
	 */
	case OPT_NOAC:
		mdatap->flags |= FUSEFS_MF_NOAC;
		break;

	case OPT_ACTIMEO:
		errno = 0;
		val = strtol(optarg, &p, 10);
		if (errno || *p != 0)
			goto badval;
		mdatap->acdirmin = mdatap->acregmin = val;
		mdatap->acdirmax = mdatap->acregmax = val;
		mdatap->flags |= FUSEFS_MF_ACDIRMAX;
		mdatap->flags |= FUSEFS_MF_ACREGMAX;
		mdatap->flags |= FUSEFS_MF_ACDIRMIN;
		mdatap->flags |= FUSEFS_MF_ACREGMIN;
		break;

	case OPT_ACREGMIN:
		errno = 0;
		val = strtol(optarg, &p, 10);
		if (errno || *p != 0)
			goto badval;
		mdatap->acregmin = val;
		mdatap->flags |= FUSEFS_MF_ACREGMIN;
		break;

	case OPT_ACREGMAX:
		errno = 0;
		val = strtol(optarg, &p, 10);
		if (errno || *p != 0)
			goto badval;
		mdatap->acregmax = val;
		mdatap->flags |= FUSEFS_MF_ACREGMAX;
		break;

	case OPT_ACDIRMIN:
		errno = 0;
		val = strtol(optarg, &p, 10);
		if (errno || *p != 0)
			goto badval;
		mdatap->acdirmin = val;
		mdatap->flags |= FUSEFS_MF_ACDIRMIN;
		break;

	case OPT_ACDIRMAX:
		errno = 0;
		val = strtol(optarg, &p, 10);
		if (errno || *p != 0)
			goto badval;
		mdatap->acdirmax = val;
		mdatap->flags |= FUSEFS_MF_ACDIRMAX;
		break;

	/*
	 * FUSEFS-specific options.  Some of these
	 * don't go through the mount system call,
	 * but just set libfuse options.
	 */
	case OPT_SUBTYPE:
		/* XXX ignore for now */
		break;

#if 0	/* XXX not yet */
	case OPT_USER:
		rc = EX_OPT;
		break;
#endif

	case OPT_UID:
		pwd = isdigit(optarg[0]) ?
		    getpwuid(atoi(optarg)) : getpwnam(optarg);
		if (pwd == NULL) {
			if (!qflg)
				warnx(gettext("unknown user '%s'"), optarg);
			rc = EX_OPT;
		} else {
			mdatap->uid = pwd->pw_uid;
		}
		break;

	case OPT_GID:
		grp = isdigit(optarg[0]) ?
		    getgrgid(atoi(optarg)) : getgrnam(optarg);
		if (grp == NULL) {
			if (!qflg)
				warnx(gettext("unknown group '%s'"), optarg);
			rc = EX_OPT;
		} else {
			mdatap->gid = grp->gr_gid;
		}
		break;

	case OPT_DIRPERMS:
		errno = 0;
		val = strtol(optarg, &p, 8);
		if (errno || *p != 0)
			goto badval;
		mdatap->dir_mode = val;
		break;

	case OPT_FILEPERMS:
		errno = 0;
		val = strtol(optarg, &p, 8);
		if (errno || *p != 0)
			goto badval;
		mdatap->file_mode = val;
		break;

	case OPT_NOPROMPT:
		noprompt++;
		break;

	default:
	badopt:
		if (!qflg)
			warnx(gettext("unknown option %s"), subopt);
		rc = EX_OPT;
		break;

	badval:
		if (!qflg)
			warnx(gettext("invalid value for %s"), subopt);
		rc = EX_OPT;
		break;
	}

	/* Undo changes made to subopt */
	if (equals)
		*equals = '=';

	return (rc);
}

static void
usage(void)
{
	fprintf(stderr, "%s\n",
	gettext("usage: mount -F fusefs [-Orq] [-o option[,option]]"
	"	//[workgroup;][user[:password]@]server[/share] path"));

	exit(EX_OPT);
}
