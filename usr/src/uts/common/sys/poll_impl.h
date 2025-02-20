/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2003 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Copyright 2017 Joyent, Inc.
 * Copyright 2022 Oxide Computer Company
 */

#ifndef _SYS_POLL_IMPL_H
#define	_SYS_POLL_IMPL_H

/*
 * Caching Poll Subsystem:
 *
 * Each kernel thread (1), if engaged in poll system call, has a reference to
 * a pollstate_t (2), which contains relevant flags and locks.  The pollstate_t
 * contains a pointer to a pollcache_t (3), which caches the state of previous
 * calls to poll.  A bitmap (4) is stored inside the poll cache, where each
 * bit represents a file descriptor.  The bits are set if the corresponding
 * device has a polled event pending.  Only fds with their bit set will be
 * examined on the next poll invocation.  The pollstate_t also contains a list
 * of fd sets (5), which are represented by the pollcacheset_t type.  These
 * structures keep track of the pollfd_t arrays (6) passed in from userland.
 * Each polled file descriptor has a corresponding polldat_t which can be
 * chained onto a device's pollhead, and these are kept in a hash table (7)
 * inside the pollcache_t.  The hash table allows efficient conversion of a
 * given fd to its corresponding polldat_t.
 *
 * (1)              (2)
 * +-----------+    +-------------+
 * | kthread_t |--->| pollstate_t |-->+-------------+  (6)
 * +-----------+    +-------------+(5)| pcacheset_t |->[_][_][_][_] pollfd_t
 *                          |         +-------------+
 *                          |         | pcacheset_t |->[_][_][_][_] pollfd_t
 * (1a)                     |         +-------------+
 * +---------------+	    |
 * | /dev/poll tbl |	    |
 * +-v-------------+	    |
 *   |			    |
 *   +------------------+   |
 * (7)              (3) V   v
 * polldat hash     +-------------+    (4) bitmap representing fd space
 * [_][_][_][_]<----|             |--->000010010010001010101010101010110
 *  |  |  |  |      | pollcache_t |
 *  .  v  .  .      |             |
 *    [polldat_t]   +-------------+
 *     |
 *    [polldat_t]
 *     |
 *     v
 *     NULL
 *
 *
 * Both poll system call and /dev/poll use the pollcache_t structure
 * definition and the routines managing the structure. But poll(2) and
 * /dev/poll have their own copy of the structures. The /dev/poll driver
 * table (1a) contains an array of pointers, each pointing at a pollcache_t
 * struct (3). A device minor number is used as an device table index.
 *
 */
#include <sys/poll.h>

#if defined(_KERNEL) || defined(_KMEMUSER)

#include <sys/thread.h>
#include <sys/file.h>
#include <sys/port_kernel.h>

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Typedefs
 */
struct pollcache;
struct pollstate;
struct pcachelink;
struct polldat;

typedef struct pollcache pollcache_t;
typedef struct pollstate pollstate_t;
typedef struct pcachelink pcachelink_t;
typedef struct polldat polldat_t;

/*
 * description of pollcacheset structure
 */
typedef struct pollcacheset {
	uintptr_t	pcs_usradr;	/* usr pollfd array address */
	pollfd_t	*pcs_pollfd;	/* cached poll lists */
	size_t		pcs_nfds;	/* number of poll fd in cached list */
	ulong_t		pcs_count;	/* for LU replacement policy */
} pollcacheset_t;

#define	POLLFDSETS	2

/*
 * Maximum depth for recusive poll operations.
 */
#define	POLLMAXDEPTH	5

/*
 * State information kept by each polling thread
 */
struct pollstate {
	pollfd_t	*ps_pollfd;	/* hold the current poll list */
	size_t		ps_nfds;	/* size of ps_pollfd */
	kmutex_t	ps_lock;	/* mutex for sleep/wakeup */
	pollcache_t	*ps_pcache;	/* cached poll fd set */
	pollcacheset_t	*ps_pcacheset;	/* cached poll lists */
	int		ps_nsets;	/* no. of cached poll sets */
	pollfd_t	*ps_dpbuf;	/* return pollfd buf used by devpoll */
	size_t		ps_dpbufsize;	/* size of ps_dpbuf */
	int		ps_depth;	/* epoll recursion depth */
	pollcache_t	*ps_pc_stack[POLLMAXDEPTH]; /* epoll recursion state */
	pollcache_t	*ps_contend_pc;		/* pollcache waited on */
	pollstate_t	*ps_contend_nextp;	/* next in contender list */
	pollstate_t	**ps_contend_pnextp;	/* pointer-to-previous-next */
	int		ps_flags;	/* state flags */
	short		ps_implicit_ev;	/* implicit poll event interest */
};

/* pollstate flags */
#define	POLLSTATE_STALEMATE	0x1
#define	POLLSTATE_ULFAIL	0x2

/* pollstate_enter results */
#define	PSE_SUCCESS		0
#define	PSE_FAIL_DEPTH		1
#define	PSE_FAIL_LOOP		2
#define	PSE_FAIL_DEADLOCK	3
#define	PSE_FAIL_POLLSTATE	4

