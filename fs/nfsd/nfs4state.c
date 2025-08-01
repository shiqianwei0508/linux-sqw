/*
*  Copyright (c) 2001 The Regents of the University of Michigan.
*  All rights reserved.
*
*  Kendrick Smith <kmsmith@umich.edu>
*  Andy Adamson <kandros@umich.edu>
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*  1. Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*  2. Redistributions in binary form must reproduce the above copyright
*     notice, this list of conditions and the following disclaimer in the
*     documentation and/or other materials provided with the distribution.
*  3. Neither the name of the University nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED ``AS IS'' AND ANY EXPRESS OR IMPLIED
*  WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
*  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
*  DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
*  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
*  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
*  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
*  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
*  LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
*  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
*  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*
*/

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/namei.h>
#include <linux/swap.h>
#include <linux/pagemap.h>
#include <linux/ratelimit.h>
#include <linux/sunrpc/svcauth_gss.h>
#include <linux/sunrpc/addr.h>
#include <linux/jhash.h>
#include <linux/string_helpers.h>
#include <linux/fsnotify.h>
#include <linux/rhashtable.h>
#include <linux/nfs_ssc.h>

#include "xdr4.h"
#include "xdr4cb.h"
#include "vfs.h"
#include "current_stateid.h"

#include "netns.h"
#include "pnfs.h"
#include "filecache.h"
#include "trace.h"

#define NFSDDBG_FACILITY                NFSDDBG_PROC

#define all_ones {{ ~0, ~0}, ~0}
static const stateid_t one_stateid = {
	.si_generation = ~0,
	.si_opaque = all_ones,
};
static const stateid_t zero_stateid = {
	/* all fields zero */
};
static const stateid_t currentstateid = {
	.si_generation = 1,
};
static const stateid_t close_stateid = {
	.si_generation = 0xffffffffU,
};

static u64 current_sessionid = 1;

#define ZERO_STATEID(stateid) (!memcmp((stateid), &zero_stateid, sizeof(stateid_t)))
#define ONE_STATEID(stateid)  (!memcmp((stateid), &one_stateid, sizeof(stateid_t)))
#define CURRENT_STATEID(stateid) (!memcmp((stateid), &currentstateid, sizeof(stateid_t)))
#define CLOSE_STATEID(stateid)  (!memcmp((stateid), &close_stateid, sizeof(stateid_t)))

/* forward declarations */
static bool check_for_locks(struct nfs4_file *fp, struct nfs4_lockowner *lowner);
static void nfs4_free_ol_stateid(struct nfs4_stid *stid);
void nfsd4_end_grace(struct nfsd_net *nn);
static void _free_cpntf_state_locked(struct nfsd_net *nn, struct nfs4_cpntf_state *cps);
static void nfsd4_file_hash_remove(struct nfs4_file *fi);
static void deleg_reaper(struct nfsd_net *nn);

/* Locking: */

/*
 * Currently used for the del_recall_lru and file hash table.  In an
 * effort to decrease the scope of the client_mutex, this spinlock may
 * eventually cover more:
 */
static DEFINE_SPINLOCK(state_lock);

enum nfsd4_st_mutex_lock_subclass {
	OPEN_STATEID_MUTEX = 0,
	LOCK_STATEID_MUTEX = 1,
};

/*
 * A waitqueue for all in-progress 4.0 CLOSE operations that are waiting for
 * the refcount on the open stateid to drop.
 */
static DECLARE_WAIT_QUEUE_HEAD(close_wq);

/*
 * A waitqueue where a writer to clients/#/ctl destroying a client can
 * wait for cl_rpc_users to drop to 0 and then for the client to be
 * unhashed.
 */
static DECLARE_WAIT_QUEUE_HEAD(expiry_wq);

static struct kmem_cache *client_slab;
static struct kmem_cache *openowner_slab;
static struct kmem_cache *lockowner_slab;
static struct kmem_cache *file_slab;
static struct kmem_cache *stateid_slab;
static struct kmem_cache *deleg_slab;
static struct kmem_cache *odstate_slab;

static void free_session(struct nfsd4_session *);

static const struct nfsd4_callback_ops nfsd4_cb_recall_ops;
static const struct nfsd4_callback_ops nfsd4_cb_notify_lock_ops;
static const struct nfsd4_callback_ops nfsd4_cb_getattr_ops;

static struct workqueue_struct *laundry_wq;

int nfsd4_create_laundry_wq(void)
{
	int rc = 0;

	laundry_wq = alloc_workqueue("%s", WQ_UNBOUND, 0, "nfsd4");
	if (laundry_wq == NULL)
		rc = -ENOMEM;
	return rc;
}

void nfsd4_destroy_laundry_wq(void)
{
	destroy_workqueue(laundry_wq);
}

static bool is_session_dead(struct nfsd4_session *ses)
{
	return ses->se_dead;
}

static __be32 mark_session_dead_locked(struct nfsd4_session *ses, int ref_held_by_me)
{
	if (atomic_read(&ses->se_ref) > ref_held_by_me)
		return nfserr_jukebox;
	ses->se_dead = true;
	return nfs_ok;
}

static bool is_client_expired(struct nfs4_client *clp)
{
	return clp->cl_time == 0;
}

static void nfsd4_dec_courtesy_client_count(struct nfsd_net *nn,
					struct nfs4_client *clp)
{
	if (clp->cl_state != NFSD4_ACTIVE)
		atomic_add_unless(&nn->nfsd_courtesy_clients, -1, 0);
}

static __be32 get_client_locked(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	lockdep_assert_held(&nn->client_lock);

	if (is_client_expired(clp))
		return nfserr_expired;
	atomic_inc(&clp->cl_rpc_users);
	nfsd4_dec_courtesy_client_count(nn, clp);
	clp->cl_state = NFSD4_ACTIVE;
	return nfs_ok;
}

/* must be called under the client_lock */
static inline void
renew_client_locked(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	if (is_client_expired(clp)) {
		WARN_ON(1);
		printk("%s: client (clientid %08x/%08x) already expired\n",
			__func__,
			clp->cl_clientid.cl_boot,
			clp->cl_clientid.cl_id);
		return;
	}

	list_move_tail(&clp->cl_lru, &nn->client_lru);
	clp->cl_time = ktime_get_boottime_seconds();
	nfsd4_dec_courtesy_client_count(nn, clp);
	clp->cl_state = NFSD4_ACTIVE;
}

static void put_client_renew_locked(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	lockdep_assert_held(&nn->client_lock);

	if (!atomic_dec_and_test(&clp->cl_rpc_users))
		return;
	if (!is_client_expired(clp))
		renew_client_locked(clp);
	else
		wake_up_all(&expiry_wq);
}

static void put_client_renew(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	if (!atomic_dec_and_lock(&clp->cl_rpc_users, &nn->client_lock))
		return;
	if (!is_client_expired(clp))
		renew_client_locked(clp);
	else
		wake_up_all(&expiry_wq);
	spin_unlock(&nn->client_lock);
}

static __be32 nfsd4_get_session_locked(struct nfsd4_session *ses)
{
	__be32 status;

	if (is_session_dead(ses))
		return nfserr_badsession;
	status = get_client_locked(ses->se_client);
	if (status)
		return status;
	atomic_inc(&ses->se_ref);
	return nfs_ok;
}

static void nfsd4_put_session_locked(struct nfsd4_session *ses)
{
	struct nfs4_client *clp = ses->se_client;
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	lockdep_assert_held(&nn->client_lock);

	if (atomic_dec_and_test(&ses->se_ref) && is_session_dead(ses))
		free_session(ses);
	put_client_renew_locked(clp);
}

static void nfsd4_put_session(struct nfsd4_session *ses)
{
	struct nfs4_client *clp = ses->se_client;
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	spin_lock(&nn->client_lock);
	nfsd4_put_session_locked(ses);
	spin_unlock(&nn->client_lock);
}

static struct nfsd4_blocked_lock *
find_blocked_lock(struct nfs4_lockowner *lo, struct knfsd_fh *fh,
			struct nfsd_net *nn)
{
	struct nfsd4_blocked_lock *cur, *found = NULL;

	spin_lock(&nn->blocked_locks_lock);
	list_for_each_entry(cur, &lo->lo_blocked, nbl_list) {
		if (fh_match(fh, &cur->nbl_fh)) {
			list_del_init(&cur->nbl_list);
			WARN_ON(list_empty(&cur->nbl_lru));
			list_del_init(&cur->nbl_lru);
			found = cur;
			break;
		}
	}
	spin_unlock(&nn->blocked_locks_lock);
	if (found)
		locks_delete_block(&found->nbl_lock);
	return found;
}

static struct nfsd4_blocked_lock *
find_or_allocate_block(struct nfs4_lockowner *lo, struct knfsd_fh *fh,
			struct nfsd_net *nn)
{
	struct nfsd4_blocked_lock *nbl;

	nbl = find_blocked_lock(lo, fh, nn);
	if (!nbl) {
		nbl = kmalloc(sizeof(*nbl), GFP_KERNEL);
		if (nbl) {
			INIT_LIST_HEAD(&nbl->nbl_list);
			INIT_LIST_HEAD(&nbl->nbl_lru);
			fh_copy_shallow(&nbl->nbl_fh, fh);
			locks_init_lock(&nbl->nbl_lock);
			kref_init(&nbl->nbl_kref);
			nfsd4_init_cb(&nbl->nbl_cb, lo->lo_owner.so_client,
					&nfsd4_cb_notify_lock_ops,
					NFSPROC4_CLNT_CB_NOTIFY_LOCK);
		}
	}
	return nbl;
}

static void
free_nbl(struct kref *kref)
{
	struct nfsd4_blocked_lock *nbl;

	nbl = container_of(kref, struct nfsd4_blocked_lock, nbl_kref);
	locks_release_private(&nbl->nbl_lock);
	kfree(nbl);
}

static void
free_blocked_lock(struct nfsd4_blocked_lock *nbl)
{
	locks_delete_block(&nbl->nbl_lock);
	kref_put(&nbl->nbl_kref, free_nbl);
}

static void
remove_blocked_locks(struct nfs4_lockowner *lo)
{
	struct nfs4_client *clp = lo->lo_owner.so_client;
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);
	struct nfsd4_blocked_lock *nbl;
	LIST_HEAD(reaplist);

	/* Dequeue all blocked locks */
	spin_lock(&nn->blocked_locks_lock);
	while (!list_empty(&lo->lo_blocked)) {
		nbl = list_first_entry(&lo->lo_blocked,
					struct nfsd4_blocked_lock,
					nbl_list);
		list_del_init(&nbl->nbl_list);
		WARN_ON(list_empty(&nbl->nbl_lru));
		list_move(&nbl->nbl_lru, &reaplist);
	}
	spin_unlock(&nn->blocked_locks_lock);

	/* Now free them */
	while (!list_empty(&reaplist)) {
		nbl = list_first_entry(&reaplist, struct nfsd4_blocked_lock,
					nbl_lru);
		list_del_init(&nbl->nbl_lru);
		free_blocked_lock(nbl);
	}
}

static void
nfsd4_cb_notify_lock_prepare(struct nfsd4_callback *cb)
{
	struct nfsd4_blocked_lock	*nbl = container_of(cb,
						struct nfsd4_blocked_lock, nbl_cb);
	locks_delete_block(&nbl->nbl_lock);
}

static int
nfsd4_cb_notify_lock_done(struct nfsd4_callback *cb, struct rpc_task *task)
{
	trace_nfsd_cb_notify_lock_done(&zero_stateid, task);

	/*
	 * Since this is just an optimization, we don't try very hard if it
	 * turns out not to succeed. We'll requeue it on NFS4ERR_DELAY, and
	 * just quit trying on anything else.
	 */
	switch (task->tk_status) {
	case -NFS4ERR_DELAY:
		rpc_delay(task, 1 * HZ);
		return 0;
	default:
		return 1;
	}
}

static void
nfsd4_cb_notify_lock_release(struct nfsd4_callback *cb)
{
	struct nfsd4_blocked_lock	*nbl = container_of(cb,
						struct nfsd4_blocked_lock, nbl_cb);

	free_blocked_lock(nbl);
}

static const struct nfsd4_callback_ops nfsd4_cb_notify_lock_ops = {
	.prepare	= nfsd4_cb_notify_lock_prepare,
	.done		= nfsd4_cb_notify_lock_done,
	.release	= nfsd4_cb_notify_lock_release,
	.opcode		= OP_CB_NOTIFY_LOCK,
};

/*
 * We store the NONE, READ, WRITE, and BOTH bits separately in the
 * st_{access,deny}_bmap field of the stateid, in order to track not
 * only what share bits are currently in force, but also what
 * combinations of share bits previous opens have used.  This allows us
 * to enforce the recommendation in
 * https://datatracker.ietf.org/doc/html/rfc7530#section-16.19.4 that
 * the server return an error if the client attempt to downgrade to a
 * combination of share bits not explicable by closing some of its
 * previous opens.
 *
 * This enforcement is arguably incomplete, since we don't keep
 * track of access/deny bit combinations; so, e.g., we allow:
 *
 *	OPEN allow read, deny write
 *	OPEN allow both, deny none
 *	DOWNGRADE allow read, deny none
 *
 * which we should reject.
 *
 * But you could also argue that our current code is already overkill,
 * since it only exists to return NFS4ERR_INVAL on incorrect client
 * behavior.
 */
static unsigned int
bmap_to_share_mode(unsigned long bmap)
{
	int i;
	unsigned int access = 0;

	for (i = 1; i < 4; i++) {
		if (test_bit(i, &bmap))
			access |= i;
	}
	return access;
}

/* set share access for a given stateid */
static inline void
set_access(u32 access, struct nfs4_ol_stateid *stp)
{
	unsigned char mask = 1 << access;

	WARN_ON_ONCE(access > NFS4_SHARE_ACCESS_BOTH);
	stp->st_access_bmap |= mask;
}

/* clear share access for a given stateid */
static inline void
clear_access(u32 access, struct nfs4_ol_stateid *stp)
{
	unsigned char mask = 1 << access;

	WARN_ON_ONCE(access > NFS4_SHARE_ACCESS_BOTH);
	stp->st_access_bmap &= ~mask;
}

/* test whether a given stateid has access */
static inline bool
test_access(u32 access, struct nfs4_ol_stateid *stp)
{
	unsigned char mask = 1 << access;

	return (bool)(stp->st_access_bmap & mask);
}

/* set share deny for a given stateid */
static inline void
set_deny(u32 deny, struct nfs4_ol_stateid *stp)
{
	unsigned char mask = 1 << deny;

	WARN_ON_ONCE(deny > NFS4_SHARE_DENY_BOTH);
	stp->st_deny_bmap |= mask;
}

/* clear share deny for a given stateid */
static inline void
clear_deny(u32 deny, struct nfs4_ol_stateid *stp)
{
	unsigned char mask = 1 << deny;

	WARN_ON_ONCE(deny > NFS4_SHARE_DENY_BOTH);
	stp->st_deny_bmap &= ~mask;
}

/* test whether a given stateid is denying specific access */
static inline bool
test_deny(u32 deny, struct nfs4_ol_stateid *stp)
{
	unsigned char mask = 1 << deny;

	return (bool)(stp->st_deny_bmap & mask);
}

static int nfs4_access_to_omode(u32 access)
{
	switch (access & NFS4_SHARE_ACCESS_BOTH) {
	case NFS4_SHARE_ACCESS_READ:
		return O_RDONLY;
	case NFS4_SHARE_ACCESS_WRITE:
		return O_WRONLY;
	case NFS4_SHARE_ACCESS_BOTH:
		return O_RDWR;
	}
	WARN_ON_ONCE(1);
	return O_RDONLY;
}

static inline int
access_permit_read(struct nfs4_ol_stateid *stp)
{
	return test_access(NFS4_SHARE_ACCESS_READ, stp) ||
		test_access(NFS4_SHARE_ACCESS_BOTH, stp) ||
		test_access(NFS4_SHARE_ACCESS_WRITE, stp);
}

static inline int
access_permit_write(struct nfs4_ol_stateid *stp)
{
	return test_access(NFS4_SHARE_ACCESS_WRITE, stp) ||
		test_access(NFS4_SHARE_ACCESS_BOTH, stp);
}

static inline struct nfs4_stateowner *
nfs4_get_stateowner(struct nfs4_stateowner *sop)
{
	atomic_inc(&sop->so_count);
	return sop;
}

static int
same_owner_str(struct nfs4_stateowner *sop, struct xdr_netobj *owner)
{
	return (sop->so_owner.len == owner->len) &&
		0 == memcmp(sop->so_owner.data, owner->data, owner->len);
}

static struct nfs4_openowner *
find_openstateowner_str(unsigned int hashval, struct nfsd4_open *open,
			struct nfs4_client *clp)
{
	struct nfs4_stateowner *so;

	lockdep_assert_held(&clp->cl_lock);

	list_for_each_entry(so, &clp->cl_ownerstr_hashtbl[hashval],
			    so_strhash) {
		if (!so->so_is_open_owner)
			continue;
		if (same_owner_str(so, &open->op_owner))
			return openowner(nfs4_get_stateowner(so));
	}
	return NULL;
}

static inline u32
opaque_hashval(const void *ptr, int nbytes)
{
	unsigned char *cptr = (unsigned char *) ptr;

	u32 x = 0;
	while (nbytes--) {
		x *= 37;
		x += *cptr++;
	}
	return x;
}

void
put_nfs4_file(struct nfs4_file *fi)
{
	if (refcount_dec_and_test(&fi->fi_ref)) {
		nfsd4_file_hash_remove(fi);
		WARN_ON_ONCE(!list_empty(&fi->fi_clnt_odstate));
		WARN_ON_ONCE(!list_empty(&fi->fi_delegations));
		kfree_rcu(fi, fi_rcu);
	}
}

static struct nfsd_file *
find_writeable_file_locked(struct nfs4_file *f)
{
	struct nfsd_file *ret;

	lockdep_assert_held(&f->fi_lock);

	ret = nfsd_file_get(f->fi_fds[O_WRONLY]);
	if (!ret)
		ret = nfsd_file_get(f->fi_fds[O_RDWR]);
	return ret;
}

static struct nfsd_file *
find_writeable_file(struct nfs4_file *f)
{
	struct nfsd_file *ret;

	spin_lock(&f->fi_lock);
	ret = find_writeable_file_locked(f);
	spin_unlock(&f->fi_lock);

	return ret;
}

static struct nfsd_file *
find_readable_file_locked(struct nfs4_file *f)
{
	struct nfsd_file *ret;

	lockdep_assert_held(&f->fi_lock);

	ret = nfsd_file_get(f->fi_fds[O_RDONLY]);
	if (!ret)
		ret = nfsd_file_get(f->fi_fds[O_RDWR]);
	return ret;
}

static struct nfsd_file *
find_readable_file(struct nfs4_file *f)
{
	struct nfsd_file *ret;

	spin_lock(&f->fi_lock);
	ret = find_readable_file_locked(f);
	spin_unlock(&f->fi_lock);

	return ret;
}

struct nfsd_file *
find_any_file(struct nfs4_file *f)
{
	struct nfsd_file *ret;

	if (!f)
		return NULL;
	spin_lock(&f->fi_lock);
	ret = nfsd_file_get(f->fi_fds[O_RDWR]);
	if (!ret) {
		ret = nfsd_file_get(f->fi_fds[O_WRONLY]);
		if (!ret)
			ret = nfsd_file_get(f->fi_fds[O_RDONLY]);
	}
	spin_unlock(&f->fi_lock);
	return ret;
}

static struct nfsd_file *find_any_file_locked(struct nfs4_file *f)
{
	lockdep_assert_held(&f->fi_lock);

	if (f->fi_fds[O_RDWR])
		return f->fi_fds[O_RDWR];
	if (f->fi_fds[O_WRONLY])
		return f->fi_fds[O_WRONLY];
	if (f->fi_fds[O_RDONLY])
		return f->fi_fds[O_RDONLY];
	return NULL;
}

static atomic_long_t num_delegations;
unsigned long max_delegations;

/*
 * Open owner state (share locks)
 */

/* hash tables for lock and open owners */
#define OWNER_HASH_BITS              8
#define OWNER_HASH_SIZE             (1 << OWNER_HASH_BITS)
#define OWNER_HASH_MASK             (OWNER_HASH_SIZE - 1)

static unsigned int ownerstr_hashval(struct xdr_netobj *ownername)
{
	unsigned int ret;

	ret = opaque_hashval(ownername->data, ownername->len);
	return ret & OWNER_HASH_MASK;
}

static struct rhltable nfs4_file_rhltable ____cacheline_aligned_in_smp;

static const struct rhashtable_params nfs4_file_rhash_params = {
	.key_len		= sizeof_field(struct nfs4_file, fi_inode),
	.key_offset		= offsetof(struct nfs4_file, fi_inode),
	.head_offset		= offsetof(struct nfs4_file, fi_rlist),

	/*
	 * Start with a single page hash table to reduce resizing churn
	 * on light workloads.
	 */
	.min_size		= 256,
	.automatic_shrinking	= true,
};

/*
 * Check if courtesy clients have conflicting access and resolve it if possible
 *
 * access:  is op_share_access if share_access is true.
 *	    Check if access mode, op_share_access, would conflict with
 *	    the current deny mode of the file 'fp'.
 * access:  is op_share_deny if share_access is false.
 *	    Check if the deny mode, op_share_deny, would conflict with
 *	    current access of the file 'fp'.
 * stp:     skip checking this entry.
 * new_stp: normal open, not open upgrade.
 *
 * Function returns:
 *	false - access/deny mode conflict with normal client.
 *	true  - no conflict or conflict with courtesy client(s) is resolved.
 */
static bool
nfs4_resolve_deny_conflicts_locked(struct nfs4_file *fp, bool new_stp,
		struct nfs4_ol_stateid *stp, u32 access, bool share_access)
{
	struct nfs4_ol_stateid *st;
	bool resolvable = true;
	unsigned char bmap;
	struct nfsd_net *nn;
	struct nfs4_client *clp;

	lockdep_assert_held(&fp->fi_lock);
	list_for_each_entry(st, &fp->fi_stateids, st_perfile) {
		/* ignore lock stateid */
		if (st->st_openstp)
			continue;
		if (st == stp && new_stp)
			continue;
		/* check file access against deny mode or vice versa */
		bmap = share_access ? st->st_deny_bmap : st->st_access_bmap;
		if (!(access & bmap_to_share_mode(bmap)))
			continue;
		clp = st->st_stid.sc_client;
		if (try_to_expire_client(clp))
			continue;
		resolvable = false;
		break;
	}
	if (resolvable) {
		clp = stp->st_stid.sc_client;
		nn = net_generic(clp->net, nfsd_net_id);
		mod_delayed_work(laundry_wq, &nn->laundromat_work, 0);
	}
	return resolvable;
}

static void
__nfs4_file_get_access(struct nfs4_file *fp, u32 access)
{
	lockdep_assert_held(&fp->fi_lock);

	if (access & NFS4_SHARE_ACCESS_WRITE)
		atomic_inc(&fp->fi_access[O_WRONLY]);
	if (access & NFS4_SHARE_ACCESS_READ)
		atomic_inc(&fp->fi_access[O_RDONLY]);
}

static __be32
nfs4_file_get_access(struct nfs4_file *fp, u32 access)
{
	lockdep_assert_held(&fp->fi_lock);

	/* Does this access mode make sense? */
	if (access & ~NFS4_SHARE_ACCESS_BOTH)
		return nfserr_inval;

	/* Does it conflict with a deny mode already set? */
	if ((access & fp->fi_share_deny) != 0)
		return nfserr_share_denied;

	__nfs4_file_get_access(fp, access);
	return nfs_ok;
}

static __be32 nfs4_file_check_deny(struct nfs4_file *fp, u32 deny)
{
	/* Common case is that there is no deny mode. */
	if (deny) {
		/* Does this deny mode make sense? */
		if (deny & ~NFS4_SHARE_DENY_BOTH)
			return nfserr_inval;

		if ((deny & NFS4_SHARE_DENY_READ) &&
		    atomic_read(&fp->fi_access[O_RDONLY]))
			return nfserr_share_denied;

		if ((deny & NFS4_SHARE_DENY_WRITE) &&
		    atomic_read(&fp->fi_access[O_WRONLY]))
			return nfserr_share_denied;
	}
	return nfs_ok;
}

static void __nfs4_file_put_access(struct nfs4_file *fp, int oflag)
{
	might_lock(&fp->fi_lock);

	if (atomic_dec_and_lock(&fp->fi_access[oflag], &fp->fi_lock)) {
		struct nfsd_file *f1 = NULL;
		struct nfsd_file *f2 = NULL;

		swap(f1, fp->fi_fds[oflag]);
		if (atomic_read(&fp->fi_access[1 - oflag]) == 0)
			swap(f2, fp->fi_fds[O_RDWR]);
		spin_unlock(&fp->fi_lock);
		if (f1)
			nfsd_file_put(f1);
		if (f2)
			nfsd_file_put(f2);
	}
}

static void nfs4_file_put_access(struct nfs4_file *fp, u32 access)
{
	WARN_ON_ONCE(access & ~NFS4_SHARE_ACCESS_BOTH);

	if (access & NFS4_SHARE_ACCESS_WRITE)
		__nfs4_file_put_access(fp, O_WRONLY);
	if (access & NFS4_SHARE_ACCESS_READ)
		__nfs4_file_put_access(fp, O_RDONLY);
}

/*
 * Allocate a new open/delegation state counter. This is needed for
 * pNFS for proper return on close semantics.
 *
 * Note that we only allocate it for pNFS-enabled exports, otherwise
 * all pointers to struct nfs4_clnt_odstate are always NULL.
 */
static struct nfs4_clnt_odstate *
alloc_clnt_odstate(struct nfs4_client *clp)
{
	struct nfs4_clnt_odstate *co;

	co = kmem_cache_zalloc(odstate_slab, GFP_KERNEL);
	if (co) {
		co->co_client = clp;
		refcount_set(&co->co_odcount, 1);
	}
	return co;
}

static void
hash_clnt_odstate_locked(struct nfs4_clnt_odstate *co)
{
	struct nfs4_file *fp = co->co_file;

	lockdep_assert_held(&fp->fi_lock);
	list_add(&co->co_perfile, &fp->fi_clnt_odstate);
}

static inline void
get_clnt_odstate(struct nfs4_clnt_odstate *co)
{
	if (co)
		refcount_inc(&co->co_odcount);
}

static void
put_clnt_odstate(struct nfs4_clnt_odstate *co)
{
	struct nfs4_file *fp;

	if (!co)
		return;

	fp = co->co_file;
	if (refcount_dec_and_lock(&co->co_odcount, &fp->fi_lock)) {
		list_del(&co->co_perfile);
		spin_unlock(&fp->fi_lock);

		nfsd4_return_all_file_layouts(co->co_client, fp);
		kmem_cache_free(odstate_slab, co);
	}
}

static struct nfs4_clnt_odstate *
find_or_hash_clnt_odstate(struct nfs4_file *fp, struct nfs4_clnt_odstate *new)
{
	struct nfs4_clnt_odstate *co;
	struct nfs4_client *cl;

	if (!new)
		return NULL;

	cl = new->co_client;

	spin_lock(&fp->fi_lock);
	list_for_each_entry(co, &fp->fi_clnt_odstate, co_perfile) {
		if (co->co_client == cl) {
			get_clnt_odstate(co);
			goto out;
		}
	}
	co = new;
	co->co_file = fp;
	hash_clnt_odstate_locked(new);
out:
	spin_unlock(&fp->fi_lock);
	return co;
}

struct nfs4_stid *nfs4_alloc_stid(struct nfs4_client *cl, struct kmem_cache *slab,
				  void (*sc_free)(struct nfs4_stid *))
{
	struct nfs4_stid *stid;
	int new_id;

	stid = kmem_cache_zalloc(slab, GFP_KERNEL);
	if (!stid)
		return NULL;

	idr_preload(GFP_KERNEL);
	spin_lock(&cl->cl_lock);
	/* Reserving 0 for start of file in nfsdfs "states" file: */
	new_id = idr_alloc_cyclic(&cl->cl_stateids, stid, 1, 0, GFP_NOWAIT);
	spin_unlock(&cl->cl_lock);
	idr_preload_end();
	if (new_id < 0)
		goto out_free;

	stid->sc_free = sc_free;
	stid->sc_client = cl;
	stid->sc_stateid.si_opaque.so_id = new_id;
	stid->sc_stateid.si_opaque.so_clid = cl->cl_clientid;
	/* Will be incremented before return to client: */
	refcount_set(&stid->sc_count, 1);
	spin_lock_init(&stid->sc_lock);
	INIT_LIST_HEAD(&stid->sc_cp_list);

	return stid;
out_free:
	kmem_cache_free(slab, stid);
	return NULL;
}

/*
 * Create a unique stateid_t to represent each COPY.
 */
static int nfs4_init_cp_state(struct nfsd_net *nn, copy_stateid_t *stid,
			      unsigned char cs_type)
{
	int new_id;

	stid->cs_stid.si_opaque.so_clid.cl_boot = (u32)nn->boot_time;
	stid->cs_stid.si_opaque.so_clid.cl_id = nn->s2s_cp_cl_id;

	idr_preload(GFP_KERNEL);
	spin_lock(&nn->s2s_cp_lock);
	new_id = idr_alloc_cyclic(&nn->s2s_cp_stateids, stid, 0, 0, GFP_NOWAIT);
	stid->cs_stid.si_opaque.so_id = new_id;
	stid->cs_stid.si_generation = 1;
	spin_unlock(&nn->s2s_cp_lock);
	idr_preload_end();
	if (new_id < 0)
		return 0;
	stid->cs_type = cs_type;
	return 1;
}

int nfs4_init_copy_state(struct nfsd_net *nn, struct nfsd4_copy *copy)
{
	return nfs4_init_cp_state(nn, &copy->cp_stateid, NFS4_COPY_STID);
}

struct nfs4_cpntf_state *nfs4_alloc_init_cpntf_state(struct nfsd_net *nn,
						     struct nfs4_stid *p_stid)
{
	struct nfs4_cpntf_state *cps;

	cps = kzalloc(sizeof(struct nfs4_cpntf_state), GFP_KERNEL);
	if (!cps)
		return NULL;
	cps->cpntf_time = ktime_get_boottime_seconds();
	refcount_set(&cps->cp_stateid.cs_count, 1);
	if (!nfs4_init_cp_state(nn, &cps->cp_stateid, NFS4_COPYNOTIFY_STID))
		goto out_free;
	spin_lock(&nn->s2s_cp_lock);
	list_add(&cps->cp_list, &p_stid->sc_cp_list);
	spin_unlock(&nn->s2s_cp_lock);
	return cps;
out_free:
	kfree(cps);
	return NULL;
}

void nfs4_free_copy_state(struct nfsd4_copy *copy)
{
	struct nfsd_net *nn;

	if (copy->cp_stateid.cs_type != NFS4_COPY_STID)
		return;
	nn = net_generic(copy->cp_clp->net, nfsd_net_id);
	spin_lock(&nn->s2s_cp_lock);
	idr_remove(&nn->s2s_cp_stateids,
		   copy->cp_stateid.cs_stid.si_opaque.so_id);
	spin_unlock(&nn->s2s_cp_lock);
}

static void nfs4_free_cpntf_statelist(struct net *net, struct nfs4_stid *stid)
{
	struct nfs4_cpntf_state *cps;
	struct nfsd_net *nn;

	nn = net_generic(net, nfsd_net_id);
	spin_lock(&nn->s2s_cp_lock);
	while (!list_empty(&stid->sc_cp_list)) {
		cps = list_first_entry(&stid->sc_cp_list,
				       struct nfs4_cpntf_state, cp_list);
		_free_cpntf_state_locked(nn, cps);
	}
	spin_unlock(&nn->s2s_cp_lock);
}

static struct nfs4_ol_stateid * nfs4_alloc_open_stateid(struct nfs4_client *clp)
{
	struct nfs4_stid *stid;

	stid = nfs4_alloc_stid(clp, stateid_slab, nfs4_free_ol_stateid);
	if (!stid)
		return NULL;

	return openlockstateid(stid);
}

/*
 * As the sc_free callback of deleg, this may be called by nfs4_put_stid
 * in nfsd_break_one_deleg.
 * Considering nfsd_break_one_deleg is called with the flc->flc_lock held,
 * this function mustn't ever sleep.
 */
static void nfs4_free_deleg(struct nfs4_stid *stid)
{
	struct nfs4_delegation *dp = delegstateid(stid);

	WARN_ON_ONCE(!list_empty(&stid->sc_cp_list));
	WARN_ON_ONCE(!list_empty(&dp->dl_perfile));
	WARN_ON_ONCE(!list_empty(&dp->dl_perclnt));
	WARN_ON_ONCE(!list_empty(&dp->dl_recall_lru));
	kmem_cache_free(deleg_slab, stid);
	atomic_long_dec(&num_delegations);
}

/*
 * When we recall a delegation, we should be careful not to hand it
 * out again straight away.
 * To ensure this we keep a pair of bloom filters ('new' and 'old')
 * in which the filehandles of recalled delegations are "stored".
 * If a filehandle appear in either filter, a delegation is blocked.
 * When a delegation is recalled, the filehandle is stored in the "new"
 * filter.
 * Every 30 seconds we swap the filters and clear the "new" one,
 * unless both are empty of course.  This results in delegations for a
 * given filehandle being blocked for between 30 and 60 seconds.
 *
 * Each filter is 256 bits.  We hash the filehandle to 32bit and use the
 * low 3 bytes as hash-table indices.
 *
 * 'blocked_delegations_lock', which is always taken in block_delegations(),
 * is used to manage concurrent access.  Testing does not need the lock
 * except when swapping the two filters.
 */
static DEFINE_SPINLOCK(blocked_delegations_lock);
static struct bloom_pair {
	int	entries, old_entries;
	time64_t swap_time;
	int	new; /* index into 'set' */
	DECLARE_BITMAP(set[2], 256);
} blocked_delegations;

static int delegation_blocked(struct knfsd_fh *fh)
{
	u32 hash;
	struct bloom_pair *bd = &blocked_delegations;

	if (bd->entries == 0)
		return 0;
	if (ktime_get_seconds() - bd->swap_time > 30) {
		spin_lock(&blocked_delegations_lock);
		if (ktime_get_seconds() - bd->swap_time > 30) {
			bd->entries -= bd->old_entries;
			bd->old_entries = bd->entries;
			bd->new = 1-bd->new;
			memset(bd->set[bd->new], 0,
			       sizeof(bd->set[0]));
			bd->swap_time = ktime_get_seconds();
		}
		spin_unlock(&blocked_delegations_lock);
	}
	hash = jhash(&fh->fh_raw, fh->fh_size, 0);
	if (test_bit(hash&255, bd->set[0]) &&
	    test_bit((hash>>8)&255, bd->set[0]) &&
	    test_bit((hash>>16)&255, bd->set[0]))
		return 1;

	if (test_bit(hash&255, bd->set[1]) &&
	    test_bit((hash>>8)&255, bd->set[1]) &&
	    test_bit((hash>>16)&255, bd->set[1]))
		return 1;

	return 0;
}

static void block_delegations(struct knfsd_fh *fh)
{
	u32 hash;
	struct bloom_pair *bd = &blocked_delegations;

	hash = jhash(&fh->fh_raw, fh->fh_size, 0);

	spin_lock(&blocked_delegations_lock);
	__set_bit(hash&255, bd->set[bd->new]);
	__set_bit((hash>>8)&255, bd->set[bd->new]);
	__set_bit((hash>>16)&255, bd->set[bd->new]);
	if (bd->entries == 0)
		bd->swap_time = ktime_get_seconds();
	bd->entries += 1;
	spin_unlock(&blocked_delegations_lock);
}

static struct nfs4_delegation *
alloc_init_deleg(struct nfs4_client *clp, struct nfs4_file *fp,
		 struct nfs4_clnt_odstate *odstate, u32 dl_type)
{
	struct nfs4_delegation *dp;
	struct nfs4_stid *stid;
	long n;

	dprintk("NFSD alloc_init_deleg\n");
	n = atomic_long_inc_return(&num_delegations);
	if (n < 0 || n > max_delegations)
		goto out_dec;
	if (delegation_blocked(&fp->fi_fhandle))
		goto out_dec;
	stid = nfs4_alloc_stid(clp, deleg_slab, nfs4_free_deleg);
	if (stid == NULL)
		goto out_dec;
	dp = delegstateid(stid);

	/*
	 * delegation seqid's are never incremented.  The 4.1 special
	 * meaning of seqid 0 isn't meaningful, really, but let's avoid
	 * 0 anyway just for consistency and use 1:
	 */
	dp->dl_stid.sc_stateid.si_generation = 1;
	INIT_LIST_HEAD(&dp->dl_perfile);
	INIT_LIST_HEAD(&dp->dl_perclnt);
	INIT_LIST_HEAD(&dp->dl_recall_lru);
	dp->dl_clnt_odstate = odstate;
	get_clnt_odstate(odstate);
	dp->dl_type = dl_type;
	dp->dl_retries = 1;
	dp->dl_recalled = false;
	nfsd4_init_cb(&dp->dl_recall, dp->dl_stid.sc_client,
		      &nfsd4_cb_recall_ops, NFSPROC4_CLNT_CB_RECALL);
	nfsd4_init_cb(&dp->dl_cb_fattr.ncf_getattr, dp->dl_stid.sc_client,
			&nfsd4_cb_getattr_ops, NFSPROC4_CLNT_CB_GETATTR);
	dp->dl_cb_fattr.ncf_file_modified = false;
	get_nfs4_file(fp);
	dp->dl_stid.sc_file = fp;
	return dp;
out_dec:
	atomic_long_dec(&num_delegations);
	return NULL;
}

void
nfs4_put_stid(struct nfs4_stid *s)
{
	struct nfs4_file *fp = s->sc_file;
	struct nfs4_client *clp = s->sc_client;

	might_lock(&clp->cl_lock);

	if (!refcount_dec_and_lock(&s->sc_count, &clp->cl_lock)) {
		wake_up_all(&close_wq);
		return;
	}
	idr_remove(&clp->cl_stateids, s->sc_stateid.si_opaque.so_id);
	if (s->sc_status & SC_STATUS_ADMIN_REVOKED)
		atomic_dec(&s->sc_client->cl_admin_revoked);
	nfs4_free_cpntf_statelist(clp->net, s);
	spin_unlock(&clp->cl_lock);
	s->sc_free(s);
	if (fp)
		put_nfs4_file(fp);
}

void
nfs4_inc_and_copy_stateid(stateid_t *dst, struct nfs4_stid *stid)
{
	stateid_t *src = &stid->sc_stateid;

	spin_lock(&stid->sc_lock);
	if (unlikely(++src->si_generation == 0))
		src->si_generation = 1;
	memcpy(dst, src, sizeof(*dst));
	spin_unlock(&stid->sc_lock);
}

static void put_deleg_file(struct nfs4_file *fp)
{
	struct nfsd_file *rnf = NULL;
	struct nfsd_file *nf = NULL;

	spin_lock(&fp->fi_lock);
	if (--fp->fi_delegees == 0) {
		swap(nf, fp->fi_deleg_file);
		swap(rnf, fp->fi_rdeleg_file);
	}
	spin_unlock(&fp->fi_lock);

	if (nf)
		nfsd_file_put(nf);
	if (rnf)
		nfs4_file_put_access(fp, NFS4_SHARE_ACCESS_READ);
}

static void nfs4_unlock_deleg_lease(struct nfs4_delegation *dp)
{
	struct nfs4_file *fp = dp->dl_stid.sc_file;
	struct nfsd_file *nf = fp->fi_deleg_file;

	WARN_ON_ONCE(!fp->fi_delegees);

	kernel_setlease(nf->nf_file, F_UNLCK, NULL, (void **)&dp);
	put_deleg_file(fp);
}

static void destroy_unhashed_deleg(struct nfs4_delegation *dp)
{
	put_clnt_odstate(dp->dl_clnt_odstate);
	nfs4_unlock_deleg_lease(dp);
	nfs4_put_stid(&dp->dl_stid);
}

/**
 * nfs4_delegation_exists - Discover if this delegation already exists
 * @clp:     a pointer to the nfs4_client we're granting a delegation to
 * @fp:      a pointer to the nfs4_file we're granting a delegation on
 *
 * Return:
 *      On success: true iff an existing delegation is found
 */

static bool
nfs4_delegation_exists(struct nfs4_client *clp, struct nfs4_file *fp)
{
	struct nfs4_delegation *searchdp = NULL;
	struct nfs4_client *searchclp = NULL;

	lockdep_assert_held(&state_lock);
	lockdep_assert_held(&fp->fi_lock);

	list_for_each_entry(searchdp, &fp->fi_delegations, dl_perfile) {
		searchclp = searchdp->dl_stid.sc_client;
		if (clp == searchclp) {
			return true;
		}
	}
	return false;
}

/**
 * hash_delegation_locked - Add a delegation to the appropriate lists
 * @dp:     a pointer to the nfs4_delegation we are adding.
 * @fp:     a pointer to the nfs4_file we're granting a delegation on
 *
 * Return:
 *      On success: NULL if the delegation was successfully hashed.
 *
 *      On error: -EAGAIN if one was previously granted to this
 *                 nfs4_client for this nfs4_file. Delegation is not hashed.
 *
 */

static int
hash_delegation_locked(struct nfs4_delegation *dp, struct nfs4_file *fp)
{
	struct nfs4_client *clp = dp->dl_stid.sc_client;

	lockdep_assert_held(&state_lock);
	lockdep_assert_held(&fp->fi_lock);
	lockdep_assert_held(&clp->cl_lock);

	if (nfs4_delegation_exists(clp, fp))
		return -EAGAIN;
	refcount_inc(&dp->dl_stid.sc_count);
	dp->dl_stid.sc_type = SC_TYPE_DELEG;
	list_add(&dp->dl_perfile, &fp->fi_delegations);
	list_add(&dp->dl_perclnt, &clp->cl_delegations);
	return 0;
}

static bool delegation_hashed(struct nfs4_delegation *dp)
{
	return !(list_empty(&dp->dl_perfile));
}

static bool
unhash_delegation_locked(struct nfs4_delegation *dp, unsigned short statusmask)
{
	struct nfs4_file *fp = dp->dl_stid.sc_file;

	lockdep_assert_held(&state_lock);

	if (!delegation_hashed(dp))
		return false;

	if (statusmask == SC_STATUS_REVOKED &&
	    dp->dl_stid.sc_client->cl_minorversion == 0)
		statusmask = SC_STATUS_CLOSED;
	dp->dl_stid.sc_status |= statusmask;
	if (statusmask & SC_STATUS_ADMIN_REVOKED)
		atomic_inc(&dp->dl_stid.sc_client->cl_admin_revoked);

	/* Ensure that deleg break won't try to requeue it */
	++dp->dl_time;
	spin_lock(&fp->fi_lock);
	list_del_init(&dp->dl_perclnt);
	list_del_init(&dp->dl_recall_lru);
	list_del_init(&dp->dl_perfile);
	spin_unlock(&fp->fi_lock);
	return true;
}

static void destroy_delegation(struct nfs4_delegation *dp)
{
	bool unhashed;

	spin_lock(&state_lock);
	unhashed = unhash_delegation_locked(dp, SC_STATUS_CLOSED);
	spin_unlock(&state_lock);
	if (unhashed)
		destroy_unhashed_deleg(dp);
}

/**
 * revoke_delegation - perform nfs4 delegation structure cleanup
 * @dp: pointer to the delegation
 *
 * This function assumes that it's called either from the administrative
 * interface (nfsd4_revoke_states()) that's revoking a specific delegation
 * stateid or it's called from a laundromat thread (nfsd4_landromat()) that
 * determined that this specific state has expired and needs to be revoked
 * (both mark state with the appropriate stid sc_status mode). It is also
 * assumed that a reference was taken on the @dp state.
 *
 * If this function finds that the @dp state is SC_STATUS_FREED it means
 * that a FREE_STATEID operation for this stateid has been processed and
 * we can proceed to removing it from recalled list. However, if @dp state
 * isn't marked SC_STATUS_FREED, it means we need place it on the cl_revoked
 * list and wait for the FREE_STATEID to arrive from the client. At the same
 * time, we need to mark it as SC_STATUS_FREEABLE to indicate to the
 * nfsd4_free_stateid() function that this stateid has already been added
 * to the cl_revoked list and that nfsd4_free_stateid() is now responsible
 * for removing it from the list. Inspection of where the delegation state
 * in the revocation process is protected by the clp->cl_lock.
 */
static void revoke_delegation(struct nfs4_delegation *dp)
{
	struct nfs4_client *clp = dp->dl_stid.sc_client;

	WARN_ON(!list_empty(&dp->dl_recall_lru));
	WARN_ON_ONCE(dp->dl_stid.sc_client->cl_minorversion > 0 &&
		     !(dp->dl_stid.sc_status &
		     (SC_STATUS_REVOKED | SC_STATUS_ADMIN_REVOKED)));

	trace_nfsd_stid_revoke(&dp->dl_stid);

	spin_lock(&clp->cl_lock);
	if (dp->dl_stid.sc_status & SC_STATUS_FREED) {
		list_del_init(&dp->dl_recall_lru);
		goto out;
	}
	list_add(&dp->dl_recall_lru, &clp->cl_revoked);
	dp->dl_stid.sc_status |= SC_STATUS_FREEABLE;
out:
	spin_unlock(&clp->cl_lock);
	destroy_unhashed_deleg(dp);
}

/*
 * SETCLIENTID state
 */

static unsigned int clientid_hashval(u32 id)
{
	return id & CLIENT_HASH_MASK;
}

static unsigned int clientstr_hashval(struct xdr_netobj name)
{
	return opaque_hashval(name.data, 8) & CLIENT_HASH_MASK;
}

/*
 * A stateid that had a deny mode associated with it is being released
 * or downgraded. Recalculate the deny mode on the file.
 */
static void
recalculate_deny_mode(struct nfs4_file *fp)
{
	struct nfs4_ol_stateid *stp;
	u32 old_deny;

	spin_lock(&fp->fi_lock);
	old_deny = fp->fi_share_deny;
	fp->fi_share_deny = 0;
	list_for_each_entry(stp, &fp->fi_stateids, st_perfile) {
		fp->fi_share_deny |= bmap_to_share_mode(stp->st_deny_bmap);
		if (fp->fi_share_deny == old_deny)
			break;
	}
	spin_unlock(&fp->fi_lock);
}

static void
reset_union_bmap_deny(u32 deny, struct nfs4_ol_stateid *stp)
{
	int i;
	bool change = false;

	for (i = 1; i < 4; i++) {
		if ((i & deny) != i) {
			change = true;
			clear_deny(i, stp);
		}
	}

	/* Recalculate per-file deny mode if there was a change */
	if (change)
		recalculate_deny_mode(stp->st_stid.sc_file);
}

/* release all access and file references for a given stateid */
static void
release_all_access(struct nfs4_ol_stateid *stp)
{
	int i;
	struct nfs4_file *fp = stp->st_stid.sc_file;

	if (fp && stp->st_deny_bmap != 0)
		recalculate_deny_mode(fp);

	for (i = 1; i < 4; i++) {
		if (test_access(i, stp))
			nfs4_file_put_access(stp->st_stid.sc_file, i);
		clear_access(i, stp);
	}
}

static inline void nfs4_free_stateowner(struct nfs4_stateowner *sop)
{
	kfree(sop->so_owner.data);
	sop->so_ops->so_free(sop);
}

static void nfs4_put_stateowner(struct nfs4_stateowner *sop)
{
	struct nfs4_client *clp = sop->so_client;

	might_lock(&clp->cl_lock);

	if (!atomic_dec_and_lock(&sop->so_count, &clp->cl_lock))
		return;
	sop->so_ops->so_unhash(sop);
	spin_unlock(&clp->cl_lock);
	nfs4_free_stateowner(sop);
}

static bool
nfs4_ol_stateid_unhashed(const struct nfs4_ol_stateid *stp)
{
	return list_empty(&stp->st_perfile);
}

static bool unhash_ol_stateid(struct nfs4_ol_stateid *stp)
{
	struct nfs4_file *fp = stp->st_stid.sc_file;

	lockdep_assert_held(&stp->st_stateowner->so_client->cl_lock);

	if (list_empty(&stp->st_perfile))
		return false;

	spin_lock(&fp->fi_lock);
	list_del_init(&stp->st_perfile);
	spin_unlock(&fp->fi_lock);
	list_del(&stp->st_perstateowner);
	return true;
}

static void nfs4_free_ol_stateid(struct nfs4_stid *stid)
{
	struct nfs4_ol_stateid *stp = openlockstateid(stid);

	put_clnt_odstate(stp->st_clnt_odstate);
	release_all_access(stp);
	if (stp->st_stateowner)
		nfs4_put_stateowner(stp->st_stateowner);
	WARN_ON(!list_empty(&stid->sc_cp_list));
	kmem_cache_free(stateid_slab, stid);
}

static void nfs4_free_lock_stateid(struct nfs4_stid *stid)
{
	struct nfs4_ol_stateid *stp = openlockstateid(stid);
	struct nfs4_lockowner *lo = lockowner(stp->st_stateowner);
	struct nfsd_file *nf;

	nf = find_any_file(stp->st_stid.sc_file);
	if (nf) {
		get_file(nf->nf_file);
		filp_close(nf->nf_file, (fl_owner_t)lo);
		nfsd_file_put(nf);
	}
	nfs4_free_ol_stateid(stid);
}

/*
 * Put the persistent reference to an already unhashed generic stateid, while
 * holding the cl_lock. If it's the last reference, then put it onto the
 * reaplist for later destruction.
 */
static void put_ol_stateid_locked(struct nfs4_ol_stateid *stp,
				       struct list_head *reaplist)
{
	struct nfs4_stid *s = &stp->st_stid;
	struct nfs4_client *clp = s->sc_client;

	lockdep_assert_held(&clp->cl_lock);

	WARN_ON_ONCE(!list_empty(&stp->st_locks));

	if (!refcount_dec_and_test(&s->sc_count)) {
		wake_up_all(&close_wq);
		return;
	}

	idr_remove(&clp->cl_stateids, s->sc_stateid.si_opaque.so_id);
	if (s->sc_status & SC_STATUS_ADMIN_REVOKED)
		atomic_dec(&s->sc_client->cl_admin_revoked);
	list_add(&stp->st_locks, reaplist);
}

static bool unhash_lock_stateid(struct nfs4_ol_stateid *stp)
{
	lockdep_assert_held(&stp->st_stid.sc_client->cl_lock);

	if (!unhash_ol_stateid(stp))
		return false;
	list_del_init(&stp->st_locks);
	stp->st_stid.sc_status |= SC_STATUS_CLOSED;
	return true;
}

static void release_lock_stateid(struct nfs4_ol_stateid *stp)
{
	struct nfs4_client *clp = stp->st_stid.sc_client;
	bool unhashed;

	spin_lock(&clp->cl_lock);
	unhashed = unhash_lock_stateid(stp);
	spin_unlock(&clp->cl_lock);
	if (unhashed)
		nfs4_put_stid(&stp->st_stid);
}

static void unhash_lockowner_locked(struct nfs4_lockowner *lo)
{
	struct nfs4_client *clp = lo->lo_owner.so_client;

	lockdep_assert_held(&clp->cl_lock);

	list_del_init(&lo->lo_owner.so_strhash);
}

/*
 * Free a list of generic stateids that were collected earlier after being
 * fully unhashed.
 */
static void
free_ol_stateid_reaplist(struct list_head *reaplist)
{
	struct nfs4_ol_stateid *stp;
	struct nfs4_file *fp;

	might_sleep();

	while (!list_empty(reaplist)) {
		stp = list_first_entry(reaplist, struct nfs4_ol_stateid,
				       st_locks);
		list_del(&stp->st_locks);
		fp = stp->st_stid.sc_file;
		stp->st_stid.sc_free(&stp->st_stid);
		if (fp)
			put_nfs4_file(fp);
	}
}

static void release_open_stateid_locks(struct nfs4_ol_stateid *open_stp,
				       struct list_head *reaplist)
{
	struct nfs4_ol_stateid *stp;

	lockdep_assert_held(&open_stp->st_stid.sc_client->cl_lock);

	while (!list_empty(&open_stp->st_locks)) {
		stp = list_entry(open_stp->st_locks.next,
				struct nfs4_ol_stateid, st_locks);
		unhash_lock_stateid(stp);
		put_ol_stateid_locked(stp, reaplist);
	}
}

static bool unhash_open_stateid(struct nfs4_ol_stateid *stp,
				struct list_head *reaplist)
{
	lockdep_assert_held(&stp->st_stid.sc_client->cl_lock);

	if (!unhash_ol_stateid(stp))
		return false;
	release_open_stateid_locks(stp, reaplist);
	return true;
}

static void release_open_stateid(struct nfs4_ol_stateid *stp)
{
	LIST_HEAD(reaplist);

	spin_lock(&stp->st_stid.sc_client->cl_lock);
	stp->st_stid.sc_status |= SC_STATUS_CLOSED;
	if (unhash_open_stateid(stp, &reaplist))
		put_ol_stateid_locked(stp, &reaplist);
	spin_unlock(&stp->st_stid.sc_client->cl_lock);
	free_ol_stateid_reaplist(&reaplist);
}

static bool nfs4_openowner_unhashed(struct nfs4_openowner *oo)
{
	lockdep_assert_held(&oo->oo_owner.so_client->cl_lock);

	return list_empty(&oo->oo_owner.so_strhash) &&
		list_empty(&oo->oo_perclient);
}

static void unhash_openowner_locked(struct nfs4_openowner *oo)
{
	struct nfs4_client *clp = oo->oo_owner.so_client;

	lockdep_assert_held(&clp->cl_lock);

	list_del_init(&oo->oo_owner.so_strhash);
	list_del_init(&oo->oo_perclient);
}

static void release_last_closed_stateid(struct nfs4_openowner *oo)
{
	struct nfsd_net *nn = net_generic(oo->oo_owner.so_client->net,
					  nfsd_net_id);
	struct nfs4_ol_stateid *s;

	spin_lock(&nn->client_lock);
	s = oo->oo_last_closed_stid;
	if (s) {
		list_del_init(&oo->oo_close_lru);
		oo->oo_last_closed_stid = NULL;
	}
	spin_unlock(&nn->client_lock);
	if (s)
		nfs4_put_stid(&s->st_stid);
}

static void release_openowner(struct nfs4_openowner *oo)
{
	struct nfs4_ol_stateid *stp;
	struct nfs4_client *clp = oo->oo_owner.so_client;
	LIST_HEAD(reaplist);

	spin_lock(&clp->cl_lock);
	unhash_openowner_locked(oo);
	while (!list_empty(&oo->oo_owner.so_stateids)) {
		stp = list_first_entry(&oo->oo_owner.so_stateids,
				struct nfs4_ol_stateid, st_perstateowner);
		if (unhash_open_stateid(stp, &reaplist))
			put_ol_stateid_locked(stp, &reaplist);
	}
	spin_unlock(&clp->cl_lock);
	free_ol_stateid_reaplist(&reaplist);
	release_last_closed_stateid(oo);
	nfs4_put_stateowner(&oo->oo_owner);
}

static struct nfs4_stid *find_one_sb_stid(struct nfs4_client *clp,
					  struct super_block *sb,
					  unsigned int sc_types)
{
	unsigned long id, tmp;
	struct nfs4_stid *stid;

	spin_lock(&clp->cl_lock);
	idr_for_each_entry_ul(&clp->cl_stateids, stid, tmp, id)
		if ((stid->sc_type & sc_types) &&
		    stid->sc_status == 0 &&
		    stid->sc_file->fi_inode->i_sb == sb) {
			refcount_inc(&stid->sc_count);
			break;
		}
	spin_unlock(&clp->cl_lock);
	return stid;
}

/**
 * nfsd4_revoke_states - revoke all nfsv4 states associated with given filesystem
 * @net:  used to identify instance of nfsd (there is one per net namespace)
 * @sb:   super_block used to identify target filesystem
 *
 * All nfs4 states (open, lock, delegation, layout) held by the server instance
 * and associated with a file on the given filesystem will be revoked resulting
 * in any files being closed and so all references from nfsd to the filesystem
 * being released.  Thus nfsd will no longer prevent the filesystem from being
 * unmounted.
 *
 * The clients which own the states will subsequently being notified that the
 * states have been "admin-revoked".
 */
void nfsd4_revoke_states(struct net *net, struct super_block *sb)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	unsigned int idhashval;
	unsigned int sc_types;

	sc_types = SC_TYPE_OPEN | SC_TYPE_LOCK | SC_TYPE_DELEG | SC_TYPE_LAYOUT;

	spin_lock(&nn->client_lock);
	for (idhashval = 0; idhashval < CLIENT_HASH_MASK; idhashval++) {
		struct list_head *head = &nn->conf_id_hashtbl[idhashval];
		struct nfs4_client *clp;
	retry:
		list_for_each_entry(clp, head, cl_idhash) {
			struct nfs4_stid *stid = find_one_sb_stid(clp, sb,
								  sc_types);
			if (stid) {
				struct nfs4_ol_stateid *stp;
				struct nfs4_delegation *dp;
				struct nfs4_layout_stateid *ls;

				spin_unlock(&nn->client_lock);
				switch (stid->sc_type) {
				case SC_TYPE_OPEN:
					stp = openlockstateid(stid);
					mutex_lock_nested(&stp->st_mutex,
							  OPEN_STATEID_MUTEX);

					spin_lock(&clp->cl_lock);
					if (stid->sc_status == 0) {
						stid->sc_status |=
							SC_STATUS_ADMIN_REVOKED;
						atomic_inc(&clp->cl_admin_revoked);
						spin_unlock(&clp->cl_lock);
						release_all_access(stp);
					} else
						spin_unlock(&clp->cl_lock);
					mutex_unlock(&stp->st_mutex);
					break;
				case SC_TYPE_LOCK:
					stp = openlockstateid(stid);
					mutex_lock_nested(&stp->st_mutex,
							  LOCK_STATEID_MUTEX);
					spin_lock(&clp->cl_lock);
					if (stid->sc_status == 0) {
						struct nfs4_lockowner *lo =
							lockowner(stp->st_stateowner);
						struct nfsd_file *nf;

						stid->sc_status |=
							SC_STATUS_ADMIN_REVOKED;
						atomic_inc(&clp->cl_admin_revoked);
						spin_unlock(&clp->cl_lock);
						nf = find_any_file(stp->st_stid.sc_file);
						if (nf) {
							get_file(nf->nf_file);
							filp_close(nf->nf_file,
								   (fl_owner_t)lo);
							nfsd_file_put(nf);
						}
						release_all_access(stp);
					} else
						spin_unlock(&clp->cl_lock);
					mutex_unlock(&stp->st_mutex);
					break;
				case SC_TYPE_DELEG:
					refcount_inc(&stid->sc_count);
					dp = delegstateid(stid);
					spin_lock(&state_lock);
					if (!unhash_delegation_locked(
						    dp, SC_STATUS_ADMIN_REVOKED))
						dp = NULL;
					spin_unlock(&state_lock);
					if (dp)
						revoke_delegation(dp);
					break;
				case SC_TYPE_LAYOUT:
					ls = layoutstateid(stid);
					nfsd4_close_layout(ls);
					break;
				}
				nfs4_put_stid(stid);
				spin_lock(&nn->client_lock);
				if (clp->cl_minorversion == 0)
					/* Allow cleanup after a lease period.
					 * store_release ensures cleanup will
					 * see any newly revoked states if it
					 * sees the time updated.
					 */
					nn->nfs40_last_revoke =
						ktime_get_boottime_seconds();
				goto retry;
			}
		}
	}
	spin_unlock(&nn->client_lock);
}

static inline int
hash_sessionid(struct nfs4_sessionid *sessionid)
{
	struct nfsd4_sessionid *sid = (struct nfsd4_sessionid *)sessionid;

	return sid->sequence % SESSION_HASH_SIZE;
}

#ifdef CONFIG_SUNRPC_DEBUG
static inline void
dump_sessionid(const char *fn, struct nfs4_sessionid *sessionid)
{
	u32 *ptr = (u32 *)(&sessionid->data[0]);
	dprintk("%s: %u:%u:%u:%u\n", fn, ptr[0], ptr[1], ptr[2], ptr[3]);
}
#else
static inline void
dump_sessionid(const char *fn, struct nfs4_sessionid *sessionid)
{
}
#endif

/*
 * Bump the seqid on cstate->replay_owner, and clear replay_owner if it
 * won't be used for replay.
 */
void nfsd4_bump_seqid(struct nfsd4_compound_state *cstate, __be32 nfserr)
{
	struct nfs4_stateowner *so = cstate->replay_owner;

	if (nfserr == nfserr_replay_me)
		return;

	if (!seqid_mutating_err(ntohl(nfserr))) {
		nfsd4_cstate_clear_replay(cstate);
		return;
	}
	if (!so)
		return;
	if (so->so_is_open_owner)
		release_last_closed_stateid(openowner(so));
	so->so_seqid++;
	return;
}

static void
gen_sessionid(struct nfsd4_session *ses)
{
	struct nfs4_client *clp = ses->se_client;
	struct nfsd4_sessionid *sid;

	sid = (struct nfsd4_sessionid *)ses->se_sessionid.data;
	sid->clientid = clp->cl_clientid;
	sid->sequence = current_sessionid++;
	sid->reserved = 0;
}

/*
 * The protocol defines ca_maxresponssize_cached to include the size of
 * the rpc header, but all we need to cache is the data starting after
 * the end of the initial SEQUENCE operation--the rest we regenerate
 * each time.  Therefore we can advertise a ca_maxresponssize_cached
 * value that is the number of bytes in our cache plus a few additional
 * bytes.  In order to stay on the safe side, and not promise more than
 * we can cache, those additional bytes must be the minimum possible: 24
 * bytes of rpc header (xid through accept state, with AUTH_NULL
 * verifier), 12 for the compound header (with zero-length tag), and 44
 * for the SEQUENCE op response:
 */
#define NFSD_MIN_HDR_SEQ_SZ  (24 + 12 + 44)

static struct shrinker *nfsd_slot_shrinker;
static DEFINE_SPINLOCK(nfsd_session_list_lock);
static LIST_HEAD(nfsd_session_list);
/* The sum of "target_slots-1" on every session.  The shrinker can push this
 * down, though it can take a little while for the memory to actually
 * be freed.  The "-1" is because we can never free slot 0 while the
 * session is active.
 */
static atomic_t nfsd_total_target_slots = ATOMIC_INIT(0);

static void
free_session_slots(struct nfsd4_session *ses, int from)
{
	int i;

	if (from >= ses->se_fchannel.maxreqs)
		return;

	for (i = from; i < ses->se_fchannel.maxreqs; i++) {
		struct nfsd4_slot *slot = xa_load(&ses->se_slots, i);

		/*
		 * Save the seqid in case we reactivate this slot.
		 * This will never require a memory allocation so GFP
		 * flag is irrelevant
		 */
		xa_store(&ses->se_slots, i, xa_mk_value(slot->sl_seqid), 0);
		free_svc_cred(&slot->sl_cred);
		kfree(slot);
	}
	ses->se_fchannel.maxreqs = from;
	if (ses->se_target_maxslots > from) {
		int new_target = from ?: 1;
		atomic_sub(ses->se_target_maxslots - new_target, &nfsd_total_target_slots);
		ses->se_target_maxslots = new_target;
	}
}

/**
 * reduce_session_slots - reduce the target max-slots of a session if possible
 * @ses:  The session to affect
 * @dec:  how much to decrease the target by
 *
 * This interface can be used by a shrinker to reduce the target max-slots
 * for a session so that some slots can eventually be freed.
 * It uses spin_trylock() as it may be called in a context where another
 * spinlock is held that has a dependency on client_lock.  As shrinkers are
 * best-effort, skiping a session is client_lock is already held has no
 * great coast
 *
 * Return value:
 *   The number of slots that the target was reduced by.
 */
static int
reduce_session_slots(struct nfsd4_session *ses, int dec)
{
	struct nfsd_net *nn = net_generic(ses->se_client->net,
					  nfsd_net_id);
	int ret = 0;

	if (ses->se_target_maxslots <= 1)
		return ret;
	if (!spin_trylock(&nn->client_lock))
		return ret;
	ret = min(dec, ses->se_target_maxslots-1);
	ses->se_target_maxslots -= ret;
	atomic_sub(ret, &nfsd_total_target_slots);
	ses->se_slot_gen += 1;
	if (ses->se_slot_gen == 0) {
		int i;
		ses->se_slot_gen = 1;
		for (i = 0; i < ses->se_fchannel.maxreqs; i++) {
			struct nfsd4_slot *slot = xa_load(&ses->se_slots, i);
			slot->sl_generation = 0;
		}
	}
	spin_unlock(&nn->client_lock);
	return ret;
}

static struct nfsd4_slot *nfsd4_alloc_slot(struct nfsd4_channel_attrs *fattrs,
					   int index, gfp_t gfp)
{
	struct nfsd4_slot *slot;
	size_t size;

	/*
	 * The RPC and NFS session headers are never saved in
	 * the slot reply cache buffer.
	 */
	size = fattrs->maxresp_cached < NFSD_MIN_HDR_SEQ_SZ ?
		0 : fattrs->maxresp_cached - NFSD_MIN_HDR_SEQ_SZ;

	slot = kzalloc(struct_size(slot, sl_data, size), gfp);
	if (!slot)
		return NULL;
	slot->sl_index = index;
	return slot;
}

static struct nfsd4_session *alloc_session(struct nfsd4_channel_attrs *fattrs,
					   struct nfsd4_channel_attrs *battrs)
{
	int numslots = fattrs->maxreqs;
	struct nfsd4_session *new;
	struct nfsd4_slot *slot;
	int i;

	new = kzalloc(sizeof(*new), GFP_KERNEL);
	if (!new)
		return NULL;
	xa_init(&new->se_slots);

	slot = nfsd4_alloc_slot(fattrs, 0, GFP_KERNEL);
	if (!slot || xa_is_err(xa_store(&new->se_slots, 0, slot, GFP_KERNEL)))
		goto out_free;

	for (i = 1; i < numslots; i++) {
		const gfp_t gfp = GFP_KERNEL | __GFP_NORETRY | __GFP_NOWARN;
		slot = nfsd4_alloc_slot(fattrs, i, gfp);
		if (!slot)
			break;
		if (xa_is_err(xa_store(&new->se_slots, i, slot, gfp))) {
			kfree(slot);
			break;
		}
	}
	fattrs->maxreqs = i;
	memcpy(&new->se_fchannel, fattrs, sizeof(struct nfsd4_channel_attrs));
	new->se_target_maxslots = i;
	atomic_add(i - 1, &nfsd_total_target_slots);
	new->se_cb_slot_avail = ~0U;
	new->se_cb_highest_slot = min(battrs->maxreqs - 1,
				      NFSD_BC_SLOT_TABLE_SIZE - 1);
	spin_lock_init(&new->se_lock);
	return new;
out_free:
	kfree(slot);
	xa_destroy(&new->se_slots);
	kfree(new);
	return NULL;
}

static void free_conn(struct nfsd4_conn *c)
{
	svc_xprt_put(c->cn_xprt);
	kfree(c);
}

static void nfsd4_conn_lost(struct svc_xpt_user *u)
{
	struct nfsd4_conn *c = container_of(u, struct nfsd4_conn, cn_xpt_user);
	struct nfs4_client *clp = c->cn_session->se_client;

	trace_nfsd_cb_lost(clp);

	spin_lock(&clp->cl_lock);
	if (!list_empty(&c->cn_persession)) {
		list_del(&c->cn_persession);
		free_conn(c);
	}
	nfsd4_probe_callback(clp);
	spin_unlock(&clp->cl_lock);
}

static struct nfsd4_conn *alloc_conn(struct svc_rqst *rqstp, u32 flags)
{
	struct nfsd4_conn *conn;

	conn = kmalloc(sizeof(struct nfsd4_conn), GFP_KERNEL);
	if (!conn)
		return NULL;
	svc_xprt_get(rqstp->rq_xprt);
	conn->cn_xprt = rqstp->rq_xprt;
	conn->cn_flags = flags;
	INIT_LIST_HEAD(&conn->cn_xpt_user.list);
	return conn;
}

static void __nfsd4_hash_conn(struct nfsd4_conn *conn, struct nfsd4_session *ses)
{
	conn->cn_session = ses;
	list_add(&conn->cn_persession, &ses->se_conns);
}

static void nfsd4_hash_conn(struct nfsd4_conn *conn, struct nfsd4_session *ses)
{
	struct nfs4_client *clp = ses->se_client;

	spin_lock(&clp->cl_lock);
	__nfsd4_hash_conn(conn, ses);
	spin_unlock(&clp->cl_lock);
}

static int nfsd4_register_conn(struct nfsd4_conn *conn)
{
	conn->cn_xpt_user.callback = nfsd4_conn_lost;
	return register_xpt_user(conn->cn_xprt, &conn->cn_xpt_user);
}

static void nfsd4_init_conn(struct svc_rqst *rqstp, struct nfsd4_conn *conn, struct nfsd4_session *ses)
{
	int ret;

	nfsd4_hash_conn(conn, ses);
	ret = nfsd4_register_conn(conn);
	if (ret)
		/* oops; xprt is already down: */
		nfsd4_conn_lost(&conn->cn_xpt_user);
	/* We may have gained or lost a callback channel: */
	nfsd4_probe_callback_sync(ses->se_client);
}

static struct nfsd4_conn *alloc_conn_from_crses(struct svc_rqst *rqstp, struct nfsd4_create_session *cses)
{
	u32 dir = NFS4_CDFC4_FORE;

	if (cses->flags & SESSION4_BACK_CHAN)
		dir |= NFS4_CDFC4_BACK;
	return alloc_conn(rqstp, dir);
}

/* must be called under client_lock */
static void nfsd4_del_conns(struct nfsd4_session *s)
{
	struct nfs4_client *clp = s->se_client;
	struct nfsd4_conn *c;

	spin_lock(&clp->cl_lock);
	while (!list_empty(&s->se_conns)) {
		c = list_first_entry(&s->se_conns, struct nfsd4_conn, cn_persession);
		list_del_init(&c->cn_persession);
		spin_unlock(&clp->cl_lock);

		unregister_xpt_user(c->cn_xprt, &c->cn_xpt_user);
		free_conn(c);

		spin_lock(&clp->cl_lock);
	}
	spin_unlock(&clp->cl_lock);
}

static void __free_session(struct nfsd4_session *ses)
{
	free_session_slots(ses, 0);
	xa_destroy(&ses->se_slots);
	kfree(ses);
}

static void free_session(struct nfsd4_session *ses)
{
	nfsd4_del_conns(ses);
	__free_session(ses);
}

static unsigned long
nfsd_slot_count(struct shrinker *s, struct shrink_control *sc)
{
	unsigned long cnt = atomic_read(&nfsd_total_target_slots);

	return cnt ? cnt : SHRINK_EMPTY;
}

static unsigned long
nfsd_slot_scan(struct shrinker *s, struct shrink_control *sc)
{
	struct nfsd4_session *ses;
	unsigned long scanned = 0;
	unsigned long freed = 0;

	spin_lock(&nfsd_session_list_lock);
	list_for_each_entry(ses, &nfsd_session_list, se_all_sessions) {
		freed += reduce_session_slots(ses, 1);
		scanned += 1;
		if (scanned >= sc->nr_to_scan) {
			/* Move starting point for next scan */
			list_move(&nfsd_session_list, &ses->se_all_sessions);
			break;
		}
	}
	spin_unlock(&nfsd_session_list_lock);
	sc->nr_scanned = scanned;
	return freed;
}

static void init_session(struct svc_rqst *rqstp, struct nfsd4_session *new, struct nfs4_client *clp, struct nfsd4_create_session *cses)
{
	int idx;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	new->se_client = clp;
	gen_sessionid(new);

	INIT_LIST_HEAD(&new->se_conns);

	atomic_set(&new->se_ref, 0);
	new->se_dead = false;
	new->se_cb_prog = cses->callback_prog;
	new->se_cb_sec = cses->cb_sec;

	for (idx = 0; idx < NFSD_BC_SLOT_TABLE_SIZE; ++idx)
		new->se_cb_seq_nr[idx] = 1;

	idx = hash_sessionid(&new->se_sessionid);
	list_add(&new->se_hash, &nn->sessionid_hashtbl[idx]);
	spin_lock(&clp->cl_lock);
	list_add(&new->se_perclnt, &clp->cl_sessions);
	spin_unlock(&clp->cl_lock);

	spin_lock(&nfsd_session_list_lock);
	list_add_tail(&new->se_all_sessions, &nfsd_session_list);
	spin_unlock(&nfsd_session_list_lock);

	{
		struct sockaddr *sa = svc_addr(rqstp);
		/*
		 * This is a little silly; with sessions there's no real
		 * use for the callback address.  Use the peer address
		 * as a reasonable default for now, but consider fixing
		 * the rpc client not to require an address in the
		 * future:
		 */
		rpc_copy_addr((struct sockaddr *)&clp->cl_cb_conn.cb_addr, sa);
		clp->cl_cb_conn.cb_addrlen = svc_addr_len(sa);
	}
}

/* caller must hold client_lock */
static struct nfsd4_session *
__find_in_sessionid_hashtbl(struct nfs4_sessionid *sessionid, struct net *net)
{
	struct nfsd4_session *elem;
	int idx;
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	lockdep_assert_held(&nn->client_lock);

	dump_sessionid(__func__, sessionid);
	idx = hash_sessionid(sessionid);
	/* Search in the appropriate list */
	list_for_each_entry(elem, &nn->sessionid_hashtbl[idx], se_hash) {
		if (!memcmp(elem->se_sessionid.data, sessionid->data,
			    NFS4_MAX_SESSIONID_LEN)) {
			return elem;
		}
	}

	dprintk("%s: session not found\n", __func__);
	return NULL;
}

static struct nfsd4_session *
find_in_sessionid_hashtbl(struct nfs4_sessionid *sessionid, struct net *net,
		__be32 *ret)
{
	struct nfsd4_session *session;
	__be32 status = nfserr_badsession;

	session = __find_in_sessionid_hashtbl(sessionid, net);
	if (!session)
		goto out;
	status = nfsd4_get_session_locked(session);
	if (status)
		session = NULL;
out:
	*ret = status;
	return session;
}

/* caller must hold client_lock */
static void
unhash_session(struct nfsd4_session *ses)
{
	struct nfs4_client *clp = ses->se_client;
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	lockdep_assert_held(&nn->client_lock);

	list_del(&ses->se_hash);
	spin_lock(&ses->se_client->cl_lock);
	list_del(&ses->se_perclnt);
	spin_unlock(&ses->se_client->cl_lock);
	spin_lock(&nfsd_session_list_lock);
	list_del(&ses->se_all_sessions);
	spin_unlock(&nfsd_session_list_lock);
}

/* SETCLIENTID and SETCLIENTID_CONFIRM Helper functions */
static int
STALE_CLIENTID(clientid_t *clid, struct nfsd_net *nn)
{
	/*
	 * We're assuming the clid was not given out from a boot
	 * precisely 2^32 (about 136 years) before this one.  That seems
	 * a safe assumption:
	 */
	if (clid->cl_boot == (u32)nn->boot_time)
		return 0;
	trace_nfsd_clid_stale(clid);
	return 1;
}

static struct nfs4_client *alloc_client(struct xdr_netobj name,
				struct nfsd_net *nn)
{
	struct nfs4_client *clp;
	int i;

	if (atomic_read(&nn->nfs4_client_count) >= nn->nfs4_max_clients &&
	    atomic_read(&nn->nfsd_courtesy_clients) > 0)
		mod_delayed_work(laundry_wq, &nn->laundromat_work, 0);

	clp = kmem_cache_zalloc(client_slab, GFP_KERNEL);
	if (clp == NULL)
		return NULL;
	xdr_netobj_dup(&clp->cl_name, &name, GFP_KERNEL);
	if (clp->cl_name.data == NULL)
		goto err_no_name;
	clp->cl_ownerstr_hashtbl = kmalloc_array(OWNER_HASH_SIZE,
						 sizeof(struct list_head),
						 GFP_KERNEL);
	if (!clp->cl_ownerstr_hashtbl)
		goto err_no_hashtbl;
	clp->cl_callback_wq = alloc_ordered_workqueue("nfsd4_callbacks", 0);
	if (!clp->cl_callback_wq)
		goto err_no_callback_wq;

	for (i = 0; i < OWNER_HASH_SIZE; i++)
		INIT_LIST_HEAD(&clp->cl_ownerstr_hashtbl[i]);
	INIT_LIST_HEAD(&clp->cl_sessions);
	idr_init(&clp->cl_stateids);
	atomic_set(&clp->cl_rpc_users, 0);
	clp->cl_cb_state = NFSD4_CB_UNKNOWN;
	clp->cl_state = NFSD4_ACTIVE;
	atomic_inc(&nn->nfs4_client_count);
	atomic_set(&clp->cl_delegs_in_recall, 0);
	INIT_LIST_HEAD(&clp->cl_idhash);
	INIT_LIST_HEAD(&clp->cl_openowners);
	INIT_LIST_HEAD(&clp->cl_delegations);
	INIT_LIST_HEAD(&clp->cl_lru);
	INIT_LIST_HEAD(&clp->cl_revoked);
#ifdef CONFIG_NFSD_PNFS
	INIT_LIST_HEAD(&clp->cl_lo_states);
#endif
	INIT_LIST_HEAD(&clp->async_copies);
	spin_lock_init(&clp->async_lock);
	spin_lock_init(&clp->cl_lock);
	rpc_init_wait_queue(&clp->cl_cb_waitq, "Backchannel slot table");
	return clp;
err_no_callback_wq:
	kfree(clp->cl_ownerstr_hashtbl);
err_no_hashtbl:
	kfree(clp->cl_name.data);
err_no_name:
	kmem_cache_free(client_slab, clp);
	return NULL;
}

static void __free_client(struct kref *k)
{
	struct nfsdfs_client *c = container_of(k, struct nfsdfs_client, cl_ref);
	struct nfs4_client *clp = container_of(c, struct nfs4_client, cl_nfsdfs);

	free_svc_cred(&clp->cl_cred);
	destroy_workqueue(clp->cl_callback_wq);
	kfree(clp->cl_ownerstr_hashtbl);
	kfree(clp->cl_name.data);
	kfree(clp->cl_nii_domain.data);
	kfree(clp->cl_nii_name.data);
	idr_destroy(&clp->cl_stateids);
	kfree(clp->cl_ra);
	kmem_cache_free(client_slab, clp);
}

static void drop_client(struct nfs4_client *clp)
{
	kref_put(&clp->cl_nfsdfs.cl_ref, __free_client);
}

static void
free_client(struct nfs4_client *clp)
{
	while (!list_empty(&clp->cl_sessions)) {
		struct nfsd4_session *ses;
		ses = list_entry(clp->cl_sessions.next, struct nfsd4_session,
				se_perclnt);
		list_del(&ses->se_perclnt);
		WARN_ON_ONCE(atomic_read(&ses->se_ref));
		free_session(ses);
	}
	rpc_destroy_wait_queue(&clp->cl_cb_waitq);
	if (clp->cl_nfsd_dentry) {
		nfsd_client_rmdir(clp->cl_nfsd_dentry);
		clp->cl_nfsd_dentry = NULL;
		wake_up_all(&expiry_wq);
	}
	drop_client(clp);
}

/* must be called under the client_lock */
static void
unhash_client_locked(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);
	struct nfsd4_session *ses;

	lockdep_assert_held(&nn->client_lock);

	/* Mark the client as expired! */
	clp->cl_time = 0;
	/* Make it invisible */
	if (!list_empty(&clp->cl_idhash)) {
		list_del_init(&clp->cl_idhash);
		if (test_bit(NFSD4_CLIENT_CONFIRMED, &clp->cl_flags))
			rb_erase(&clp->cl_namenode, &nn->conf_name_tree);
		else
			rb_erase(&clp->cl_namenode, &nn->unconf_name_tree);
	}
	list_del_init(&clp->cl_lru);
	spin_lock(&clp->cl_lock);
	spin_lock(&nfsd_session_list_lock);
	list_for_each_entry(ses, &clp->cl_sessions, se_perclnt) {
		list_del_init(&ses->se_hash);
		list_del_init(&ses->se_all_sessions);
	}
	spin_unlock(&nfsd_session_list_lock);
	spin_unlock(&clp->cl_lock);
}

static void
unhash_client(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	spin_lock(&nn->client_lock);
	unhash_client_locked(clp);
	spin_unlock(&nn->client_lock);
}

static __be32 mark_client_expired_locked(struct nfs4_client *clp)
{
	int users = atomic_read(&clp->cl_rpc_users);

	trace_nfsd_mark_client_expired(clp, users);

	if (users)
		return nfserr_jukebox;
	unhash_client_locked(clp);
	return nfs_ok;
}

static void
__destroy_client(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);
	int i;
	struct nfs4_openowner *oo;
	struct nfs4_delegation *dp;
	LIST_HEAD(reaplist);

	spin_lock(&state_lock);
	while (!list_empty(&clp->cl_delegations)) {
		dp = list_entry(clp->cl_delegations.next, struct nfs4_delegation, dl_perclnt);
		unhash_delegation_locked(dp, SC_STATUS_CLOSED);
		list_add(&dp->dl_recall_lru, &reaplist);
	}
	spin_unlock(&state_lock);
	while (!list_empty(&reaplist)) {
		dp = list_entry(reaplist.next, struct nfs4_delegation, dl_recall_lru);
		list_del_init(&dp->dl_recall_lru);
		destroy_unhashed_deleg(dp);
	}
	while (!list_empty(&clp->cl_revoked)) {
		dp = list_entry(clp->cl_revoked.next, struct nfs4_delegation, dl_recall_lru);
		list_del_init(&dp->dl_recall_lru);
		nfs4_put_stid(&dp->dl_stid);
	}
	while (!list_empty(&clp->cl_openowners)) {
		oo = list_entry(clp->cl_openowners.next, struct nfs4_openowner, oo_perclient);
		nfs4_get_stateowner(&oo->oo_owner);
		release_openowner(oo);
	}
	for (i = 0; i < OWNER_HASH_SIZE; i++) {
		struct nfs4_stateowner *so, *tmp;

		list_for_each_entry_safe(so, tmp, &clp->cl_ownerstr_hashtbl[i],
					 so_strhash) {
			/* Should be no openowners at this point */
			WARN_ON_ONCE(so->so_is_open_owner);
			remove_blocked_locks(lockowner(so));
		}
	}
	nfsd4_return_all_client_layouts(clp);
	nfsd4_shutdown_copy(clp);
	nfsd4_shutdown_callback(clp);
	if (clp->cl_cb_conn.cb_xprt)
		svc_xprt_put(clp->cl_cb_conn.cb_xprt);
	atomic_add_unless(&nn->nfs4_client_count, -1, 0);
	nfsd4_dec_courtesy_client_count(nn, clp);
	free_client(clp);
	wake_up_all(&expiry_wq);
}

static void
destroy_client(struct nfs4_client *clp)
{
	unhash_client(clp);
	__destroy_client(clp);
}

static void inc_reclaim_complete(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	if (!nn->track_reclaim_completes)
		return;
	if (!nfsd4_find_reclaim_client(clp->cl_name, nn))
		return;
	if (atomic_inc_return(&nn->nr_reclaim_complete) ==
			nn->reclaim_str_hashtbl_size) {
		printk(KERN_INFO "NFSD: all clients done reclaiming, ending NFSv4 grace period (net %x)\n",
				clp->net->ns.inum);
		nfsd4_end_grace(nn);
	}
}

static void expire_client(struct nfs4_client *clp)
{
	unhash_client(clp);
	nfsd4_client_record_remove(clp);
	__destroy_client(clp);
}

static void copy_verf(struct nfs4_client *target, nfs4_verifier *source)
{
	memcpy(target->cl_verifier.data, source->data,
			sizeof(target->cl_verifier.data));
}

static void copy_clid(struct nfs4_client *target, struct nfs4_client *source)
{
	target->cl_clientid.cl_boot = source->cl_clientid.cl_boot; 
	target->cl_clientid.cl_id = source->cl_clientid.cl_id; 
}

static int copy_cred(struct svc_cred *target, struct svc_cred *source)
{
	target->cr_principal = kstrdup(source->cr_principal, GFP_KERNEL);
	target->cr_raw_principal = kstrdup(source->cr_raw_principal,
								GFP_KERNEL);
	target->cr_targ_princ = kstrdup(source->cr_targ_princ, GFP_KERNEL);
	if ((source->cr_principal && !target->cr_principal) ||
	    (source->cr_raw_principal && !target->cr_raw_principal) ||
	    (source->cr_targ_princ && !target->cr_targ_princ))
		return -ENOMEM;

	target->cr_flavor = source->cr_flavor;
	target->cr_uid = source->cr_uid;
	target->cr_gid = source->cr_gid;
	target->cr_group_info = source->cr_group_info;
	get_group_info(target->cr_group_info);
	target->cr_gss_mech = source->cr_gss_mech;
	if (source->cr_gss_mech)
		gss_mech_get(source->cr_gss_mech);
	return 0;
}

static int
compare_blob(const struct xdr_netobj *o1, const struct xdr_netobj *o2)
{
	if (o1->len < o2->len)
		return -1;
	if (o1->len > o2->len)
		return 1;
	return memcmp(o1->data, o2->data, o1->len);
}

static int
same_verf(nfs4_verifier *v1, nfs4_verifier *v2)
{
	return 0 == memcmp(v1->data, v2->data, sizeof(v1->data));
}

static int
same_clid(clientid_t *cl1, clientid_t *cl2)
{
	return (cl1->cl_boot == cl2->cl_boot) && (cl1->cl_id == cl2->cl_id);
}

static bool groups_equal(struct group_info *g1, struct group_info *g2)
{
	int i;

	if (g1->ngroups != g2->ngroups)
		return false;
	for (i=0; i<g1->ngroups; i++)
		if (!gid_eq(g1->gid[i], g2->gid[i]))
			return false;
	return true;
}

/*
 * RFC 3530 language requires clid_inuse be returned when the
 * "principal" associated with a requests differs from that previously
 * used.  We use uid, gid's, and gss principal string as our best
 * approximation.  We also don't want to allow non-gss use of a client
 * established using gss: in theory cr_principal should catch that
 * change, but in practice cr_principal can be null even in the gss case
 * since gssd doesn't always pass down a principal string.
 */
static bool is_gss_cred(struct svc_cred *cr)
{
	/* Is cr_flavor one of the gss "pseudoflavors"?: */
	return (cr->cr_flavor > RPC_AUTH_MAXFLAVOR);
}


static bool
same_creds(struct svc_cred *cr1, struct svc_cred *cr2)
{
	if ((is_gss_cred(cr1) != is_gss_cred(cr2))
		|| (!uid_eq(cr1->cr_uid, cr2->cr_uid))
		|| (!gid_eq(cr1->cr_gid, cr2->cr_gid))
		|| !groups_equal(cr1->cr_group_info, cr2->cr_group_info))
		return false;
	/* XXX: check that cr_targ_princ fields match ? */
	if (cr1->cr_principal == cr2->cr_principal)
		return true;
	if (!cr1->cr_principal || !cr2->cr_principal)
		return false;
	return 0 == strcmp(cr1->cr_principal, cr2->cr_principal);
}

static bool svc_rqst_integrity_protected(struct svc_rqst *rqstp)
{
	struct svc_cred *cr = &rqstp->rq_cred;
	u32 service;

	if (!cr->cr_gss_mech)
		return false;
	service = gss_pseudoflavor_to_service(cr->cr_gss_mech, cr->cr_flavor);
	return service == RPC_GSS_SVC_INTEGRITY ||
	       service == RPC_GSS_SVC_PRIVACY;
}

bool nfsd4_mach_creds_match(struct nfs4_client *cl, struct svc_rqst *rqstp)
{
	struct svc_cred *cr = &rqstp->rq_cred;

	if (!cl->cl_mach_cred)
		return true;
	if (cl->cl_cred.cr_gss_mech != cr->cr_gss_mech)
		return false;
	if (!svc_rqst_integrity_protected(rqstp))
		return false;
	if (cl->cl_cred.cr_raw_principal)
		return 0 == strcmp(cl->cl_cred.cr_raw_principal,
						cr->cr_raw_principal);
	if (!cr->cr_principal)
		return false;
	return 0 == strcmp(cl->cl_cred.cr_principal, cr->cr_principal);
}

static void gen_confirm(struct nfs4_client *clp, struct nfsd_net *nn)
{
	__be32 verf[2];

	/*
	 * This is opaque to client, so no need to byte-swap. Use
	 * __force to keep sparse happy
	 */
	verf[0] = (__force __be32)(u32)ktime_get_real_seconds();
	verf[1] = (__force __be32)nn->clverifier_counter++;
	memcpy(clp->cl_confirm.data, verf, sizeof(clp->cl_confirm.data));
}

static void gen_clid(struct nfs4_client *clp, struct nfsd_net *nn)
{
	clp->cl_clientid.cl_boot = (u32)nn->boot_time;
	clp->cl_clientid.cl_id = nn->clientid_counter++;
	gen_confirm(clp, nn);
}

static struct nfs4_stid *
find_stateid_locked(struct nfs4_client *cl, stateid_t *t)
{
	struct nfs4_stid *ret;

	ret = idr_find(&cl->cl_stateids, t->si_opaque.so_id);
	if (!ret || !ret->sc_type)
		return NULL;
	return ret;
}

static struct nfs4_stid *
find_stateid_by_type(struct nfs4_client *cl, stateid_t *t,
		     unsigned short typemask, unsigned short ok_states)
{
	struct nfs4_stid *s;

	spin_lock(&cl->cl_lock);
	s = find_stateid_locked(cl, t);
	if (s != NULL) {
		if ((s->sc_status & ~ok_states) == 0 &&
		    (typemask & s->sc_type))
			refcount_inc(&s->sc_count);
		else
			s = NULL;
	}
	spin_unlock(&cl->cl_lock);
	return s;
}

static struct nfs4_client *get_nfsdfs_clp(struct inode *inode)
{
	struct nfsdfs_client *nc;
	nc = get_nfsdfs_client(inode);
	if (!nc)
		return NULL;
	return container_of(nc, struct nfs4_client, cl_nfsdfs);
}

static void seq_quote_mem(struct seq_file *m, char *data, int len)
{
	seq_puts(m, "\"");
	seq_escape_mem(m, data, len, ESCAPE_HEX | ESCAPE_NAP | ESCAPE_APPEND, "\"\\");
	seq_puts(m, "\"");
}

static const char *cb_state2str(int state)
{
	switch (state) {
	case NFSD4_CB_UP:
		return "UP";
	case NFSD4_CB_UNKNOWN:
		return "UNKNOWN";
	case NFSD4_CB_DOWN:
		return "DOWN";
	case NFSD4_CB_FAULT:
		return "FAULT";
	}
	return "UNDEFINED";
}

static int client_info_show(struct seq_file *m, void *v)
{
	struct inode *inode = file_inode(m->file);
	struct nfsd4_session *ses;
	struct nfs4_client *clp;
	u64 clid;

	clp = get_nfsdfs_clp(inode);
	if (!clp)
		return -ENXIO;
	memcpy(&clid, &clp->cl_clientid, sizeof(clid));
	seq_printf(m, "clientid: 0x%llx\n", clid);
	seq_printf(m, "address: \"%pISpc\"\n", (struct sockaddr *)&clp->cl_addr);

	if (clp->cl_state == NFSD4_COURTESY)
		seq_puts(m, "status: courtesy\n");
	else if (clp->cl_state == NFSD4_EXPIRABLE)
		seq_puts(m, "status: expirable\n");
	else if (test_bit(NFSD4_CLIENT_CONFIRMED, &clp->cl_flags))
		seq_puts(m, "status: confirmed\n");
	else
		seq_puts(m, "status: unconfirmed\n");
	seq_printf(m, "seconds from last renew: %lld\n",
		ktime_get_boottime_seconds() - clp->cl_time);
	seq_puts(m, "name: ");
	seq_quote_mem(m, clp->cl_name.data, clp->cl_name.len);
	seq_printf(m, "\nminor version: %d\n", clp->cl_minorversion);
	if (clp->cl_nii_domain.data) {
		seq_puts(m, "Implementation domain: ");
		seq_quote_mem(m, clp->cl_nii_domain.data,
					clp->cl_nii_domain.len);
		seq_puts(m, "\nImplementation name: ");
		seq_quote_mem(m, clp->cl_nii_name.data, clp->cl_nii_name.len);
		seq_printf(m, "\nImplementation time: [%lld, %ld]\n",
			clp->cl_nii_time.tv_sec, clp->cl_nii_time.tv_nsec);
	}
	seq_printf(m, "callback state: %s\n", cb_state2str(clp->cl_cb_state));
	seq_printf(m, "callback address: \"%pISpc\"\n", &clp->cl_cb_conn.cb_addr);
	seq_printf(m, "admin-revoked states: %d\n",
		   atomic_read(&clp->cl_admin_revoked));
	spin_lock(&clp->cl_lock);
	seq_printf(m, "session slots:");
	list_for_each_entry(ses, &clp->cl_sessions, se_perclnt)
		seq_printf(m, " %u", ses->se_fchannel.maxreqs);
	seq_printf(m, "\nsession target slots:");
	list_for_each_entry(ses, &clp->cl_sessions, se_perclnt)
		seq_printf(m, " %u", ses->se_target_maxslots);
	spin_unlock(&clp->cl_lock);
	seq_puts(m, "\n");

	drop_client(clp);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(client_info);

static void *states_start(struct seq_file *s, loff_t *pos)
	__acquires(&clp->cl_lock)
{
	struct nfs4_client *clp = s->private;
	unsigned long id = *pos;
	void *ret;

	spin_lock(&clp->cl_lock);
	ret = idr_get_next_ul(&clp->cl_stateids, &id);
	*pos = id;
	return ret;
}

static void *states_next(struct seq_file *s, void *v, loff_t *pos)
{
	struct nfs4_client *clp = s->private;
	unsigned long id = *pos;
	void *ret;

	id = *pos;
	id++;
	ret = idr_get_next_ul(&clp->cl_stateids, &id);
	*pos = id;
	return ret;
}

static void states_stop(struct seq_file *s, void *v)
	__releases(&clp->cl_lock)
{
	struct nfs4_client *clp = s->private;

	spin_unlock(&clp->cl_lock);
}

static void nfs4_show_fname(struct seq_file *s, struct nfsd_file *f)
{
         seq_printf(s, "filename: \"%pD2\"", f->nf_file);
}

static void nfs4_show_superblock(struct seq_file *s, struct nfsd_file *f)
{
	struct inode *inode = file_inode(f->nf_file);

	seq_printf(s, "superblock: \"%02x:%02x:%ld\"",
					MAJOR(inode->i_sb->s_dev),
					 MINOR(inode->i_sb->s_dev),
					 inode->i_ino);
}

static void nfs4_show_owner(struct seq_file *s, struct nfs4_stateowner *oo)
{
	seq_puts(s, "owner: ");
	seq_quote_mem(s, oo->so_owner.data, oo->so_owner.len);
}

static void nfs4_show_stateid(struct seq_file *s, stateid_t *stid)
{
	seq_printf(s, "0x%.8x", stid->si_generation);
	seq_printf(s, "%12phN", &stid->si_opaque);
}

static int nfs4_show_open(struct seq_file *s, struct nfs4_stid *st)
{
	struct nfs4_ol_stateid *ols;
	struct nfs4_file *nf;
	struct nfsd_file *file;
	struct nfs4_stateowner *oo;
	unsigned int access, deny;

	ols = openlockstateid(st);
	oo = ols->st_stateowner;
	nf = st->sc_file;

	seq_puts(s, "- ");
	nfs4_show_stateid(s, &st->sc_stateid);
	seq_puts(s, ": { type: open, ");

	access = bmap_to_share_mode(ols->st_access_bmap);
	deny   = bmap_to_share_mode(ols->st_deny_bmap);

	seq_printf(s, "access: %s%s, ",
		access & NFS4_SHARE_ACCESS_READ ? "r" : "-",
		access & NFS4_SHARE_ACCESS_WRITE ? "w" : "-");
	seq_printf(s, "deny: %s%s, ",
		deny & NFS4_SHARE_ACCESS_READ ? "r" : "-",
		deny & NFS4_SHARE_ACCESS_WRITE ? "w" : "-");

	if (nf) {
		spin_lock(&nf->fi_lock);
		file = find_any_file_locked(nf);
		if (file) {
			nfs4_show_superblock(s, file);
			seq_puts(s, ", ");
			nfs4_show_fname(s, file);
			seq_puts(s, ", ");
		}
		spin_unlock(&nf->fi_lock);
	} else
		seq_puts(s, "closed, ");
	nfs4_show_owner(s, oo);
	if (st->sc_status & SC_STATUS_ADMIN_REVOKED)
		seq_puts(s, ", admin-revoked");
	seq_puts(s, " }\n");
	return 0;
}

static int nfs4_show_lock(struct seq_file *s, struct nfs4_stid *st)
{
	struct nfs4_ol_stateid *ols;
	struct nfs4_file *nf;
	struct nfsd_file *file;
	struct nfs4_stateowner *oo;

	ols = openlockstateid(st);
	oo = ols->st_stateowner;
	nf = st->sc_file;

	seq_puts(s, "- ");
	nfs4_show_stateid(s, &st->sc_stateid);
	seq_puts(s, ": { type: lock, ");

	spin_lock(&nf->fi_lock);
	file = find_any_file_locked(nf);
	if (file) {
		/*
		 * Note: a lock stateid isn't really the same thing as a lock,
		 * it's the locking state held by one owner on a file, and there
		 * may be multiple (or no) lock ranges associated with it.
		 * (Same for the matter is true of open stateids.)
		 */

		nfs4_show_superblock(s, file);
		/* XXX: open stateid? */
		seq_puts(s, ", ");
		nfs4_show_fname(s, file);
		seq_puts(s, ", ");
	}
	nfs4_show_owner(s, oo);
	if (st->sc_status & SC_STATUS_ADMIN_REVOKED)
		seq_puts(s, ", admin-revoked");
	seq_puts(s, " }\n");
	spin_unlock(&nf->fi_lock);
	return 0;
}

static char *nfs4_show_deleg_type(u32 dl_type)
{
	switch (dl_type) {
	case OPEN_DELEGATE_READ:
		return "r";
	case OPEN_DELEGATE_WRITE:
		return "w";
	case OPEN_DELEGATE_READ_ATTRS_DELEG:
		return "ra";
	case OPEN_DELEGATE_WRITE_ATTRS_DELEG:
		return "wa";
	}
	return "?";
}

static int nfs4_show_deleg(struct seq_file *s, struct nfs4_stid *st)
{
	struct nfs4_delegation *ds;
	struct nfs4_file *nf;
	struct nfsd_file *file;

	ds = delegstateid(st);
	nf = st->sc_file;

	seq_puts(s, "- ");
	nfs4_show_stateid(s, &st->sc_stateid);
	seq_puts(s, ": { type: deleg, ");

	seq_printf(s, "access: %s", nfs4_show_deleg_type(ds->dl_type));

	/* XXX: lease time, whether it's being recalled. */

	spin_lock(&nf->fi_lock);
	file = nf->fi_deleg_file;
	if (file) {
		seq_puts(s, ", ");
		nfs4_show_superblock(s, file);
		seq_puts(s, ", ");
		nfs4_show_fname(s, file);
	}
	spin_unlock(&nf->fi_lock);
	if (st->sc_status & SC_STATUS_ADMIN_REVOKED)
		seq_puts(s, ", admin-revoked");
	seq_puts(s, " }\n");
	return 0;
}

static int nfs4_show_layout(struct seq_file *s, struct nfs4_stid *st)
{
	struct nfs4_layout_stateid *ls;
	struct nfsd_file *file;

	ls = container_of(st, struct nfs4_layout_stateid, ls_stid);

	seq_puts(s, "- ");
	nfs4_show_stateid(s, &st->sc_stateid);
	seq_puts(s, ": { type: layout");

	/* XXX: What else would be useful? */

	spin_lock(&ls->ls_stid.sc_file->fi_lock);
	file = ls->ls_file;
	if (file) {
		seq_puts(s, ", ");
		nfs4_show_superblock(s, file);
		seq_puts(s, ", ");
		nfs4_show_fname(s, file);
	}
	spin_unlock(&ls->ls_stid.sc_file->fi_lock);
	if (st->sc_status & SC_STATUS_ADMIN_REVOKED)
		seq_puts(s, ", admin-revoked");
	seq_puts(s, " }\n");

	return 0;
}

static int states_show(struct seq_file *s, void *v)
{
	struct nfs4_stid *st = v;

	switch (st->sc_type) {
	case SC_TYPE_OPEN:
		return nfs4_show_open(s, st);
	case SC_TYPE_LOCK:
		return nfs4_show_lock(s, st);
	case SC_TYPE_DELEG:
		return nfs4_show_deleg(s, st);
	case SC_TYPE_LAYOUT:
		return nfs4_show_layout(s, st);
	default:
		return 0; /* XXX: or SEQ_SKIP? */
	}
	/* XXX: copy stateids? */
}

static struct seq_operations states_seq_ops = {
	.start = states_start,
	.next = states_next,
	.stop = states_stop,
	.show = states_show
};

static int client_states_open(struct inode *inode, struct file *file)
{
	struct seq_file *s;
	struct nfs4_client *clp;
	int ret;

	clp = get_nfsdfs_clp(inode);
	if (!clp)
		return -ENXIO;

	ret = seq_open(file, &states_seq_ops);
	if (ret)
		return ret;
	s = file->private_data;
	s->private = clp;
	return 0;
}

static int client_opens_release(struct inode *inode, struct file *file)
{
	struct seq_file *m = file->private_data;
	struct nfs4_client *clp = m->private;

	/* XXX: alternatively, we could get/drop in seq start/stop */
	drop_client(clp);
	return seq_release(inode, file);
}

static const struct file_operations client_states_fops = {
	.open		= client_states_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= client_opens_release,
};

/*
 * Normally we refuse to destroy clients that are in use, but here the
 * administrator is telling us to just do it.  We also want to wait
 * so the caller has a guarantee that the client's locks are gone by
 * the time the write returns:
 */
static void force_expire_client(struct nfs4_client *clp)
{
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);
	bool already_expired;

	trace_nfsd_clid_admin_expired(&clp->cl_clientid);

	spin_lock(&nn->client_lock);
	clp->cl_time = 0;
	spin_unlock(&nn->client_lock);

	wait_event(expiry_wq, atomic_read(&clp->cl_rpc_users) == 0);
	spin_lock(&nn->client_lock);
	already_expired = list_empty(&clp->cl_lru);
	if (!already_expired)
		unhash_client_locked(clp);
	spin_unlock(&nn->client_lock);

	if (!already_expired)
		expire_client(clp);
	else
		wait_event(expiry_wq, clp->cl_nfsd_dentry == NULL);
}

static ssize_t client_ctl_write(struct file *file, const char __user *buf,
				   size_t size, loff_t *pos)
{
	char *data;
	struct nfs4_client *clp;

	data = simple_transaction_get(file, buf, size);
	if (IS_ERR(data))
		return PTR_ERR(data);
	if (size != 7 || 0 != memcmp(data, "expire\n", 7))
		return -EINVAL;
	clp = get_nfsdfs_clp(file_inode(file));
	if (!clp)
		return -ENXIO;
	force_expire_client(clp);
	drop_client(clp);
	return 7;
}

static const struct file_operations client_ctl_fops = {
	.write		= client_ctl_write,
	.release	= simple_transaction_release,
};

static const struct tree_descr client_files[] = {
	[0] = {"info", &client_info_fops, S_IRUSR},
	[1] = {"states", &client_states_fops, S_IRUSR},
	[2] = {"ctl", &client_ctl_fops, S_IWUSR},
	[3] = {""},
};

static int
nfsd4_cb_recall_any_done(struct nfsd4_callback *cb,
				struct rpc_task *task)
{
	trace_nfsd_cb_recall_any_done(cb, task);
	switch (task->tk_status) {
	case -NFS4ERR_DELAY:
		rpc_delay(task, 2 * HZ);
		return 0;
	default:
		return 1;
	}
}

static void
nfsd4_cb_recall_any_release(struct nfsd4_callback *cb)
{
	struct nfs4_client *clp = cb->cb_clp;

	drop_client(clp);
}

static int
nfsd4_cb_getattr_done(struct nfsd4_callback *cb, struct rpc_task *task)
{
	struct nfs4_cb_fattr *ncf =
			container_of(cb, struct nfs4_cb_fattr, ncf_getattr);
	struct nfs4_delegation *dp =
			container_of(ncf, struct nfs4_delegation, dl_cb_fattr);

	trace_nfsd_cb_getattr_done(&dp->dl_stid.sc_stateid, task);
	ncf->ncf_cb_status = task->tk_status;
	switch (task->tk_status) {
	case -NFS4ERR_DELAY:
		rpc_delay(task, 2 * HZ);
		return 0;
	default:
		return 1;
	}
}

static void
nfsd4_cb_getattr_release(struct nfsd4_callback *cb)
{
	struct nfs4_cb_fattr *ncf =
			container_of(cb, struct nfs4_cb_fattr, ncf_getattr);
	struct nfs4_delegation *dp =
			container_of(ncf, struct nfs4_delegation, dl_cb_fattr);

	nfs4_put_stid(&dp->dl_stid);
}

static const struct nfsd4_callback_ops nfsd4_cb_recall_any_ops = {
	.done		= nfsd4_cb_recall_any_done,
	.release	= nfsd4_cb_recall_any_release,
	.opcode		= OP_CB_RECALL_ANY,
};

static const struct nfsd4_callback_ops nfsd4_cb_getattr_ops = {
	.done		= nfsd4_cb_getattr_done,
	.release	= nfsd4_cb_getattr_release,
	.opcode		= OP_CB_GETATTR,
};

static void nfs4_cb_getattr(struct nfs4_cb_fattr *ncf)
{
	struct nfs4_delegation *dp =
			container_of(ncf, struct nfs4_delegation, dl_cb_fattr);

	if (test_and_set_bit(NFSD4_CALLBACK_RUNNING, &ncf->ncf_getattr.cb_flags))
		return;

	/* set to proper status when nfsd4_cb_getattr_done runs */
	ncf->ncf_cb_status = NFS4ERR_IO;

	/* ensure that wake_bit is done when RUNNING is cleared */
	set_bit(NFSD4_CALLBACK_WAKE, &ncf->ncf_getattr.cb_flags);

	refcount_inc(&dp->dl_stid.sc_count);
	nfsd4_run_cb(&ncf->ncf_getattr);
}

static struct nfs4_client *create_client(struct xdr_netobj name,
		struct svc_rqst *rqstp, nfs4_verifier *verf)
{
	struct nfs4_client *clp;
	struct sockaddr *sa = svc_addr(rqstp);
	int ret;
	struct net *net = SVC_NET(rqstp);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	struct dentry *dentries[ARRAY_SIZE(client_files)];

	clp = alloc_client(name, nn);
	if (clp == NULL)
		return NULL;

	ret = copy_cred(&clp->cl_cred, &rqstp->rq_cred);
	if (ret) {
		free_client(clp);
		return NULL;
	}
	gen_clid(clp, nn);
	kref_init(&clp->cl_nfsdfs.cl_ref);
	nfsd4_init_cb(&clp->cl_cb_null, clp, NULL, NFSPROC4_CLNT_CB_NULL);
	clp->cl_time = ktime_get_boottime_seconds();
	copy_verf(clp, verf);
	memcpy(&clp->cl_addr, sa, sizeof(struct sockaddr_storage));
	clp->cl_cb_session = NULL;
	clp->net = net;
	clp->cl_nfsd_dentry = nfsd_client_mkdir(
		nn, &clp->cl_nfsdfs,
		clp->cl_clientid.cl_id - nn->clientid_base,
		client_files, dentries);
	clp->cl_nfsd_info_dentry = dentries[0];
	if (!clp->cl_nfsd_dentry) {
		free_client(clp);
		return NULL;
	}
	clp->cl_ra = kzalloc(sizeof(*clp->cl_ra), GFP_KERNEL);
	if (!clp->cl_ra) {
		free_client(clp);
		return NULL;
	}
	clp->cl_ra_time = 0;
	nfsd4_init_cb(&clp->cl_ra->ra_cb, clp, &nfsd4_cb_recall_any_ops,
			NFSPROC4_CLNT_CB_RECALL_ANY);
	return clp;
}

static void
add_clp_to_name_tree(struct nfs4_client *new_clp, struct rb_root *root)
{
	struct rb_node **new = &(root->rb_node), *parent = NULL;
	struct nfs4_client *clp;

	while (*new) {
		clp = rb_entry(*new, struct nfs4_client, cl_namenode);
		parent = *new;

		if (compare_blob(&clp->cl_name, &new_clp->cl_name) > 0)
			new = &((*new)->rb_left);
		else
			new = &((*new)->rb_right);
	}

	rb_link_node(&new_clp->cl_namenode, parent, new);
	rb_insert_color(&new_clp->cl_namenode, root);
}

static struct nfs4_client *
find_clp_in_name_tree(struct xdr_netobj *name, struct rb_root *root)
{
	int cmp;
	struct rb_node *node = root->rb_node;
	struct nfs4_client *clp;

	while (node) {
		clp = rb_entry(node, struct nfs4_client, cl_namenode);
		cmp = compare_blob(&clp->cl_name, name);
		if (cmp > 0)
			node = node->rb_left;
		else if (cmp < 0)
			node = node->rb_right;
		else
			return clp;
	}
	return NULL;
}

static void
add_to_unconfirmed(struct nfs4_client *clp)
{
	unsigned int idhashval;
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	lockdep_assert_held(&nn->client_lock);

	clear_bit(NFSD4_CLIENT_CONFIRMED, &clp->cl_flags);
	add_clp_to_name_tree(clp, &nn->unconf_name_tree);
	idhashval = clientid_hashval(clp->cl_clientid.cl_id);
	list_add(&clp->cl_idhash, &nn->unconf_id_hashtbl[idhashval]);
	renew_client_locked(clp);
}

static void
move_to_confirmed(struct nfs4_client *clp)
{
	unsigned int idhashval = clientid_hashval(clp->cl_clientid.cl_id);
	struct nfsd_net *nn = net_generic(clp->net, nfsd_net_id);

	lockdep_assert_held(&nn->client_lock);

	list_move(&clp->cl_idhash, &nn->conf_id_hashtbl[idhashval]);
	rb_erase(&clp->cl_namenode, &nn->unconf_name_tree);
	add_clp_to_name_tree(clp, &nn->conf_name_tree);
	set_bit(NFSD4_CLIENT_CONFIRMED, &clp->cl_flags);
	trace_nfsd_clid_confirmed(&clp->cl_clientid);
	renew_client_locked(clp);
}

static struct nfs4_client *
find_client_in_id_table(struct list_head *tbl, clientid_t *clid, bool sessions)
{
	struct nfs4_client *clp;
	unsigned int idhashval = clientid_hashval(clid->cl_id);

	list_for_each_entry(clp, &tbl[idhashval], cl_idhash) {
		if (same_clid(&clp->cl_clientid, clid)) {
			if ((bool)clp->cl_minorversion != sessions)
				return NULL;
			renew_client_locked(clp);
			return clp;
		}
	}
	return NULL;
}

static struct nfs4_client *
find_confirmed_client(clientid_t *clid, bool sessions, struct nfsd_net *nn)
{
	struct list_head *tbl = nn->conf_id_hashtbl;

	lockdep_assert_held(&nn->client_lock);
	return find_client_in_id_table(tbl, clid, sessions);
}

static struct nfs4_client *
find_unconfirmed_client(clientid_t *clid, bool sessions, struct nfsd_net *nn)
{
	struct list_head *tbl = nn->unconf_id_hashtbl;

	lockdep_assert_held(&nn->client_lock);
	return find_client_in_id_table(tbl, clid, sessions);
}

static bool clp_used_exchangeid(struct nfs4_client *clp)
{
	return clp->cl_exchange_flags != 0;
} 

static struct nfs4_client *
find_confirmed_client_by_name(struct xdr_netobj *name, struct nfsd_net *nn)
{
	lockdep_assert_held(&nn->client_lock);
	return find_clp_in_name_tree(name, &nn->conf_name_tree);
}

static struct nfs4_client *
find_unconfirmed_client_by_name(struct xdr_netobj *name, struct nfsd_net *nn)
{
	lockdep_assert_held(&nn->client_lock);
	return find_clp_in_name_tree(name, &nn->unconf_name_tree);
}

static void
gen_callback(struct nfs4_client *clp, struct nfsd4_setclientid *se, struct svc_rqst *rqstp)
{
	struct nfs4_cb_conn *conn = &clp->cl_cb_conn;
	struct sockaddr	*sa = svc_addr(rqstp);
	u32 scopeid = rpc_get_scope_id(sa);
	unsigned short expected_family;

	/* Currently, we only support tcp and tcp6 for the callback channel */
	if (se->se_callback_netid_len == 3 &&
	    !memcmp(se->se_callback_netid_val, "tcp", 3))
		expected_family = AF_INET;
	else if (se->se_callback_netid_len == 4 &&
		 !memcmp(se->se_callback_netid_val, "tcp6", 4))
		expected_family = AF_INET6;
	else
		goto out_err;

	conn->cb_addrlen = rpc_uaddr2sockaddr(clp->net, se->se_callback_addr_val,
					    se->se_callback_addr_len,
					    (struct sockaddr *)&conn->cb_addr,
					    sizeof(conn->cb_addr));

	if (!conn->cb_addrlen || conn->cb_addr.ss_family != expected_family)
		goto out_err;

	if (conn->cb_addr.ss_family == AF_INET6)
		((struct sockaddr_in6 *)&conn->cb_addr)->sin6_scope_id = scopeid;

	conn->cb_prog = se->se_callback_prog;
	conn->cb_ident = se->se_callback_ident;
	memcpy(&conn->cb_saddr, &rqstp->rq_daddr, rqstp->rq_daddrlen);
	trace_nfsd_cb_args(clp, conn);
	return;
out_err:
	conn->cb_addr.ss_family = AF_UNSPEC;
	conn->cb_addrlen = 0;
	trace_nfsd_cb_nodelegs(clp);
	return;
}

/*
 * Cache a reply. nfsd4_check_resp_size() has bounded the cache size.
 */
static void
nfsd4_store_cache_entry(struct nfsd4_compoundres *resp)
{
	struct xdr_buf *buf = resp->xdr->buf;
	struct nfsd4_slot *slot = resp->cstate.slot;
	unsigned int base;

	dprintk("--> %s slot %p\n", __func__, slot);

	slot->sl_flags |= NFSD4_SLOT_INITIALIZED;
	slot->sl_opcnt = resp->opcnt;
	slot->sl_status = resp->cstate.status;
	free_svc_cred(&slot->sl_cred);
	copy_cred(&slot->sl_cred, &resp->rqstp->rq_cred);

	if (!nfsd4_cache_this(resp)) {
		slot->sl_flags &= ~NFSD4_SLOT_CACHED;
		return;
	}
	slot->sl_flags |= NFSD4_SLOT_CACHED;

	base = resp->cstate.data_offset;
	slot->sl_datalen = buf->len - base;
	if (read_bytes_from_xdr_buf(buf, base, slot->sl_data, slot->sl_datalen))
		WARN(1, "%s: sessions DRC could not cache compound\n",
		     __func__);
	return;
}

/*
 * Encode the replay sequence operation from the slot values.
 * If cachethis is FALSE encode the uncached rep error on the next
 * operation which sets resp->p and increments resp->opcnt for
 * nfs4svc_encode_compoundres.
 *
 */
static __be32
nfsd4_enc_sequence_replay(struct nfsd4_compoundargs *args,
			  struct nfsd4_compoundres *resp)
{
	struct nfsd4_op *op;
	struct nfsd4_slot *slot = resp->cstate.slot;

	/* Encode the replayed sequence operation */
	op = &args->ops[resp->opcnt - 1];
	nfsd4_encode_operation(resp, op);

	if (slot->sl_flags & NFSD4_SLOT_CACHED)
		return op->status;
	if (args->opcnt == 1) {
		/*
		 * The original operation wasn't a solo sequence--we
		 * always cache those--so this retry must not match the
		 * original:
		 */
		op->status = nfserr_seq_false_retry;
	} else {
		op = &args->ops[resp->opcnt++];
		op->status = nfserr_retry_uncached_rep;
		nfsd4_encode_operation(resp, op);
	}
	return op->status;
}

/*
 * The sequence operation is not cached because we can use the slot and
 * session values.
 */
static __be32
nfsd4_replay_cache_entry(struct nfsd4_compoundres *resp,
			 struct nfsd4_sequence *seq)
{
	struct nfsd4_slot *slot = resp->cstate.slot;
	struct xdr_stream *xdr = resp->xdr;
	__be32 *p;
	__be32 status;

	dprintk("--> %s slot %p\n", __func__, slot);

	status = nfsd4_enc_sequence_replay(resp->rqstp->rq_argp, resp);
	if (status)
		return status;

	p = xdr_reserve_space(xdr, slot->sl_datalen);
	if (!p) {
		WARN_ON_ONCE(1);
		return nfserr_serverfault;
	}
	xdr_encode_opaque_fixed(p, slot->sl_data, slot->sl_datalen);
	xdr_commit_encode(xdr);

	resp->opcnt = slot->sl_opcnt;
	return slot->sl_status;
}

/*
 * Set the exchange_id flags returned by the server.
 */
static void
nfsd4_set_ex_flags(struct nfs4_client *new, struct nfsd4_exchange_id *clid)
{
#ifdef CONFIG_NFSD_PNFS
	new->cl_exchange_flags |= EXCHGID4_FLAG_USE_PNFS_MDS;
#else
	new->cl_exchange_flags |= EXCHGID4_FLAG_USE_NON_PNFS;
#endif

	/* Referrals are supported, Migration is not. */
	new->cl_exchange_flags |= EXCHGID4_FLAG_SUPP_MOVED_REFER;

	/* set the wire flags to return to client. */
	clid->flags = new->cl_exchange_flags;
}

static bool client_has_openowners(struct nfs4_client *clp)
{
	struct nfs4_openowner *oo;

	list_for_each_entry(oo, &clp->cl_openowners, oo_perclient) {
		if (!list_empty(&oo->oo_owner.so_stateids))
			return true;
	}
	return false;
}

static bool client_has_state(struct nfs4_client *clp)
{
	return client_has_openowners(clp)
#ifdef CONFIG_NFSD_PNFS
		|| !list_empty(&clp->cl_lo_states)
#endif
		|| !list_empty(&clp->cl_delegations)
		|| !list_empty(&clp->cl_sessions)
		|| nfsd4_has_active_async_copies(clp);
}

