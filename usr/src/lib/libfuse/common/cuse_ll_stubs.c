/*
  CUSE: Character device in Userspace
  Copyright (C) 2008       SUSE Linux Products GmbH
  Copyright (C) 2008       Tejun Heo <teheo@suse.de>

  This program can be distributed under the terms of the GNU LGPLv2.
  See the file COPYING.LIB.
*/

#include "cuse_lowlevel.h"
#include "fuse_kernel.h"
#include "fuse_i.h"
#include "fuse_opt.h"
#include "fuse_misc.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <unistd.h>

/*
 * This implementation does not support any "lowlevel" stuff,
 * so just stub these out.
 */

/* ARGSUSED */
struct fuse_session *cuse_lowlevel_new(struct fuse_args *args,
				       const struct cuse_info *ci,
				       const struct cuse_lowlevel_ops *clop,
				       void *userdata)
{
	return (NULL);
}

/* ARGSUSED */
struct fuse_session *cuse_lowlevel_setup(int argc, char *argv[],
					 const struct cuse_info *ci,
					 const struct cuse_lowlevel_ops *clop,
					 int *multithreaded, void *userdata)
{
	return (NULL);
}

/* ARGSUSED */
void cuse_lowlevel_teardown(struct fuse_session *se)
{
}

/* ARGSUSED */
int cuse_lowlevel_main(int argc, char *argv[], const struct cuse_info *ci,
		       const struct cuse_lowlevel_ops *clop, void *userdata)
{
	return (1);
}
