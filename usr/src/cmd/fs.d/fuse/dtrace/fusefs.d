#!/usr/sbin/dtrace -s

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

#pragma D option flowindent

/*
 * Trace everything in fusefs.
 */

fbt:fusefs::entry
{
  self->trace++;
  printf("\t0x%x", arg0);
  printf("\t0x%x", arg1);
  printf("\t0x%x", arg2);
  printf("\t0x%x", arg3);
  printf("\t0x%x", arg4);
  printf("\t0x%x", arg5);
}

fbt:fusefs::return
{
  printf("\t0x%x", arg1);
  self->trace--;
}

/*
 * Print file names.
 */

fbt:fusefs:fusefs_lookup:entry,
fbt:fusefs:fusefs_create:entry,
fbt:fusefs:fusefs_remove:entry,
fbt:fusefs:fusefs_mkdir:entry,
fbt:fusefs:fusefs_rmdir:entry,
fbt:fusefs:fusefslookup:entry
{
  printf("name=%s\n", stringof(arg1));
}

fbt:fusefs:fusefs_rename:entry,
fbt:fusefs:fusefsrename:entry
{
  printf("old name=%s\n", stringof(arg1));
  printf("new name=%s\n", stringof(arg3));
}

/*
 * Print vnode pointers passed or returned by reference.
 */

fbt:fusefs:fusefs_lookup:entry,
fbt:fusefs:fusefslookup:entry
{
  self->vpp = args[2];
}

fbt:fusefs:fusefs_create:entry
{
  self->vpp = args[5];
}

fbt:fusefs:fusefs_mkdir:entry
{
  self->vpp = args[3];
}

fbt:fusefs:fusefs_lookup:return,
fbt:fusefs:fusefslookup:return,
fbt:fusefs:fusefs_create:return,
fbt:fusefs:fusefs_mkdir:return
{
  printf("vp=%p\n", self->vpp);
}

fbt:fusefs:fusefs_open:entry
{
  self->vpp = args[0];
  printf("vp=%p\n", self->vpp);
}

/*
 * Show debug messages in calls from fusefs.
 */

sdt:fusefs::debugmsg2
/self->trace/
{
  printf("\n\t %s: %s\n", stringof(arg0), stringof(arg1));
}

sdt:fusefs::debugmsg3
/self->trace/
{
  printf("\n\t %s: %s\n", stringof(arg0), stringof(arg1));
  tracemem(arg2, 16);
}
