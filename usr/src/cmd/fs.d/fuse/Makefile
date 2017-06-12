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
# cmd/fs.d/fuse/Makefile
#

include $(SRC)/Makefile.master

SUBDIRS_CATALOG=	fuse-dmn mount umount
SUBDIRS=		$(SUBDIRS_CATALOG) config dtrace example fuse-cli

# for messaging catalog files
#
POFILES=        fuse-dmn/fuse-dmn.po mount/mount.po umount/umount.po
POFILE=         fuse.po

all:=		TARGET= all
install:=	TARGET= install
clean:=		TARGET= clean
clobber:=	TARGET= clobber
lint:=		TARGET= lint
catalog:=       TARGET= catalog

.KEEP_STATE:

.PARALLEL:	$(SUBDIRS)

all install clean clobber lint: $(SUBDIRS)

catalog: $(SUBDIRS_CATALOG)
	$(RM) $(POFILE)
	cat $(POFILES) > $(POFILE)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:
