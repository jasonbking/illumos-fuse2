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
 * Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
 */

/*
 * Block comment that describes the contents of this file.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/time.h>
#include <sys/vnode.h>
#include <sys/sunddi.h>
#include <sys/cmn_err.h>
#include <sys/sdt.h>

#include "fusefs.h"
#include "fusefs_node.h"
#include "fusefs_subr.h"

/*
 * fusefs_attrcache_enter, fusefs_attrcache_lookup replaced by
 * code more closely resembling NFS.  See fusefs_client.c
 */

/*
 * Helper for the FUSEFS_ERROR macro, etc.
 * This is also a good place for a breakpoint
 * or a dtrace probe, i.e. fbt:fusefs:fusefs_errmsg
 */
void
fusefs_errmsg(int cel, const char *func_name, const char *fmt, ...)
{
	va_list adx;
	char buf[100];

	va_start(adx, fmt);
	if (cel == CE_CONT) {
		/*
		 * This is one of our xxxDEBUG macros.
		 * Don't bother to log these, but just
		 * fire a dtrace probe with the message.
		 */
		(void) vsnprintf(buf, sizeof (buf), fmt, adx);
		DTRACE_PROBE2(debugmsg2,
		    (char *), func_name,
		    (char *), buf);
	} else {
		/*
		 * This is one of our xxxERROR macros.
		 * Add a prefix to the fmt string,
		 * then let vcmn_err do the args.
		 */
		(void) snprintf(buf, sizeof (buf), "?%s: %s", func_name, fmt);
		DTRACE_PROBE3(debugmsg3,
		    (char *), func_name,
		    (char *), buf,
		    va_list, adx);
		vcmn_err(cel, buf, adx);
	}
	va_end(adx);
}

/*
 * Simple name hash for generating inode numbers.
 * Borrowed from smbfs.
 */

/* Magic constants for name hashing. */
#define	FNV_32_PRIME ((uint32_t)0x01000193UL)
#define	FNV1_32_INIT ((uint32_t)33554467UL)

static inline uint32_t
fusefs_hash(uint32_t ival, const char *name, int nmlen)
{
	uint32_t v;

	for (v = ival; nmlen; name++, nmlen--) {
		v *= FNV_32_PRIME;
		v ^= (uint32_t)*name;
	}
	return (v);
}

/*
 * Compute the hash of the full (remote) path name
 * using the three parts supplied separately.
 */
uint32_t
fusefs_gethash(const char *rpath, int rplen)
{
	uint32_t v;

	v = fusefs_hash(FNV1_32_INIT, rpath, rplen);
	return (v);
}

/*
 * Like fusefs_gethash, but optimized a little by
 * starting with the directory hash.
 */
uint32_t
fusefs_getino(struct fusenode *dnp, const char *name, int nmlen)
{
	uint32_t ino;
	char sep;

	/* Start with directory hash */
	ino = (uint32_t)dnp->n_ino;

	/* separator (maybe) */
	sep = FUSEFS_DNP_SEP(dnp);
	if (sep)
		ino = fusefs_hash(ino, &sep, 1);

	/* Now hash this component. */
	ino = fusefs_hash(ino, name, nmlen);

	return (ino);
}
