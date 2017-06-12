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
# cmd/fs.d/fuse/fuse-dmn/Makefile
#

FSTYPE=		fuse
TYPEPROG=	fuse-dmn

include		../../Makefile.fstype

OBJS=	dmn_main.o dmn_calls.o fakes.o
SRCS=	$(OBJS:%.o=%.c)
POFILE=	$(TYPEPROG).po

CFLAGS += $(CCVERBOSE)
C99MODE= $(C99_ENABLE)

LDLIBS += -lumem

CPPFLAGS += -I$(SRC)/lib/libfuse \
	-I$(SRC)/uts/common/fs/fusefs -I$(SRC)/uts/common

# Debugging
${NOT_RELEASE_BUILD} CPPFLAGS += -DDEBUG

# uncomment these for dbx debugging
#COPTFLAG = -g
#CTF_FLAGS =
#CTFCONVERT_O=
#CTFMERGE_LIB=

all:	$(TYPEPROG)

$(TYPEPROG):	$(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDLIBS)
	$(POST_PROCESS)

catalog:	$(POFILE)

lint:	lint_SRCS

clean:
	$(RM) $(OBJS) $(POFILE)

.KEEP_STATE:
