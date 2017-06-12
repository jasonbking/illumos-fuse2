#!/usr/sbin/dtrace -s
#pragma D option flowindent

/*
 * User-level dtrace for libfuse
 * Usage: dtrace -s libfuse.dt -c hello ...
 */

pid$target:libfuse.so::entry
{
  printf("\t0x%x", arg0);
  printf("\t0x%x", arg1);
  printf("\t0x%x", arg2);
  printf("\t0x%x", arg3);
  printf("\t0x%x", arg4);
  printf("\t0x%x", arg5);
  printf("\t0x%x", arg6);
  printf("\t0x%x", arg7);
}

pid$target:libfuse.so::return
{
  printf("\t0x%x", arg1);
}
