#
# CDDL HEADER START
#
# The contents of this file are subject to the terms of the
# Common Development and Distribution License (the "License").
# You may not use this file except in compliance with the License.
#
# You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
# or http://www.opensolaris.org/os/licensing.
# See the License for the specific language governing permissions
# and limitations under the License.
#
# When distributing Covered Code, include this CDDL HEADER in each
# file and include the License file at usr/src/OPENSOLARIS.LICENSE.
# If applicable, add the following below this CDDL HEADER, with the
# fields enclosed by brackets "[]" replaced with your own identifying
# information: Portions Copyright [yyyy] [name of copyright owner]
#
# CDDL HEADER END
#

#
# Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
# Use is subject to license terms.
#
# Copyright 2011 Nexenta Systems, Inc.  All rights reserved.
#

#
# lib/libfuse/Makefile.com
#

LIBRARY=	libfuse.a
VERS=		.2.8

COBJS=	\
	fuse.o \
	cuse_ll_stubs.o	\
	fuse_ll_doorsvc.o \
	fuse_mt.o \
	fuse_opt.o \
	fuse_session.o \
	fuse_signals.o \
	helper.o \
	mount_doorsvc.o

MOBJS=	\
	iconv.o \
	subdir.o

OBJECTS= $(COBJS) $(MOBJS)

include $(SRC)/lib/Makefile.lib

LIBS=	$(DYNLIB) $(LINTLIB)

SRCDIR=	../common
MODDIR=	../modules

SRCS=	$(COBJS:%.o=$(SRCDIR)/%.c) \
	$(MOBJS:%.o=$(MODDIR)/%.c)

$(LINTLIB) :=	SRCS = $(SRCDIR)/$(LINTSRC)

C99MODE=	$(C99_ENABLE)

LDLIBS += -lc

# normal warnings...
CFLAGS +=	$(CCVERBOSE) 

CPPFLAGS += \
	-D__SOLARIS__ \
	-D_XOPEN_SOURCE=600 \
	-D__EXTENSIONS__ \
	-D_FILE_OFFSET_BITS=64 \
	-DFUSE_USE_VERSION=26 \
	-I$(SRC)/uts/common/fs/fusefs \
	-I$(SRC)/uts/common \
	-I../include

CPPFLAGS += -I$(SRC)/uts/common/fs/fusefs # -I$(SRC)/uts/common

# Debugging
${NOT_RELEASE_BUILD} CPPFLAGS += -DDEBUG

# uncomment these for dbx debugging
#COPTFLAG = -g
#CTF_FLAGS =
#CTFCONVERT_O=
#CTFMERGE_LIB=

all:	$(LIBS)

lint:	lintcheck_t

include ../../Makefile.targ

lintcheck_t: $$(SRCS)
	$(LINT.c) $(LINTCHECKFLAGS) $(SRCS) $(LDLIBS) $(LTAIL)

objs/%.o pics/%.o: $(MODDIR)/%.c
	$(COMPILE.c) -W0,-xc99=pragma -o $@ $<
	$(POST_PROCESS_O)

.KEEP_STATE:
