#! /usr/bin/sh
#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
#

prefix=/usr
exec_prefix=${prefix}
libdir=${exec_prefix}/lib
includedir=${prefix}/include

# Name: fuse
# Description: Filesystem in Userspace
# Version: 2.8.5
# Libs: -L${libdir} -lfuse -pthread  
# Cflags: -I${includedir}/fuse -D_FILE_OFFSET_BITS=64

CFLAGS="-I/usr/include/fuse -D_FILE_OFFSET_BITS=64"
LDLIBS="-lfuse"

for x
do
    case $x in
	--atleast-pkgconfig-version)
	    exit 0
	    ;;
	--exists)
	    exit 0
	    ;;
	--cflags)
	    echo "$CFLAGS"
	    exit 0
	    ;;
	--libs)
	    echo "$LDLIBS"
	    exit 0
	    ;;
	-*)
	    echo "%x: huh?"
	    exit 1
	    ;;
	*)
	    ;;
    esac
done
exit 0