static __be32 copy_impl_id(struct nfs4_client *clp,
				struct nfsd4_exchange_id *exid)
{
	if (!exid->nii_domain.data)
		return 0;
	xdr_netobj_dup(&clp->cl_nii_domain, &exid->nii_domain, GFP_KERNEL);
	if (!clp->cl_nii_domain.data)
		return nfserr_jukebox;
	xdr_netobj_dup(&clp->cl_nii_name, &exid->nii_name, GFP_KERNEL);
	if (!clp->cl_nii_name.data)
		return nfserr_jukebox;
	clp->cl_nii_time = exid->nii_time;
	return 0;
}

__be32
nfsd4_exchange_id(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	struct nfsd4_exchange_id *exid = &u->exchange_id;
	struct nfs4_client *conf, *new;
	struct nfs4_client *unconf = NULL;
	__be32 status;
	char			addr_str[INET6_ADDRSTRLEN];
	nfs4_verifier		verf = exid->verifier;
	struct sockaddr		*sa = svc_addr(rqstp);
	bool	update = exid->flags & EXCHGID4_FLAG_UPD_CONFIRMED_REC_A;
	struct nfsd_net		*nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	rpc_ntop(sa, addr_str, sizeof(addr_str));
	dprintk("%s rqstp=%p exid=%p clname.len=%u clname.data=%p "
		"ip_addr=%s flags %x, spa_how %u\n",
		__func__, rqstp, exid, exid->clname.len, exid->clname.data,
		addr_str, exid->flags, exid->spa_how);

	exid->server_impl_name = kasprintf(GFP_KERNEL, "%s %s %s %s",
					   utsname()->sysname, utsname()->release,
					   utsname()->version, utsname()->machine);
	if (!exid->server_impl_name)
		return nfserr_jukebox;

	if (exid->flags & ~EXCHGID4_FLAG_MASK_A)
		return nfserr_inval;

	new = create_client(exid->clname, rqstp, &verf);
	if (new == NULL)
		return nfserr_jukebox;
	status = copy_impl_id(new, exid);
	if (status)
		goto out_nolock;

	switch (exid->spa_how) {
	case SP4_MACH_CRED:
		exid->spo_must_enforce[0] = 0;
		exid->spo_must_enforce[1] = (
			1 << (OP_BIND_CONN_TO_SESSION - 32) |
			1 << (OP_EXCHANGE_ID - 32) |
			1 << (OP_CREATE_SESSION - 32) |
			1 << (OP_DESTROY_SESSION - 32) |
			1 << (OP_DESTROY_CLIENTID - 32));

		exid->spo_must_allow[0] &= (1 << (OP_CLOSE) |
					1 << (OP_OPEN_DOWNGRADE) |
					1 << (OP_LOCKU) |
					1 << (OP_DELEGRETURN));

		exid->spo_must_allow[1] &= (
					1 << (OP_TEST_STATEID - 32) |
					1 << (OP_FREE_STATEID - 32));
		if (!svc_rqst_integrity_protected(rqstp)) {
			status = nfserr_inval;
			goto out_nolock;
		}
		/*
		 * Sometimes userspace doesn't give us a principal.
		 * Which is a bug, really.  Anyway, we can't enforce
		 * MACH_CRED in that case, better to give up now:
		 */
		if (!new->cl_cred.cr_principal &&
					!new->cl_cred.cr_raw_principal) {
			status = nfserr_serverfault;
			goto out_nolock;
		}
		new->cl_mach_cred = true;
		break;
	case SP4_NONE:
		break;
	default:				/* checked by xdr code */
		WARN_ON_ONCE(1);
		fallthrough;
	case SP4_SSV:
		status = nfserr_encr_alg_unsupp;
		goto out_nolock;
	}

	/* Cases below refer to rfc 5661 section 18.35.4: */
	spin_lock(&nn->client_lock);
	conf = find_confirmed_client_by_name(&exid->clname, nn);
	if (conf) {
		bool creds_match = same_creds(&conf->cl_cred, &rqstp->rq_cred);
		bool verfs_match = same_verf(&verf, &conf->cl_verifier);

		if (update) {
			if (!clp_used_exchangeid(conf)) { /* buggy client */
				status = nfserr_inval;
				goto out;
			}
			if (!nfsd4_mach_creds_match(conf, rqstp)) {
				status = nfserr_wrong_cred;
				goto out;
			}
			if (!creds_match) { /* case 9 */
				status = nfserr_perm;
				goto out;
			}
			if (!verfs_match) { /* case 8 */
				status = nfserr_not_same;
				goto out;
			}
			/* case 6 */
			exid->flags |= EXCHGID4_FLAG_CONFIRMED_R;
			trace_nfsd_clid_confirmed_r(conf);
			goto out_copy;
		}
		if (!creds_match) { /* case 3 */
			if (client_has_state(conf)) {
				status = nfserr_clid_inuse;
				trace_nfsd_clid_cred_mismatch(conf, rqstp);
				goto out;
			}
			goto out_new;
		}
		if (verfs_match) { /* case 2 */
			conf->cl_exchange_flags |= EXCHGID4_FLAG_CONFIRMED_R;
			trace_nfsd_clid_confirmed_r(conf);
			goto out_copy;
		}
		/* case 5, client reboot */
		trace_nfsd_clid_verf_mismatch(conf, rqstp, &verf);
		conf = NULL;
		goto out_new;
	}

	if (update) { /* case 7 */
		status = nfserr_noent;
		goto out;
	}

	unconf = find_unconfirmed_client_by_name(&exid->clname, nn);
	if (unconf) /* case 4, possible retry or client restart */
		unhash_client_locked(unconf);

	/* case 1, new owner ID */
	trace_nfsd_clid_fresh(new);

out_new:
	if (conf) {
		status = mark_client_expired_locked(conf);
		if (status)
			goto out;
		trace_nfsd_clid_replaced(&conf->cl_clientid);
	}
	new->cl_minorversion = cstate->minorversion;
	new->cl_spo_must_allow.u.words[0] = exid->spo_must_allow[0];
	new->cl_spo_must_allow.u.words[1] = exid->spo_must_allow[1];

	/* Contrived initial CREATE_SESSION response */
	new->cl_cs_slot.sl_status = nfserr_seq_misordered;

	add_to_unconfirmed(new);
	swap(new, conf);
out_copy:
	exid->clientid.cl_boot = conf->cl_clientid.cl_boot;
	exid->clientid.cl_id = conf->cl_clientid.cl_id;

	exid->seqid = conf->cl_cs_slot.sl_seqid + 1;
	nfsd4_set_ex_flags(conf, exid);

	exid->nii_domain.len = sizeof("kernel.org") - 1;
	exid->nii_domain.data = "kernel.org";

	/*
	 * Note that RFC 8881 places no length limit on
	 * nii_name, but this implementation permits no
	 * more than NFS4_OPAQUE_LIMIT bytes.
	 */
	exid->nii_name.len = strlen(exid->server_impl_name);
	if (exid->nii_name.len > NFS4_OPAQUE_LIMIT)
		exid->nii_name.len = NFS4_OPAQUE_LIMIT;
	exid->nii_name.data = exid->server_impl_name;

	/* just send zeros - the date is in nii_name */
	exid->nii_time.tv_sec = 0;
	exid->nii_time.tv_nsec = 0;

	dprintk("nfsd4_exchange_id seqid %d flags %x\n",
		conf->cl_cs_slot.sl_seqid, conf->cl_exchange_flags);
	status = nfs_ok;

out:
	spin_unlock(&nn->client_lock);
out_nolock:
	if (new)
		expire_client(new);
	if (unconf) {
		trace_nfsd_clid_expire_unconf(&unconf->cl_clientid);
		expire_client(unconf);
	}
	return status;
}

void
nfsd4_exchange_id_release(union nfsd4_op_u *u)
{
	struct nfsd4_exchange_id *exid = &u->exchange_id;

	kfree(exid->server_impl_name);
}

static __be32 check_slot_seqid(u32 seqid, u32 slot_seqid, u8 flags)
{
	/* The slot is in use, and no response has been sent. */
	if (flags & NFSD4_SLOT_INUSE) {
		if (seqid == slot_seqid)
			return nfserr_jukebox;
		else
			return nfserr_seq_misordered;
	}
	/* Note unsigned 32-bit arithmetic handles wraparound: */
	if (likely(seqid == slot_seqid + 1))
		return nfs_ok;
	if ((flags & NFSD4_SLOT_REUSED) && seqid == 1)
		return nfs_ok;
	if (seqid == slot_seqid)
		return nfserr_replay_cache;
	return nfserr_seq_misordered;
}

/*
 * Cache the create session result into the create session single DRC
 * slot cache by saving the xdr structure. sl_seqid has been set.
 * Do this for solo or embedded create session operations.
 */
static void
nfsd4_cache_create_session(struct nfsd4_create_session *cr_ses,
			   struct nfsd4_clid_slot *slot, __be32 nfserr)
{
	slot->sl_status = nfserr;
	memcpy(&slot->sl_cr_ses, cr_ses, sizeof(*cr_ses));
}

static __be32
nfsd4_replay_create_session(struct nfsd4_create_session *cr_ses,
			    struct nfsd4_clid_slot *slot)
{
	memcpy(cr_ses, &slot->sl_cr_ses, sizeof(*cr_ses));
	return slot->sl_status;
}

#define NFSD_MIN_REQ_HDR_SEQ_SZ	((\
			2 * 2 + /* credential,verifier: AUTH_NULL, length 0 */ \
			1 +	/* MIN tag is length with zero, only length */ \
			3 +	/* version, opcount, opcode */ \
			XDR_QUADLEN(NFS4_MAX_SESSIONID_LEN) + \
				/* seqid, slotID, slotID, cache */ \
			4 ) * sizeof(__be32))

#define NFSD_MIN_RESP_HDR_SEQ_SZ ((\
			2 +	/* verifier: AUTH_NULL, length 0 */\
			1 +	/* status */ \
			1 +	/* MIN tag is length with zero, only length */ \
			3 +	/* opcount, opcode, opstatus*/ \
			XDR_QUADLEN(NFS4_MAX_SESSIONID_LEN) + \
				/* seqid, slotID, slotID, slotID, status */ \
			5 ) * sizeof(__be32))

static __be32 check_forechannel_attrs(struct nfsd4_channel_attrs *ca, struct nfsd_net *nn)
{
	u32 maxrpc = nn->nfsd_serv->sv_max_mesg;

	if (ca->maxreq_sz < NFSD_MIN_REQ_HDR_SEQ_SZ)
		return nfserr_toosmall;
	if (ca->maxresp_sz < NFSD_MIN_RESP_HDR_SEQ_SZ)
		return nfserr_toosmall;
	ca->headerpadsz = 0;
	ca->maxreq_sz = min_t(u32, ca->maxreq_sz, maxrpc);
	ca->maxresp_sz = min_t(u32, ca->maxresp_sz, maxrpc);
	ca->maxresp_cached = min_t(u32, ca->maxresp_cached,
			NFSD_SLOT_CACHE_SIZE + NFSD_MIN_HDR_SEQ_SZ);
	ca->maxreqs = min_t(u32, ca->maxreqs, NFSD_MAX_SLOTS_PER_SESSION);

	return nfs_ok;
}

/*
 * Server's NFSv4.1 backchannel support is AUTH_SYS-only for now.
 * These are based on similar macros in linux/sunrpc/msg_prot.h .
 */
#define RPC_MAX_HEADER_WITH_AUTH_SYS \
	(RPC_CALLHDRSIZE + 2 * (2 + UNX_CALLSLACK))

#define RPC_MAX_REPHEADER_WITH_AUTH_SYS \
	(RPC_REPHDRSIZE + (2 + NUL_REPLYSLACK))

#define NFSD_CB_MAX_REQ_SZ	((NFS4_enc_cb_recall_sz + \
				 RPC_MAX_HEADER_WITH_AUTH_SYS) * sizeof(__be32))
#define NFSD_CB_MAX_RESP_SZ	((NFS4_dec_cb_recall_sz + \
				 RPC_MAX_REPHEADER_WITH_AUTH_SYS) * \
				 sizeof(__be32))

static __be32 check_backchannel_attrs(struct nfsd4_channel_attrs *ca)
{
	ca->headerpadsz = 0;

	if (ca->maxreq_sz < NFSD_CB_MAX_REQ_SZ)
		return nfserr_toosmall;
	if (ca->maxresp_sz < NFSD_CB_MAX_RESP_SZ)
		return nfserr_toosmall;
	ca->maxresp_cached = 0;
	if (ca->maxops < 2)
		return nfserr_toosmall;

	return nfs_ok;
}

static __be32 nfsd4_check_cb_sec(struct nfsd4_cb_sec *cbs)
{
	switch (cbs->flavor) {
	case RPC_AUTH_NULL:
	case RPC_AUTH_UNIX:
		return nfs_ok;
	default:
		/*
		 * GSS case: the spec doesn't allow us to return this
		 * error.  But it also doesn't allow us not to support
		 * GSS.
		 * I'd rather this fail hard than return some error the
		 * client might think it can already handle:
		 */
		return nfserr_encr_alg_unsupp;
	}
}

__be32
nfsd4_create_session(struct svc_rqst *rqstp,
		struct nfsd4_compound_state *cstate, union nfsd4_op_u *u)
{
	struct nfsd4_create_session *cr_ses = &u->create_session;
	struct sockaddr *sa = svc_addr(rqstp);
	struct nfs4_client *conf, *unconf;
	struct nfsd4_clid_slot *cs_slot;
	struct nfs4_client *old = NULL;
	struct nfsd4_session *new;
	struct nfsd4_conn *conn;
	__be32 status = 0;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	if (cr_ses->flags & ~SESSION4_FLAG_MASK_A)
		return nfserr_inval;
	status = nfsd4_check_cb_sec(&cr_ses->cb_sec);
	if (status)
		return status;
	status = check_forechannel_attrs(&cr_ses->fore_channel, nn);
	if (status)
		return status;
	status = check_backchannel_attrs(&cr_ses->back_channel);
	if (status)
		goto out_err;
	status = nfserr_jukebox;
	new = alloc_session(&cr_ses->fore_channel, &cr_ses->back_channel);
	if (!new)
		goto out_err;
	conn = alloc_conn_from_crses(rqstp, cr_ses);
	if (!conn)
		goto out_free_session;

	spin_lock(&nn->client_lock);

	/* RFC 8881 Section 18.36.4 Phase 1: Client record look-up. */
	unconf = find_unconfirmed_client(&cr_ses->clientid, true, nn);
	conf = find_confirmed_client(&cr_ses->clientid, true, nn);
	if (!conf && !unconf) {
		status = nfserr_stale_clientid;
		goto out_free_conn;
	}

	/* RFC 8881 Section 18.36.4 Phase 2: Sequence ID processing. */
	if (conf) {
		cs_slot = &conf->cl_cs_slot;
		trace_nfsd_slot_seqid_conf(conf, cr_ses);
	} else {
		cs_slot = &unconf->cl_cs_slot;
		trace_nfsd_slot_seqid_unconf(unconf, cr_ses);
	}
	status = check_slot_seqid(cr_ses->seqid, cs_slot->sl_seqid, 0);
	switch (status) {
	case nfs_ok:
		cs_slot->sl_seqid++;
		cr_ses->seqid = cs_slot->sl_seqid;
		break;
	case nfserr_replay_cache:
		status = nfsd4_replay_create_session(cr_ses, cs_slot);
		fallthrough;
	case nfserr_jukebox:
		/* The server MUST NOT cache NFS4ERR_DELAY */
		goto out_free_conn;
	default:
		goto out_cache_error;
	}

	/* RFC 8881 Section 18.36.4 Phase 3: Client ID confirmation. */
	if (conf) {
		status = nfserr_wrong_cred;
		if (!nfsd4_mach_creds_match(conf, rqstp))
			goto out_cache_error;
	} else {
		status = nfserr_clid_inuse;
		if (!same_creds(&unconf->cl_cred, &rqstp->rq_cred) ||
		    !rpc_cmp_addr(sa, (struct sockaddr *) &unconf->cl_addr)) {
			trace_nfsd_clid_cred_mismatch(unconf, rqstp);
			goto out_cache_error;
		}
		status = nfserr_wrong_cred;
		if (!nfsd4_mach_creds_match(unconf, rqstp))
			goto out_cache_error;
		old = find_confirmed_client_by_name(&unconf->cl_name, nn);
		if (old) {
			status = mark_client_expired_locked(old);
			if (status)
				goto out_expired_error;
			trace_nfsd_clid_replaced(&old->cl_clientid);
		}
		move_to_confirmed(unconf);
		conf = unconf;
	}

	/* RFC 8881 Section 18.36.4 Phase 4: Session creation. */
	status = nfs_ok;
	/* Persistent sessions are not supported */
	cr_ses->flags &= ~SESSION4_PERSIST;
	/* Upshifting from TCP to RDMA is not supported */
	cr_ses->flags &= ~SESSION4_RDMA;
	/* Report the correct number of backchannel slots */
	cr_ses->back_channel.maxreqs = new->se_cb_highest_slot + 1;

	init_session(rqstp, new, conf, cr_ses);
	nfsd4_get_session_locked(new);

	memcpy(cr_ses->sessionid.data, new->se_sessionid.data,
	       NFS4_MAX_SESSIONID_LEN);

	/* cache solo and embedded create sessions under the client_lock */
	nfsd4_cache_create_session(cr_ses, cs_slot, status);
	spin_unlock(&nn->client_lock);
	if (conf == unconf)
		fsnotify_dentry(conf->cl_nfsd_info_dentry, FS_MODIFY);
	/* init connection and backchannel */
	nfsd4_init_conn(rqstp, conn, new);
	nfsd4_put_session(new);
	if (old)
		expire_client(old);
	return status;

out_expired_error:
	/*
	 * Revert the slot seq_nr change so the server will process
	 * the client's resend instead of returning a cached response.
	 */
	if (status == nfserr_jukebox) {
		cs_slot->sl_seqid--;
		cr_ses->seqid = cs_slot->sl_seqid;
		goto out_free_conn;
	}
out_cache_error:
	nfsd4_cache_create_session(cr_ses, cs_slot, status);
out_free_conn:
	spin_unlock(&nn->client_lock);
	free_conn(conn);
out_free_session:
	__free_session(new);
out_err:
	return status;
}

static __be32 nfsd4_map_bcts_dir(u32 *dir)
{
	switch (*dir) {
	case NFS4_CDFC4_FORE:
	case NFS4_CDFC4_BACK:
		return nfs_ok;
	case NFS4_CDFC4_FORE_OR_BOTH:
	case NFS4_CDFC4_BACK_OR_BOTH:
		*dir = NFS4_CDFC4_BOTH;
		return nfs_ok;
	}
	return nfserr_inval;
}

__be32 nfsd4_backchannel_ctl(struct svc_rqst *rqstp,
		struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	struct nfsd4_backchannel_ctl *bc = &u->backchannel_ctl;
	struct nfsd4_session *session = cstate->session;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);
	__be32 status;

	status = nfsd4_check_cb_sec(&bc->bc_cb_sec);
	if (status)
		return status;
	spin_lock(&nn->client_lock);
	session->se_cb_prog = bc->bc_cb_program;
	session->se_cb_sec = bc->bc_cb_sec;
	spin_unlock(&nn->client_lock);

	nfsd4_probe_callback(session->se_client);

	return nfs_ok;
}

static struct nfsd4_conn *__nfsd4_find_conn(struct svc_xprt *xpt, struct nfsd4_session *s)
{
	struct nfsd4_conn *c;

	list_for_each_entry(c, &s->se_conns, cn_persession) {
		if (c->cn_xprt == xpt) {
			return c;
		}
	}
	return NULL;
}

static __be32 nfsd4_match_existing_connection(struct svc_rqst *rqst,
		struct nfsd4_session *session, u32 req, struct nfsd4_conn **conn)
{
	struct nfs4_client *clp = session->se_client;
	struct svc_xprt *xpt = rqst->rq_xprt;
	struct nfsd4_conn *c;
	__be32 status;

	/* Following the last paragraph of RFC 5661 Section 18.34.3: */
	spin_lock(&clp->cl_lock);
	c = __nfsd4_find_conn(xpt, session);
	if (!c)
		status = nfserr_noent;
	else if (req == c->cn_flags)
		status = nfs_ok;
	else if (req == NFS4_CDFC4_FORE_OR_BOTH &&
				c->cn_flags != NFS4_CDFC4_BACK)
		status = nfs_ok;
	else if (req == NFS4_CDFC4_BACK_OR_BOTH &&
				c->cn_flags != NFS4_CDFC4_FORE)
		status = nfs_ok;
	else
		status = nfserr_inval;
	spin_unlock(&clp->cl_lock);
	if (status == nfs_ok && conn)
		*conn = c;
	return status;
}

__be32 nfsd4_bind_conn_to_session(struct svc_rqst *rqstp,
		     struct nfsd4_compound_state *cstate,
		     union nfsd4_op_u *u)
{
	struct nfsd4_bind_conn_to_session *bcts = &u->bind_conn_to_session;
	__be32 status;
	struct nfsd4_conn *conn;
	struct nfsd4_session *session;
	struct net *net = SVC_NET(rqstp);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	if (!nfsd4_last_compound_op(rqstp))
		return nfserr_not_only_op;
	spin_lock(&nn->client_lock);
	session = find_in_sessionid_hashtbl(&bcts->sessionid, net, &status);
	spin_unlock(&nn->client_lock);
	if (!session)
		goto out_no_session;
	status = nfserr_wrong_cred;
	if (!nfsd4_mach_creds_match(session->se_client, rqstp))
		goto out;
	status = nfsd4_match_existing_connection(rqstp, session,
			bcts->dir, &conn);
	if (status == nfs_ok) {
		if (bcts->dir == NFS4_CDFC4_FORE_OR_BOTH ||
				bcts->dir == NFS4_CDFC4_BACK)
			conn->cn_flags |= NFS4_CDFC4_BACK;
		nfsd4_probe_callback(session->se_client);
		goto out;
	}
	if (status == nfserr_inval)
		goto out;
	status = nfsd4_map_bcts_dir(&bcts->dir);
	if (status)
		goto out;
	conn = alloc_conn(rqstp, bcts->dir);
	status = nfserr_jukebox;
	if (!conn)
		goto out;
	nfsd4_init_conn(rqstp, conn, session);
	status = nfs_ok;
out:
	nfsd4_put_session(session);
out_no_session:
	return status;
}

static bool nfsd4_compound_in_session(struct nfsd4_compound_state *cstate, struct nfs4_sessionid *sid)
{
	if (!cstate->session)
		return false;
	return !memcmp(sid, &cstate->session->se_sessionid, sizeof(*sid));
}

__be32
nfsd4_destroy_session(struct svc_rqst *r, struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	struct nfs4_sessionid *sessionid = &u->destroy_session.sessionid;
	struct nfsd4_session *ses;
	__be32 status;
	int ref_held_by_me = 0;
	struct net *net = SVC_NET(r);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	status = nfserr_not_only_op;
	if (nfsd4_compound_in_session(cstate, sessionid)) {
		if (!nfsd4_last_compound_op(r))
			goto out;
		ref_held_by_me++;
	}
	dump_sessionid(__func__, sessionid);
	spin_lock(&nn->client_lock);
	ses = find_in_sessionid_hashtbl(sessionid, net, &status);
	if (!ses)
		goto out_client_lock;
	status = nfserr_wrong_cred;
	if (!nfsd4_mach_creds_match(ses->se_client, r))
		goto out_put_session;
	status = mark_session_dead_locked(ses, 1 + ref_held_by_me);
	if (status)
		goto out_put_session;
	unhash_session(ses);
	spin_unlock(&nn->client_lock);

	nfsd4_probe_callback_sync(ses->se_client);

	spin_lock(&nn->client_lock);
	status = nfs_ok;
out_put_session:
	nfsd4_put_session_locked(ses);
out_client_lock:
	spin_unlock(&nn->client_lock);
out:
	return status;
}

static __be32 nfsd4_sequence_check_conn(struct nfsd4_conn *new, struct nfsd4_session *ses)
{
	struct nfs4_client *clp = ses->se_client;
	struct nfsd4_conn *c;
	__be32 status = nfs_ok;
	int ret;

	spin_lock(&clp->cl_lock);
	c = __nfsd4_find_conn(new->cn_xprt, ses);
	if (c)
		goto out_free;
	status = nfserr_conn_not_bound_to_session;
	if (clp->cl_mach_cred)
		goto out_free;
	__nfsd4_hash_conn(new, ses);
	spin_unlock(&clp->cl_lock);
	ret = nfsd4_register_conn(new);
	if (ret)
		/* oops; xprt is already down: */
		nfsd4_conn_lost(&new->cn_xpt_user);
	return nfs_ok;
out_free:
	spin_unlock(&clp->cl_lock);
	free_conn(new);
	return status;
}

static bool nfsd4_session_too_many_ops(struct svc_rqst *rqstp, struct nfsd4_session *session)
{
	struct nfsd4_compoundargs *args = rqstp->rq_argp;

	return args->opcnt > session->se_fchannel.maxops;
}

static bool nfsd4_request_too_big(struct svc_rqst *rqstp,
				  struct nfsd4_session *session)
{
	struct xdr_buf *xb = &rqstp->rq_arg;

	return xb->len > session->se_fchannel.maxreq_sz;
}

static bool replay_matches_cache(struct svc_rqst *rqstp,
		 struct nfsd4_sequence *seq, struct nfsd4_slot *slot)
{
	struct nfsd4_compoundargs *argp = rqstp->rq_argp;

	if ((bool)(slot->sl_flags & NFSD4_SLOT_CACHETHIS) !=
	    (bool)seq->cachethis)
		return false;
	/*
	 * If there's an error then the reply can have fewer ops than
	 * the call.
	 */
	if (slot->sl_opcnt < argp->opcnt && !slot->sl_status)
		return false;
	/*
	 * But if we cached a reply with *more* ops than the call you're
	 * sending us now, then this new call is clearly not really a
	 * replay of the old one:
	 */
	if (slot->sl_opcnt > argp->opcnt)
		return false;
	/* This is the only check explicitly called by spec: */
	if (!same_creds(&rqstp->rq_cred, &slot->sl_cred))
		return false;
	/*
	 * There may be more comparisons we could actually do, but the
	 * spec doesn't require us to catch every case where the calls
	 * don't match (that would require caching the call as well as
	 * the reply), so we don't bother.
	 */
	return true;
}

__be32
nfsd4_sequence(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	struct nfsd4_sequence *seq = &u->sequence;
	struct nfsd4_compoundres *resp = rqstp->rq_resp;
	struct xdr_stream *xdr = resp->xdr;
	struct nfsd4_session *session;
	struct nfs4_client *clp;
	struct nfsd4_slot *slot;
	struct nfsd4_conn *conn;
	__be32 status;
	int buflen;
	struct net *net = SVC_NET(rqstp);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	if (resp->opcnt != 1)
		return nfserr_sequence_pos;

	/*
	 * Will be either used or freed by nfsd4_sequence_check_conn
	 * below.
	 */
	conn = alloc_conn(rqstp, NFS4_CDFC4_FORE);
	if (!conn)
		return nfserr_jukebox;

	spin_lock(&nn->client_lock);
	session = find_in_sessionid_hashtbl(&seq->sessionid, net, &status);
	if (!session)
		goto out_no_session;
	clp = session->se_client;

	status = nfserr_too_many_ops;
	if (nfsd4_session_too_many_ops(rqstp, session))
		goto out_put_session;

	status = nfserr_req_too_big;
	if (nfsd4_request_too_big(rqstp, session))
		goto out_put_session;

	status = nfserr_badslot;
	if (seq->slotid >= session->se_fchannel.maxreqs)
		goto out_put_session;

	slot = xa_load(&session->se_slots, seq->slotid);
	dprintk("%s: slotid %d\n", __func__, seq->slotid);

	trace_nfsd_slot_seqid_sequence(clp, seq, slot);
	status = check_slot_seqid(seq->seqid, slot->sl_seqid, slot->sl_flags);
	if (status == nfserr_replay_cache) {
		status = nfserr_seq_misordered;
		if (!(slot->sl_flags & NFSD4_SLOT_INITIALIZED))
			goto out_put_session;
		status = nfserr_seq_false_retry;
		if (!replay_matches_cache(rqstp, seq, slot))
			goto out_put_session;
		cstate->slot = slot;
		cstate->session = session;
		cstate->clp = clp;
		/* Return the cached reply status and set cstate->status
		 * for nfsd4_proc_compound processing */
		status = nfsd4_replay_cache_entry(resp, seq);
		cstate->status = nfserr_replay_cache;
		goto out;
	}
	if (status)
		goto out_put_session;

	status = nfsd4_sequence_check_conn(conn, session);
	conn = NULL;
	if (status)
		goto out_put_session;

	if (session->se_target_maxslots < session->se_fchannel.maxreqs &&
	    slot->sl_generation == session->se_slot_gen &&
	    seq->maxslots <= session->se_target_maxslots)
		/* Client acknowledged our reduce maxreqs */
		free_session_slots(session, session->se_target_maxslots);

	buflen = (seq->cachethis) ?
			session->se_fchannel.maxresp_cached :
			session->se_fchannel.maxresp_sz;
	status = (seq->cachethis) ? nfserr_rep_too_big_to_cache :
				    nfserr_rep_too_big;
	if (xdr_restrict_buflen(xdr, buflen - rqstp->rq_auth_slack))
		goto out_put_session;
	svc_reserve_auth(rqstp, buflen);

	status = nfs_ok;
	/* Success! accept new slot seqid */
	slot->sl_seqid = seq->seqid;
	slot->sl_flags &= ~NFSD4_SLOT_REUSED;
	slot->sl_flags |= NFSD4_SLOT_INUSE;
	slot->sl_generation = session->se_slot_gen;
	if (seq->cachethis)
		slot->sl_flags |= NFSD4_SLOT_CACHETHIS;
	else
		slot->sl_flags &= ~NFSD4_SLOT_CACHETHIS;

	cstate->slot = slot;
	cstate->session = session;
	cstate->clp = clp;

	/*
	 * If the client ever uses the highest available slot,
	 * gently try to allocate another 20%.  This allows
	 * fairly quick growth without grossly over-shooting what
	 * the client might use.
	 */
	if (seq->slotid == session->se_fchannel.maxreqs - 1 &&
	    session->se_target_maxslots >= session->se_fchannel.maxreqs &&
	    session->se_fchannel.maxreqs < NFSD_MAX_SLOTS_PER_SESSION) {
		int s = session->se_fchannel.maxreqs;
		int cnt = DIV_ROUND_UP(s, 5);
		void *prev_slot;

		do {
			/*
			 * GFP_NOWAIT both allows allocation under a
			 * spinlock, and only succeeds if there is
			 * plenty of memory.
			 */
			slot = nfsd4_alloc_slot(&session->se_fchannel, s,
						GFP_NOWAIT);
			prev_slot = xa_load(&session->se_slots, s);
			if (xa_is_value(prev_slot) && slot) {
				slot->sl_seqid = xa_to_value(prev_slot);
				slot->sl_flags |= NFSD4_SLOT_REUSED;
			}
			if (slot &&
			    !xa_is_err(xa_store(&session->se_slots, s, slot,
						GFP_NOWAIT))) {
				s += 1;
				session->se_fchannel.maxreqs = s;
				atomic_add(s - session->se_target_maxslots,
					   &nfsd_total_target_slots);
				session->se_target_maxslots = s;
			} else {
				kfree(slot);
				slot = NULL;
			}
		} while (slot && --cnt > 0);
	}

out:
	seq->maxslots = max(session->se_target_maxslots, seq->maxslots);
	seq->target_maxslots = session->se_target_maxslots;

	switch (clp->cl_cb_state) {
	case NFSD4_CB_DOWN:
		seq->status_flags = SEQ4_STATUS_CB_PATH_DOWN;
		break;
	case NFSD4_CB_FAULT:
		seq->status_flags = SEQ4_STATUS_BACKCHANNEL_FAULT;
		break;
	default:
		seq->status_flags = 0;
	}
	if (!list_empty(&clp->cl_revoked))
		seq->status_flags |= SEQ4_STATUS_RECALLABLE_STATE_REVOKED;
	if (atomic_read(&clp->cl_admin_revoked))
		seq->status_flags |= SEQ4_STATUS_ADMIN_STATE_REVOKED;
	trace_nfsd_seq4_status(rqstp, seq);
out_no_session:
	if (conn)
		free_conn(conn);
	spin_unlock(&nn->client_lock);
	return status;
out_put_session:
	nfsd4_put_session_locked(session);
	goto out_no_session;
}

void
nfsd4_sequence_done(struct nfsd4_compoundres *resp)
{
	struct nfsd4_compound_state *cs = &resp->cstate;

	if (nfsd4_has_session(cs)) {
		if (cs->status != nfserr_replay_cache) {
			nfsd4_store_cache_entry(resp);
			cs->slot->sl_flags &= ~NFSD4_SLOT_INUSE;
		}
		/* Drop session reference that was taken in nfsd4_sequence() */
		nfsd4_put_session(cs->session);
	} else if (cs->clp)
		put_client_renew(cs->clp);
}

__be32
nfsd4_destroy_clientid(struct svc_rqst *rqstp,
		struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	struct nfsd4_destroy_clientid *dc = &u->destroy_clientid;
	struct nfs4_client *conf, *unconf;
	struct nfs4_client *clp = NULL;
	__be32 status = 0;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	spin_lock(&nn->client_lock);
	unconf = find_unconfirmed_client(&dc->clientid, true, nn);
	conf = find_confirmed_client(&dc->clientid, true, nn);
	WARN_ON_ONCE(conf && unconf);

	if (conf) {
		if (client_has_state(conf)) {
			status = nfserr_clientid_busy;
			goto out;
		}
		status = mark_client_expired_locked(conf);
		if (status)
			goto out;
		clp = conf;
	} else if (unconf)
		clp = unconf;
	else {
		status = nfserr_stale_clientid;
		goto out;
	}
	if (!nfsd4_mach_creds_match(clp, rqstp)) {
		clp = NULL;
		status = nfserr_wrong_cred;
		goto out;
	}
	trace_nfsd_clid_destroyed(&clp->cl_clientid);
	unhash_client_locked(clp);
out:
	spin_unlock(&nn->client_lock);
	if (clp)
		expire_client(clp);
	return status;
}

__be32
nfsd4_reclaim_complete(struct svc_rqst *rqstp,
		struct nfsd4_compound_state *cstate, union nfsd4_op_u *u)
{
	struct nfsd4_reclaim_complete *rc = &u->reclaim_complete;
	struct nfs4_client *clp = cstate->clp;
	__be32 status = 0;

	if (rc->rca_one_fs) {
		if (!cstate->current_fh.fh_dentry)
			return nfserr_nofilehandle;
		/*
		 * We don't take advantage of the rca_one_fs case.
		 * That's OK, it's optional, we can safely ignore it.
		 */
		return nfs_ok;
	}

	status = nfserr_complete_already;
	if (test_and_set_bit(NFSD4_CLIENT_RECLAIM_COMPLETE, &clp->cl_flags))
		goto out;

	status = nfserr_stale_clientid;
	if (is_client_expired(clp))
		/*
		 * The following error isn't really legal.
		 * But we only get here if the client just explicitly
		 * destroyed the client.  Surely it no longer cares what
		 * error it gets back on an operation for the dead
		 * client.
		 */
		goto out;

	status = nfs_ok;
	trace_nfsd_clid_reclaim_complete(&clp->cl_clientid);
	nfsd4_client_record_create(clp);
	inc_reclaim_complete(clp);
out:
	return status;
}

__be32
nfsd4_setclientid(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
		  union nfsd4_op_u *u)
{
	struct nfsd4_setclientid *setclid = &u->setclientid;
	struct xdr_netobj 	clname = setclid->se_name;
	nfs4_verifier		clverifier = setclid->se_verf;
	struct nfs4_client	*conf, *new;
	struct nfs4_client	*unconf = NULL;
	__be32 			status;
	struct nfsd_net		*nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	new = create_client(clname, rqstp, &clverifier);
	if (new == NULL)
		return nfserr_jukebox;
	spin_lock(&nn->client_lock);
	conf = find_confirmed_client_by_name(&clname, nn);
	if (conf && client_has_state(conf)) {
		status = nfserr_clid_inuse;
		if (clp_used_exchangeid(conf))
			goto out;
		if (!same_creds(&conf->cl_cred, &rqstp->rq_cred)) {
			trace_nfsd_clid_cred_mismatch(conf, rqstp);
			goto out;
		}
	}
	unconf = find_unconfirmed_client_by_name(&clname, nn);
	if (unconf)
		unhash_client_locked(unconf);
	if (conf) {
		if (same_verf(&conf->cl_verifier, &clverifier)) {
			copy_clid(new, conf);
			gen_confirm(new, nn);
		} else
			trace_nfsd_clid_verf_mismatch(conf, rqstp,
						      &clverifier);
	} else
		trace_nfsd_clid_fresh(new);
	new->cl_minorversion = 0;
	gen_callback(new, setclid, rqstp);
	add_to_unconfirmed(new);
	setclid->se_clientid.cl_boot = new->cl_clientid.cl_boot;
	setclid->se_clientid.cl_id = new->cl_clientid.cl_id;
	memcpy(setclid->se_confirm.data, new->cl_confirm.data, sizeof(setclid->se_confirm.data));
	new = NULL;
	status = nfs_ok;
out:
	spin_unlock(&nn->client_lock);
	if (new)
		free_client(new);
	if (unconf) {
		trace_nfsd_clid_expire_unconf(&unconf->cl_clientid);
		expire_client(unconf);
	}
	return status;
}

__be32
nfsd4_setclientid_confirm(struct svc_rqst *rqstp,
			struct nfsd4_compound_state *cstate,
			union nfsd4_op_u *u)
{
	struct nfsd4_setclientid_confirm *setclientid_confirm =
			&u->setclientid_confirm;
	struct nfs4_client *conf, *unconf;
	struct nfs4_client *old = NULL;
	nfs4_verifier confirm = setclientid_confirm->sc_confirm; 
	clientid_t * clid = &setclientid_confirm->sc_clientid;
	__be32 status;
	struct nfsd_net	*nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	if (STALE_CLIENTID(clid, nn))
		return nfserr_stale_clientid;

	spin_lock(&nn->client_lock);
	conf = find_confirmed_client(clid, false, nn);
	unconf = find_unconfirmed_client(clid, false, nn);
	/*
	 * We try hard to give out unique clientid's, so if we get an
	 * attempt to confirm the same clientid with a different cred,
	 * the client may be buggy; this should never happen.
	 *
	 * Nevertheless, RFC 7530 recommends INUSE for this case:
	 */
	status = nfserr_clid_inuse;
	if (unconf && !same_creds(&unconf->cl_cred, &rqstp->rq_cred)) {
		trace_nfsd_clid_cred_mismatch(unconf, rqstp);
		goto out;
	}
	if (conf && !same_creds(&conf->cl_cred, &rqstp->rq_cred)) {
		trace_nfsd_clid_cred_mismatch(conf, rqstp);
		goto out;
	}
	if (!unconf || !same_verf(&confirm, &unconf->cl_confirm)) {
		if (conf && same_verf(&confirm, &conf->cl_confirm)) {
			status = nfs_ok;
		} else
			status = nfserr_stale_clientid;
		goto out;
	}
	status = nfs_ok;
	if (conf) {
		if (get_client_locked(conf) == nfs_ok) {
			old = unconf;
			unhash_client_locked(old);
			nfsd4_change_callback(conf, &unconf->cl_cb_conn);
		} else {
			conf = NULL;
		}
	}

