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
 * FUSE Deamon (door service for fusefs)
 */

#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/dirent.h>
#include <sys/note.h>

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

#define	EXIT_FAIL	1
#define	EXIT_OK		0

int fuse_debug = 0;
int dmn_terminating = 0;

int
main(int argc, char **argv)
{
	static const int door_attrs =
	    DOOR_REFUSE_DESC | DOOR_NO_CANCEL;
	static char door_path[64];
	sigset_t oldmask, tmpmask;
	int door_fd = -1, tmp_fd = -1;
	int i, sig;
	int rc = EXIT_FAIL;
	int pid = getpid();

	/* Debugging support. */
	fuse_debug = 1;	/* XXX */

	/*
	 * XXX: Todo - connect to some libfuse plugin...
	 */


	/*
	 * Create a file for the door, making sure that
	 * only the owner of this process has access.
	 */
	snprintf(door_path, sizeof (door_path),
	    "/tmp/fuse-dmn-%d", pid);
	(void) unlink(door_path);
	tmp_fd = open(door_path, O_RDWR|O_CREAT|O_EXCL, 0600);
	if (tmp_fd < 0) {
		perror(door_path);
		exit(EXIT_FAIL);
	}
	close(tmp_fd);
	tmp_fd = -1;

	printf("FUSE_DOOR_PATH=%s\n", door_path);

	if (fuse_debug == 0) {
		/*
		 * Close FDs 0,1,2 so we don't have a TTY, and
		 * re-open them on /dev/null so they won't be
		 * used for device handles (etc.) later, and
		 * we don't have to worry about printf calls
		 * or whatever going to these FDs.
		 */
		for (i = 0; i < 3; i++) {
			close(i);
			tmp_fd = open("/dev/null", O_RDWR);
			if (tmp_fd < 0)
				perror("/dev/null");
			if (tmp_fd != i)
				DPRINT("Open /dev/null - wrong fd?\n");
		}

		/*
		 * Become session leader.
		 */
		setsid();
	}

	/*
	 * Create door service threads with signals blocked.
	 */
	sigfillset(&tmpmask);
	sigprocmask(SIG_BLOCK, &tmpmask, &oldmask);

	/* Setup the door service. */
	door_fd = door_create(dmn_dispatch, NULL, door_attrs);
	if (door_fd < 0) {
		fprintf(stderr, "%s: door_create failed\n", argv[0]);
		rc = EXIT_FAIL;
		goto errout;
	}
	fdetach(door_path);
	if (fattach(door_fd, door_path) < 0) {
		fprintf(stderr, "%s: fattach failed\n", argv[0]);
		rc = EXIT_FAIL;
		goto errout;
	}

	/*
	 * Main thread just waits for signals.
	 */
again:
	sig = sigwait(&tmpmask);
	DPRINT("main: sig=%d\n", sig);

	dmn_terminating = 1;
	rc = EXIT_OK;

errout:
	fdetach(door_path);
	door_revoke(door_fd);
	door_fd = -1;
	unlink(door_path);

	return (rc);
}
