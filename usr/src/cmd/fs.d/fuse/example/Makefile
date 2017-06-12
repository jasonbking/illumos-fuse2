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

#
# cmd/fs.d/fuse/example/Makefile
#

FSTYPE=		fuse
TYPEPROG= 	fioc fioclient \
		fsel fselclient \
		fusexmp hello null

include		../../Makefile.fstype

CFLAGS += $(CCVERBOSE)
C99MODE= $(C99_ENABLE)

LDLIBS += -lfuse

CPPFLAGS += -D_FILE_OFFSET_BITS=64 \
	-I$(SRC)/lib/libfuse/include

# Don't want to fix warnings in these examples.
CERRWARN=

# Debugging
${NOT_RELEASE_BUILD} CPPFLAGS += -DDEBUG

# uncomment these for dbx debugging
#COPTFLAG = -g
#CTF_FLAGS =
#CTFCONVERT_O=
#CTFMERGE_LIB=

all:	$(TYPEPROG)

catalog:

lint:	lint_SRCS

clean:
	$(RM) $(OBJS) $(POFILE)

.KEEP_STATE:
