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
 * Test program for the client side of the FUSE daemon.
 */

#include <stdio.h>

#include "cli_calls.h"

fuse_ssn_t *ssn;
void cmd_loop(void);
void do_cat(char *);
void do_df(char *);
void do_ls(char *);

int
main(int argc, char **argv)
{
	int err;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <door_path>\n", argv[0]);
		return (1);
	}

	err = cli_ssn_create(argv[1], &ssn);
	if (err) {
		fprintf(stderr, "%s: ssn_create, err=%d\n", argv[0], err);
		return (1);
	}

	cmd_loop();

	cli_ssn_rele(ssn);
	return (0);
}

void
cmd_loop(void)
{
	static char lbuf[80];
	char *p;

	printf("Type commands: ls, cat, df\n");
	while ((p = fgets(lbuf, sizeof (lbuf), stdin)) != NULL) {
		switch (lbuf[0]) {
		case 'c':
			do_cat(lbuf);
			break;
		case 'd':
			do_df(lbuf);
			break;
		case 'l':
			do_ls(lbuf);
			break;
		default:
			printf("Huh?\n");
			break;
		}
	}
}

void
do_cat(char *p)
{
	printf("not yet\n");
}

void
do_df(char *p)
{
	struct fuse_statvfs stvfs;
	int err;

	err = cli_call_statvfs(ssn, &stvfs);
	if (err) {
		fprintf(stderr, "call_statfs, err=%d", err);
		return;
	}

	printf("f_bsize  = %ld\n", (long)stvfs.f_bsize);
	printf("f_frsize = %ld\n", (long)stvfs.f_frsize);
	printf("f_blocks = %ld\n", (long)stvfs.f_blocks);
	printf("f_bfree  = %ld\n", (long)stvfs.f_bfree);
	printf("f_bavail = %ld\n", (long)stvfs.f_bavail);
	printf("f_files  = %ld\n", (long)stvfs.f_files);
	printf("f_ffree  = %ld\n", (long)stvfs.f_ffree);
	printf("f_favail = %ld\n", (long)stvfs.f_favail);
}

void
do_ls(char *p)
{
	int eof, off, rc;
	uint64_t fid;
	struct {
		struct fuse_stat st;
		struct fuse_dirent de;
		char pad[256];
	} ret;

	rc = cli_call_opendir(ssn, 1, "/", &fid);
	if (rc) {
		printf("opendir, rc=%d\n", rc);
		return;
	}

	eof = off = 0;
	for (;;) {
		rc = cli_call_readdir(ssn, fid, off,
		    &ret.st, &ret.de, &eof);
		if (rc) {
			printf("readdir, rc=%d\n", rc);
			break;
		}
		printf("\noff=%d:\n", off);

		printf("st_ino  = %d\n",  (int)ret.st.st_ino);
		printf("st_mode = 0%o\n", (int)ret.st.st_mode);
		printf("st_uid  = %d\n",  (int)ret.st.st_uid);
		printf("st_gid  = %d\n",  (int)ret.st.st_gid);
		printf("st_size = %d\n",  (int)ret.st.st_size);

		printf("d_ino  = %d\n",   (int)ret.de.d_ino);
		printf("d_off  = %d\n",   (int)ret.de.d_off);
		printf("d_nmlen = %d\n",  (int)ret.de.d_nmlen);
		printf("d_name = \"%s\"\n", ret.de.d_name);

		if (eof)
			break;
		off = ret.de.d_off;
	}

	(void) cli_call_close(ssn, fid);
}
