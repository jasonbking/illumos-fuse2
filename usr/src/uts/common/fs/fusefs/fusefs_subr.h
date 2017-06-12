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

#ifndef _FS_FUSEFS_FUSEFS_SUBR_H_
#define	_FS_FUSEFS_FUSEFS_SUBR_H_

#include <sys/cmn_err.h>
#include <sys/uio.h>

#if defined(DEBUG) || defined(lint)
#define	FUSE_VNODE_DEBUG 1
#endif

#ifndef FALSE
#define	FALSE   (0)
#endif

#ifndef TRUE
#define	TRUE    (1)
#endif

/* Helper function for SMBERROR */
/*PRINTFLIKE3*/
extern void fusefs_errmsg(int, const char *, const char *, ...)
	__KPRINTFLIKE(3);

/*
 * Let's use C99 standard variadic macros!
 * Also the C99 __func__ (function name) feature.
 */
#define	FUSEFS_ERROR(...) \
	fusefs_errmsg(CE_NOTE, __func__, __VA_ARGS__)
#define	FUSEFS_DEBUG(...) \
	fusefs_errmsg(CE_CONT, __func__, __VA_ARGS__)

/*
 * Possible lock commands
 */
#define	FUSE_LOCK_EXCL		0
#define	FUSE_LOCK_SHARED		1
#define	FUSE_LOCK_RELEASE	2

/*
 * VFS-level init, fini stuff
 */

int fusefs_vfsinit(void);
void fusefs_vfsfini(void);
int fusefs_subrinit(void);
void fusefs_subrfini(void);
int fusefs_clntinit(void);
void fusefs_clntfini(void);

void fusefs_zonelist_add(fusemntinfo_t *);
void fusefs_zonelist_remove(fusemntinfo_t *);

int fusefs_check_table(struct vfs *vfsp, struct fusenode *srp);
void fusefs_destroy_table(struct vfs *vfsp);
void fusefs_rflush(struct vfs *vfsp, cred_t *cr);

/*
 * Function definitions - those having to do with
 * fusefs nodes, vnodes, etc
 */

void fusefs_attrcache_prune(struct fusenode *np);
void fusefs_attrcache_remove(struct fusenode *np);
void fusefs_attrcache_rm_locked(struct fusenode *np);
#ifndef	DEBUG
#define	fusefs_attrcache_rm_locked(np)	(np)->r_attrtime = gethrtime()
#endif
void fusefs_attr_touchdir(struct fusenode *);
void fusefs_attrcache_fa(vnode_t *, fusefattr_t *);
void fusefs_attrcache_va(vnode_t *, vattr_t *);
void fusefs_cache_check(struct vnode *, fusefattr_t *);

void fusefs_addfree(struct fusenode *sp);
void fusefs_rmhash(struct fusenode *);

/* See avl_create in fusefs_vfsops.c */
void fusefs_init_hash_avl(avl_tree_t *);

uint32_t fusefs_gethash(const char *rpath, int prlen);
uint32_t fusefs_getino(struct fusenode *dnp, const char *name, int nmlen);

extern fusefattr_t fusefs_fattr0;
fusenode_t *fusefs_node_findcreate(fusemntinfo_t *mi,
    const char *dir, int dirlen,
    const char *name, int nmlen,
    char sep, fusefattr_t *);

int fusefs_nget(vnode_t *dvp, const char *name, int nmlen,
	fusefattr_t *fap, vnode_t **vpp);

int fusefsgetattr(vnode_t *vp, struct vattr *vap, cred_t *cr);

/* For Solaris, interruptible rwlock */
int fusefs_rw_enter_sig(fusefs_rwlock_t *l, krw_t rw, int intr);
int fusefs_rw_tryenter(fusefs_rwlock_t *l, krw_t rw);
void fusefs_rw_exit(fusefs_rwlock_t *l);
int fusefs_rw_lock_held(fusefs_rwlock_t *l, krw_t rw);
void fusefs_rw_init(fusefs_rwlock_t *l, char *name, krw_type_t type, void *arg);
void fusefs_rw_destroy(fusefs_rwlock_t *l);

#endif /* !_FS_FUSEFS_FUSEFS_SUBR_H_ */