/*
 * poll cache size defines
 */
#define	POLLCHUNKSHIFT		8	/* hash table increment size is 256 */
#define	POLLHASHCHUNKSZ		(1 << POLLCHUNKSHIFT)
#define	POLLHASHINC		2	/* poll hash table growth factor */
#define	POLLHASHTHRESHOLD	2	/* poll hash list length threshold */
#define	POLLHASH(x, y)	((y) % (x))	/* poll hash function */

/*
 * poll.c assumes the POLLMAPCHUNK is power of 2
 */
#define	POLLMAPCHUNK	2048	/* bitmap inc -- each for 2K of polled fd's */

/*
 * used to refrence from watched fd back to the fd position in cached
 * poll list for quick revents update.
 */
typedef struct xref {
	ssize_t	xf_position;    /* xref fd position in poll fd list */
	short	xf_refcnt;	/* ref cnt of same fd in poll list */
} xref_t;

#define	POLLPOSINVAL	(-1L)	/* xf_position is invalid */
#define	POLLPOSTRANS	(-2L)	/* xf_position is transient state */


typedef enum pclstate {
	PCL_INIT = 0,	/* just allocated/zeroed, prior */
	PCL_VALID,	/* linked with both parent and child pollcaches */
	PCL_STALE,	/* still linked but marked stale, pending refresh */
	PCL_INVALID,	/* dissociated from one pollcache, awaiting cleanup */
	PCL_FREE	/* only meant to indicate use-after-free */
} pclstate_t;

/*
 * The pcachelink struct creates an association between parent and child
 * pollcaches in a recursive /dev/poll operation.  Fields are protected by
 * pcl_lock although manipulation of pcl_child_next or pcl_parent_next also
 * requires holding pc_lock in the respective pcl_parent_pc or pcl_child_pc
 * pollcache.
 */
struct pcachelink {
	kmutex_t	pcl_lock;		/* protects contents */
	pclstate_t	pcl_state;		/* status of link entry */
	int		pcl_refcnt;		/* ref cnt of linked pcaches */
	pollcache_t	*pcl_child_pc;		/* child pollcache */
	pollcache_t	*pcl_parent_pc;		/* parent pollcache */
	pcachelink_t	*pcl_child_next;	/* next in child list */
	pcachelink_t	*pcl_parent_next;	/* next in parents list */
};


/*
 * polldat is an entry for a cached poll fd. A polldat struct can be in
 * poll cache table as well as on pollhead ph_list, which is used by
 * pollwakeup to wake up a sleeping poller. There should be one polldat
 * per polled fd hanging off pollstate struct.
 */
struct polldat {
	int		pd_fd;		/* cached poll fd */
	int		pd_events;	/* union of all polled events */
	file_t		*pd_fp;		/* used to detect fd reuse */
	pollhead_t	*pd_php;	/* used to undo poll registration */
	kthread_t	*pd_thread;	/* used for waking up a sleep thrd */
	pollcache_t	*pd_pcache;	/* a ptr to the pollcache of this fd */
	polldat_t	*pd_next;	/* next on pollhead's ph_list */
	polldat_t	*pd_hashnext;	/* next on pollhead's ph_list */
	int		pd_count;	/* total count from all ref'ed sets */
	int		pd_nsets;	/* num of xref sets, used by poll(2) */
	xref_t		*pd_ref;	/* ptr to xref info, 1 for each set */
	port_kevent_t	*pd_portev;	/* associated port event struct */
	uf_entry_gen_t	pd_gen;		/* fd generation at cache time */
	uint64_t	pd_epolldata;	/* epoll data, if any */
};

/*
 * One cache for each thread that polls. Points to a bitmap (used by pollwakeup)
 * and a hash table of polldats.
 * The offset of pc_lock field must be kept in sync with the pc_lock offset
 * of port_fdcache_t, both structs implement pc_lock with offset 0 (see also
 * pollrelock()).
 */
struct pollcache {
	kmutex_t	pc_lock;	/* lock to protect pollcache */
	ulong_t		*pc_bitmap;	/* point to poll fd bitmap */
	polldat_t	**pc_hash;	/* points to a hash table of ptrs */
	int		pc_mapend;	/* the largest fd encountered so far */
	int		pc_mapsize;	/* the size of current map */
	int		pc_hashsize;	/* the size of current hash table */
	int		pc_fdcount;	/* track how many fd's are hashed */
	int		pc_flag;	/* see pc_flag define below */
	int		pc_busy;	/* can only exit when its 0 */
	kmutex_t	pc_no_exit;	/* protects pc_busy*, can't be nested */
	kcondvar_t	pc_busy_cv;	/* cv to wait on if ps_busy != 0 */
	kcondvar_t	pc_cv;		/* cv to wait on if needed */
	pid_t		pc_pid;		/* for check acc rights, devpoll only */
	int		pc_mapstart;	/* where search start, devpoll only */
	pcachelink_t	*pc_parents;	/* linked list of epoll parents */
	pcachelink_t	*pc_children;	/* linked list of epoll children */
};