	if (!conf) {
		old = find_confirmed_client_by_name(&unconf->cl_name, nn);
		if (old) {
			status = nfserr_clid_inuse;
			if (client_has_state(old)
					&& !same_creds(&unconf->cl_cred,
							&old->cl_cred)) {
				old = NULL;
				goto out;
			}
			status = mark_client_expired_locked(old);
			if (status) {
				old = NULL;
				goto out;
			}
			trace_nfsd_clid_replaced(&old->cl_clientid);
		}
		status = get_client_locked(unconf);
		if (status != nfs_ok) {
			old = NULL;
			goto out;
		}
		move_to_confirmed(unconf);
		conf = unconf;
	}
	spin_unlock(&nn->client_lock);
	if (conf == unconf)
		fsnotify_dentry(conf->cl_nfsd_info_dentry, FS_MODIFY);
	nfsd4_probe_callback(conf);
	spin_lock(&nn->client_lock);
	put_client_renew_locked(conf);
out:
	spin_unlock(&nn->client_lock);
	if (old)
		expire_client(old);
	return status;
}

static struct nfs4_file *nfsd4_alloc_file(void)
{
	return kmem_cache_alloc(file_slab, GFP_KERNEL);
}

/* OPEN Share state helper functions */

static void nfsd4_file_init(const struct svc_fh *fh, struct nfs4_file *fp)
{
	refcount_set(&fp->fi_ref, 1);
	spin_lock_init(&fp->fi_lock);
	INIT_LIST_HEAD(&fp->fi_stateids);
	INIT_LIST_HEAD(&fp->fi_delegations);
	INIT_LIST_HEAD(&fp->fi_clnt_odstate);
	fh_copy_shallow(&fp->fi_fhandle, &fh->fh_handle);
	fp->fi_deleg_file = NULL;
	fp->fi_rdeleg_file = NULL;
	fp->fi_had_conflict = false;
	fp->fi_share_deny = 0;
	memset(fp->fi_fds, 0, sizeof(fp->fi_fds));
	memset(fp->fi_access, 0, sizeof(fp->fi_access));
	fp->fi_aliased = false;
	fp->fi_inode = d_inode(fh->fh_dentry);
#ifdef CONFIG_NFSD_PNFS
	INIT_LIST_HEAD(&fp->fi_lo_states);
	atomic_set(&fp->fi_lo_recalls, 0);
#endif
}

void
nfsd4_free_slabs(void)
{
	kmem_cache_destroy(client_slab);
	kmem_cache_destroy(openowner_slab);
	kmem_cache_destroy(lockowner_slab);
	kmem_cache_destroy(file_slab);
	kmem_cache_destroy(stateid_slab);
	kmem_cache_destroy(deleg_slab);
	kmem_cache_destroy(odstate_slab);
}

int
nfsd4_init_slabs(void)
{
	client_slab = KMEM_CACHE(nfs4_client, 0);
	if (client_slab == NULL)
		goto out;
	openowner_slab = KMEM_CACHE(nfs4_openowner, 0);
	if (openowner_slab == NULL)
		goto out_free_client_slab;
	lockowner_slab = KMEM_CACHE(nfs4_lockowner, 0);
	if (lockowner_slab == NULL)
		goto out_free_openowner_slab;
	file_slab = KMEM_CACHE(nfs4_file, 0);
	if (file_slab == NULL)
		goto out_free_lockowner_slab;
	stateid_slab = KMEM_CACHE(nfs4_ol_stateid, 0);
	if (stateid_slab == NULL)
		goto out_free_file_slab;
	deleg_slab = KMEM_CACHE(nfs4_delegation, 0);
	if (deleg_slab == NULL)
		goto out_free_stateid_slab;
	odstate_slab = KMEM_CACHE(nfs4_clnt_odstate, 0);
	if (odstate_slab == NULL)
		goto out_free_deleg_slab;
	return 0;

out_free_deleg_slab:
	kmem_cache_destroy(deleg_slab);
out_free_stateid_slab:
	kmem_cache_destroy(stateid_slab);
out_free_file_slab:
	kmem_cache_destroy(file_slab);
out_free_lockowner_slab:
	kmem_cache_destroy(lockowner_slab);
out_free_openowner_slab:
	kmem_cache_destroy(openowner_slab);
out_free_client_slab:
	kmem_cache_destroy(client_slab);
out:
	return -ENOMEM;
}

static unsigned long
nfsd4_state_shrinker_count(struct shrinker *shrink, struct shrink_control *sc)
{
	struct nfsd_net *nn = shrink->private_data;
	long count;

	count = atomic_read(&nn->nfsd_courtesy_clients);
	if (!count)
		count = atomic_long_read(&num_delegations);
	if (count)
		queue_work(laundry_wq, &nn->nfsd_shrinker_work);
	return (unsigned long)count;
}

static unsigned long
nfsd4_state_shrinker_scan(struct shrinker *shrink, struct shrink_control *sc)
{
	return SHRINK_STOP;
}

void
nfsd4_init_leases_net(struct nfsd_net *nn)
{
	struct sysinfo si;
	u64 max_clients;

	nn->nfsd4_lease = 90;	/* default lease time */
	nn->nfsd4_grace = 90;
	nn->somebody_reclaimed = false;
	nn->track_reclaim_completes = false;
	nn->clverifier_counter = get_random_u32();
	nn->clientid_base = get_random_u32();
	nn->clientid_counter = nn->clientid_base + 1;
	nn->s2s_cp_cl_id = nn->clientid_counter++;

	atomic_set(&nn->nfs4_client_count, 0);
	si_meminfo(&si);
	max_clients = (u64)si.totalram * si.mem_unit / (1024 * 1024 * 1024);
	max_clients *= NFS4_CLIENTS_PER_GB;
	nn->nfs4_max_clients = max_t(int, max_clients, NFS4_CLIENTS_PER_GB);

	atomic_set(&nn->nfsd_courtesy_clients, 0);
}

enum rp_lock {
	RP_UNLOCKED,
	RP_LOCKED,
	RP_UNHASHED,
};

static void init_nfs4_replay(struct nfs4_replay *rp)
{
	rp->rp_status = nfserr_serverfault;
	rp->rp_buflen = 0;
	rp->rp_buf = rp->rp_ibuf;
	rp->rp_locked = RP_UNLOCKED;
}

static int nfsd4_cstate_assign_replay(struct nfsd4_compound_state *cstate,
				      struct nfs4_stateowner *so)
{
	if (!nfsd4_has_session(cstate)) {
		wait_var_event(&so->so_replay.rp_locked,
			       cmpxchg(&so->so_replay.rp_locked,
				       RP_UNLOCKED, RP_LOCKED) != RP_LOCKED);
		if (so->so_replay.rp_locked == RP_UNHASHED)
			return -EAGAIN;
		cstate->replay_owner = nfs4_get_stateowner(so);
	}
	return 0;
}

void nfsd4_cstate_clear_replay(struct nfsd4_compound_state *cstate)
{
	struct nfs4_stateowner *so = cstate->replay_owner;

	if (so != NULL) {
		cstate->replay_owner = NULL;
		store_release_wake_up(&so->so_replay.rp_locked, RP_UNLOCKED);
		nfs4_put_stateowner(so);
	}
}

static inline void *alloc_stateowner(struct kmem_cache *slab, struct xdr_netobj *owner, struct nfs4_client *clp)
{
	struct nfs4_stateowner *sop;

	sop = kmem_cache_alloc(slab, GFP_KERNEL);
	if (!sop)
		return NULL;

	xdr_netobj_dup(&sop->so_owner, owner, GFP_KERNEL);
	if (!sop->so_owner.data) {
		kmem_cache_free(slab, sop);
		return NULL;
	}

	INIT_LIST_HEAD(&sop->so_stateids);
	sop->so_client = clp;
	init_nfs4_replay(&sop->so_replay);
	atomic_set(&sop->so_count, 1);
	return sop;
}

static void hash_openowner(struct nfs4_openowner *oo, struct nfs4_client *clp, unsigned int strhashval)
{
	lockdep_assert_held(&clp->cl_lock);

	list_add(&oo->oo_owner.so_strhash,
		 &clp->cl_ownerstr_hashtbl[strhashval]);
	list_add(&oo->oo_perclient, &clp->cl_openowners);
}

static void nfs4_unhash_openowner(struct nfs4_stateowner *so)
{
	unhash_openowner_locked(openowner(so));
}

static void nfs4_free_openowner(struct nfs4_stateowner *so)
{
	struct nfs4_openowner *oo = openowner(so);

	kmem_cache_free(openowner_slab, oo);
}

static const struct nfs4_stateowner_operations openowner_ops = {
	.so_unhash =	nfs4_unhash_openowner,
	.so_free =	nfs4_free_openowner,
};

static struct nfs4_ol_stateid *
nfsd4_find_existing_open(struct nfs4_file *fp, struct nfsd4_open *open)
{
	struct nfs4_ol_stateid *local, *ret = NULL;
	struct nfs4_openowner *oo = open->op_openowner;

	lockdep_assert_held(&fp->fi_lock);

	list_for_each_entry(local, &fp->fi_stateids, st_perfile) {
		/* ignore lock owners */
		if (local->st_stateowner->so_is_open_owner == 0)
			continue;
		if (local->st_stateowner != &oo->oo_owner)
			continue;
		if (local->st_stid.sc_type == SC_TYPE_OPEN &&
		    !local->st_stid.sc_status) {
			ret = local;
			refcount_inc(&ret->st_stid.sc_count);
			break;
		}
	}
	return ret;
}

static void nfsd4_drop_revoked_stid(struct nfs4_stid *s)
	__releases(&s->sc_client->cl_lock)
{
	struct nfs4_client *cl = s->sc_client;
	LIST_HEAD(reaplist);
	struct nfs4_ol_stateid *stp;
	struct nfs4_delegation *dp;
	bool unhashed;

	switch (s->sc_type) {
	case SC_TYPE_OPEN:
		stp = openlockstateid(s);
		if (unhash_open_stateid(stp, &reaplist))
			put_ol_stateid_locked(stp, &reaplist);
		spin_unlock(&cl->cl_lock);
		free_ol_stateid_reaplist(&reaplist);
		break;
	case SC_TYPE_LOCK:
		stp = openlockstateid(s);
		unhashed = unhash_lock_stateid(stp);
		spin_unlock(&cl->cl_lock);
		if (unhashed)
			nfs4_put_stid(s);
		break;
	case SC_TYPE_DELEG:
		dp = delegstateid(s);
		list_del_init(&dp->dl_recall_lru);
		spin_unlock(&cl->cl_lock);
		nfs4_put_stid(s);
		break;
	default:
		spin_unlock(&cl->cl_lock);
	}
}

static void nfsd40_drop_revoked_stid(struct nfs4_client *cl,
				    stateid_t *stid)
{
	/* NFSv4.0 has no way for the client to tell the server
	 * that it can forget an admin-revoked stateid.
	 * So we keep it around until the first time that the
	 * client uses it, and drop it the first time
	 * nfserr_admin_revoked is returned.
	 * For v4.1 and later we wait until explicitly told
	 * to free the stateid.
	 */
	if (cl->cl_minorversion == 0) {
		struct nfs4_stid *st;

		spin_lock(&cl->cl_lock);
		st = find_stateid_locked(cl, stid);
		if (st)
			nfsd4_drop_revoked_stid(st);
		else
			spin_unlock(&cl->cl_lock);
	}
}

static __be32
nfsd4_verify_open_stid(struct nfs4_stid *s)
{
	__be32 ret = nfs_ok;

	if (s->sc_status & SC_STATUS_ADMIN_REVOKED)
		ret = nfserr_admin_revoked;
	else if (s->sc_status & SC_STATUS_REVOKED)
		ret = nfserr_deleg_revoked;
	else if (s->sc_status & SC_STATUS_CLOSED)
		ret = nfserr_bad_stateid;
	return ret;
}

/* Lock the stateid st_mutex, and deal with races with CLOSE */
static __be32
nfsd4_lock_ol_stateid(struct nfs4_ol_stateid *stp)
{
	__be32 ret;

	mutex_lock_nested(&stp->st_mutex, LOCK_STATEID_MUTEX);
	ret = nfsd4_verify_open_stid(&stp->st_stid);
	if (ret == nfserr_admin_revoked)
		nfsd40_drop_revoked_stid(stp->st_stid.sc_client,
					&stp->st_stid.sc_stateid);

	if (ret != nfs_ok)
		mutex_unlock(&stp->st_mutex);
	return ret;
}

static struct nfs4_ol_stateid *
nfsd4_find_and_lock_existing_open(struct nfs4_file *fp, struct nfsd4_open *open)
{
	struct nfs4_ol_stateid *stp;
	for (;;) {
		spin_lock(&fp->fi_lock);
		stp = nfsd4_find_existing_open(fp, open);
		spin_unlock(&fp->fi_lock);
		if (!stp || nfsd4_lock_ol_stateid(stp) == nfs_ok)
			break;
		nfs4_put_stid(&stp->st_stid);
	}
	return stp;
}

static struct nfs4_openowner *
find_or_alloc_open_stateowner(unsigned int strhashval, struct nfsd4_open *open,
			      struct nfsd4_compound_state *cstate)
{
	struct nfs4_client *clp = cstate->clp;
	struct nfs4_openowner *oo, *new = NULL;

retry:
	spin_lock(&clp->cl_lock);
	oo = find_openstateowner_str(strhashval, open, clp);
	if (!oo && new) {
		hash_openowner(new, clp, strhashval);
		spin_unlock(&clp->cl_lock);
		return new;
	}
	spin_unlock(&clp->cl_lock);

	if (oo && !(oo->oo_flags & NFS4_OO_CONFIRMED)) {
		/* Replace unconfirmed owners without checking for replay. */
		release_openowner(oo);
		oo = NULL;
	}
	if (oo) {
		if (new)
			nfs4_free_stateowner(&new->oo_owner);
		return oo;
	}

	new = alloc_stateowner(openowner_slab, &open->op_owner, clp);
	if (!new)
		return NULL;
	new->oo_owner.so_ops = &openowner_ops;
	new->oo_owner.so_is_open_owner = 1;
	new->oo_owner.so_seqid = open->op_seqid;
	new->oo_flags = 0;
	if (nfsd4_has_session(cstate))
		new->oo_flags |= NFS4_OO_CONFIRMED;
	new->oo_time = 0;
	new->oo_last_closed_stid = NULL;
	INIT_LIST_HEAD(&new->oo_close_lru);
	goto retry;
}

static struct nfs4_ol_stateid *
init_open_stateid(struct nfs4_file *fp, struct nfsd4_open *open)
{

	struct nfs4_openowner *oo = open->op_openowner;
	struct nfs4_ol_stateid *retstp = NULL;
	struct nfs4_ol_stateid *stp;

	stp = open->op_stp;
	/* We are moving these outside of the spinlocks to avoid the warnings */
	mutex_init(&stp->st_mutex);
	mutex_lock_nested(&stp->st_mutex, OPEN_STATEID_MUTEX);

retry:
	spin_lock(&oo->oo_owner.so_client->cl_lock);
	spin_lock(&fp->fi_lock);

	if (nfs4_openowner_unhashed(oo)) {
		mutex_unlock(&stp->st_mutex);
		stp = NULL;
		goto out_unlock;
	}

	retstp = nfsd4_find_existing_open(fp, open);
	if (retstp)
		goto out_unlock;

	open->op_stp = NULL;
	refcount_inc(&stp->st_stid.sc_count);
	stp->st_stid.sc_type = SC_TYPE_OPEN;
	INIT_LIST_HEAD(&stp->st_locks);
	stp->st_stateowner = nfs4_get_stateowner(&oo->oo_owner);
	get_nfs4_file(fp);
	stp->st_stid.sc_file = fp;
	stp->st_access_bmap = 0;
	stp->st_deny_bmap = 0;
	stp->st_openstp = NULL;
	list_add(&stp->st_perstateowner, &oo->oo_owner.so_stateids);
	list_add(&stp->st_perfile, &fp->fi_stateids);

out_unlock:
	spin_unlock(&fp->fi_lock);
	spin_unlock(&oo->oo_owner.so_client->cl_lock);
	if (retstp) {
		/* Handle races with CLOSE */
		if (nfsd4_lock_ol_stateid(retstp) != nfs_ok) {
			nfs4_put_stid(&retstp->st_stid);
			goto retry;
		}
		/* To keep mutex tracking happy */
		mutex_unlock(&stp->st_mutex);
		stp = retstp;
	}
	return stp;
}

/*
 * In the 4.0 case we need to keep the owners around a little while to handle
 * CLOSE replay. We still do need to release any file access that is held by
 * them before returning however.
 */
static void
move_to_close_lru(struct nfs4_ol_stateid *s, struct net *net)
{
	struct nfs4_ol_stateid *last;
	struct nfs4_openowner *oo = openowner(s->st_stateowner);
	struct nfsd_net *nn = net_generic(s->st_stid.sc_client->net,
						nfsd_net_id);

	dprintk("NFSD: move_to_close_lru nfs4_openowner %p\n", oo);

	/*
	 * We know that we hold one reference via nfsd4_close, and another
	 * "persistent" reference for the client. If the refcount is higher
	 * than 2, then there are still calls in progress that are using this
	 * stateid. We can't put the sc_file reference until they are finished.
	 * Wait for the refcount to drop to 2. Since it has been unhashed,
	 * there should be no danger of the refcount going back up again at
	 * this point.
	 * Some threads with a reference might be waiting for rp_locked,
	 * so tell them to stop waiting.
	 */
	store_release_wake_up(&oo->oo_owner.so_replay.rp_locked, RP_UNHASHED);
	wait_event(close_wq, refcount_read(&s->st_stid.sc_count) == 2);

	release_all_access(s);
	if (s->st_stid.sc_file) {
		put_nfs4_file(s->st_stid.sc_file);
		s->st_stid.sc_file = NULL;
	}

	spin_lock(&nn->client_lock);
	last = oo->oo_last_closed_stid;
	oo->oo_last_closed_stid = s;
	list_move_tail(&oo->oo_close_lru, &nn->close_lru);
	oo->oo_time = ktime_get_boottime_seconds();
	spin_unlock(&nn->client_lock);
	if (last)
		nfs4_put_stid(&last->st_stid);
}

static noinline_for_stack struct nfs4_file *
nfsd4_file_hash_lookup(const struct svc_fh *fhp)
{
	struct inode *inode = d_inode(fhp->fh_dentry);
	struct rhlist_head *tmp, *list;
	struct nfs4_file *fi;

	rcu_read_lock();
	list = rhltable_lookup(&nfs4_file_rhltable, &inode,
			       nfs4_file_rhash_params);
	rhl_for_each_entry_rcu(fi, tmp, list, fi_rlist) {
		if (fh_match(&fi->fi_fhandle, &fhp->fh_handle)) {
			if (refcount_inc_not_zero(&fi->fi_ref)) {
				rcu_read_unlock();
				return fi;
			}
		}
	}
	rcu_read_unlock();
	return NULL;
}

/*
 * On hash insertion, identify entries with the same inode but
 * distinct filehandles. They will all be on the list returned
 * by rhltable_lookup().
 *
 * inode->i_lock prevents racing insertions from adding an entry
 * for the same inode/fhp pair twice.
 */
static noinline_for_stack struct nfs4_file *
nfsd4_file_hash_insert(struct nfs4_file *new, const struct svc_fh *fhp)
{
	struct inode *inode = d_inode(fhp->fh_dentry);
	struct rhlist_head *tmp, *list;
	struct nfs4_file *ret = NULL;
	bool alias_found = false;
	struct nfs4_file *fi;
	int err;

	rcu_read_lock();
	spin_lock(&inode->i_lock);

	list = rhltable_lookup(&nfs4_file_rhltable, &inode,
			       nfs4_file_rhash_params);
	rhl_for_each_entry_rcu(fi, tmp, list, fi_rlist) {
		if (fh_match(&fi->fi_fhandle, &fhp->fh_handle)) {
			if (refcount_inc_not_zero(&fi->fi_ref))
				ret = fi;
		} else
			fi->fi_aliased = alias_found = true;
	}
	if (ret)
		goto out_unlock;

	nfsd4_file_init(fhp, new);
	err = rhltable_insert(&nfs4_file_rhltable, &new->fi_rlist,
			      nfs4_file_rhash_params);
	if (err)
		goto out_unlock;

	new->fi_aliased = alias_found;
	ret = new;

out_unlock:
	spin_unlock(&inode->i_lock);
	rcu_read_unlock();
	return ret;
}

static noinline_for_stack void nfsd4_file_hash_remove(struct nfs4_file *fi)
{
	rhltable_remove(&nfs4_file_rhltable, &fi->fi_rlist,
			nfs4_file_rhash_params);
}

/*
 * Called to check deny when READ with all zero stateid or
 * WRITE with all zero or all one stateid
 */
static __be32
nfs4_share_conflict(struct svc_fh *current_fh, unsigned int deny_type)
{
	struct nfs4_file *fp;
	__be32 ret = nfs_ok;

	fp = nfsd4_file_hash_lookup(current_fh);
	if (!fp)
		return ret;

	/* Check for conflicting share reservations */
	spin_lock(&fp->fi_lock);
	if (fp->fi_share_deny & deny_type)
		ret = nfserr_locked;
	spin_unlock(&fp->fi_lock);
	put_nfs4_file(fp);
	return ret;
}

static bool nfsd4_deleg_present(const struct inode *inode)
{
	struct file_lock_context *ctx = locks_inode_context(inode);

	return ctx && !list_empty_careful(&ctx->flc_lease);
}

/**
 * nfsd_wait_for_delegreturn - wait for delegations to be returned
 * @rqstp: the RPC transaction being executed
 * @inode: in-core inode of the file being waited for
 *
 * The timeout prevents deadlock if all nfsd threads happen to be
 * tied up waiting for returning delegations.
 *
 * Return values:
 *   %true: delegation was returned
 *   %false: timed out waiting for delegreturn
 */
bool nfsd_wait_for_delegreturn(struct svc_rqst *rqstp, struct inode *inode)
{
	long __maybe_unused timeo;

	timeo = wait_var_event_timeout(inode, !nfsd4_deleg_present(inode),
				       NFSD_DELEGRETURN_TIMEOUT);
	trace_nfsd_delegret_wakeup(rqstp, inode, timeo);
	return timeo > 0;
}

static void nfsd4_cb_recall_prepare(struct nfsd4_callback *cb)
{
	struct nfs4_delegation *dp = cb_to_delegation(cb);
	struct nfsd_net *nn = net_generic(dp->dl_stid.sc_client->net,
					  nfsd_net_id);

	block_delegations(&dp->dl_stid.sc_file->fi_fhandle);

	/*
	 * We can't do this in nfsd_break_deleg_cb because it is
	 * already holding inode->i_lock.
	 *
	 * If the dl_time != 0, then we know that it has already been
	 * queued for a lease break. Don't queue it again.
	 */
	spin_lock(&state_lock);
	if (delegation_hashed(dp) && dp->dl_time == 0) {
		dp->dl_time = ktime_get_boottime_seconds();
		list_add_tail(&dp->dl_recall_lru, &nn->del_recall_lru);
	}
	spin_unlock(&state_lock);
}

static int nfsd4_cb_recall_done(struct nfsd4_callback *cb,
		struct rpc_task *task)
{
	struct nfs4_delegation *dp = cb_to_delegation(cb);

	trace_nfsd_cb_recall_done(&dp->dl_stid.sc_stateid, task);

	if (dp->dl_stid.sc_status)
		/* CLOSED or REVOKED */
		return 1;

	switch (task->tk_status) {
	case 0:
		return 1;
	case -NFS4ERR_DELAY:
		rpc_delay(task, 2 * HZ);
		return 0;
	case -EBADHANDLE:
	case -NFS4ERR_BAD_STATEID:
		/*
		 * Race: client probably got cb_recall before open reply
		 * granting delegation.
		 */
		if (dp->dl_retries--) {
			rpc_delay(task, 2 * HZ);
			return 0;
		}
		fallthrough;
	default:
		return 1;
	}
}

static void nfsd4_cb_recall_release(struct nfsd4_callback *cb)
{
	struct nfs4_delegation *dp = cb_to_delegation(cb);

	nfs4_put_stid(&dp->dl_stid);
}

static const struct nfsd4_callback_ops nfsd4_cb_recall_ops = {
	.prepare	= nfsd4_cb_recall_prepare,
	.done		= nfsd4_cb_recall_done,
	.release	= nfsd4_cb_recall_release,
	.opcode		= OP_CB_RECALL,
};

static void nfsd_break_one_deleg(struct nfs4_delegation *dp)
{
	bool queued;

	if (test_and_set_bit(NFSD4_CALLBACK_RUNNING, &dp->dl_recall.cb_flags))
		return;

	/*
	 * We're assuming the state code never drops its reference
	 * without first removing the lease.  Since we're in this lease
	 * callback (and since the lease code is serialized by the
	 * flc_lock) we know the server hasn't removed the lease yet, and
	 * we know it's safe to take a reference.
	 */
	refcount_inc(&dp->dl_stid.sc_count);
	queued = nfsd4_run_cb(&dp->dl_recall);
	WARN_ON_ONCE(!queued);
	if (!queued)
		refcount_dec(&dp->dl_stid.sc_count);
}

/* Called from break_lease() with flc_lock held. */
static bool
nfsd_break_deleg_cb(struct file_lease *fl)
{
	struct nfs4_delegation *dp = (struct nfs4_delegation *) fl->c.flc_owner;
	struct nfs4_file *fp = dp->dl_stid.sc_file;
	struct nfs4_client *clp = dp->dl_stid.sc_client;
	struct nfsd_net *nn;

	trace_nfsd_cb_recall(&dp->dl_stid);

	dp->dl_recalled = true;
	atomic_inc(&clp->cl_delegs_in_recall);
	if (try_to_expire_client(clp)) {
		nn = net_generic(clp->net, nfsd_net_id);
		mod_delayed_work(laundry_wq, &nn->laundromat_work, 0);
	}

	/*
	 * We don't want the locks code to timeout the lease for us;
	 * we'll remove it ourself if a delegation isn't returned
	 * in time:
	 */
	fl->fl_break_time = 0;

	fp->fi_had_conflict = true;
	nfsd_break_one_deleg(dp);
	return false;
}

/**
 * nfsd_breaker_owns_lease - Check if lease conflict was resolved
 * @fl: Lock state to check
 *
 * Return values:
 *   %true: Lease conflict was resolved
 *   %false: Lease conflict was not resolved.
 */
static bool nfsd_breaker_owns_lease(struct file_lease *fl)
{
	struct nfs4_delegation *dl = fl->c.flc_owner;
	struct svc_rqst *rqst;
	struct nfs4_client *clp;

	rqst = nfsd_current_rqst();
	if (!nfsd_v4client(rqst))
		return false;
	clp = *(rqst->rq_lease_breaker);
	return dl->dl_stid.sc_client == clp;
}

static int
nfsd_change_deleg_cb(struct file_lease *onlist, int arg,
		     struct list_head *dispose)
{
	struct nfs4_delegation *dp = (struct nfs4_delegation *) onlist->c.flc_owner;
	struct nfs4_client *clp = dp->dl_stid.sc_client;

	if (arg & F_UNLCK) {
		if (dp->dl_recalled)
			atomic_dec(&clp->cl_delegs_in_recall);
		return lease_modify(onlist, arg, dispose);
	} else
		return -EAGAIN;
}

static const struct lease_manager_operations nfsd_lease_mng_ops = {
	.lm_breaker_owns_lease = nfsd_breaker_owns_lease,
	.lm_break = nfsd_break_deleg_cb,
	.lm_change = nfsd_change_deleg_cb,
};

static __be32 nfsd4_check_seqid(struct nfsd4_compound_state *cstate, struct nfs4_stateowner *so, u32 seqid)
{
	if (nfsd4_has_session(cstate))
		return nfs_ok;
	if (seqid == so->so_seqid - 1)
		return nfserr_replay_me;
	if (seqid == so->so_seqid)
		return nfs_ok;
	return nfserr_bad_seqid;
}

static struct nfs4_client *lookup_clientid(clientid_t *clid, bool sessions,
						struct nfsd_net *nn)
{
	struct nfs4_client *found;

	spin_lock(&nn->client_lock);
	found = find_confirmed_client(clid, sessions, nn);
	if (found)
		atomic_inc(&found->cl_rpc_users);
	spin_unlock(&nn->client_lock);
	return found;
}

static __be32 set_client(clientid_t *clid,
		struct nfsd4_compound_state *cstate,
		struct nfsd_net *nn)
{
	if (cstate->clp) {
		if (!same_clid(&cstate->clp->cl_clientid, clid))
			return nfserr_stale_clientid;
		return nfs_ok;
	}
	if (STALE_CLIENTID(clid, nn))
		return nfserr_stale_clientid;
	/*
	 * We're in the 4.0 case (otherwise the SEQUENCE op would have
	 * set cstate->clp), so session = false:
	 */
	cstate->clp = lookup_clientid(clid, false, nn);
	if (!cstate->clp)
		return nfserr_expired;
	return nfs_ok;
}

__be32
nfsd4_process_open1(struct nfsd4_compound_state *cstate,
		    struct nfsd4_open *open, struct nfsd_net *nn)
{
	clientid_t *clientid = &open->op_clientid;
	struct nfs4_client *clp = NULL;
	unsigned int strhashval;
	struct nfs4_openowner *oo = NULL;
	__be32 status;

	/*
	 * In case we need it later, after we've already created the
	 * file and don't want to risk a further failure:
	 */
	open->op_file = nfsd4_alloc_file();
	if (open->op_file == NULL)
		return nfserr_jukebox;

	status = set_client(clientid, cstate, nn);
	if (status)
		return status;
	clp = cstate->clp;

	strhashval = ownerstr_hashval(&open->op_owner);
retry:
	oo = find_or_alloc_open_stateowner(strhashval, open, cstate);
	open->op_openowner = oo;
	if (!oo)
		return nfserr_jukebox;
	if (nfsd4_cstate_assign_replay(cstate, &oo->oo_owner) == -EAGAIN) {
		nfs4_put_stateowner(&oo->oo_owner);
		goto retry;
	}
	status = nfsd4_check_seqid(cstate, &oo->oo_owner, open->op_seqid);
	if (status)
		return status;

	open->op_stp = nfs4_alloc_open_stateid(clp);
	if (!open->op_stp)
		return nfserr_jukebox;

	if (nfsd4_has_session(cstate) &&
	    (cstate->current_fh.fh_export->ex_flags & NFSEXP_PNFS)) {
		open->op_odstate = alloc_clnt_odstate(clp);
		if (!open->op_odstate)
			return nfserr_jukebox;
	}

	return nfs_ok;
}

static inline __be32
nfs4_check_delegmode(struct nfs4_delegation *dp, int flags)
{
	if (!(flags & RD_STATE) && deleg_is_read(dp->dl_type))
		return nfserr_openmode;
	else
		return nfs_ok;
}

static int share_access_to_flags(u32 share_access)
{
	return share_access == NFS4_SHARE_ACCESS_READ ? RD_STATE : WR_STATE;
}

static struct nfs4_delegation *find_deleg_stateid(struct nfs4_client *cl,
						  stateid_t *s)
{
	struct nfs4_stid *ret;

	ret = find_stateid_by_type(cl, s, SC_TYPE_DELEG, SC_STATUS_REVOKED);
	if (!ret)
		return NULL;
	return delegstateid(ret);
}

static bool nfsd4_is_deleg_cur(struct nfsd4_open *open)
{
	return open->op_claim_type == NFS4_OPEN_CLAIM_DELEGATE_CUR ||
	       open->op_claim_type == NFS4_OPEN_CLAIM_DELEG_CUR_FH;
}

static __be32
nfs4_check_deleg(struct nfs4_client *cl, struct nfsd4_open *open,
		struct nfs4_delegation **dp)
{
	int flags;
	__be32 status = nfserr_bad_stateid;
	struct nfs4_delegation *deleg;

	deleg = find_deleg_stateid(cl, &open->op_delegate_stateid);
	if (deleg == NULL)
		goto out;
	if (deleg->dl_stid.sc_status & SC_STATUS_ADMIN_REVOKED) {
		nfs4_put_stid(&deleg->dl_stid);
		status = nfserr_admin_revoked;
		goto out;
	}
	if (deleg->dl_stid.sc_status & SC_STATUS_REVOKED) {
		nfs4_put_stid(&deleg->dl_stid);
		nfsd40_drop_revoked_stid(cl, &open->op_delegate_stateid);
		status = nfserr_deleg_revoked;
		goto out;
	}
	flags = share_access_to_flags(open->op_share_access);
	status = nfs4_check_delegmode(deleg, flags);
	if (status) {
		nfs4_put_stid(&deleg->dl_stid);
		goto out;
	}
	*dp = deleg;
out:
	if (!nfsd4_is_deleg_cur(open))
		return nfs_ok;
	if (status)
		return status;
	open->op_openowner->oo_flags |= NFS4_OO_CONFIRMED;
	return nfs_ok;
}

static inline int nfs4_access_to_access(u32 nfs4_access)
{
	int flags = 0;

	if (nfs4_access & NFS4_SHARE_ACCESS_READ)
		flags |= NFSD_MAY_READ;
	if (nfs4_access & NFS4_SHARE_ACCESS_WRITE)
		flags |= NFSD_MAY_WRITE;
	return flags;
}

static inline __be32
nfsd4_truncate(struct svc_rqst *rqstp, struct svc_fh *fh,
		struct nfsd4_open *open)
{
	struct iattr iattr = {
		.ia_valid = ATTR_SIZE,
		.ia_size = 0,
	};
	struct nfsd_attrs attrs = {
		.na_iattr	= &iattr,
	};
	if (!open->op_truncate)
		return 0;
	if (!(open->op_share_access & NFS4_SHARE_ACCESS_WRITE))
		return nfserr_inval;
	return nfsd_setattr(rqstp, fh, &attrs, NULL);
}

static __be32 nfs4_get_vfs_file(struct svc_rqst *rqstp, struct nfs4_file *fp,
		struct svc_fh *cur_fh, struct nfs4_ol_stateid *stp,
		struct nfsd4_open *open, bool new_stp)
{
	struct nfsd_file *nf = NULL;
	__be32 status;
	int oflag = nfs4_access_to_omode(open->op_share_access);
	int access = nfs4_access_to_access(open->op_share_access);
	unsigned char old_access_bmap, old_deny_bmap;

	spin_lock(&fp->fi_lock);

	/*
	 * Are we trying to set a deny mode that would conflict with
	 * current access?
	 */
	status = nfs4_file_check_deny(fp, open->op_share_deny);
	if (status != nfs_ok) {
		if (status != nfserr_share_denied) {
			spin_unlock(&fp->fi_lock);
			goto out;
		}
		if (nfs4_resolve_deny_conflicts_locked(fp, new_stp,
				stp, open->op_share_deny, false))
			status = nfserr_jukebox;
		spin_unlock(&fp->fi_lock);
		goto out;
	}

	/* set access to the file */
	status = nfs4_file_get_access(fp, open->op_share_access);
	if (status != nfs_ok) {
		if (status != nfserr_share_denied) {
			spin_unlock(&fp->fi_lock);
			goto out;
		}
		if (nfs4_resolve_deny_conflicts_locked(fp, new_stp,
				stp, open->op_share_access, true))
			status = nfserr_jukebox;
		spin_unlock(&fp->fi_lock);
		goto out;
	}

	/* Set access bits in stateid */
	old_access_bmap = stp->st_access_bmap;
	set_access(open->op_share_access, stp);

	/* Set new deny mask */
	old_deny_bmap = stp->st_deny_bmap;
	set_deny(open->op_share_deny, stp);
	fp->fi_share_deny |= (open->op_share_deny & NFS4_SHARE_DENY_BOTH);

	if (!fp->fi_fds[oflag]) {
		spin_unlock(&fp->fi_lock);

		status = nfsd_file_acquire_opened(rqstp, cur_fh, access,
						  open->op_filp, &nf);
		if (status != nfs_ok)
			goto out_put_access;

		spin_lock(&fp->fi_lock);
		if (!fp->fi_fds[oflag]) {
			fp->fi_fds[oflag] = nf;
			nf = NULL;
		}
	}
	spin_unlock(&fp->fi_lock);
	if (nf)
		nfsd_file_put(nf);

	status = nfserrno(nfsd_open_break_lease(cur_fh->fh_dentry->d_inode,
								access));
	if (status)
		goto out_put_access;

	status = nfsd4_truncate(rqstp, cur_fh, open);
	if (status)
		goto out_put_access;
out:
	return status;
out_put_access:
	stp->st_access_bmap = old_access_bmap;
	nfs4_file_put_access(fp, open->op_share_access);
	reset_union_bmap_deny(bmap_to_share_mode(old_deny_bmap), stp);
	goto out;
}

static __be32
nfs4_upgrade_open(struct svc_rqst *rqstp, struct nfs4_file *fp,
		struct svc_fh *cur_fh, struct nfs4_ol_stateid *stp,
		struct nfsd4_open *open)
{
	__be32 status;
	unsigned char old_deny_bmap = stp->st_deny_bmap;

	if (!test_access(open->op_share_access, stp))
		return nfs4_get_vfs_file(rqstp, fp, cur_fh, stp, open, false);

	/* test and set deny mode */
	spin_lock(&fp->fi_lock);
	status = nfs4_file_check_deny(fp, open->op_share_deny);
	switch (status) {
	case nfs_ok:
		set_deny(open->op_share_deny, stp);
		fp->fi_share_deny |=
			(open->op_share_deny & NFS4_SHARE_DENY_BOTH);
		break;
	case nfserr_share_denied:
		if (nfs4_resolve_deny_conflicts_locked(fp, false,
				stp, open->op_share_deny, false))
			status = nfserr_jukebox;
		break;
	}
	spin_unlock(&fp->fi_lock);

	if (status != nfs_ok)
		return status;

	status = nfsd4_truncate(rqstp, cur_fh, open);
	if (status != nfs_ok)
		reset_union_bmap_deny(old_deny_bmap, stp);
	return status;
}

/* Should we give out recallable state?: */
static bool nfsd4_cb_channel_good(struct nfs4_client *clp)
{
	if (clp->cl_cb_state == NFSD4_CB_UP)
		return true;
	/*
	 * In the sessions case, since we don't have to establish a
	 * separate connection for callbacks, we assume it's OK
	 * until we hear otherwise:
	 */
	return clp->cl_minorversion && clp->cl_cb_state == NFSD4_CB_UNKNOWN;
}

static struct file_lease *nfs4_alloc_init_lease(struct nfs4_delegation *dp)
{
	struct file_lease *fl;

	fl = locks_alloc_lease();
	if (!fl)
		return NULL;
	fl->fl_lmops = &nfsd_lease_mng_ops;
	fl->c.flc_flags = FL_DELEG;
	fl->c.flc_type = deleg_is_read(dp->dl_type) ? F_RDLCK : F_WRLCK;
	fl->c.flc_owner = (fl_owner_t)dp;
	fl->c.flc_pid = current->tgid;
	fl->c.flc_file = dp->dl_stid.sc_file->fi_deleg_file->nf_file;
	return fl;
}

static int nfsd4_check_conflicting_opens(struct nfs4_client *clp,
					 struct nfs4_file *fp)
{
	struct nfs4_ol_stateid *st;
	struct file *f = fp->fi_deleg_file->nf_file;
	struct inode *ino = file_inode(f);
	int writes;