/* pc_flag */
#define	PC_POLLWAKE	0x02	/* pollwakeup() occurred */
#define	PC_EPOLL	0x04	/* pollcache is epoll-enabled */

#if defined(_KERNEL)
/*
 * Internal routines.
 */
extern void pollnotify(pollcache_t *, int);

/*
 * public poll head interfaces (see poll.h):
 *
 *  pollhead_clean      clean up all polldats on a pollhead list
 */
extern void pollhead_clean(pollhead_t *);

/*
 * private poll head interfaces:
 *
 *  polldat_associate		adds a polldat to a pollhead list
 *  polldat_disassociate	remove polldat from its assoc'd pollhead list
 */
extern void polldat_associate(polldat_t *, pollhead_t *);
extern void polldat_disassociate(polldat_t *);

/*
 * poll state interfaces:
 *
 *  pollstate_create	initializes per-thread pollstate
 *  pollstate_destroy	cleans up per-thread pollstate
 *  pollstate_enter	safely lock pollcache for pollstate
 *  pollstate_exit	unlock pollcache from pollstate
 */
extern pollstate_t *pollstate_create(void);
extern void pollstate_destroy(pollstate_t *);
extern int pollstate_enter(pollcache_t *);
extern void pollstate_exit(pollcache_t *);

/*
 * public pcache interfaces:
 *
 *  pcache_alloc	allocate a poll cache skeleton
 *  pcache_create       creates all poll cache supporting data struct
 *  pcache_insert	cache a poll fd, calls pcache_insert_fd
 *  pcache_lookup       given an fd list, returns a cookie
 *  pcache_poll         polls the cache for fd's having events on them
 *  pcache_clean        clean up all the pollhead and fpollinfo reference
 *  pcache_destroy      destroys the pcache
 */
extern pollcache_t *pcache_alloc();
extern void pcache_create(pollcache_t *, nfds_t);
extern int pcache_insert(pollstate_t *, file_t *, pollfd_t *, int *, ssize_t,
    int);
extern int pcache_poll(pollfd_t *, pollstate_t *, nfds_t, int *, int);
extern void pcache_clean(pollcache_t *);
extern void pcache_destroy(pollcache_t *);

/*
 * private pcache interfaces:
 *
 *  pcache_lookup_fd	lookup an fd, returns a polldat
 *  pcache_alloc_fd	allocates and returns a polldat
 *  pcache_insert_fd	insert an fd into pcache (called by pcache_insert)
 *  pcache_delete_fd	insert an fd into pcache (called by pcacheset_delete_fd)
 *  pcache_grow_hashtbl	grows the pollcache hash table and rehash
 *  pcache_grow_map	grows the pollcache bitmap
 *  pcache_update_xref	update cross ref (from polldat back to cacheset) info
 *  pcache_clean_entry	cleanup an entry in pcache and more...
 *  pcache_wake_parents	wake linked parent pollcaches
 */
extern polldat_t *pcache_lookup_fd(pollcache_t *, int);
extern polldat_t *pcache_alloc_fd(int);
extern void pcache_insert_fd(pollcache_t *, polldat_t *, nfds_t);
extern int pcache_delete_fd(pollstate_t *, int, size_t, int, uint_t);
extern void pcache_grow_hashtbl(pollcache_t *, nfds_t);
extern void pcache_grow_map(pollcache_t *, int);
extern void pcache_update_xref(pollcache_t *, int, ssize_t, int);
extern void pcache_clean_entry(pollstate_t *, int);
extern void pcache_wake_parents(pollcache_t *);

/*
 * pcacheset interfaces:
 *
 * pcacheset_create     creates new pcachesets (easier for dynamic pcachesets)
 * pcacheset_destroy    destroys a pcacheset
 * pcacheset_cache_list caches and polls a new poll list
 * pcacheset_remove_list removes (usually a partial) cached poll list
 * pcacheset_resolve    resolves extant pcacheset and fd list
 * pcacheset_cmp        compares a pcacheset with an fd list
 * pcacheset_invalidate invalidate entries in pcachesets
 * pcacheset_reset_count resets the usage counter of pcachesets
 * pcacheset_replace	selects a poll cacheset for replacement
 */
extern pollcacheset_t *pcacheset_create(int);
extern void pcacheset_destroy(pollcacheset_t *, int);
extern int pcacheset_cache_list(pollstate_t *, pollfd_t *, int *, int);
extern void pcacheset_remove_list(pollstate_t *, pollfd_t *, int, int, int,
    int);
extern int pcacheset_resolve(pollstate_t *, nfds_t, int *, int);
extern int pcacheset_cmp(pollfd_t *, pollfd_t *, pollfd_t *, int);
extern void pcacheset_invalidate(pollstate_t *, polldat_t *);
extern void pcacheset_reset_count(pollstate_t *, int);
extern int pcacheset_replace(pollstate_t *);

#endif /* defined(_KERNEL) */

#ifdef	__cplusplus
}
#endif

#endif /* defined(_KERNEL) || defined(_KMEMUSER) */

#endif	/* _SYS_POLL_IMPL_H */