	writes = atomic_read(&ino->i_writecount);
	if (!writes)
		return 0;
	/*
	 * There could be multiple filehandles (hence multiple
	 * nfs4_files) referencing this file, but that's not too
	 * common; let's just give up in that case rather than
	 * trying to go look up all the clients using that other
	 * nfs4_file as well:
	 */
	if (fp->fi_aliased)
		return -EAGAIN;
	/*
	 * If there's a close in progress, make sure that we see it
	 * clear any fi_fds[] entries before we see it decrement
	 * i_writecount:
	 */
	smp_mb__after_atomic();

	if (fp->fi_fds[O_WRONLY])
		writes--;
	if (fp->fi_fds[O_RDWR])
		writes--;
	if (writes > 0)
		return -EAGAIN; /* There may be non-NFSv4 writers */
	/*
	 * It's possible there are non-NFSv4 write opens in progress,
	 * but if they haven't incremented i_writecount yet then they
	 * also haven't called break lease yet; so, they'll break this
	 * lease soon enough.  So, all that's left to check for is NFSv4
	 * opens:
	 */
	spin_lock(&fp->fi_lock);
	list_for_each_entry(st, &fp->fi_stateids, st_perfile) {
		if (st->st_openstp == NULL /* it's an open */ &&
		    access_permit_write(st) &&
		    st->st_stid.sc_client != clp) {
			spin_unlock(&fp->fi_lock);
			return -EAGAIN;
		}
	}
	spin_unlock(&fp->fi_lock);
	/*
	 * There's a small chance that we could be racing with another
	 * NFSv4 open.  However, any open that hasn't added itself to
	 * the fi_stateids list also hasn't called break_lease yet; so,
	 * they'll break this lease soon enough.
	 */
	return 0;
}

/*
 * It's possible that between opening the dentry and setting the delegation,
 * that it has been renamed or unlinked. Redo the lookup to verify that this
 * hasn't happened.
 */
static int
nfsd4_verify_deleg_dentry(struct nfsd4_open *open, struct nfs4_file *fp,
			  struct svc_fh *parent)
{
	struct svc_export *exp;
	struct dentry *child;
	__be32 err;

	err = nfsd_lookup_dentry(open->op_rqstp, parent,
				 open->op_fname, open->op_fnamelen,
				 &exp, &child);

	if (err)
		return -EAGAIN;

	exp_put(exp);
	dput(child);
	if (child != file_dentry(fp->fi_deleg_file->nf_file))
		return -EAGAIN;

	return 0;
}

/*
 * We avoid breaking delegations held by a client due to its own activity, but
 * clearing setuid/setgid bits on a write is an implicit activity and the client
 * may not notice and continue using the old mode. Avoid giving out a delegation
 * on setuid/setgid files when the client is requesting an open for write.
 */
static int
nfsd4_verify_setuid_write(struct nfsd4_open *open, struct nfsd_file *nf)
{
	struct inode *inode = file_inode(nf->nf_file);

	if ((open->op_share_access & NFS4_SHARE_ACCESS_WRITE) &&
	    (inode->i_mode & (S_ISUID|S_ISGID)))
		return -EAGAIN;
	return 0;
}

#ifdef CONFIG_NFSD_V4_DELEG_TIMESTAMPS
static bool nfsd4_want_deleg_timestamps(const struct nfsd4_open *open)
{
	return open->op_deleg_want & OPEN4_SHARE_ACCESS_WANT_DELEG_TIMESTAMPS;
}
#else /* CONFIG_NFSD_V4_DELEG_TIMESTAMPS */
static bool nfsd4_want_deleg_timestamps(const struct nfsd4_open *open)
{
	return false;
}
#endif /* CONFIG NFSD_V4_DELEG_TIMESTAMPS */

static struct nfs4_delegation *
nfs4_set_delegation(struct nfsd4_open *open, struct nfs4_ol_stateid *stp,
		    struct svc_fh *parent)
{
	bool deleg_ts = nfsd4_want_deleg_timestamps(open);
	struct nfs4_client *clp = stp->st_stid.sc_client;
	struct nfs4_file *fp = stp->st_stid.sc_file;
	struct nfs4_clnt_odstate *odstate = stp->st_clnt_odstate;
	struct nfs4_delegation *dp;
	struct nfsd_file *nf = NULL;
	struct file_lease *fl;
	int status = 0;
	u32 dl_type;

	/*
	 * The fi_had_conflict and nfs_get_existing_delegation checks
	 * here are just optimizations; we'll need to recheck them at
	 * the end:
	 */
	if (fp->fi_had_conflict)
		return ERR_PTR(-EAGAIN);

	/*
	 * Try for a write delegation first. RFC8881 section 10.4 says:
	 *
	 *  "An OPEN_DELEGATE_WRITE delegation allows the client to handle,
	 *   on its own, all opens."
	 *
	 * Furthermore, section 9.1.2 says:
	 *
	 *  "In the case of READ, the server may perform the corresponding
	 *  check on the access mode, or it may choose to allow READ for
	 *  OPEN4_SHARE_ACCESS_WRITE, to accommodate clients whose WRITE
	 *  implementation may unavoidably do reads (e.g., due to buffer
	 *  cache constraints)."
	 *
	 *  We choose to offer a write delegation for OPEN with the
	 *  OPEN4_SHARE_ACCESS_WRITE access mode to accommodate such clients.
	 */
	if (open->op_share_access & NFS4_SHARE_ACCESS_WRITE) {
		nf = find_writeable_file(fp);
		dl_type = deleg_ts ? OPEN_DELEGATE_WRITE_ATTRS_DELEG : OPEN_DELEGATE_WRITE;
	}

	/*
	 * If the file is being opened O_RDONLY or we couldn't get a O_RDWR
	 * file for some reason, then try for a read delegation instead.
	 */
	if (!nf && (open->op_share_access & NFS4_SHARE_ACCESS_READ)) {
		nf = find_readable_file(fp);
		dl_type = deleg_ts ? OPEN_DELEGATE_READ_ATTRS_DELEG : OPEN_DELEGATE_READ;
	}

	if (!nf)
		return ERR_PTR(-EAGAIN);

	/*
	 * File delegations and associated locks cannot be recovered if the
	 * export is from an NFS proxy server.
	 */
	if (exportfs_cannot_lock(nf->nf_file->f_path.mnt->mnt_sb->s_export_op)) {
		nfsd_file_put(nf);
		return ERR_PTR(-EOPNOTSUPP);
	}

	spin_lock(&state_lock);
	spin_lock(&fp->fi_lock);
	if (nfs4_delegation_exists(clp, fp))
		status = -EAGAIN;
	else if (nfsd4_verify_setuid_write(open, nf))
		status = -EAGAIN;
	else if (!fp->fi_deleg_file) {
		fp->fi_deleg_file = nf;
		/* increment early to prevent fi_deleg_file from being
		 * cleared */
		fp->fi_delegees = 1;
		nf = NULL;
	} else
		fp->fi_delegees++;
	spin_unlock(&fp->fi_lock);
	spin_unlock(&state_lock);
	if (nf)
		nfsd_file_put(nf);
	if (status)
		return ERR_PTR(status);

	status = -ENOMEM;
	dp = alloc_init_deleg(clp, fp, odstate, dl_type);
	if (!dp)
		goto out_delegees;

	fl = nfs4_alloc_init_lease(dp);
	if (!fl)
		goto out_clnt_odstate;

	status = kernel_setlease(fp->fi_deleg_file->nf_file,
				      fl->c.flc_type, &fl, NULL);
	if (fl)
		locks_free_lease(fl);
	if (status)
		goto out_clnt_odstate;

	if (parent) {
		status = nfsd4_verify_deleg_dentry(open, fp, parent);
		if (status)
			goto out_unlock;
	}

	status = nfsd4_check_conflicting_opens(clp, fp);
	if (status)
		goto out_unlock;

	/*
	 * Now that the deleg is set, check again to ensure that nothing
	 * raced in and changed the mode while we weren't looking.
	 */
	status = nfsd4_verify_setuid_write(open, fp->fi_deleg_file);
	if (status)
		goto out_unlock;

	status = -EAGAIN;
	if (fp->fi_had_conflict)
		goto out_unlock;

	spin_lock(&state_lock);
	spin_lock(&clp->cl_lock);
	spin_lock(&fp->fi_lock);
	status = hash_delegation_locked(dp, fp);
	spin_unlock(&fp->fi_lock);
	spin_unlock(&clp->cl_lock);
	spin_unlock(&state_lock);

	if (status)
		goto out_unlock;

	return dp;
out_unlock:
	kernel_setlease(fp->fi_deleg_file->nf_file, F_UNLCK, NULL, (void **)&dp);
out_clnt_odstate:
	put_clnt_odstate(dp->dl_clnt_odstate);
	nfs4_put_stid(&dp->dl_stid);
out_delegees:
	put_deleg_file(fp);
	return ERR_PTR(status);
}

static void nfsd4_open_deleg_none_ext(struct nfsd4_open *open, int status)
{
	open->op_delegate_type = OPEN_DELEGATE_NONE_EXT;
	if (status == -EAGAIN)
		open->op_why_no_deleg = WND4_CONTENTION;
	else {
		open->op_why_no_deleg = WND4_RESOURCE;
		switch (open->op_deleg_want) {
		case OPEN4_SHARE_ACCESS_WANT_READ_DELEG:
		case OPEN4_SHARE_ACCESS_WANT_WRITE_DELEG:
		case OPEN4_SHARE_ACCESS_WANT_ANY_DELEG:
			break;
		case OPEN4_SHARE_ACCESS_WANT_CANCEL:
			open->op_why_no_deleg = WND4_CANCELLED;
			break;
		case OPEN4_SHARE_ACCESS_WANT_NO_DELEG:
			WARN_ON_ONCE(1);
		}
	}
}

static bool
nfs4_delegation_stat(struct nfs4_delegation *dp, struct svc_fh *currentfh,
		     struct kstat *stat)
{
	struct nfsd_file *nf = find_writeable_file(dp->dl_stid.sc_file);
	struct path path;
	int rc;

	if (!nf)
		return false;

	path.mnt = currentfh->fh_export->ex_path.mnt;
	path.dentry = file_dentry(nf->nf_file);

	rc = vfs_getattr(&path, stat,
			 (STATX_MODE | STATX_SIZE | STATX_CTIME | STATX_CHANGE_COOKIE),
			 AT_STATX_SYNC_AS_STAT);

	nfsd_file_put(nf);
	return rc == 0;
}

/*
 * Add NFS4_SHARE_ACCESS_READ to the write delegation granted on OPEN
 * with NFS4_SHARE_ACCESS_WRITE by allocating separate nfsd_file and
 * struct file to be used for read with delegation stateid.
 *
 */
static bool
nfsd4_add_rdaccess_to_wrdeleg(struct svc_rqst *rqstp, struct nfsd4_open *open,
			      struct svc_fh *fh, struct nfs4_ol_stateid *stp)
{
	struct nfs4_file *fp;
	struct nfsd_file *nf = NULL;

	if ((open->op_share_access & NFS4_SHARE_ACCESS_BOTH) ==
			NFS4_SHARE_ACCESS_WRITE) {
		if (nfsd_file_acquire_opened(rqstp, fh, NFSD_MAY_READ, NULL, &nf))
			return (false);
		fp = stp->st_stid.sc_file;
		spin_lock(&fp->fi_lock);
		__nfs4_file_get_access(fp, NFS4_SHARE_ACCESS_READ);
		fp = stp->st_stid.sc_file;
		fp->fi_fds[O_RDONLY] = nf;
		fp->fi_rdeleg_file = nf;
		spin_unlock(&fp->fi_lock);
	}
	return true;
}

/*
 * The Linux NFS server does not offer write delegations to NFSv4.0
 * clients in order to avoid conflicts between write delegations and
 * GETATTRs requesting CHANGE or SIZE attributes.
 *
 * With NFSv4.1 and later minorversions, the SEQUENCE operation that
 * begins each COMPOUND contains a client ID. Delegation recall can
 * be avoided when the server recognizes the client sending a
 * GETATTR also holds write delegation it conflicts with.
 *
 * However, the NFSv4.0 protocol does not enable a server to
 * determine that a GETATTR originated from the client holding the
 * conflicting delegation versus coming from some other client. Per
 * RFC 7530 Section 16.7.5, the server must recall or send a
 * CB_GETATTR even when the GETATTR originates from the client that
 * holds the conflicting delegation.
 *
 * An NFSv4.0 client can trigger a pathological situation if it
 * always sends a DELEGRETURN preceded by a conflicting GETATTR in
 * the same COMPOUND. COMPOUND execution will always stop at the
 * GETATTR and the DELEGRETURN will never get executed. The server
 * eventually revokes the delegation, which can result in loss of
 * open or lock state.
 */
static void
nfs4_open_delegation(struct svc_rqst *rqstp, struct nfsd4_open *open,
		     struct nfs4_ol_stateid *stp, struct svc_fh *currentfh,
		     struct svc_fh *fh)
{
	struct nfs4_openowner *oo = openowner(stp->st_stateowner);
	bool deleg_ts = nfsd4_want_deleg_timestamps(open);
	struct nfs4_client *clp = stp->st_stid.sc_client;
	struct svc_fh *parent = NULL;
	struct nfs4_delegation *dp;
	struct kstat stat;
	int status = 0;
	int cb_up;

	cb_up = nfsd4_cb_channel_good(oo->oo_owner.so_client);
	open->op_recall = false;
	switch (open->op_claim_type) {
		case NFS4_OPEN_CLAIM_PREVIOUS:
			if (!cb_up)
				open->op_recall = true;
			break;
		case NFS4_OPEN_CLAIM_NULL:
			parent = currentfh;
			fallthrough;
		case NFS4_OPEN_CLAIM_FH:
			/*
			 * Let's not give out any delegations till everyone's
			 * had the chance to reclaim theirs, *and* until
			 * NLM locks have all been reclaimed:
			 */
			if (locks_in_grace(clp->net))
				goto out_no_deleg;
			if (!cb_up || !(oo->oo_flags & NFS4_OO_CONFIRMED))
				goto out_no_deleg;
			if (open->op_share_access & NFS4_SHARE_ACCESS_WRITE &&
					!clp->cl_minorversion)
				goto out_no_deleg;
			break;
		default:
			goto out_no_deleg;
	}
	dp = nfs4_set_delegation(open, stp, parent);
	if (IS_ERR(dp))
		goto out_no_deleg;

	memcpy(&open->op_delegate_stateid, &dp->dl_stid.sc_stateid, sizeof(dp->dl_stid.sc_stateid));

	if (open->op_share_access & NFS4_SHARE_ACCESS_WRITE) {
		if (!nfsd4_add_rdaccess_to_wrdeleg(rqstp, open, fh, stp) ||
				!nfs4_delegation_stat(dp, currentfh, &stat)) {
			nfs4_put_stid(&dp->dl_stid);
			destroy_delegation(dp);
			goto out_no_deleg;
		}
		open->op_delegate_type = deleg_ts ? OPEN_DELEGATE_WRITE_ATTRS_DELEG :
						    OPEN_DELEGATE_WRITE;
		dp->dl_cb_fattr.ncf_cur_fsize = stat.size;
		dp->dl_cb_fattr.ncf_initial_cinfo = nfsd4_change_attribute(&stat);
		trace_nfsd_deleg_write(&dp->dl_stid.sc_stateid);
	} else {
		open->op_delegate_type = deleg_ts ? OPEN_DELEGATE_READ_ATTRS_DELEG :
						    OPEN_DELEGATE_READ;
		trace_nfsd_deleg_read(&dp->dl_stid.sc_stateid);
	}
	nfs4_put_stid(&dp->dl_stid);
	return;
out_no_deleg:
	open->op_delegate_type = OPEN_DELEGATE_NONE;
	if (open->op_claim_type == NFS4_OPEN_CLAIM_PREVIOUS &&
	    open->op_delegate_type != OPEN_DELEGATE_NONE) {
		dprintk("NFSD: WARNING: refusing delegation reclaim\n");
		open->op_recall = true;
	}

	/* 4.1 client asking for a delegation? */
	if (open->op_deleg_want)
		nfsd4_open_deleg_none_ext(open, status);
	return;
}

static void nfsd4_deleg_xgrade_none_ext(struct nfsd4_open *open,
					struct nfs4_delegation *dp)
{
	if (deleg_is_write(dp->dl_type)) {
		if (open->op_deleg_want & OPEN4_SHARE_ACCESS_WANT_READ_DELEG) {
			open->op_delegate_type = OPEN_DELEGATE_NONE_EXT;
			open->op_why_no_deleg = WND4_NOT_SUPP_DOWNGRADE;
		} else if (open->op_deleg_want & OPEN4_SHARE_ACCESS_WANT_WRITE_DELEG) {
			open->op_delegate_type = OPEN_DELEGATE_NONE_EXT;
			open->op_why_no_deleg = WND4_NOT_SUPP_UPGRADE;
		}
	}
	/* Otherwise the client must be confused wanting a delegation
	 * it already has, therefore we don't return
	 * OPEN_DELEGATE_NONE_EXT and reason.
	 */
}

/* Are we returning only a delegation stateid? */
static bool open_xor_delegation(struct nfsd4_open *open)
{
	if (!(open->op_deleg_want & OPEN4_SHARE_ACCESS_WANT_OPEN_XOR_DELEGATION))
		return false;
	/* Did we actually get a delegation? */
	if (!deleg_is_read(open->op_delegate_type) && !deleg_is_write(open->op_delegate_type))
		return false;
	return true;
}

/**
 * nfsd4_process_open2 - finish open processing
 * @rqstp: the RPC transaction being executed
 * @current_fh: NFSv4 COMPOUND's current filehandle
 * @open: OPEN arguments
 *
 * If successful, (1) truncate the file if open->op_truncate was
 * set, (2) set open->op_stateid, (3) set open->op_delegation.
 *
 * Returns %nfs_ok on success; otherwise an nfs4stat value in
 * network byte order is returned.
 */
__be32
nfsd4_process_open2(struct svc_rqst *rqstp, struct svc_fh *current_fh, struct nfsd4_open *open)
{
	struct nfsd4_compoundres *resp = rqstp->rq_resp;
	struct nfs4_client *cl = open->op_openowner->oo_owner.so_client;
	struct nfs4_file *fp = NULL;
	struct nfs4_ol_stateid *stp = NULL;
	struct nfs4_delegation *dp = NULL;
	__be32 status;
	bool new_stp = false;

	/*
	 * Lookup file; if found, lookup stateid and check open request,
	 * and check for delegations in the process of being recalled.
	 * If not found, create the nfs4_file struct
	 */
	fp = nfsd4_file_hash_insert(open->op_file, current_fh);
	if (unlikely(!fp))
		return nfserr_jukebox;
	if (fp != open->op_file) {
		status = nfs4_check_deleg(cl, open, &dp);
		if (status)
			goto out;
		if (dp && nfsd4_is_deleg_cur(open) &&
				(dp->dl_stid.sc_file != fp)) {
			/*
			 * RFC8881 section 8.2.4 mandates the server to return
			 * NFS4ERR_BAD_STATEID if the selected table entry does
			 * not match the current filehandle. However returning
			 * NFS4ERR_BAD_STATEID in the OPEN can cause the client
			 * to repeatedly retry the operation with the same
			 * stateid, since the stateid itself is valid. To avoid
			 * this situation NFSD returns NFS4ERR_INVAL instead.
			 */
			status = nfserr_inval;
			goto out;
		}
		stp = nfsd4_find_and_lock_existing_open(fp, open);
	} else {
		open->op_file = NULL;
		status = nfserr_bad_stateid;
		if (nfsd4_is_deleg_cur(open))
			goto out;
	}

	if (!stp) {
		stp = init_open_stateid(fp, open);
		if (!stp) {
			status = nfserr_jukebox;
			goto out;
		}

		if (!open->op_stp)
			new_stp = true;
	}

	/*
	 * OPEN the file, or upgrade an existing OPEN.
	 * If truncate fails, the OPEN fails.
	 *
	 * stp is already locked.
	 */
	if (!new_stp) {
		/* Stateid was found, this is an OPEN upgrade */
		status = nfs4_upgrade_open(rqstp, fp, current_fh, stp, open);
		if (status) {
			mutex_unlock(&stp->st_mutex);
			goto out;
		}
	} else {
		status = nfs4_get_vfs_file(rqstp, fp, current_fh, stp, open, true);
		if (status) {
			release_open_stateid(stp);
			mutex_unlock(&stp->st_mutex);
			goto out;
		}

		stp->st_clnt_odstate = find_or_hash_clnt_odstate(fp,
							open->op_odstate);
		if (stp->st_clnt_odstate == open->op_odstate)
			open->op_odstate = NULL;
	}

	nfs4_inc_and_copy_stateid(&open->op_stateid, &stp->st_stid);
	mutex_unlock(&stp->st_mutex);

	if (nfsd4_has_session(&resp->cstate)) {
		if (open->op_deleg_want & OPEN4_SHARE_ACCESS_WANT_NO_DELEG) {
			open->op_delegate_type = OPEN_DELEGATE_NONE_EXT;
			open->op_why_no_deleg = WND4_NOT_WANTED;
			goto nodeleg;
		}
	}

	/*
	* Attempt to hand out a delegation. No error return, because the
	* OPEN succeeds even if we fail.
	*/
	nfs4_open_delegation(rqstp, open, stp,
		&resp->cstate.current_fh, current_fh);

	/*
	 * If there is an existing open stateid, it must be updated and
	 * returned. Only respect WANT_OPEN_XOR_DELEGATION when a new
	 * open stateid would have to be created.
	 */
	if (new_stp && open_xor_delegation(open)) {
		memcpy(&open->op_stateid, &zero_stateid, sizeof(open->op_stateid));
		open->op_rflags |= OPEN4_RESULT_NO_OPEN_STATEID;
		release_open_stateid(stp);
	}
nodeleg:
	status = nfs_ok;
	trace_nfsd_open(&stp->st_stid.sc_stateid);
out:
	/* 4.1 client trying to upgrade/downgrade delegation? */
	if (open->op_delegate_type == OPEN_DELEGATE_NONE && dp &&
	    open->op_deleg_want)
		nfsd4_deleg_xgrade_none_ext(open, dp);

	if (fp)
		put_nfs4_file(fp);
	if (status == 0 && open->op_claim_type == NFS4_OPEN_CLAIM_PREVIOUS)
		open->op_openowner->oo_flags |= NFS4_OO_CONFIRMED;
	/*
	* To finish the open response, we just need to set the rflags.
	*/
	open->op_rflags |= NFS4_OPEN_RESULT_LOCKTYPE_POSIX;
	if (nfsd4_has_session(&resp->cstate))
		open->op_rflags |= NFS4_OPEN_RESULT_MAY_NOTIFY_LOCK;
	else if (!(open->op_openowner->oo_flags & NFS4_OO_CONFIRMED))
		open->op_rflags |= NFS4_OPEN_RESULT_CONFIRM;

	if (dp)
		nfs4_put_stid(&dp->dl_stid);
	if (stp)
		nfs4_put_stid(&stp->st_stid);

	return status;
}

void nfsd4_cleanup_open_state(struct nfsd4_compound_state *cstate,
			      struct nfsd4_open *open)
{
	if (open->op_openowner)
		nfs4_put_stateowner(&open->op_openowner->oo_owner);
	if (open->op_file)
		kmem_cache_free(file_slab, open->op_file);
	if (open->op_stp)
		nfs4_put_stid(&open->op_stp->st_stid);
	if (open->op_odstate)
		kmem_cache_free(odstate_slab, open->op_odstate);
}

__be32
nfsd4_renew(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
	    union nfsd4_op_u *u)
{
	clientid_t *clid = &u->renew;
	struct nfs4_client *clp;
	__be32 status;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	trace_nfsd_clid_renew(clid);
	status = set_client(clid, cstate, nn);
	if (status)
		return status;
	clp = cstate->clp;
	if (!list_empty(&clp->cl_delegations)
			&& clp->cl_cb_state != NFSD4_CB_UP)
		return nfserr_cb_path_down;
	return nfs_ok;
}

void
nfsd4_end_grace(struct nfsd_net *nn)
{
	/* do nothing if grace period already ended */
	if (nn->grace_ended)
		return;

	trace_nfsd_grace_complete(nn);
	nn->grace_ended = true;
	/*
	 * If the server goes down again right now, an NFSv4
	 * client will still be allowed to reclaim after it comes back up,
	 * even if it hasn't yet had a chance to reclaim state this time.
	 *
	 */
	nfsd4_record_grace_done(nn);
	/*
	 * At this point, NFSv4 clients can still reclaim.  But if the
	 * server crashes, any that have not yet reclaimed will be out
	 * of luck on the next boot.
	 *
	 * (NFSv4.1+ clients are considered to have reclaimed once they
	 * call RECLAIM_COMPLETE.  NFSv4.0 clients are considered to
	 * have reclaimed after their first OPEN.)
	 */
	locks_end_grace(&nn->nfsd4_manager);
	/*
	 * At this point, and once lockd and/or any other containers
	 * exit their grace period, further reclaims will fail and
	 * regular locking can resume.
	 */
}

/*
 * If we've waited a lease period but there are still clients trying to
 * reclaim, wait a little longer to give them a chance to finish.
 */
static bool clients_still_reclaiming(struct nfsd_net *nn)
{
	time64_t double_grace_period_end = nn->boot_time +
					   2 * nn->nfsd4_lease;

	if (nn->track_reclaim_completes &&
			atomic_read(&nn->nr_reclaim_complete) ==
			nn->reclaim_str_hashtbl_size)
		return false;
	if (!nn->somebody_reclaimed)
		return false;
	nn->somebody_reclaimed = false;
	/*
	 * If we've given them *two* lease times to reclaim, and they're
	 * still not done, give up:
	 */
	if (ktime_get_boottime_seconds() > double_grace_period_end)
		return false;
	return true;
}

struct laundry_time {
	time64_t cutoff;
	time64_t new_timeo;
};

static bool state_expired(struct laundry_time *lt, time64_t last_refresh)
{
	time64_t time_remaining;

	if (last_refresh < lt->cutoff)
		return true;
	time_remaining = last_refresh - lt->cutoff;
	lt->new_timeo = min(lt->new_timeo, time_remaining);
	return false;
}

#ifdef CONFIG_NFSD_V4_2_INTER_SSC
void nfsd4_ssc_init_umount_work(struct nfsd_net *nn)
{
	spin_lock_init(&nn->nfsd_ssc_lock);
	INIT_LIST_HEAD(&nn->nfsd_ssc_mount_list);
	init_waitqueue_head(&nn->nfsd_ssc_waitq);
}

/*
 * This is called when nfsd is being shutdown, after all inter_ssc
 * cleanup were done, to destroy the ssc delayed unmount list.
 */
static void nfsd4_ssc_shutdown_umount(struct nfsd_net *nn)
{
	struct nfsd4_ssc_umount_item *ni = NULL;
	struct nfsd4_ssc_umount_item *tmp;

	spin_lock(&nn->nfsd_ssc_lock);
	list_for_each_entry_safe(ni, tmp, &nn->nfsd_ssc_mount_list, nsui_list) {
		list_del(&ni->nsui_list);
		spin_unlock(&nn->nfsd_ssc_lock);
		mntput(ni->nsui_vfsmount);
		kfree(ni);
		spin_lock(&nn->nfsd_ssc_lock);
	}
	spin_unlock(&nn->nfsd_ssc_lock);
}

static void nfsd4_ssc_expire_umount(struct nfsd_net *nn)
{
	bool do_wakeup = false;
	struct nfsd4_ssc_umount_item *ni = NULL;
	struct nfsd4_ssc_umount_item *tmp;

	spin_lock(&nn->nfsd_ssc_lock);
	list_for_each_entry_safe(ni, tmp, &nn->nfsd_ssc_mount_list, nsui_list) {
		if (time_after(jiffies, ni->nsui_expire)) {
			if (refcount_read(&ni->nsui_refcnt) > 1)
				continue;

			/* mark being unmount */
			ni->nsui_busy = true;
			spin_unlock(&nn->nfsd_ssc_lock);
			mntput(ni->nsui_vfsmount);
			spin_lock(&nn->nfsd_ssc_lock);

			/* waiters need to start from begin of list */
			list_del(&ni->nsui_list);
			kfree(ni);

			/* wakeup ssc_connect waiters */
			do_wakeup = true;
			continue;
		}
		break;
	}
	if (do_wakeup)
		wake_up_all(&nn->nfsd_ssc_waitq);
	spin_unlock(&nn->nfsd_ssc_lock);
}
#endif

/* Check if any lock belonging to this lockowner has any blockers */
static bool
nfs4_lockowner_has_blockers(struct nfs4_lockowner *lo)
{
	struct file_lock_context *ctx;
	struct nfs4_ol_stateid *stp;
	struct nfs4_file *nf;

	list_for_each_entry(stp, &lo->lo_owner.so_stateids, st_perstateowner) {
		nf = stp->st_stid.sc_file;
		ctx = locks_inode_context(nf->fi_inode);
		if (!ctx)
			continue;
		if (locks_owner_has_blockers(ctx, lo))
			return true;
	}
	return false;
}

static bool
nfs4_anylock_blockers(struct nfs4_client *clp)
{
	int i;
	struct nfs4_stateowner *so;
	struct nfs4_lockowner *lo;

	if (atomic_read(&clp->cl_delegs_in_recall))
		return true;
	spin_lock(&clp->cl_lock);
	for (i = 0; i < OWNER_HASH_SIZE; i++) {
		list_for_each_entry(so, &clp->cl_ownerstr_hashtbl[i],
				so_strhash) {
			if (so->so_is_open_owner)
				continue;
			lo = lockowner(so);
			if (nfs4_lockowner_has_blockers(lo)) {
				spin_unlock(&clp->cl_lock);
				return true;
			}
		}
	}
	spin_unlock(&clp->cl_lock);
	return false;
}

static void
nfs4_get_client_reaplist(struct nfsd_net *nn, struct list_head *reaplist,
				struct laundry_time *lt)
{
	unsigned int maxreap, reapcnt = 0;
	struct list_head *pos, *next;
	struct nfs4_client *clp;

	maxreap = (atomic_read(&nn->nfs4_client_count) >= nn->nfs4_max_clients) ?
			NFSD_CLIENT_MAX_TRIM_PER_RUN : 0;
	INIT_LIST_HEAD(reaplist);
	spin_lock(&nn->client_lock);
	list_for_each_safe(pos, next, &nn->client_lru) {
		clp = list_entry(pos, struct nfs4_client, cl_lru);
		if (clp->cl_state == NFSD4_EXPIRABLE)
			goto exp_client;
		if (!state_expired(lt, clp->cl_time))
			break;
		if (!atomic_read(&clp->cl_rpc_users)) {
			if (clp->cl_state == NFSD4_ACTIVE)
				atomic_inc(&nn->nfsd_courtesy_clients);
			clp->cl_state = NFSD4_COURTESY;
		}
		if (!client_has_state(clp))
			goto exp_client;
		if (!nfs4_anylock_blockers(clp))
			if (reapcnt >= maxreap)
				continue;
exp_client:
		if (!mark_client_expired_locked(clp)) {
			list_add(&clp->cl_lru, reaplist);
			reapcnt++;
		}
	}
	spin_unlock(&nn->client_lock);
}

static void
nfs4_get_courtesy_client_reaplist(struct nfsd_net *nn,
				struct list_head *reaplist)
{
	unsigned int maxreap = 0, reapcnt = 0;
	struct list_head *pos, *next;
	struct nfs4_client *clp;

	maxreap = NFSD_CLIENT_MAX_TRIM_PER_RUN;
	INIT_LIST_HEAD(reaplist);

	spin_lock(&nn->client_lock);
	list_for_each_safe(pos, next, &nn->client_lru) {
		clp = list_entry(pos, struct nfs4_client, cl_lru);
		if (clp->cl_state == NFSD4_ACTIVE)
			break;
		if (reapcnt >= maxreap)
			break;
		if (!mark_client_expired_locked(clp)) {
			list_add(&clp->cl_lru, reaplist);
			reapcnt++;
		}
	}
	spin_unlock(&nn->client_lock);
}

static void
nfs4_process_client_reaplist(struct list_head *reaplist)
{
	struct list_head *pos, *next;
	struct nfs4_client *clp;

	list_for_each_safe(pos, next, reaplist) {
		clp = list_entry(pos, struct nfs4_client, cl_lru);
		trace_nfsd_clid_purged(&clp->cl_clientid);
		list_del_init(&clp->cl_lru);
		expire_client(clp);
	}
}

static void nfs40_clean_admin_revoked(struct nfsd_net *nn,
				      struct laundry_time *lt)
{
	struct nfs4_client *clp;

	spin_lock(&nn->client_lock);
	if (nn->nfs40_last_revoke == 0 ||
	    nn->nfs40_last_revoke > lt->cutoff) {
		spin_unlock(&nn->client_lock);
		return;
	}
	nn->nfs40_last_revoke = 0;

retry:
	list_for_each_entry(clp, &nn->client_lru, cl_lru) {
		unsigned long id, tmp;
		struct nfs4_stid *stid;

		if (atomic_read(&clp->cl_admin_revoked) == 0)
			continue;

		spin_lock(&clp->cl_lock);
		idr_for_each_entry_ul(&clp->cl_stateids, stid, tmp, id)
			if (stid->sc_status & SC_STATUS_ADMIN_REVOKED) {
				refcount_inc(&stid->sc_count);
				spin_unlock(&nn->client_lock);
				/* this function drops ->cl_lock */
				nfsd4_drop_revoked_stid(stid);
				nfs4_put_stid(stid);
				spin_lock(&nn->client_lock);
				goto retry;
			}
		spin_unlock(&clp->cl_lock);
	}
	spin_unlock(&nn->client_lock);
}

static time64_t
nfs4_laundromat(struct nfsd_net *nn)
{
	struct nfs4_openowner *oo;
	struct nfs4_delegation *dp;
	struct nfs4_ol_stateid *stp;
	struct nfsd4_blocked_lock *nbl;
	struct list_head *pos, *next, reaplist;
	struct laundry_time lt = {
		.cutoff = ktime_get_boottime_seconds() - nn->nfsd4_lease,
		.new_timeo = nn->nfsd4_lease
	};
	struct nfs4_cpntf_state *cps;
	copy_stateid_t *cps_t;
	int i;

	if (clients_still_reclaiming(nn)) {
		lt.new_timeo = 0;
		goto out;
	}
	nfsd4_end_grace(nn);

	spin_lock(&nn->s2s_cp_lock);
	idr_for_each_entry(&nn->s2s_cp_stateids, cps_t, i) {
		cps = container_of(cps_t, struct nfs4_cpntf_state, cp_stateid);
		if (cps->cp_stateid.cs_type == NFS4_COPYNOTIFY_STID &&
				state_expired(&lt, cps->cpntf_time))
			_free_cpntf_state_locked(nn, cps);
	}
	spin_unlock(&nn->s2s_cp_lock);
	nfsd4_async_copy_reaper(nn);
	nfs4_get_client_reaplist(nn, &reaplist, &lt);
	nfs4_process_client_reaplist(&reaplist);

	nfs40_clean_admin_revoked(nn, &lt);

	spin_lock(&state_lock);
	list_for_each_safe(pos, next, &nn->del_recall_lru) {
		dp = list_entry (pos, struct nfs4_delegation, dl_recall_lru);
		if (!state_expired(&lt, dp->dl_time))
			break;
		refcount_inc(&dp->dl_stid.sc_count);
		unhash_delegation_locked(dp, SC_STATUS_REVOKED);
		list_add(&dp->dl_recall_lru, &reaplist);
	}
	spin_unlock(&state_lock);
	while (!list_empty(&reaplist)) {
		dp = list_first_entry(&reaplist, struct nfs4_delegation,
					dl_recall_lru);
		list_del_init(&dp->dl_recall_lru);
		revoke_delegation(dp);
	}

	spin_lock(&nn->client_lock);
	while (!list_empty(&nn->close_lru)) {
		oo = list_first_entry(&nn->close_lru, struct nfs4_openowner,
					oo_close_lru);
		if (!state_expired(&lt, oo->oo_time))
			break;
		list_del_init(&oo->oo_close_lru);
		stp = oo->oo_last_closed_stid;
		oo->oo_last_closed_stid = NULL;
		spin_unlock(&nn->client_lock);
		nfs4_put_stid(&stp->st_stid);
		spin_lock(&nn->client_lock);
	}
	spin_unlock(&nn->client_lock);

	/*
	 * It's possible for a client to try and acquire an already held lock
	 * that is being held for a long time, and then lose interest in it.
	 * So, we clean out any un-revisited request after a lease period
	 * under the assumption that the client is no longer interested.
	 *
	 * RFC5661, sec. 9.6 states that the client must not rely on getting
	 * notifications and must continue to poll for locks, even when the
	 * server supports them. Thus this shouldn't lead to clients blocking
	 * indefinitely once the lock does become free.
	 */
	BUG_ON(!list_empty(&reaplist));
	spin_lock(&nn->blocked_locks_lock);
	while (!list_empty(&nn->blocked_locks_lru)) {
		nbl = list_first_entry(&nn->blocked_locks_lru,
					struct nfsd4_blocked_lock, nbl_lru);
		if (!state_expired(&lt, nbl->nbl_time))
			break;
		list_move(&nbl->nbl_lru, &reaplist);
		list_del_init(&nbl->nbl_list);
	}
	spin_unlock(&nn->blocked_locks_lock);

	while (!list_empty(&reaplist)) {
		nbl = list_first_entry(&reaplist,
					struct nfsd4_blocked_lock, nbl_lru);
		list_del_init(&nbl->nbl_lru);
		free_blocked_lock(nbl);
	}
#ifdef CONFIG_NFSD_V4_2_INTER_SSC
	/* service the server-to-server copy delayed unmount list */
	nfsd4_ssc_expire_umount(nn);
#endif
	if (atomic_long_read(&num_delegations) >= max_delegations)
		deleg_reaper(nn);
out:
	return max_t(time64_t, lt.new_timeo, NFSD_LAUNDROMAT_MINTIMEOUT);
}

static void laundromat_main(struct work_struct *);

static void
laundromat_main(struct work_struct *laundry)
{
	time64_t t;
	struct delayed_work *dwork = to_delayed_work(laundry);
	struct nfsd_net *nn = container_of(dwork, struct nfsd_net,
					   laundromat_work);

	t = nfs4_laundromat(nn);
	queue_delayed_work(laundry_wq, &nn->laundromat_work, t*HZ);
}

static void
courtesy_client_reaper(struct nfsd_net *nn)
{
	struct list_head reaplist;

	nfs4_get_courtesy_client_reaplist(nn, &reaplist);
	nfs4_process_client_reaplist(&reaplist);
}

static void
deleg_reaper(struct nfsd_net *nn)
{
	struct list_head *pos, *next;
	struct nfs4_client *clp;

	spin_lock(&nn->client_lock);
	list_for_each_safe(pos, next, &nn->client_lru) {
		clp = list_entry(pos, struct nfs4_client, cl_lru);

		if (clp->cl_state != NFSD4_ACTIVE)
			continue;
		if (list_empty(&clp->cl_delegations))
			continue;
		if (atomic_read(&clp->cl_delegs_in_recall))
			continue;
		if (test_and_set_bit(NFSD4_CALLBACK_RUNNING, &clp->cl_ra->ra_cb.cb_flags))
			continue;
		if (ktime_get_boottime_seconds() - clp->cl_ra_time < 5)
			continue;
		if (clp->cl_cb_state != NFSD4_CB_UP)
			continue;

		/* release in nfsd4_cb_recall_any_release */
		kref_get(&clp->cl_nfsdfs.cl_ref);
		clp->cl_ra_time = ktime_get_boottime_seconds();
		clp->cl_ra->ra_keep = 0;
		clp->cl_ra->ra_bmval[0] = BIT(RCA4_TYPE_MASK_RDATA_DLG) |
						BIT(RCA4_TYPE_MASK_WDATA_DLG);
		trace_nfsd_cb_recall_any(clp->cl_ra);
		nfsd4_run_cb(&clp->cl_ra->ra_cb);
	}
	spin_unlock(&nn->client_lock);
}

static void
nfsd4_state_shrinker_worker(struct work_struct *work)
{
	struct nfsd_net *nn = container_of(work, struct nfsd_net,
				nfsd_shrinker_work);

	courtesy_client_reaper(nn);
	deleg_reaper(nn);
}

static inline __be32 nfs4_check_fh(struct svc_fh *fhp, struct nfs4_stid *stp)
{
	if (!fh_match(&fhp->fh_handle, &stp->sc_file->fi_fhandle))
		return nfserr_bad_stateid;
	return nfs_ok;
}

static
__be32 nfs4_check_openmode(struct nfs4_ol_stateid *stp, int flags)
{
        __be32 status = nfserr_openmode;

	/* For lock stateid's, we test the parent open, not the lock: */
	if (stp->st_openstp)
		stp = stp->st_openstp;
	if ((flags & WR_STATE) && !access_permit_write(stp))
                goto out;
	if ((flags & RD_STATE) && !access_permit_read(stp))
                goto out;
	status = nfs_ok;
out:
	return status;
}

static inline __be32
check_special_stateids(struct net *net, svc_fh *current_fh, stateid_t *stateid, int flags)
{
	if (ONE_STATEID(stateid) && (flags & RD_STATE))
		return nfs_ok;
	else if (opens_in_grace(net)) {
		/* Answer in remaining cases depends on existence of
		 * conflicting state; so we must wait out the grace period. */
		return nfserr_grace;
	} else if (flags & WR_STATE)
		return nfs4_share_conflict(current_fh,
				NFS4_SHARE_DENY_WRITE);
	else /* (flags & RD_STATE) && ZERO_STATEID(stateid) */
		return nfs4_share_conflict(current_fh,
				NFS4_SHARE_DENY_READ);
}

static __be32 check_stateid_generation(stateid_t *in, stateid_t *ref, bool has_session)
{
	/*
	 * When sessions are used the stateid generation number is ignored
	 * when it is zero.
	 */
	if (has_session && in->si_generation == 0)
		return nfs_ok;

	if (in->si_generation == ref->si_generation)
		return nfs_ok;

	/* If the client sends us a stateid from the future, it's buggy: */
	if (nfsd4_stateid_generation_after(in, ref))
		return nfserr_bad_stateid;
	/*
	 * However, we could see a stateid from the past, even from a
	 * non-buggy client.  For example, if the client sends a lock
	 * while some IO is outstanding, the lock may bump si_generation
	 * while the IO is still in flight.  The client could avoid that
	 * situation by waiting for responses on all the IO requests,
	 * but better performance may result in retrying IO that
	 * receives an old_stateid error if requests are rarely
	 * reordered in flight:
	 */
	return nfserr_old_stateid;
}

static __be32 nfsd4_stid_check_stateid_generation(stateid_t *in, struct nfs4_stid *s, bool has_session)
{
	__be32 ret;

	spin_lock(&s->sc_lock);
	ret = nfsd4_verify_open_stid(s);
	if (ret == nfs_ok)
		ret = check_stateid_generation(in, &s->sc_stateid, has_session);
	spin_unlock(&s->sc_lock);
	if (ret == nfserr_admin_revoked)
		nfsd40_drop_revoked_stid(s->sc_client,
					&s->sc_stateid);
	return ret;
}

static __be32 nfsd4_check_openowner_confirmed(struct nfs4_ol_stateid *ols)
{
	if (ols->st_stateowner->so_is_open_owner &&
	    !(openowner(ols->st_stateowner)->oo_flags & NFS4_OO_CONFIRMED))
		return nfserr_bad_stateid;
	return nfs_ok;
}

static __be32 nfsd4_validate_stateid(struct nfs4_client *cl, stateid_t *stateid)
{
	struct nfs4_stid *s;
	__be32 status = nfserr_bad_stateid;

	if (ZERO_STATEID(stateid) || ONE_STATEID(stateid) ||
		CLOSE_STATEID(stateid))
		return status;
	spin_lock(&cl->cl_lock);
	s = find_stateid_locked(cl, stateid);
	if (!s)
		goto out_unlock;
	status = nfsd4_stid_check_stateid_generation(stateid, s, 1);
	if (status)
		goto out_unlock;
	status = nfsd4_verify_open_stid(s);
	if (status)
		goto out_unlock;

	switch (s->sc_type) {
	case SC_TYPE_DELEG:
		status = nfs_ok;
		break;
	case SC_TYPE_OPEN:
	case SC_TYPE_LOCK:
		status = nfsd4_check_openowner_confirmed(openlockstateid(s));
		break;
	default:
		printk("unknown stateid type %x\n", s->sc_type);
		status = nfserr_bad_stateid;
	}
out_unlock:
	spin_unlock(&cl->cl_lock);
	if (status == nfserr_admin_revoked)
		nfsd40_drop_revoked_stid(cl, stateid);
	return status;
}

__be32
nfsd4_lookup_stateid(struct nfsd4_compound_state *cstate,
		     stateid_t *stateid,
		     unsigned short typemask, unsigned short statusmask,
		     struct nfs4_stid **s, struct nfsd_net *nn)
{
	__be32 status;
	struct nfs4_stid *stid;
	bool return_revoked = false;

	/*
	 *  only return revoked delegations if explicitly asked.
	 *  otherwise we report revoked or bad_stateid status.
	 */
	if (statusmask & SC_STATUS_REVOKED)
		return_revoked = true;
	if (typemask & SC_TYPE_DELEG)
		/* Always allow REVOKED for DELEG so we can
		 * return the appropriate error.
		 */
		statusmask |= SC_STATUS_REVOKED;

	statusmask |= SC_STATUS_ADMIN_REVOKED | SC_STATUS_FREEABLE;

	if (ZERO_STATEID(stateid) || ONE_STATEID(stateid) ||
		CLOSE_STATEID(stateid))
		return nfserr_bad_stateid;
	status = set_client(&stateid->si_opaque.so_clid, cstate, nn);
	if (status == nfserr_stale_clientid) {
		if (cstate->session)
			return nfserr_bad_stateid;
		return nfserr_stale_stateid;
	}
	if (status)
		return status;
	stid = find_stateid_by_type(cstate->clp, stateid, typemask, statusmask);
	if (!stid)
		return nfserr_bad_stateid;
	if ((stid->sc_status & SC_STATUS_REVOKED) && !return_revoked) {
		nfs4_put_stid(stid);
		return nfserr_deleg_revoked;
	}
	if (stid->sc_status & SC_STATUS_ADMIN_REVOKED) {
		nfsd40_drop_revoked_stid(cstate->clp, stateid);
		nfs4_put_stid(stid);
		return nfserr_admin_revoked;
	}
	*s = stid;
	return nfs_ok;
}

static struct nfsd_file *
nfs4_find_file(struct nfs4_stid *s, int flags)
{
	struct nfsd_file *ret = NULL;

	if (!s || s->sc_status)
		return NULL;

	switch (s->sc_type) {
	case SC_TYPE_DELEG:
	case SC_TYPE_OPEN:
	case SC_TYPE_LOCK:
		if (flags & RD_STATE)
			ret = find_readable_file(s->sc_file);
		else
			ret = find_writeable_file(s->sc_file);
	}

	return ret;
}

static __be32
nfs4_check_olstateid(struct nfs4_ol_stateid *ols, int flags)
{
	__be32 status;

	status = nfsd4_check_openowner_confirmed(ols);
	if (status)
		return status;
	return nfs4_check_openmode(ols, flags);
}

static __be32
nfs4_check_file(struct svc_rqst *rqstp, struct svc_fh *fhp, struct nfs4_stid *s,
		struct nfsd_file **nfp, int flags)
{
	int acc = (flags & RD_STATE) ? NFSD_MAY_READ : NFSD_MAY_WRITE;
	struct nfsd_file *nf;
	__be32 status;

	nf = nfs4_find_file(s, flags);
	if (nf) {
		status = nfsd_permission(&rqstp->rq_cred,
					 fhp->fh_export, fhp->fh_dentry,
				acc | NFSD_MAY_OWNER_OVERRIDE);
		if (status) {
			nfsd_file_put(nf);
			goto out;
		}
	} else {
		status = nfsd_file_acquire(rqstp, fhp, acc, &nf);
		if (status)
			return status;
	}
	*nfp = nf;
out:
	return status;
}
static void
_free_cpntf_state_locked(struct nfsd_net *nn, struct nfs4_cpntf_state *cps)
{
	WARN_ON_ONCE(cps->cp_stateid.cs_type != NFS4_COPYNOTIFY_STID);
	if (!refcount_dec_and_test(&cps->cp_stateid.cs_count))
		return;
	list_del(&cps->cp_list);
	idr_remove(&nn->s2s_cp_stateids,
		   cps->cp_stateid.cs_stid.si_opaque.so_id);
	kfree(cps);
}
/*
 * A READ from an inter server to server COPY will have a
 * copy stateid. Look up the copy notify stateid from the
 * idr structure and take a reference on it.
 */
__be32 manage_cpntf_state(struct nfsd_net *nn, stateid_t *st,
			  struct nfs4_client *clp,
			  struct nfs4_cpntf_state **cps)
{
	copy_stateid_t *cps_t;
	struct nfs4_cpntf_state *state = NULL;

	if (st->si_opaque.so_clid.cl_id != nn->s2s_cp_cl_id)
		return nfserr_bad_stateid;
	spin_lock(&nn->s2s_cp_lock);
	cps_t = idr_find(&nn->s2s_cp_stateids, st->si_opaque.so_id);
	if (cps_t) {
		state = container_of(cps_t, struct nfs4_cpntf_state,
				     cp_stateid);
		if (state->cp_stateid.cs_type != NFS4_COPYNOTIFY_STID) {
			state = NULL;
			goto unlock;
		}
		if (!clp)
			refcount_inc(&state->cp_stateid.cs_count);
		else
			_free_cpntf_state_locked(nn, state);
	}
unlock:
	spin_unlock(&nn->s2s_cp_lock);
	if (!state)
		return nfserr_bad_stateid;
	if (!clp)
		*cps = state;
	return 0;
}

static __be32 find_cpntf_state(struct nfsd_net *nn, stateid_t *st,
			       struct nfs4_stid **stid)
{
	__be32 status;
	struct nfs4_cpntf_state *cps = NULL;
	struct nfs4_client *found;

	status = manage_cpntf_state(nn, st, NULL, &cps);
	if (status)
		return status;

	cps->cpntf_time = ktime_get_boottime_seconds();

	status = nfserr_expired;
	found = lookup_clientid(&cps->cp_p_clid, true, nn);
	if (!found)
		goto out;

	*stid = find_stateid_by_type(found, &cps->cp_p_stateid,
				     SC_TYPE_DELEG|SC_TYPE_OPEN|SC_TYPE_LOCK,
				     0);
	if (*stid)
		status = nfs_ok;
	else
		status = nfserr_bad_stateid;

	put_client_renew(found);
out:
	nfs4_put_cpntf_state(nn, cps);
	return status;
}

void nfs4_put_cpntf_state(struct nfsd_net *nn, struct nfs4_cpntf_state *cps)
{
	spin_lock(&nn->s2s_cp_lock);
	_free_cpntf_state_locked(nn, cps);
	spin_unlock(&nn->s2s_cp_lock);
}

/**
 * nfs4_preprocess_stateid_op - find and prep stateid for an operation
 * @rqstp: incoming request from client
 * @cstate: current compound state
 * @fhp: filehandle associated with requested stateid
 * @stateid: stateid (provided by client)
 * @flags: flags describing type of operation to be done
 * @nfp: optional nfsd_file return pointer (may be NULL)
 * @cstid: optional returned nfs4_stid pointer (may be NULL)
 *
 * Given info from the client, look up a nfs4_stid for the operation. On
 * success, it returns a reference to the nfs4_stid and/or the nfsd_file
 * associated with it.
 */
__be32
nfs4_preprocess_stateid_op(struct svc_rqst *rqstp,
		struct nfsd4_compound_state *cstate, struct svc_fh *fhp,
		stateid_t *stateid, int flags, struct nfsd_file **nfp,
		struct nfs4_stid **cstid)
{
	struct net *net = SVC_NET(rqstp);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	struct nfs4_stid *s = NULL;
	__be32 status;

	if (nfp)
		*nfp = NULL;

	if (ZERO_STATEID(stateid) || ONE_STATEID(stateid)) {
		status = check_special_stateids(net, fhp, stateid, flags);
		goto done;
	}

	status = nfsd4_lookup_stateid(cstate, stateid,
				SC_TYPE_DELEG|SC_TYPE_OPEN|SC_TYPE_LOCK,
				0, &s, nn);
	if (status == nfserr_bad_stateid)
		status = find_cpntf_state(nn, stateid, &s);
	if (status)
		return status;
	status = nfsd4_stid_check_stateid_generation(stateid, s,
			nfsd4_has_session(cstate));
	if (status)
		goto out;

	switch (s->sc_type) {
	case SC_TYPE_DELEG:
		status = nfs4_check_delegmode(delegstateid(s), flags);
		break;
	case SC_TYPE_OPEN:
	case SC_TYPE_LOCK:
		status = nfs4_check_olstateid(openlockstateid(s), flags);
		break;
	}
	if (status)
		goto out;
	status = nfs4_check_fh(fhp, s);

done:
	if (status == nfs_ok && nfp)
		status = nfs4_check_file(rqstp, fhp, s, nfp, flags);
out:
	if (s) {
		if (!status && cstid)
			*cstid = s;
		else
			nfs4_put_stid(s);
	}
	return status;
}

/*
 * Test if the stateid is valid
 */
__be32
nfsd4_test_stateid(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
		   union nfsd4_op_u *u)
{
	struct nfsd4_test_stateid *test_stateid = &u->test_stateid;
	struct nfsd4_test_stateid_id *stateid;
	struct nfs4_client *cl = cstate->clp;

	list_for_each_entry(stateid, &test_stateid->ts_stateid_list, ts_id_list)
		stateid->ts_id_status =
			nfsd4_validate_stateid(cl, &stateid->ts_id_stateid);

	return nfs_ok;
}

static __be32
nfsd4_free_lock_stateid(stateid_t *stateid, struct nfs4_stid *s)
{
	struct nfs4_ol_stateid *stp = openlockstateid(s);
	__be32 ret;

	ret = nfsd4_lock_ol_stateid(stp);
	if (ret)
		goto out_put_stid;

	ret = check_stateid_generation(stateid, &s->sc_stateid, 1);
	if (ret)
		goto out;

	ret = nfserr_locks_held;
	if (check_for_locks(stp->st_stid.sc_file,
			    lockowner(stp->st_stateowner)))
		goto out;

	release_lock_stateid(stp);
	ret = nfs_ok;

out:
	mutex_unlock(&stp->st_mutex);
out_put_stid:
	nfs4_put_stid(s);
	return ret;
}

__be32
nfsd4_free_stateid(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
		   union nfsd4_op_u *u)
{
	struct nfsd4_free_stateid *free_stateid = &u->free_stateid;
	stateid_t *stateid = &free_stateid->fr_stateid;
	struct nfs4_stid *s;
	struct nfs4_delegation *dp;
	struct nfs4_client *cl = cstate->clp;
	__be32 ret = nfserr_bad_stateid;

	spin_lock(&cl->cl_lock);
	s = find_stateid_locked(cl, stateid);
	if (!s || s->sc_status & SC_STATUS_CLOSED)
		goto out_unlock;
	if (s->sc_status & SC_STATUS_ADMIN_REVOKED) {
		nfsd4_drop_revoked_stid(s);
		ret = nfs_ok;
		goto out;
	}
	spin_lock(&s->sc_lock);
	switch (s->sc_type) {
	case SC_TYPE_DELEG:
		if (s->sc_status & SC_STATUS_REVOKED) {
			s->sc_status |= SC_STATUS_CLOSED;
			spin_unlock(&s->sc_lock);
			dp = delegstateid(s);
			if (s->sc_status & SC_STATUS_FREEABLE)
				list_del_init(&dp->dl_recall_lru);
			s->sc_status |= SC_STATUS_FREED;
			spin_unlock(&cl->cl_lock);
			nfs4_put_stid(s);
			ret = nfs_ok;
			goto out;
		}
		ret = nfserr_locks_held;
		break;
	case SC_TYPE_OPEN:
		ret = check_stateid_generation(stateid, &s->sc_stateid, 1);
		if (ret)
			break;
		ret = nfserr_locks_held;
		break;
	case SC_TYPE_LOCK:
		spin_unlock(&s->sc_lock);
		refcount_inc(&s->sc_count);
		spin_unlock(&cl->cl_lock);
		ret = nfsd4_free_lock_stateid(stateid, s);
		goto out;
	}
	spin_unlock(&s->sc_lock);
out_unlock:
	spin_unlock(&cl->cl_lock);
out:
	return ret;
}

static inline int
setlkflg (int type)
{
	return (type == NFS4_READW_LT || type == NFS4_READ_LT) ?
		RD_STATE : WR_STATE;
}

static __be32 nfs4_seqid_op_checks(struct nfsd4_compound_state *cstate, stateid_t *stateid, u32 seqid, struct nfs4_ol_stateid *stp)
{
	struct svc_fh *current_fh = &cstate->current_fh;
	struct nfs4_stateowner *sop = stp->st_stateowner;
	__be32 status;

	status = nfsd4_check_seqid(cstate, sop, seqid);
	if (status)
		return status;
	status = nfsd4_lock_ol_stateid(stp);
	if (status != nfs_ok)
		return status;
	status = check_stateid_generation(stateid, &stp->st_stid.sc_stateid, nfsd4_has_session(cstate));
	if (status == nfs_ok)
		status = nfs4_check_fh(current_fh, &stp->st_stid);
	if (status != nfs_ok)
		mutex_unlock(&stp->st_mutex);
	return status;
}

/**
 * nfs4_preprocess_seqid_op - find and prep an ol_stateid for a seqid-morphing op
 * @cstate: compund state
 * @seqid: seqid (provided by client)
 * @stateid: stateid (provided by client)
 * @typemask: mask of allowable types for this operation
 * @statusmask: mask of allowed states: 0 or STID_CLOSED
 * @stpp: return pointer for the stateid found
 * @nn: net namespace for request
 *
 * Given a stateid+seqid from a client, look up an nfs4_ol_stateid and
 * return it in @stpp. On a nfs_ok return, the returned stateid will
 * have its st_mutex locked.
 */
static __be32
nfs4_preprocess_seqid_op(struct nfsd4_compound_state *cstate, u32 seqid,
			 stateid_t *stateid,
			 unsigned short typemask, unsigned short statusmask,
			 struct nfs4_ol_stateid **stpp,
			 struct nfsd_net *nn)
{
	__be32 status;
	struct nfs4_stid *s;
	struct nfs4_ol_stateid *stp = NULL;

	trace_nfsd_preprocess(seqid, stateid);

	*stpp = NULL;
retry:
	status = nfsd4_lookup_stateid(cstate, stateid,
				      typemask, statusmask, &s, nn);
	if (status)
		return status;
	stp = openlockstateid(s);
	if (nfsd4_cstate_assign_replay(cstate, stp->st_stateowner) == -EAGAIN) {
		nfs4_put_stateowner(stp->st_stateowner);
		goto retry;
	}

	status = nfs4_seqid_op_checks(cstate, stateid, seqid, stp);
	if (!status)
		*stpp = stp;
	else
		nfs4_put_stid(&stp->st_stid);
	return status;
}

static __be32 nfs4_preprocess_confirmed_seqid_op(struct nfsd4_compound_state *cstate, u32 seqid,
						 stateid_t *stateid, struct nfs4_ol_stateid **stpp, struct nfsd_net *nn)
{
	__be32 status;
	struct nfs4_openowner *oo;
	struct nfs4_ol_stateid *stp;

	status = nfs4_preprocess_seqid_op(cstate, seqid, stateid,
					  SC_TYPE_OPEN, 0, &stp, nn);
	if (status)
		return status;
	oo = openowner(stp->st_stateowner);
	if (!(oo->oo_flags & NFS4_OO_CONFIRMED)) {
		mutex_unlock(&stp->st_mutex);
		nfs4_put_stid(&stp->st_stid);
		return nfserr_bad_stateid;
	}
	*stpp = stp;
	return nfs_ok;
}

__be32
nfsd4_open_confirm(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
		   union nfsd4_op_u *u)
{
	struct nfsd4_open_confirm *oc = &u->open_confirm;
	__be32 status;
	struct nfs4_openowner *oo;
	struct nfs4_ol_stateid *stp;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	dprintk("NFSD: nfsd4_open_confirm on file %pd\n",
			cstate->current_fh.fh_dentry);

	status = fh_verify(rqstp, &cstate->current_fh, S_IFREG, 0);
	if (status)
		return status;

	status = nfs4_preprocess_seqid_op(cstate,
					  oc->oc_seqid, &oc->oc_req_stateid,
					  SC_TYPE_OPEN, 0, &stp, nn);
	if (status)
		goto out;
	oo = openowner(stp->st_stateowner);
	status = nfserr_bad_stateid;
	if (oo->oo_flags & NFS4_OO_CONFIRMED) {
		mutex_unlock(&stp->st_mutex);
		goto put_stateid;
	}
	oo->oo_flags |= NFS4_OO_CONFIRMED;
	nfs4_inc_and_copy_stateid(&oc->oc_resp_stateid, &stp->st_stid);
	mutex_unlock(&stp->st_mutex);
	trace_nfsd_open_confirm(oc->oc_seqid, &stp->st_stid.sc_stateid);
	nfsd4_client_record_create(oo->oo_owner.so_client);
	status = nfs_ok;
put_stateid:
	nfs4_put_stid(&stp->st_stid);
out:
	nfsd4_bump_seqid(cstate, status);
	return status;
}

static inline void nfs4_stateid_downgrade_bit(struct nfs4_ol_stateid *stp, u32 access)
{
	if (!test_access(access, stp))
		return;
	nfs4_file_put_access(stp->st_stid.sc_file, access);
	clear_access(access, stp);
}

static inline void nfs4_stateid_downgrade(struct nfs4_ol_stateid *stp, u32 to_access)
{
	switch (to_access) {
	case NFS4_SHARE_ACCESS_READ:
		nfs4_stateid_downgrade_bit(stp, NFS4_SHARE_ACCESS_WRITE);
		nfs4_stateid_downgrade_bit(stp, NFS4_SHARE_ACCESS_BOTH);
		break;
	case NFS4_SHARE_ACCESS_WRITE:
		nfs4_stateid_downgrade_bit(stp, NFS4_SHARE_ACCESS_READ);
		nfs4_stateid_downgrade_bit(stp, NFS4_SHARE_ACCESS_BOTH);
		break;
	case NFS4_SHARE_ACCESS_BOTH:
		break;
	default:
		WARN_ON_ONCE(1);
	}
}

__be32
nfsd4_open_downgrade(struct svc_rqst *rqstp,
		     struct nfsd4_compound_state *cstate, union nfsd4_op_u *u)
{
	struct nfsd4_open_downgrade *od = &u->open_downgrade;
	__be32 status;
	struct nfs4_ol_stateid *stp;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	dprintk("NFSD: nfsd4_open_downgrade on file %pd\n", 
			cstate->current_fh.fh_dentry);

	/* We don't yet support WANT bits: */
	if (od->od_deleg_want)
		dprintk("NFSD: %s: od_deleg_want=0x%x ignored\n", __func__,
			od->od_deleg_want);

	status = nfs4_preprocess_confirmed_seqid_op(cstate, od->od_seqid,
					&od->od_stateid, &stp, nn);
	if (status)
		goto out; 
	status = nfserr_inval;
	if (!test_access(od->od_share_access, stp)) {
		dprintk("NFSD: access not a subset of current bitmap: 0x%hhx, input access=%08x\n",
			stp->st_access_bmap, od->od_share_access);
		goto put_stateid;
	}
	if (!test_deny(od->od_share_deny, stp)) {
		dprintk("NFSD: deny not a subset of current bitmap: 0x%hhx, input deny=%08x\n",
			stp->st_deny_bmap, od->od_share_deny);
		goto put_stateid;
	}
	nfs4_stateid_downgrade(stp, od->od_share_access);
	reset_union_bmap_deny(od->od_share_deny, stp);
	nfs4_inc_and_copy_stateid(&od->od_stateid, &stp->st_stid);
	status = nfs_ok;
put_stateid:
	mutex_unlock(&stp->st_mutex);
	nfs4_put_stid(&stp->st_stid);
out:
	nfsd4_bump_seqid(cstate, status);
	return status;
}

static bool nfsd4_close_open_stateid(struct nfs4_ol_stateid *s)
{
	struct nfs4_client *clp = s->st_stid.sc_client;
	bool unhashed;
	LIST_HEAD(reaplist);
	struct nfs4_ol_stateid *stp;

	spin_lock(&clp->cl_lock);
	unhashed = unhash_open_stateid(s, &reaplist);

	if (clp->cl_minorversion) {
		if (unhashed)
			put_ol_stateid_locked(s, &reaplist);
		spin_unlock(&clp->cl_lock);
		list_for_each_entry(stp, &reaplist, st_locks)
			nfs4_free_cpntf_statelist(clp->net, &stp->st_stid);
		free_ol_stateid_reaplist(&reaplist);
		return false;
	} else {
		spin_unlock(&clp->cl_lock);
		free_ol_stateid_reaplist(&reaplist);
		return unhashed;
	}
}

/*
 * nfs4_unlock_state() called after encode
 */
__be32
nfsd4_close(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	struct nfsd4_close *close = &u->close;
	__be32 status;
	struct nfs4_ol_stateid *stp;
	struct net *net = SVC_NET(rqstp);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	bool need_move_to_close_list;

	dprintk("NFSD: nfsd4_close on file %pd\n",
			cstate->current_fh.fh_dentry);

	status = nfs4_preprocess_seqid_op(cstate, close->cl_seqid,
					  &close->cl_stateid,
					  SC_TYPE_OPEN, SC_STATUS_CLOSED,
					  &stp, nn);
	nfsd4_bump_seqid(cstate, status);
	if (status)
		goto out;

	spin_lock(&stp->st_stid.sc_client->cl_lock);
	stp->st_stid.sc_status |= SC_STATUS_CLOSED;
	spin_unlock(&stp->st_stid.sc_client->cl_lock);

	/*
	 * Technically we don't _really_ have to increment or copy it, since
	 * it should just be gone after this operation and we clobber the
	 * copied value below, but we continue to do so here just to ensure
	 * that racing ops see that there was a state change.
	 */
	nfs4_inc_and_copy_stateid(&close->cl_stateid, &stp->st_stid);

	need_move_to_close_list = nfsd4_close_open_stateid(stp);
	mutex_unlock(&stp->st_mutex);
	if (need_move_to_close_list)
		move_to_close_lru(stp, net);

	/* v4.1+ suggests that we send a special stateid in here, since the
	 * clients should just ignore this anyway. Since this is not useful
	 * for v4.0 clients either, we set it to the special close_stateid
	 * universally.
	 *
	 * See RFC5661 section 18.2.4, and RFC7530 section 16.2.5
	 */
	memcpy(&close->cl_stateid, &close_stateid, sizeof(close->cl_stateid));

	/* put reference from nfs4_preprocess_seqid_op */
	nfs4_put_stid(&stp->st_stid);
out:
	return status;
}

__be32
nfsd4_delegreturn(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
		  union nfsd4_op_u *u)
{
	struct nfsd4_delegreturn *dr = &u->delegreturn;
	struct nfs4_delegation *dp;
	stateid_t *stateid = &dr->dr_stateid;
	struct nfs4_stid *s;
	__be32 status;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	if ((status = fh_verify(rqstp, &cstate->current_fh, S_IFREG, 0)))
		return status;

	status = nfsd4_lookup_stateid(cstate, stateid, SC_TYPE_DELEG, SC_STATUS_REVOKED, &s, nn);
	if (status)
		goto out;
	dp = delegstateid(s);
	status = nfsd4_stid_check_stateid_generation(stateid, &dp->dl_stid, nfsd4_has_session(cstate));
	if (status)
		goto put_stateid;

	trace_nfsd_deleg_return(stateid);
	destroy_delegation(dp);
	smp_mb__after_atomic();
	wake_up_var(d_inode(cstate->current_fh.fh_dentry));
put_stateid:
	nfs4_put_stid(&dp->dl_stid);
out:
	return status;
}

/* last octet in a range */
static inline u64
last_byte_offset(u64 start, u64 len)
{
	u64 end;

	WARN_ON_ONCE(!len);
	end = start + len;
	return end > start ? end - 1: NFS4_MAX_UINT64;
}

/*
 * TODO: Linux file offsets are _signed_ 64-bit quantities, which means that
 * we can't properly handle lock requests that go beyond the (2^63 - 1)-th
 * byte, because of sign extension problems.  Since NFSv4 calls for 64-bit
 * locking, this prevents us from being completely protocol-compliant.  The
 * real solution to this problem is to start using unsigned file offsets in
 * the VFS, but this is a very deep change!
 */
static inline void
nfs4_transform_lock_offset(struct file_lock *lock)
{
	if (lock->fl_start < 0)
		lock->fl_start = OFFSET_MAX;
	if (lock->fl_end < 0)
		lock->fl_end = OFFSET_MAX;
}

static fl_owner_t
nfsd4_lm_get_owner(fl_owner_t owner)
{
	struct nfs4_lockowner *lo = (struct nfs4_lockowner *)owner;

	nfs4_get_stateowner(&lo->lo_owner);
	return owner;
}

static void
nfsd4_lm_put_owner(fl_owner_t owner)
{
	struct nfs4_lockowner *lo = (struct nfs4_lockowner *)owner;

	if (lo)
		nfs4_put_stateowner(&lo->lo_owner);
}

/* return pointer to struct nfs4_client if client is expirable */
static bool
nfsd4_lm_lock_expirable(struct file_lock *cfl)
{
	struct nfs4_lockowner *lo = (struct nfs4_lockowner *) cfl->c.flc_owner;
	struct nfs4_client *clp = lo->lo_owner.so_client;
	struct nfsd_net *nn;

	if (try_to_expire_client(clp)) {
		nn = net_generic(clp->net, nfsd_net_id);
		mod_delayed_work(laundry_wq, &nn->laundromat_work, 0);
		return true;
	}
	return false;
}

/* schedule laundromat to run immediately and wait for it to complete */
static void
nfsd4_lm_expire_lock(void)
{
	flush_workqueue(laundry_wq);
}

static void
nfsd4_lm_notify(struct file_lock *fl)
{
	struct nfs4_lockowner		*lo = (struct nfs4_lockowner *) fl->c.flc_owner;
	struct net			*net = lo->lo_owner.so_client->net;
	struct nfsd_net			*nn = net_generic(net, nfsd_net_id);
	struct nfsd4_blocked_lock	*nbl = container_of(fl,
						struct nfsd4_blocked_lock, nbl_lock);
	bool queue = false;

	/* An empty list means that something else is going to be using it */
	spin_lock(&nn->blocked_locks_lock);
	if (!list_empty(&nbl->nbl_list)) {
		list_del_init(&nbl->nbl_list);
		list_del_init(&nbl->nbl_lru);
		queue = true;
	}
	spin_unlock(&nn->blocked_locks_lock);

	if (queue) {
		trace_nfsd_cb_notify_lock(lo, nbl);
		nfsd4_try_run_cb(&nbl->nbl_cb);
	}
}

static const struct lock_manager_operations nfsd_posix_mng_ops  = {
	.lm_mod_owner = THIS_MODULE,
	.lm_notify = nfsd4_lm_notify,
	.lm_get_owner = nfsd4_lm_get_owner,
	.lm_put_owner = nfsd4_lm_put_owner,
	.lm_lock_expirable = nfsd4_lm_lock_expirable,
	.lm_expire_lock = nfsd4_lm_expire_lock,
};

static inline void
nfs4_set_lock_denied(struct file_lock *fl, struct nfsd4_lock_denied *deny)
{
	struct nfs4_lockowner *lo;

	if (fl->fl_lmops == &nfsd_posix_mng_ops) {
		lo = (struct nfs4_lockowner *) fl->c.flc_owner;
		xdr_netobj_dup(&deny->ld_owner, &lo->lo_owner.so_owner,
						GFP_KERNEL);
		if (!deny->ld_owner.data)
			/* We just don't care that much */
			goto nevermind;
		deny->ld_clientid = lo->lo_owner.so_client->cl_clientid;
	} else {
nevermind:
		deny->ld_owner.len = 0;
		deny->ld_owner.data = NULL;
		deny->ld_clientid.cl_boot = 0;
		deny->ld_clientid.cl_id = 0;
	}
	deny->ld_start = fl->fl_start;
	deny->ld_length = NFS4_MAX_UINT64;
	if (fl->fl_end != NFS4_MAX_UINT64)
		deny->ld_length = fl->fl_end - fl->fl_start + 1;        
	deny->ld_type = NFS4_READ_LT;
	if (fl->c.flc_type != F_RDLCK)
		deny->ld_type = NFS4_WRITE_LT;
}

static struct nfs4_lockowner *
find_lockowner_str_locked(struct nfs4_client *clp, struct xdr_netobj *owner)
{
	unsigned int strhashval = ownerstr_hashval(owner);
	struct nfs4_stateowner *so;

	lockdep_assert_held(&clp->cl_lock);

	list_for_each_entry(so, &clp->cl_ownerstr_hashtbl[strhashval],
			    so_strhash) {
		if (so->so_is_open_owner)
			continue;
		if (same_owner_str(so, owner))
			return lockowner(nfs4_get_stateowner(so));
	}
	return NULL;
}

static struct nfs4_lockowner *
find_lockowner_str(struct nfs4_client *clp, struct xdr_netobj *owner)
{
	struct nfs4_lockowner *lo;

	spin_lock(&clp->cl_lock);
	lo = find_lockowner_str_locked(clp, owner);
	spin_unlock(&clp->cl_lock);
	return lo;
}

static void nfs4_unhash_lockowner(struct nfs4_stateowner *sop)
{
	unhash_lockowner_locked(lockowner(sop));
}

static void nfs4_free_lockowner(struct nfs4_stateowner *sop)
{
	struct nfs4_lockowner *lo = lockowner(sop);

	kmem_cache_free(lockowner_slab, lo);
}

static const struct nfs4_stateowner_operations lockowner_ops = {
	.so_unhash =	nfs4_unhash_lockowner,
	.so_free =	nfs4_free_lockowner,
};

/*
 * Alloc a lock owner structure.
 * Called in nfsd4_lock - therefore, OPEN and OPEN_CONFIRM (if needed) has 
 * occurred. 
 *
 * strhashval = ownerstr_hashval
 */
static struct nfs4_lockowner *
alloc_init_lock_stateowner(unsigned int strhashval, struct nfs4_client *clp,
			   struct nfs4_ol_stateid *open_stp,
			   struct nfsd4_lock *lock)
{
	struct nfs4_lockowner *lo, *ret;

	lo = alloc_stateowner(lockowner_slab, &lock->lk_new_owner, clp);
	if (!lo)
		return NULL;
	INIT_LIST_HEAD(&lo->lo_blocked);
	INIT_LIST_HEAD(&lo->lo_owner.so_stateids);
	lo->lo_owner.so_is_open_owner = 0;
	lo->lo_owner.so_seqid = lock->lk_new_lock_seqid;
	lo->lo_owner.so_ops = &lockowner_ops;
	spin_lock(&clp->cl_lock);
	ret = find_lockowner_str_locked(clp, &lock->lk_new_owner);
	if (ret == NULL) {
		list_add(&lo->lo_owner.so_strhash,
			 &clp->cl_ownerstr_hashtbl[strhashval]);
		ret = lo;
	} else
		nfs4_free_stateowner(&lo->lo_owner);

	spin_unlock(&clp->cl_lock);
	return ret;
}

static struct nfs4_ol_stateid *
find_lock_stateid(const struct nfs4_lockowner *lo,
		  const struct nfs4_ol_stateid *ost)
{
	struct nfs4_ol_stateid *lst;

	lockdep_assert_held(&ost->st_stid.sc_client->cl_lock);

	/* If ost is not hashed, ost->st_locks will not be valid */
	if (!nfs4_ol_stateid_unhashed(ost))
		list_for_each_entry(lst, &ost->st_locks, st_locks) {
			if (lst->st_stateowner == &lo->lo_owner) {
				refcount_inc(&lst->st_stid.sc_count);
				return lst;
			}
		}
	return NULL;
}

static struct nfs4_ol_stateid *
init_lock_stateid(struct nfs4_ol_stateid *stp, struct nfs4_lockowner *lo,
		  struct nfs4_file *fp, struct inode *inode,
		  struct nfs4_ol_stateid *open_stp)
{
	struct nfs4_client *clp = lo->lo_owner.so_client;
	struct nfs4_ol_stateid *retstp;

	mutex_init(&stp->st_mutex);
	mutex_lock_nested(&stp->st_mutex, OPEN_STATEID_MUTEX);
retry:
	spin_lock(&clp->cl_lock);
	if (nfs4_ol_stateid_unhashed(open_stp))
		goto out_close;
	retstp = find_lock_stateid(lo, open_stp);
	if (retstp)
		goto out_found;
	refcount_inc(&stp->st_stid.sc_count);
	stp->st_stid.sc_type = SC_TYPE_LOCK;
	stp->st_stateowner = nfs4_get_stateowner(&lo->lo_owner);
	get_nfs4_file(fp);
	stp->st_stid.sc_file = fp;
	stp->st_access_bmap = 0;
	stp->st_deny_bmap = open_stp->st_deny_bmap;
	stp->st_openstp = open_stp;
	spin_lock(&fp->fi_lock);
	list_add(&stp->st_locks, &open_stp->st_locks);
	list_add(&stp->st_perstateowner, &lo->lo_owner.so_stateids);
	list_add(&stp->st_perfile, &fp->fi_stateids);
	spin_unlock(&fp->fi_lock);
	spin_unlock(&clp->cl_lock);
	return stp;
out_found:
	spin_unlock(&clp->cl_lock);
	if (nfsd4_lock_ol_stateid(retstp) != nfs_ok) {
		nfs4_put_stid(&retstp->st_stid);
		goto retry;
	}
	/* To keep mutex tracking happy */
	mutex_unlock(&stp->st_mutex);
	return retstp;
out_close:
	spin_unlock(&clp->cl_lock);
	mutex_unlock(&stp->st_mutex);
	return NULL;
}

static struct nfs4_ol_stateid *
find_or_create_lock_stateid(struct nfs4_lockowner *lo, struct nfs4_file *fi,
			    struct inode *inode, struct nfs4_ol_stateid *ost,
			    bool *new)
{
	struct nfs4_stid *ns = NULL;
	struct nfs4_ol_stateid *lst;
	struct nfs4_openowner *oo = openowner(ost->st_stateowner);
	struct nfs4_client *clp = oo->oo_owner.so_client;

	*new = false;
	spin_lock(&clp->cl_lock);
	lst = find_lock_stateid(lo, ost);
	spin_unlock(&clp->cl_lock);
	if (lst != NULL) {
		if (nfsd4_lock_ol_stateid(lst) == nfs_ok)
			goto out;
		nfs4_put_stid(&lst->st_stid);
	}
	ns = nfs4_alloc_stid(clp, stateid_slab, nfs4_free_lock_stateid);
	if (ns == NULL)
		return NULL;

	lst = init_lock_stateid(openlockstateid(ns), lo, fi, inode, ost);
	if (lst == openlockstateid(ns))
		*new = true;
	else
		nfs4_put_stid(ns);
out:
	return lst;
}

static int
check_lock_length(u64 offset, u64 length)
{
	return ((length == 0) || ((length != NFS4_MAX_UINT64) &&
		(length > ~offset)));
}

static void get_lock_access(struct nfs4_ol_stateid *lock_stp, u32 access)
{
	struct nfs4_file *fp = lock_stp->st_stid.sc_file;

	lockdep_assert_held(&fp->fi_lock);

	if (test_access(access, lock_stp))
		return;
	__nfs4_file_get_access(fp, access);
	set_access(access, lock_stp);
}

static __be32
lookup_or_create_lock_state(struct nfsd4_compound_state *cstate,
			    struct nfs4_ol_stateid *ost,
			    struct nfsd4_lock *lock,
			    struct nfs4_ol_stateid **plst, bool *new)
{
	__be32 status;
	struct nfs4_file *fi = ost->st_stid.sc_file;
	struct nfs4_openowner *oo = openowner(ost->st_stateowner);
	struct nfs4_client *cl = oo->oo_owner.so_client;
	struct inode *inode = d_inode(cstate->current_fh.fh_dentry);
	struct nfs4_lockowner *lo;
	struct nfs4_ol_stateid *lst;
	unsigned int strhashval;

	lo = find_lockowner_str(cl, &lock->lk_new_owner);
	if (!lo) {
		strhashval = ownerstr_hashval(&lock->lk_new_owner);
		lo = alloc_init_lock_stateowner(strhashval, cl, ost, lock);
		if (lo == NULL)
			return nfserr_jukebox;
	} else {
		/* with an existing lockowner, seqids must be the same */
		status = nfserr_bad_seqid;
		if (!cstate->minorversion &&
		    lock->lk_new_lock_seqid != lo->lo_owner.so_seqid)
			goto out;
	}

	lst = find_or_create_lock_stateid(lo, fi, inode, ost, new);
	if (lst == NULL) {
		status = nfserr_jukebox;
		goto out;
	}

	status = nfs_ok;
	*plst = lst;
out:
	nfs4_put_stateowner(&lo->lo_owner);
	return status;
}

/*
 *  LOCK operation 
 */
__be32
nfsd4_lock(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
	   union nfsd4_op_u *u)
{
	struct nfsd4_lock *lock = &u->lock;
	struct nfs4_openowner *open_sop = NULL;
	struct nfs4_lockowner *lock_sop = NULL;
	struct nfs4_ol_stateid *lock_stp = NULL;
	struct nfs4_ol_stateid *open_stp = NULL;
	struct nfs4_file *fp;
	struct nfsd_file *nf = NULL;
	struct nfsd4_blocked_lock *nbl = NULL;
	struct file_lock *file_lock = NULL;
	struct file_lock *conflock = NULL;
	__be32 status = 0;
	int lkflg;
	int err;
	bool new = false;
	unsigned char type;
	unsigned int flags = FL_POSIX;
	struct net *net = SVC_NET(rqstp);
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	dprintk("NFSD: nfsd4_lock: start=%Ld length=%Ld\n",
		(long long) lock->lk_offset,
		(long long) lock->lk_length);

	if (check_lock_length(lock->lk_offset, lock->lk_length))
		 return nfserr_inval;

	status = fh_verify(rqstp, &cstate->current_fh, S_IFREG, 0);
	if (status != nfs_ok)
		return status;
	if (exportfs_cannot_lock(cstate->current_fh.fh_dentry->d_sb->s_export_op)) {
		status = nfserr_notsupp;
		goto out;
	}

	if (lock->lk_is_new) {
		if (nfsd4_has_session(cstate))
			/* See rfc 5661 18.10.3: given clientid is ignored: */
			memcpy(&lock->lk_new_clientid,
				&cstate->clp->cl_clientid,
				sizeof(clientid_t));

		/* validate and update open stateid and open seqid */
		status = nfs4_preprocess_confirmed_seqid_op(cstate,
				        lock->lk_new_open_seqid,
		                        &lock->lk_new_open_stateid,
					&open_stp, nn);
		if (status)
			goto out;
		mutex_unlock(&open_stp->st_mutex);
		open_sop = openowner(open_stp->st_stateowner);
		status = nfserr_bad_stateid;
		if (!same_clid(&open_sop->oo_owner.so_client->cl_clientid,
						&lock->lk_new_clientid))
			goto out;
		status = lookup_or_create_lock_state(cstate, open_stp, lock,
							&lock_stp, &new);
	} else {
		status = nfs4_preprocess_seqid_op(cstate,
						  lock->lk_old_lock_seqid,
						  &lock->lk_old_lock_stateid,
						  SC_TYPE_LOCK, 0, &lock_stp,
						  nn);
	}
	if (status)
		goto out;
	lock_sop = lockowner(lock_stp->st_stateowner);

	lkflg = setlkflg(lock->lk_type);
	status = nfs4_check_openmode(lock_stp, lkflg);
	if (status)
		goto out;

	status = nfserr_grace;
	if (locks_in_grace(net) && !lock->lk_reclaim)
		goto out;
	status = nfserr_no_grace;
	if (!locks_in_grace(net) && lock->lk_reclaim)
		goto out;

	if (lock->lk_reclaim)
		flags |= FL_RECLAIM;

	fp = lock_stp->st_stid.sc_file;
	switch (lock->lk_type) {
		case NFS4_READW_LT:
			fallthrough;
		case NFS4_READ_LT:
			spin_lock(&fp->fi_lock);
			nf = find_readable_file_locked(fp);
			if (nf)
				get_lock_access(lock_stp, NFS4_SHARE_ACCESS_READ);
			spin_unlock(&fp->fi_lock);
			type = F_RDLCK;
			break;
		case NFS4_WRITEW_LT:
			fallthrough;
		case NFS4_WRITE_LT:
			spin_lock(&fp->fi_lock);
			nf = find_writeable_file_locked(fp);
			if (nf)
				get_lock_access(lock_stp, NFS4_SHARE_ACCESS_WRITE);
			spin_unlock(&fp->fi_lock);
			type = F_WRLCK;
			break;
		default:
			status = nfserr_inval;
		goto out;
	}

	if (!nf) {
		status = nfserr_openmode;
		goto out;
	}

	if (lock->lk_type & (NFS4_READW_LT | NFS4_WRITEW_LT) &&
		nfsd4_has_session(cstate) &&
		locks_can_async_lock(nf->nf_file->f_op))
			flags |= FL_SLEEP;

	nbl = find_or_allocate_block(lock_sop, &fp->fi_fhandle, nn);
	if (!nbl) {
		dprintk("NFSD: %s: unable to allocate block!\n", __func__);
		status = nfserr_jukebox;
		goto out;
	}

	file_lock = &nbl->nbl_lock;
	file_lock->c.flc_type = type;
	file_lock->c.flc_owner = (fl_owner_t)lockowner(nfs4_get_stateowner(&lock_sop->lo_owner));
	file_lock->c.flc_pid = current->tgid;
	file_lock->c.flc_file = nf->nf_file;
	file_lock->c.flc_flags = flags;
	file_lock->fl_lmops = &nfsd_posix_mng_ops;
	file_lock->fl_start = lock->lk_offset;
	file_lock->fl_end = last_byte_offset(lock->lk_offset, lock->lk_length);
	nfs4_transform_lock_offset(file_lock);

	conflock = locks_alloc_lock();
	if (!conflock) {
		dprintk("NFSD: %s: unable to allocate lock!\n", __func__);
		status = nfserr_jukebox;
		goto out;
	}

	if (flags & FL_SLEEP) {
		nbl->nbl_time = ktime_get_boottime_seconds();
		spin_lock(&nn->blocked_locks_lock);
		list_add_tail(&nbl->nbl_list, &lock_sop->lo_blocked);
		list_add_tail(&nbl->nbl_lru, &nn->blocked_locks_lru);
		kref_get(&nbl->nbl_kref);
		spin_unlock(&nn->blocked_locks_lock);
	}

	err = vfs_lock_file(nf->nf_file, F_SETLK, file_lock, conflock);
	switch (err) {
	case 0: /* success! */
		nfs4_inc_and_copy_stateid(&lock->lk_resp_stateid, &lock_stp->st_stid);
		status = 0;
		if (lock->lk_reclaim)
			nn->somebody_reclaimed = true;
		break;
	case FILE_LOCK_DEFERRED:
		kref_put(&nbl->nbl_kref, free_nbl);
		nbl = NULL;
		fallthrough;
	case -EAGAIN:		/* conflock holds conflicting lock */
		status = nfserr_denied;
		dprintk("NFSD: nfsd4_lock: conflicting lock found!\n");
		nfs4_set_lock_denied(conflock, &lock->lk_denied);
		break;
	case -EDEADLK:
		status = nfserr_deadlock;
		break;
	default:
		dprintk("NFSD: nfsd4_lock: vfs_lock_file() failed! status %d\n",err);
		status = nfserrno(err);
		break;
	}
out:
	if (nbl) {
		/* dequeue it if we queued it before */
		if (flags & FL_SLEEP) {
			spin_lock(&nn->blocked_locks_lock);
			if (!list_empty(&nbl->nbl_list) &&
			    !list_empty(&nbl->nbl_lru)) {
				list_del_init(&nbl->nbl_list);
				list_del_init(&nbl->nbl_lru);
				kref_put(&nbl->nbl_kref, free_nbl);
			}
			/* nbl can use one of lists to be linked to reaplist */
			spin_unlock(&nn->blocked_locks_lock);
		}
		free_blocked_lock(nbl);
	}
	if (nf)
		nfsd_file_put(nf);
	if (lock_stp) {
		/* Bump seqid manually if the 4.0 replay owner is openowner */
		if (cstate->replay_owner &&
		    cstate->replay_owner != &lock_sop->lo_owner &&
		    seqid_mutating_err(ntohl(status)))
			lock_sop->lo_owner.so_seqid++;

		/*
		 * If this is a new, never-before-used stateid, and we are
		 * returning an error, then just go ahead and release it.
		 */
		if (status && new)
			release_lock_stateid(lock_stp);

		mutex_unlock(&lock_stp->st_mutex);

		nfs4_put_stid(&lock_stp->st_stid);
	}
	if (open_stp)
		nfs4_put_stid(&open_stp->st_stid);
	nfsd4_bump_seqid(cstate, status);
	if (conflock)
		locks_free_lock(conflock);
	return status;
}

void nfsd4_lock_release(union nfsd4_op_u *u)
{
	struct nfsd4_lock *lock = &u->lock;
	struct nfsd4_lock_denied *deny = &lock->lk_denied;

	kfree(deny->ld_owner.data);
}

/*
 * The NFSv4 spec allows a client to do a LOCKT without holding an OPEN,
 * so we do a temporary open here just to get an open file to pass to
 * vfs_test_lock.
 */
static __be32 nfsd_test_lock(struct svc_rqst *rqstp, struct svc_fh *fhp, struct file_lock *lock)
{
	struct nfsd_file *nf;
	struct inode *inode;
	__be32 err;

	err = nfsd_file_acquire(rqstp, fhp, NFSD_MAY_READ, &nf);
	if (err)
		return err;
	inode = fhp->fh_dentry->d_inode;
	inode_lock(inode); /* to block new leases till after test_lock: */
	err = nfserrno(nfsd_open_break_lease(inode, NFSD_MAY_READ));
	if (err)
		goto out;
	lock->c.flc_file = nf->nf_file;
	err = nfserrno(vfs_test_lock(nf->nf_file, lock));
	lock->c.flc_file = NULL;
out:
	inode_unlock(inode);
	nfsd_file_put(nf);
	return err;
}

/*
 * LOCKT operation
 */
__be32
nfsd4_lockt(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
	    union nfsd4_op_u *u)
{
	struct nfsd4_lockt *lockt = &u->lockt;
	struct file_lock *file_lock = NULL;
	struct nfs4_lockowner *lo = NULL;
	__be32 status;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	if (locks_in_grace(SVC_NET(rqstp)))
		return nfserr_grace;

	if (check_lock_length(lockt->lt_offset, lockt->lt_length))
		 return nfserr_inval;

	if (!nfsd4_has_session(cstate)) {
		status = set_client(&lockt->lt_clientid, cstate, nn);
		if (status)
			goto out;
	}

	if ((status = fh_verify(rqstp, &cstate->current_fh, S_IFREG, 0)))
		goto out;

	file_lock = locks_alloc_lock();
	if (!file_lock) {
		dprintk("NFSD: %s: unable to allocate lock!\n", __func__);
		status = nfserr_jukebox;
		goto out;
	}

	switch (lockt->lt_type) {
		case NFS4_READ_LT:
		case NFS4_READW_LT:
			file_lock->c.flc_type = F_RDLCK;
			break;
		case NFS4_WRITE_LT:
		case NFS4_WRITEW_LT:
			file_lock->c.flc_type = F_WRLCK;
			break;
		default:
			dprintk("NFSD: nfs4_lockt: bad lock type!\n");
			status = nfserr_inval;
			goto out;
	}

	lo = find_lockowner_str(cstate->clp, &lockt->lt_owner);
	if (lo)
		file_lock->c.flc_owner = (fl_owner_t)lo;
	file_lock->c.flc_pid = current->tgid;
	file_lock->c.flc_flags = FL_POSIX;

	file_lock->fl_start = lockt->lt_offset;
	file_lock->fl_end = last_byte_offset(lockt->lt_offset, lockt->lt_length);

	nfs4_transform_lock_offset(file_lock);

	status = nfsd_test_lock(rqstp, &cstate->current_fh, file_lock);
	if (status)
		goto out;

	if (file_lock->c.flc_type != F_UNLCK) {
		status = nfserr_denied;
		nfs4_set_lock_denied(file_lock, &lockt->lt_denied);
	}
out:
	if (lo)
		nfs4_put_stateowner(&lo->lo_owner);
	if (file_lock)
		locks_free_lock(file_lock);
	return status;
}

void nfsd4_lockt_release(union nfsd4_op_u *u)
{
	struct nfsd4_lockt *lockt = &u->lockt;
	struct nfsd4_lock_denied *deny = &lockt->lt_denied;

	kfree(deny->ld_owner.data);
}

__be32
nfsd4_locku(struct svc_rqst *rqstp, struct nfsd4_compound_state *cstate,
	    union nfsd4_op_u *u)
{
	struct nfsd4_locku *locku = &u->locku;
	struct nfs4_ol_stateid *stp;
	struct nfsd_file *nf = NULL;
	struct file_lock *file_lock = NULL;
	__be32 status;
	int err;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);

	dprintk("NFSD: nfsd4_locku: start=%Ld length=%Ld\n",
		(long long) locku->lu_offset,
		(long long) locku->lu_length);

	if (check_lock_length(locku->lu_offset, locku->lu_length))
		 return nfserr_inval;

	status = nfs4_preprocess_seqid_op(cstate, locku->lu_seqid,
					  &locku->lu_stateid, SC_TYPE_LOCK, 0,
					  &stp, nn);
	if (status)
		goto out;
	nf = find_any_file(stp->st_stid.sc_file);
	if (!nf) {
		status = nfserr_lock_range;
		goto put_stateid;
	}
	if (exportfs_cannot_lock(nf->nf_file->f_path.mnt->mnt_sb->s_export_op)) {
		status = nfserr_notsupp;
		goto put_file;
	}

	file_lock = locks_alloc_lock();
	if (!file_lock) {
		dprintk("NFSD: %s: unable to allocate lock!\n", __func__);
		status = nfserr_jukebox;
		goto put_file;
	}

	file_lock->c.flc_type = F_UNLCK;
	file_lock->c.flc_owner = (fl_owner_t)lockowner(nfs4_get_stateowner(stp->st_stateowner));
	file_lock->c.flc_pid = current->tgid;
	file_lock->c.flc_file = nf->nf_file;
	file_lock->c.flc_flags = FL_POSIX;
	file_lock->fl_lmops = &nfsd_posix_mng_ops;
	file_lock->fl_start = locku->lu_offset;

	file_lock->fl_end = last_byte_offset(locku->lu_offset,
						locku->lu_length);
	nfs4_transform_lock_offset(file_lock);

	err = vfs_lock_file(nf->nf_file, F_SETLK, file_lock, NULL);
	if (err) {
		dprintk("NFSD: nfs4_locku: vfs_lock_file failed!\n");
		goto out_nfserr;
	}
	nfs4_inc_and_copy_stateid(&locku->lu_stateid, &stp->st_stid);
put_file:
	nfsd_file_put(nf);
put_stateid:
	mutex_unlock(&stp->st_mutex);
	nfs4_put_stid(&stp->st_stid);
out:
	nfsd4_bump_seqid(cstate, status);
	if (file_lock)
		locks_free_lock(file_lock);
	return status;

out_nfserr:
	status = nfserrno(err);
	goto put_file;
}

/*
 * returns
 * 	true:  locks held by lockowner
 * 	false: no locks held by lockowner
 */
static bool
check_for_locks(struct nfs4_file *fp, struct nfs4_lockowner *lowner)
{
	struct file_lock *fl;
	int status = false;
	struct nfsd_file *nf;
	struct inode *inode;
	struct file_lock_context *flctx;

	spin_lock(&fp->fi_lock);
	nf = find_any_file_locked(fp);
	if (!nf) {
		/* Any valid lock stateid should have some sort of access */
		WARN_ON_ONCE(1);
		goto out;
	}

	inode = file_inode(nf->nf_file);
	flctx = locks_inode_context(inode);

	if (flctx && !list_empty_careful(&flctx->flc_posix)) {
		spin_lock(&flctx->flc_lock);
		for_each_file_lock(fl, &flctx->flc_posix) {
			if (fl->c.flc_owner == (fl_owner_t)lowner) {
				status = true;
				break;
			}
		}
		spin_unlock(&flctx->flc_lock);
	}
out:
	spin_unlock(&fp->fi_lock);
	return status;
}

/**
 * nfsd4_release_lockowner - process NFSv4.0 RELEASE_LOCKOWNER operations
 * @rqstp: RPC transaction
 * @cstate: NFSv4 COMPOUND state
 * @u: RELEASE_LOCKOWNER arguments
 *
 * Check if there are any locks still held and if not, free the lockowner
 * and any lock state that is owned.
 *
 * Return values:
 *   %nfs_ok: lockowner released or not found
 *   %nfserr_locks_held: lockowner still in use
 *   %nfserr_stale_clientid: clientid no longer active
 *   %nfserr_expired: clientid not recognized
 */
__be32
nfsd4_release_lockowner(struct svc_rqst *rqstp,
			struct nfsd4_compound_state *cstate,
			union nfsd4_op_u *u)
{
	struct nfsd4_release_lockowner *rlockowner = &u->release_lockowner;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);
	clientid_t *clid = &rlockowner->rl_clientid;
	struct nfs4_ol_stateid *stp;
	struct nfs4_lockowner *lo;
	struct nfs4_client *clp;
	LIST_HEAD(reaplist);
	__be32 status;

	dprintk("nfsd4_release_lockowner clientid: (%08x/%08x):\n",
		clid->cl_boot, clid->cl_id);

	status = set_client(clid, cstate, nn);
	if (status)
		return status;
	clp = cstate->clp;

	spin_lock(&clp->cl_lock);
	lo = find_lockowner_str_locked(clp, &rlockowner->rl_owner);
	if (!lo) {
		spin_unlock(&clp->cl_lock);
		return nfs_ok;
	}

	list_for_each_entry(stp, &lo->lo_owner.so_stateids, st_perstateowner) {
		if (check_for_locks(stp->st_stid.sc_file, lo)) {
			spin_unlock(&clp->cl_lock);
			nfs4_put_stateowner(&lo->lo_owner);
			return nfserr_locks_held;
		}
	}
	unhash_lockowner_locked(lo);
	while (!list_empty(&lo->lo_owner.so_stateids)) {
		stp = list_first_entry(&lo->lo_owner.so_stateids,
				       struct nfs4_ol_stateid,
				       st_perstateowner);
		unhash_lock_stateid(stp);
		put_ol_stateid_locked(stp, &reaplist);
	}
	spin_unlock(&clp->cl_lock);

	free_ol_stateid_reaplist(&reaplist);
	remove_blocked_locks(lo);
	nfs4_put_stateowner(&lo->lo_owner);
	return nfs_ok;
}

static inline struct nfs4_client_reclaim *
alloc_reclaim(void)
{
	return kmalloc(sizeof(struct nfs4_client_reclaim), GFP_KERNEL);
}

bool
nfs4_has_reclaimed_state(struct xdr_netobj name, struct nfsd_net *nn)
{
	struct nfs4_client_reclaim *crp;

	crp = nfsd4_find_reclaim_client(name, nn);
	return (crp && crp->cr_clp);
}

/*
 * failure => all reset bets are off, nfserr_no_grace...
 *
 * The caller is responsible for freeing name.data if NULL is returned (it
 * will be freed in nfs4_remove_reclaim_record in the normal case).
 */
struct nfs4_client_reclaim *
nfs4_client_to_reclaim(struct xdr_netobj name, struct xdr_netobj princhash,
		struct nfsd_net *nn)
{
	unsigned int strhashval;
	struct nfs4_client_reclaim *crp;

	crp = alloc_reclaim();
	if (crp) {
		strhashval = clientstr_hashval(name);
		INIT_LIST_HEAD(&crp->cr_strhash);
		list_add(&crp->cr_strhash, &nn->reclaim_str_hashtbl[strhashval]);
		crp->cr_name.data = name.data;
		crp->cr_name.len = name.len;
		crp->cr_princhash.data = princhash.data;
		crp->cr_princhash.len = princhash.len;
		crp->cr_clp = NULL;
		nn->reclaim_str_hashtbl_size++;
	}
	return crp;
}

void
nfs4_remove_reclaim_record(struct nfs4_client_reclaim *crp, struct nfsd_net *nn)
{
	list_del(&crp->cr_strhash);
	kfree(crp->cr_name.data);
	kfree(crp->cr_princhash.data);
	kfree(crp);
	nn->reclaim_str_hashtbl_size--;
}

void
nfs4_release_reclaim(struct nfsd_net *nn)
{
	struct nfs4_client_reclaim *crp = NULL;
	int i;

	for (i = 0; i < CLIENT_HASH_SIZE; i++) {
		while (!list_empty(&nn->reclaim_str_hashtbl[i])) {
			crp = list_entry(nn->reclaim_str_hashtbl[i].next,
			                struct nfs4_client_reclaim, cr_strhash);
			nfs4_remove_reclaim_record(crp, nn);
		}
	}
	WARN_ON_ONCE(nn->reclaim_str_hashtbl_size);
}

/*
 * called from OPEN, CLAIM_PREVIOUS with a new clientid. */
struct nfs4_client_reclaim *
nfsd4_find_reclaim_client(struct xdr_netobj name, struct nfsd_net *nn)
{
	unsigned int strhashval;
	struct nfs4_client_reclaim *crp = NULL;

	strhashval = clientstr_hashval(name);
	list_for_each_entry(crp, &nn->reclaim_str_hashtbl[strhashval], cr_strhash) {
		if (compare_blob(&crp->cr_name, &name) == 0) {
			return crp;
		}
	}
	return NULL;
}

__be32
nfs4_check_open_reclaim(struct nfs4_client *clp)
{
	if (test_bit(NFSD4_CLIENT_RECLAIM_COMPLETE, &clp->cl_flags))
		return nfserr_no_grace;

	if (nfsd4_client_record_check(clp))
		return nfserr_reclaim_bad;

	return nfs_ok;
}

/*
 * Since the lifetime of a delegation isn't limited to that of an open, a
 * client may quite reasonably hang on to a delegation as long as it has
 * the inode cached.  This becomes an obvious problem the first time a
 * client's inode cache approaches the size of the server's total memory.
 *
 * For now we avoid this problem by imposing a hard limit on the number
 * of delegations, which varies according to the server's memory size.
 */
static void
set_max_delegations(void)
{
	/*
	 * Allow at most 4 delegations per megabyte of RAM.  Quick
	 * estimates suggest that in the worst case (where every delegation
	 * is for a different inode), a delegation could take about 1.5K,
	 * giving a worst case usage of about 6% of memory.
	 */
	max_delegations = nr_free_buffer_pages() >> (20 - 2 - PAGE_SHIFT);
}

static int nfs4_state_create_net(struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	int i;

	nn->conf_id_hashtbl = kmalloc_array(CLIENT_HASH_SIZE,
					    sizeof(struct list_head),
					    GFP_KERNEL);
	if (!nn->conf_id_hashtbl)
		goto err;
	nn->unconf_id_hashtbl = kmalloc_array(CLIENT_HASH_SIZE,
					      sizeof(struct list_head),
					      GFP_KERNEL);
	if (!nn->unconf_id_hashtbl)
		goto err_unconf_id;
	nn->sessionid_hashtbl = kmalloc_array(SESSION_HASH_SIZE,
					      sizeof(struct list_head),
					      GFP_KERNEL);
	if (!nn->sessionid_hashtbl)
		goto err_sessionid;

	for (i = 0; i < CLIENT_HASH_SIZE; i++) {
		INIT_LIST_HEAD(&nn->conf_id_hashtbl[i]);
		INIT_LIST_HEAD(&nn->unconf_id_hashtbl[i]);
	}
	for (i = 0; i < SESSION_HASH_SIZE; i++)
		INIT_LIST_HEAD(&nn->sessionid_hashtbl[i]);
	nn->conf_name_tree = RB_ROOT;
	nn->unconf_name_tree = RB_ROOT;
	nn->boot_time = ktime_get_real_seconds();
	nn->grace_ended = false;
	nn->nfsd4_manager.block_opens = true;
	INIT_LIST_HEAD(&nn->nfsd4_manager.list);
	INIT_LIST_HEAD(&nn->client_lru);
	INIT_LIST_HEAD(&nn->close_lru);
	INIT_LIST_HEAD(&nn->del_recall_lru);
	spin_lock_init(&nn->client_lock);
	spin_lock_init(&nn->s2s_cp_lock);
	idr_init(&nn->s2s_cp_stateids);
	atomic_set(&nn->pending_async_copies, 0);

	spin_lock_init(&nn->blocked_locks_lock);
	INIT_LIST_HEAD(&nn->blocked_locks_lru);

	INIT_DELAYED_WORK(&nn->laundromat_work, laundromat_main);
	INIT_WORK(&nn->nfsd_shrinker_work, nfsd4_state_shrinker_worker);
	get_net(net);

	nn->nfsd_client_shrinker = shrinker_alloc(0, "nfsd-client");
	if (!nn->nfsd_client_shrinker)
		goto err_shrinker;

	nn->nfsd_client_shrinker->scan_objects = nfsd4_state_shrinker_scan;
	nn->nfsd_client_shrinker->count_objects = nfsd4_state_shrinker_count;
	nn->nfsd_client_shrinker->private_data = nn;

	shrinker_register(nn->nfsd_client_shrinker);

	return 0;

err_shrinker:
	put_net(net);
	kfree(nn->sessionid_hashtbl);
err_sessionid:
	kfree(nn->unconf_id_hashtbl);
err_unconf_id:
	kfree(nn->conf_id_hashtbl);
err:
	return -ENOMEM;
}

static void
nfs4_state_destroy_net(struct net *net)
{
	int i;
	struct nfs4_client *clp = NULL;
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	for (i = 0; i < CLIENT_HASH_SIZE; i++) {
		while (!list_empty(&nn->conf_id_hashtbl[i])) {
			clp = list_entry(nn->conf_id_hashtbl[i].next, struct nfs4_client, cl_idhash);
			destroy_client(clp);
		}
	}

	WARN_ON(!list_empty(&nn->blocked_locks_lru));

	for (i = 0; i < CLIENT_HASH_SIZE; i++) {
		while (!list_empty(&nn->unconf_id_hashtbl[i])) {
			clp = list_entry(nn->unconf_id_hashtbl[i].next, struct nfs4_client, cl_idhash);
			destroy_client(clp);
		}
	}

	kfree(nn->sessionid_hashtbl);
	kfree(nn->unconf_id_hashtbl);
	kfree(nn->conf_id_hashtbl);
	put_net(net);
}

int
nfs4_state_start_net(struct net *net)
{
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);
	int ret;

	ret = nfs4_state_create_net(net);
	if (ret)
		return ret;
	locks_start_grace(net, &nn->nfsd4_manager);
	nfsd4_client_tracking_init(net);
	if (nn->track_reclaim_completes && nn->reclaim_str_hashtbl_size == 0)
		goto skip_grace;
	printk(KERN_INFO "NFSD: starting %lld-second grace period (net %x)\n",
	       nn->nfsd4_grace, net->ns.inum);
	trace_nfsd_grace_start(nn);
	queue_delayed_work(laundry_wq, &nn->laundromat_work, nn->nfsd4_grace * HZ);
	return 0;

skip_grace:
	printk(KERN_INFO "NFSD: no clients to reclaim, skipping NFSv4 grace period (net %x)\n",
			net->ns.inum);
	queue_delayed_work(laundry_wq, &nn->laundromat_work, nn->nfsd4_lease * HZ);
	nfsd4_end_grace(nn);
	return 0;
}

/* initialization to perform when the nfsd service is started: */
int
nfs4_state_start(void)
{
	int ret;

	ret = rhltable_init(&nfs4_file_rhltable, &nfs4_file_rhash_params);
	if (ret)
		return ret;

	nfsd_slot_shrinker = shrinker_alloc(0, "nfsd-DRC-slot");
	if (!nfsd_slot_shrinker) {
		rhltable_destroy(&nfs4_file_rhltable);
		return -ENOMEM;
	}
	nfsd_slot_shrinker->count_objects = nfsd_slot_count;
	nfsd_slot_shrinker->scan_objects = nfsd_slot_scan;
	shrinker_register(nfsd_slot_shrinker);

	set_max_delegations();
	return 0;
}

void
nfs4_state_shutdown_net(struct net *net)
{
	struct nfs4_delegation *dp = NULL;
	struct list_head *pos, *next, reaplist;
	struct nfsd_net *nn = net_generic(net, nfsd_net_id);

	shrinker_free(nn->nfsd_client_shrinker);
	cancel_work_sync(&nn->nfsd_shrinker_work);
	cancel_delayed_work_sync(&nn->laundromat_work);
	locks_end_grace(&nn->nfsd4_manager);

	INIT_LIST_HEAD(&reaplist);
	spin_lock(&state_lock);
	list_for_each_safe(pos, next, &nn->del_recall_lru) {
		dp = list_entry (pos, struct nfs4_delegation, dl_recall_lru);
		unhash_delegation_locked(dp, SC_STATUS_CLOSED);
		list_add(&dp->dl_recall_lru, &reaplist);
	}
	spin_unlock(&state_lock);
	list_for_each_safe(pos, next, &reaplist) {
		dp = list_entry (pos, struct nfs4_delegation, dl_recall_lru);
		list_del_init(&dp->dl_recall_lru);
		destroy_unhashed_deleg(dp);
	}

	nfsd4_client_tracking_exit(net);
	nfs4_state_destroy_net(net);
#ifdef CONFIG_NFSD_V4_2_INTER_SSC
	nfsd4_ssc_shutdown_umount(nn);
#endif
}

void
nfs4_state_shutdown(void)
{
	rhltable_destroy(&nfs4_file_rhltable);
	shrinker_free(nfsd_slot_shrinker);
}

static void
get_stateid(struct nfsd4_compound_state *cstate, stateid_t *stateid)
{
	if (HAS_CSTATE_FLAG(cstate, CURRENT_STATE_ID_FLAG) &&
	    CURRENT_STATEID(stateid))
		memcpy(stateid, &cstate->current_stateid, sizeof(stateid_t));
}

static void
put_stateid(struct nfsd4_compound_state *cstate, stateid_t *stateid)
{
	if (cstate->minorversion) {
		memcpy(&cstate->current_stateid, stateid, sizeof(stateid_t));
		SET_CSTATE_FLAG(cstate, CURRENT_STATE_ID_FLAG);
	}
}

void
clear_current_stateid(struct nfsd4_compound_state *cstate)
{
	CLEAR_CSTATE_FLAG(cstate, CURRENT_STATE_ID_FLAG);
}

/*
 * functions to set current state id
 */
void
nfsd4_set_opendowngradestateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	put_stateid(cstate, &u->open_downgrade.od_stateid);
}

void
nfsd4_set_openstateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	put_stateid(cstate, &u->open.op_stateid);
}

void
nfsd4_set_closestateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	put_stateid(cstate, &u->close.cl_stateid);
}

void
nfsd4_set_lockstateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	put_stateid(cstate, &u->lock.lk_resp_stateid);
}

/*
 * functions to consume current state id
 */

void
nfsd4_get_opendowngradestateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	get_stateid(cstate, &u->open_downgrade.od_stateid);
}

void
nfsd4_get_delegreturnstateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	get_stateid(cstate, &u->delegreturn.dr_stateid);
}

void
nfsd4_get_freestateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	get_stateid(cstate, &u->free_stateid.fr_stateid);
}

void
nfsd4_get_setattrstateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	get_stateid(cstate, &u->setattr.sa_stateid);
}

void
nfsd4_get_closestateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	get_stateid(cstate, &u->close.cl_stateid);
}

void
nfsd4_get_lockustateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	get_stateid(cstate, &u->locku.lu_stateid);
}

void
nfsd4_get_readstateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	get_stateid(cstate, &u->read.rd_stateid);
}

void
nfsd4_get_writestateid(struct nfsd4_compound_state *cstate,
		union nfsd4_op_u *u)
{
	get_stateid(cstate, &u->write.wr_stateid);
}

/**
 * set_cb_time - vet and set the timespec for a cb_getattr update
 * @cb: timestamp from the CB_GETATTR response
 * @orig: original timestamp in the inode
 * @now: current time
 *
 * Given a timestamp in a CB_GETATTR response, check it against the
 * current timestamp in the inode and the current time. Returns true
 * if the inode's timestamp needs to be updated, and false otherwise.
 * @cb may also be changed if the timestamp needs to be clamped.
 */
static bool set_cb_time(struct timespec64 *cb, const struct timespec64 *orig,
			const struct timespec64 *now)
{

	/*
	 * "When the time presented is before the original time, then the
	 *  update is ignored." Also no need to update if there is no change.
	 */
	if (timespec64_compare(cb, orig) <= 0)
		return false;

	/*
	 * "When the time presented is in the future, the server can either
	 *  clamp the new time to the current time, or it may
	 *  return NFS4ERR_DELAY to the client, allowing it to retry."
	 */
	if (timespec64_compare(cb, now) > 0) {
		/* clamp it */
		*cb = *now;
	}

	return true;
}

static int cb_getattr_update_times(struct dentry *dentry, struct nfs4_delegation *dp)
{
	struct inode *inode = d_inode(dentry);
	struct timespec64 now = current_time(inode);
	struct nfs4_cb_fattr *ncf = &dp->dl_cb_fattr;
	struct iattr attrs = { };
	int ret;

	if (deleg_attrs_deleg(dp->dl_type)) {
		struct timespec64 atime = inode_get_atime(inode);
		struct timespec64 mtime = inode_get_mtime(inode);

		attrs.ia_atime = ncf->ncf_cb_atime;
		attrs.ia_mtime = ncf->ncf_cb_mtime;

		if (set_cb_time(&attrs.ia_atime, &atime, &now))
			attrs.ia_valid |= ATTR_ATIME | ATTR_ATIME_SET;

		if (set_cb_time(&attrs.ia_mtime, &mtime, &now)) {
			attrs.ia_valid |= ATTR_CTIME | ATTR_MTIME | ATTR_MTIME_SET;
			attrs.ia_ctime = attrs.ia_mtime;
		}
	} else {
		attrs.ia_valid |= ATTR_MTIME | ATTR_CTIME;
		attrs.ia_mtime = attrs.ia_ctime = now;
	}

	if (!attrs.ia_valid)
		return 0;

	attrs.ia_valid |= ATTR_DELEG;
	inode_lock(inode);
	ret = notify_change(&nop_mnt_idmap, dentry, &attrs, NULL);
	inode_unlock(inode);
	return ret;
}

/**
 * nfsd4_deleg_getattr_conflict - Recall if GETATTR causes conflict
 * @rqstp: RPC transaction context
 * @dentry: dentry of inode to be checked for a conflict
 * @pdp: returned WRITE delegation, if one was found
 *
 * This function is called when there is a conflict between a write
 * delegation and a change/size GETATTR from another client. The server
 * must either use the CB_GETATTR to get the current values of the
 * attributes from the client that holds the delegation or recall the
 * delegation before replying to the GETATTR. See RFC 8881 section
 * 18.7.4.
 *
 * Returns 0 if there is no conflict; otherwise an nfs_stat
 * code is returned. If @pdp is set to a non-NULL value, then the
 * caller must put the reference.
 */
__be32
nfsd4_deleg_getattr_conflict(struct svc_rqst *rqstp, struct dentry *dentry,
			     struct nfs4_delegation **pdp)
{
	__be32 status;
	struct nfsd_net *nn = net_generic(SVC_NET(rqstp), nfsd_net_id);
	struct file_lock_context *ctx;
	struct nfs4_delegation *dp = NULL;
	struct file_lease *fl;
	struct nfs4_cb_fattr *ncf;
	struct inode *inode = d_inode(dentry);

	ctx = locks_inode_context(inode);
	if (!ctx)
		return nfs_ok;

#define NON_NFSD_LEASE ((void *)1)

	spin_lock(&ctx->flc_lock);
	for_each_file_lock(fl, &ctx->flc_lease) {
		if (fl->c.flc_flags == FL_LAYOUT)
			continue;
		if (fl->c.flc_type == F_WRLCK) {
			if (fl->fl_lmops == &nfsd_lease_mng_ops)
				dp = fl->c.flc_owner;
			else
				dp = NON_NFSD_LEASE;
		}
		break;
	}
	if (dp == NULL || dp == NON_NFSD_LEASE ||
	    dp->dl_recall.cb_clp == *(rqstp->rq_lease_breaker)) {
		spin_unlock(&ctx->flc_lock);
		if (dp == NON_NFSD_LEASE) {
			status = nfserrno(nfsd_open_break_lease(inode,
								NFSD_MAY_READ));
			if (status != nfserr_jukebox ||
			    !nfsd_wait_for_delegreturn(rqstp, inode))
				return status;
		}
		return 0;
	}

	nfsd_stats_wdeleg_getattr_inc(nn);
	refcount_inc(&dp->dl_stid.sc_count);
	ncf = &dp->dl_cb_fattr;
	nfs4_cb_getattr(&dp->dl_cb_fattr);
	spin_unlock(&ctx->flc_lock);

	wait_on_bit_timeout(&ncf->ncf_getattr.cb_flags, NFSD4_CALLBACK_RUNNING,
			    TASK_UNINTERRUPTIBLE, NFSD_CB_GETATTR_TIMEOUT);
	if (ncf->ncf_cb_status) {
		/* Recall delegation only if client didn't respond */
		status = nfserrno(nfsd_open_break_lease(inode, NFSD_MAY_READ));
		if (status != nfserr_jukebox ||
		    !nfsd_wait_for_delegreturn(rqstp, inode))
			goto out_status;
	}
	if (!ncf->ncf_file_modified &&
	    (ncf->ncf_initial_cinfo != ncf->ncf_cb_change ||
	     ncf->ncf_cur_fsize != ncf->ncf_cb_fsize))
		ncf->ncf_file_modified = true;
	if (ncf->ncf_file_modified) {
		int err;

		/*
		 * Per section 10.4.3 of RFC 8881, the server would
		 * not update the file's metadata with the client's
		 * modified size
		 */
		err = cb_getattr_update_times(dentry, dp);
		if (err) {
			status = nfserrno(err);
			goto out_status;
		}
		ncf->ncf_cur_fsize = ncf->ncf_cb_fsize;
		*pdp = dp;
		return nfs_ok;
	}
	status = nfs_ok;
out_status:
	nfs4_put_stid(&dp->dl_stid);
	return status;
}
