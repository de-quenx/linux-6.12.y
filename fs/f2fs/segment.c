// SPDX-License-Identifier: GPL-2.0
/*
 * fs/f2fs/segment.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 */
#include <linux/fs.h>
#include <linux/f2fs_fs.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/sched/mm.h>
#include <linux/prefetch.h>
#include <linux/kthread.h>
#include <linux/swap.h>
#include <linux/timer.h>
#include <linux/freezer.h>
#include <linux/sched/signal.h>
#include <linux/random.h>

#include "f2fs.h"
#include "segment.h"
#include "node.h"
#include "gc.h"
#include "iostat.h"
#include <trace/events/f2fs.h>

#define __reverse_ffz(x) __reverse_ffs(~(x))

static struct kmem_cache *discard_entry_slab;
static struct kmem_cache *discard_cmd_slab;
static struct kmem_cache *sit_entry_set_slab;
static struct kmem_cache *revoke_entry_slab;

static unsigned long __reverse_ulong(unsigned char *str)
{
	unsigned long tmp = 0;
	int shift = 24, idx = 0;

#if BITS_PER_LONG == 64
	shift = 56;
#endif
	while (shift >= 0) {
		tmp |= (unsigned long)str[idx++] << shift;
		shift -= BITS_PER_BYTE;
	}
	return tmp;
}

/*
 * __reverse_ffs is copied from include/asm-generic/bitops/__ffs.h since
 * MSB and LSB are reversed in a byte by f2fs_set_bit.
 */
static inline unsigned long __reverse_ffs(unsigned long word)
{
	int num = 0;

#if BITS_PER_LONG == 64
	if ((word & 0xffffffff00000000UL) == 0)
		num += 32;
	else
		word >>= 32;
#endif
	if ((word & 0xffff0000) == 0)
		num += 16;
	else
		word >>= 16;

	if ((word & 0xff00) == 0)
		num += 8;
	else
		word >>= 8;

	if ((word & 0xf0) == 0)
		num += 4;
	else
		word >>= 4;

	if ((word & 0xc) == 0)
		num += 2;
	else
		word >>= 2;

	if ((word & 0x2) == 0)
		num += 1;
	return num;
}

/*
 * __find_rev_next(_zero)_bit is copied from lib/find_next_bit.c because
 * f2fs_set_bit makes MSB and LSB reversed in a byte.
 * @size must be integral times of unsigned long.
 * Example:
 *                             MSB <--> LSB
 *   f2fs_set_bit(0, bitmap) => 1000 0000
 *   f2fs_set_bit(7, bitmap) => 0000 0001
 */
static unsigned long __find_rev_next_bit(const unsigned long *addr,
			unsigned long size, unsigned long offset)
{
	const unsigned long *p = addr + BIT_WORD(offset);
	unsigned long result = size;
	unsigned long tmp;

	if (offset >= size)
		return size;

	size -= (offset & ~(BITS_PER_LONG - 1));
	offset %= BITS_PER_LONG;

	while (1) {
		if (*p == 0)
			goto pass;

		tmp = __reverse_ulong((unsigned char *)p);

		tmp &= ~0UL >> offset;
		if (size < BITS_PER_LONG)
			tmp &= (~0UL << (BITS_PER_LONG - size));
		if (tmp)
			goto found;
pass:
		if (size <= BITS_PER_LONG)
			break;
		size -= BITS_PER_LONG;
		offset = 0;
		p++;
	}
	return result;
found:
	return result - size + __reverse_ffs(tmp);
}

static unsigned long __find_rev_next_zero_bit(const unsigned long *addr,
			unsigned long size, unsigned long offset)
{
	const unsigned long *p = addr + BIT_WORD(offset);
	unsigned long result = size;
	unsigned long tmp;

	if (offset >= size)
		return size;

	size -= (offset & ~(BITS_PER_LONG - 1));
	offset %= BITS_PER_LONG;

	while (1) {
		if (*p == ~0UL)
			goto pass;

		tmp = __reverse_ulong((unsigned char *)p);

		if (offset)
			tmp |= ~0UL << (BITS_PER_LONG - offset);
		if (size < BITS_PER_LONG)
			tmp |= ~0UL >> size;
		if (tmp != ~0UL)
			goto found;
pass:
		if (size <= BITS_PER_LONG)
			break;
		size -= BITS_PER_LONG;
		offset = 0;
		p++;
	}
	return result;
found:
	return result - size + __reverse_ffz(tmp);
}

bool f2fs_need_SSR(struct f2fs_sb_info *sbi)
{
	int node_secs = get_blocktype_secs(sbi, F2FS_DIRTY_NODES);
	int dent_secs = get_blocktype_secs(sbi, F2FS_DIRTY_DENTS);
	int imeta_secs = get_blocktype_secs(sbi, F2FS_DIRTY_IMETA);

	if (f2fs_lfs_mode(sbi))
		return false;
	if (sbi->gc_mode == GC_URGENT_HIGH)
		return true;
	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return true;

	return free_sections(sbi) <= (node_secs + 2 * dent_secs + imeta_secs +
			SM_I(sbi)->min_ssr_sections + reserved_sections(sbi));
}

void f2fs_abort_atomic_write(struct inode *inode, bool clean)
{
	struct f2fs_inode_info *fi = F2FS_I(inode);

	if (!f2fs_is_atomic_file(inode))
		return;

	if (clean)
		truncate_inode_pages_final(inode->i_mapping);

	release_atomic_write_cnt(inode);
	clear_inode_flag(inode, FI_ATOMIC_COMMITTED);
	clear_inode_flag(inode, FI_ATOMIC_REPLACE);
	clear_inode_flag(inode, FI_ATOMIC_FILE);
	if (is_inode_flag_set(inode, FI_ATOMIC_DIRTIED)) {
		clear_inode_flag(inode, FI_ATOMIC_DIRTIED);
		f2fs_mark_inode_dirty_sync(inode, true);
	}
	stat_dec_atomic_inode(inode);

	F2FS_I(inode)->atomic_write_task = NULL;

	if (clean) {
		f2fs_i_size_write(inode, fi->original_i_size);
		fi->original_i_size = 0;
	}
	/* avoid stale dirty inode during eviction */
	sync_inode_metadata(inode, 0);
}

static int __replace_atomic_write_block(struct inode *inode, pgoff_t index,
			block_t new_addr, block_t *old_addr, bool recover)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct dnode_of_data dn;
	struct node_info ni;
	int err;

retry:
	set_new_dnode(&dn, inode, NULL, NULL, 0);
	err = f2fs_get_dnode_of_data(&dn, index, ALLOC_NODE);
	if (err) {
		if (err == -ENOMEM) {
			f2fs_io_schedule_timeout(DEFAULT_IO_TIMEOUT);
			goto retry;
		}
		return err;
	}

	err = f2fs_get_node_info(sbi, dn.nid, &ni, false);
	if (err) {
		f2fs_put_dnode(&dn);
		return err;
	}

	if (recover) {
		/* dn.data_blkaddr is always valid */
		if (!__is_valid_data_blkaddr(new_addr)) {
			if (new_addr == NULL_ADDR)
				dec_valid_block_count(sbi, inode, 1);
			f2fs_invalidate_blocks(sbi, dn.data_blkaddr);
			f2fs_update_data_blkaddr(&dn, new_addr);
		} else {
			f2fs_replace_block(sbi, &dn, dn.data_blkaddr,
				new_addr, ni.version, true, true);
		}
	} else {
		blkcnt_t count = 1;

		err = inc_valid_block_count(sbi, inode, &count, true);
		if (err) {
			f2fs_put_dnode(&dn);
			return err;
		}

		*old_addr = dn.data_blkaddr;
		f2fs_truncate_data_blocks_range(&dn, 1);
		dec_valid_block_count(sbi, F2FS_I(inode)->cow_inode, count);

		f2fs_replace_block(sbi, &dn, dn.data_blkaddr, new_addr,
					ni.version, true, false);
	}

	f2fs_put_dnode(&dn);

	trace_f2fs_replace_atomic_write_block(inode, F2FS_I(inode)->cow_inode,
			index, old_addr ? *old_addr : 0, new_addr, recover);
	return 0;
}

static void __complete_revoke_list(struct inode *inode, struct list_head *head,
					bool revoke)
{
	struct revoke_entry *cur, *tmp;
	pgoff_t start_index = 0;
	bool truncate = is_inode_flag_set(inode, FI_ATOMIC_REPLACE);

	list_for_each_entry_safe(cur, tmp, head, list) {
		if (revoke) {
			__replace_atomic_write_block(inode, cur->index,
						cur->old_addr, NULL, true);
		} else if (truncate) {
			f2fs_truncate_hole(inode, start_index, cur->index);
			start_index = cur->index + 1;
		}

		list_del(&cur->list);
		kmem_cache_free(revoke_entry_slab, cur);
	}

	if (!revoke && truncate)
		f2fs_do_truncate_blocks(inode, start_index * PAGE_SIZE, false);
}

static int __f2fs_commit_atomic_write(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	struct inode *cow_inode = fi->cow_inode;
	struct revoke_entry *new;
	struct list_head revoke_list;
	block_t blkaddr;
	struct dnode_of_data dn;
	pgoff_t len = DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
	pgoff_t off = 0, blen, index;
	int ret = 0, i;

	INIT_LIST_HEAD(&revoke_list);

	while (len) {
		blen = min_t(pgoff_t, ADDRS_PER_BLOCK(cow_inode), len);

		set_new_dnode(&dn, cow_inode, NULL, NULL, 0);
		ret = f2fs_get_dnode_of_data(&dn, off, LOOKUP_NODE_RA);
		if (ret && ret != -ENOENT) {
			goto out;
		} else if (ret == -ENOENT) {
			ret = 0;
			if (dn.max_level == 0)
				goto out;
			goto next;
		}

		blen = min((pgoff_t)ADDRS_PER_PAGE(dn.node_page, cow_inode),
				len);
		index = off;
		for (i = 0; i < blen; i++, dn.ofs_in_node++, index++) {
			blkaddr = f2fs_data_blkaddr(&dn);

			if (!__is_valid_data_blkaddr(blkaddr)) {
				continue;
			} else if (!f2fs_is_valid_blkaddr(sbi, blkaddr,
					DATA_GENERIC_ENHANCE)) {
				f2fs_put_dnode(&dn);
				ret = -EFSCORRUPTED;
				goto out;
			}

			new = f2fs_kmem_cache_alloc(revoke_entry_slab, GFP_NOFS,
							true, NULL);

			ret = __replace_atomic_write_block(inode, index, blkaddr,
							&new->old_addr, false);
			if (ret) {
				f2fs_put_dnode(&dn);
				kmem_cache_free(revoke_entry_slab, new);
				goto out;
			}

			f2fs_update_data_blkaddr(&dn, NULL_ADDR);
			new->index = index;
			list_add_tail(&new->list, &revoke_list);
		}
		f2fs_put_dnode(&dn);
next:
		off += blen;
		len -= blen;
	}

out:
	if (ret) {
		sbi->revoked_atomic_block += fi->atomic_write_cnt;
	} else {
		sbi->committed_atomic_block += fi->atomic_write_cnt;
		set_inode_flag(inode, FI_ATOMIC_COMMITTED);

		/*
		 * inode may has no FI_ATOMIC_DIRTIED flag due to no write
		 * before commit.
		 */
		if (is_inode_flag_set(inode, FI_ATOMIC_DIRTIED)) {
			/* clear atomic dirty status and set vfs dirty status */
			clear_inode_flag(inode, FI_ATOMIC_DIRTIED);
			f2fs_mark_inode_dirty_sync(inode, true);
		}
	}

	__complete_revoke_list(inode, &revoke_list, ret ? true : false);

	return ret;
}

int f2fs_commit_atomic_write(struct inode *inode)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct f2fs_inode_info *fi = F2FS_I(inode);
	int err;

	err = filemap_write_and_wait_range(inode->i_mapping, 0, LLONG_MAX);
	if (err)
		return err;

	f2fs_down_write(&fi->i_gc_rwsem[WRITE]);
	f2fs_lock_op(sbi);

	err = __f2fs_commit_atomic_write(inode);

	f2fs_unlock_op(sbi);
	f2fs_up_write(&fi->i_gc_rwsem[WRITE]);

	return err;
}

/*
 * This function balances dirty node and dentry pages.
 * In addition, it controls garbage collection.
 */
void f2fs_balance_fs(struct f2fs_sb_info *sbi, bool need)
{
	if (f2fs_cp_error(sbi))
		return;

	if (time_to_inject(sbi, FAULT_CHECKPOINT))
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_FAULT_INJECT);

	/* balance_fs_bg is able to be pending */
	if (need && excess_cached_nats(sbi))
		f2fs_balance_fs_bg(sbi, false);

	if (!f2fs_is_checkpoint_ready(sbi))
		return;

	/*
	 * We should do GC or end up with checkpoint, if there are so many dirty
	 * dir/node pages without enough free segments.
	 */
	if (has_enough_free_secs(sbi, 0, 0))
		return;

	if (test_opt(sbi, GC_MERGE) && sbi->gc_thread &&
				sbi->gc_thread->f2fs_gc_task) {
		DEFINE_WAIT(wait);

		prepare_to_wait(&sbi->gc_thread->fggc_wq, &wait,
					TASK_UNINTERRUPTIBLE);
		wake_up(&sbi->gc_thread->gc_wait_queue_head);
		io_schedule();
		finish_wait(&sbi->gc_thread->fggc_wq, &wait);
	} else {
		struct f2fs_gc_control gc_control = {
			.victim_segno = NULL_SEGNO,
			.init_gc_type = BG_GC,
			.no_bg_gc = true,
			.should_migrate_blocks = false,
			.err_gc_skipped = false,
			.nr_free_secs = 1 };
		f2fs_down_write(&sbi->gc_lock);
		stat_inc_gc_call_count(sbi, FOREGROUND);
		f2fs_gc(sbi, &gc_control);
	}
}

static inline bool excess_dirty_threshold(struct f2fs_sb_info *sbi)
{
	int factor = f2fs_rwsem_is_locked(&sbi->cp_rwsem) ? 3 : 2;
	unsigned int dents = get_pages(sbi, F2FS_DIRTY_DENTS);
	unsigned int qdata = get_pages(sbi, F2FS_DIRTY_QDATA);
	unsigned int nodes = get_pages(sbi, F2FS_DIRTY_NODES);
	unsigned int meta = get_pages(sbi, F2FS_DIRTY_META);
	unsigned int imeta = get_pages(sbi, F2FS_DIRTY_IMETA);
	unsigned int threshold =
		SEGS_TO_BLKS(sbi, (factor * DEFAULT_DIRTY_THRESHOLD));
	unsigned int global_threshold = threshold * 3 / 2;

	if (dents >= threshold || qdata >= threshold ||
		nodes >= threshold || meta >= threshold ||
		imeta >= threshold)
		return true;
	return dents + qdata + nodes + meta + imeta >  global_threshold;
}

void f2fs_balance_fs_bg(struct f2fs_sb_info *sbi, bool from_bg)
{
	if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING)))
		return;

	/* try to shrink extent cache when there is no enough memory */
	if (!f2fs_available_free_memory(sbi, READ_EXTENT_CACHE))
		f2fs_shrink_read_extent_tree(sbi,
				READ_EXTENT_CACHE_SHRINK_NUMBER);

	/* try to shrink age extent cache when there is no enough memory */
	if (!f2fs_available_free_memory(sbi, AGE_EXTENT_CACHE))
		f2fs_shrink_age_extent_tree(sbi,
				AGE_EXTENT_CACHE_SHRINK_NUMBER);

	/* check the # of cached NAT entries */
	if (!f2fs_available_free_memory(sbi, NAT_ENTRIES))
		f2fs_try_to_free_nats(sbi, NAT_ENTRY_PER_BLOCK);

	if (!f2fs_available_free_memory(sbi, FREE_NIDS))
		f2fs_try_to_free_nids(sbi, MAX_FREE_NIDS);
	else
		f2fs_build_free_nids(sbi, false, false);

	if (excess_dirty_nats(sbi) || excess_dirty_threshold(sbi) ||
		excess_prefree_segs(sbi) || !f2fs_space_for_roll_forward(sbi))
		goto do_sync;

	/* there is background inflight IO or foreground operation recently */
	if (is_inflight_io(sbi, REQ_TIME) ||
		(!f2fs_time_over(sbi, REQ_TIME) && f2fs_rwsem_is_locked(&sbi->cp_rwsem)))
		return;

	/* exceed periodical checkpoint timeout threshold */
	if (f2fs_time_over(sbi, CP_TIME))
		goto do_sync;

	/* checkpoint is the only way to shrink partial cached entries */
	if (f2fs_available_free_memory(sbi, NAT_ENTRIES) &&
		f2fs_available_free_memory(sbi, INO_ENTRIES))
		return;

do_sync:
	if (test_opt(sbi, DATA_FLUSH) && from_bg) {
		struct blk_plug plug;

		mutex_lock(&sbi->flush_lock);

		blk_start_plug(&plug);
		f2fs_sync_dirty_inodes(sbi, FILE_INODE, false);
		blk_finish_plug(&plug);

		mutex_unlock(&sbi->flush_lock);
	}
	stat_inc_cp_call_count(sbi, BACKGROUND);
	f2fs_sync_fs(sbi->sb, 1);
}

static int __submit_flush_wait(struct f2fs_sb_info *sbi,
				struct block_device *bdev)
{
	int ret = blkdev_issue_flush(bdev);

	trace_f2fs_issue_flush(bdev, test_opt(sbi, NOBARRIER),
				test_opt(sbi, FLUSH_MERGE), ret);
	if (!ret)
		f2fs_update_iostat(sbi, NULL, FS_FLUSH_IO, 0);
	return ret;
}

static int submit_flush_wait(struct f2fs_sb_info *sbi, nid_t ino)
{
	int ret = 0;
	int i;

	if (!f2fs_is_multi_device(sbi))
		return __submit_flush_wait(sbi, sbi->sb->s_bdev);

	for (i = 0; i < sbi->s_ndevs; i++) {
		if (!f2fs_is_dirty_device(sbi, ino, i, FLUSH_INO))
			continue;
		ret = __submit_flush_wait(sbi, FDEV(i).bdev);
		if (ret)
			break;
	}
	return ret;
}

static int issue_flush_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct flush_cmd_control *fcc = SM_I(sbi)->fcc_info;
	wait_queue_head_t *q = &fcc->flush_wait_queue;
repeat:
	if (kthread_should_stop())
		return 0;

	if (!llist_empty(&fcc->issue_list)) {
		struct flush_cmd *cmd, *next;
		int ret;

		fcc->dispatch_list = llist_del_all(&fcc->issue_list);
		fcc->dispatch_list = llist_reverse_order(fcc->dispatch_list);

		cmd = llist_entry(fcc->dispatch_list, struct flush_cmd, llnode);

		ret = submit_flush_wait(sbi, cmd->ino);
		atomic_inc(&fcc->issued_flush);

		llist_for_each_entry_safe(cmd, next,
					  fcc->dispatch_list, llnode) {
			cmd->ret = ret;
			complete(&cmd->wait);
		}
		fcc->dispatch_list = NULL;
	}

	wait_event_interruptible(*q,
		kthread_should_stop() || !llist_empty(&fcc->issue_list));
	goto repeat;
}

int f2fs_issue_flush(struct f2fs_sb_info *sbi, nid_t ino)
{
	struct flush_cmd_control *fcc = SM_I(sbi)->fcc_info;
	struct flush_cmd cmd;
	int ret;

	if (test_opt(sbi, NOBARRIER))
		return 0;

	if (!test_opt(sbi, FLUSH_MERGE)) {
		atomic_inc(&fcc->queued_flush);
		ret = submit_flush_wait(sbi, ino);
		atomic_dec(&fcc->queued_flush);
		atomic_inc(&fcc->issued_flush);
		return ret;
	}

	if (atomic_inc_return(&fcc->queued_flush) == 1 ||
	    f2fs_is_multi_device(sbi)) {
		ret = submit_flush_wait(sbi, ino);
		atomic_dec(&fcc->queued_flush);

		atomic_inc(&fcc->issued_flush);
		return ret;
	}

	cmd.ino = ino;
	init_completion(&cmd.wait);

	llist_add(&cmd.llnode, &fcc->issue_list);

	/*
	 * update issue_list before we wake up issue_flush thread, this
	 * smp_mb() pairs with another barrier in ___wait_event(), see
	 * more details in comments of waitqueue_active().
	 */
	smp_mb();

	if (waitqueue_active(&fcc->flush_wait_queue))
		wake_up(&fcc->flush_wait_queue);

	if (fcc->f2fs_issue_flush) {
		wait_for_completion(&cmd.wait);
		atomic_dec(&fcc->queued_flush);
	} else {
		struct llist_node *list;

		list = llist_del_all(&fcc->issue_list);
		if (!list) {
			wait_for_completion(&cmd.wait);
			atomic_dec(&fcc->queued_flush);
		} else {
			struct flush_cmd *tmp, *next;

			ret = submit_flush_wait(sbi, ino);

			llist_for_each_entry_safe(tmp, next, list, llnode) {
				if (tmp == &cmd) {
					cmd.ret = ret;
					atomic_dec(&fcc->queued_flush);
					continue;
				}
				tmp->ret = ret;
				complete(&tmp->wait);
			}
		}
	}

	return cmd.ret;
}

int f2fs_create_flush_cmd_control(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	struct flush_cmd_control *fcc;

	if (SM_I(sbi)->fcc_info) {
		fcc = SM_I(sbi)->fcc_info;
		if (fcc->f2fs_issue_flush)
			return 0;
		goto init_thread;
	}

	fcc = f2fs_kzalloc(sbi, sizeof(struct flush_cmd_control), GFP_KERNEL);
	if (!fcc)
		return -ENOMEM;
	atomic_set(&fcc->issued_flush, 0);
	atomic_set(&fcc->queued_flush, 0);
	init_waitqueue_head(&fcc->flush_wait_queue);
	init_llist_head(&fcc->issue_list);
	SM_I(sbi)->fcc_info = fcc;
	if (!test_opt(sbi, FLUSH_MERGE))
		return 0;

init_thread:
	fcc->f2fs_issue_flush = kthread_run(issue_flush_thread, sbi,
				"f2fs_flush-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(fcc->f2fs_issue_flush)) {
		int err = PTR_ERR(fcc->f2fs_issue_flush);

		fcc->f2fs_issue_flush = NULL;
		return err;
	}

	return 0;
}

void f2fs_destroy_flush_cmd_control(struct f2fs_sb_info *sbi, bool free)
{
	struct flush_cmd_control *fcc = SM_I(sbi)->fcc_info;

	if (fcc && fcc->f2fs_issue_flush) {
		struct task_struct *flush_thread = fcc->f2fs_issue_flush;

		fcc->f2fs_issue_flush = NULL;
		kthread_stop(flush_thread);
	}
	if (free) {
		kfree(fcc);
		SM_I(sbi)->fcc_info = NULL;
	}
}

int f2fs_flush_device_cache(struct f2fs_sb_info *sbi)
{
	int ret = 0, i;

	if (!f2fs_is_multi_device(sbi))
		return 0;

	if (test_opt(sbi, NOBARRIER))
		return 0;

	for (i = 1; i < sbi->s_ndevs; i++) {
		int count = DEFAULT_RETRY_IO_COUNT;

		if (!f2fs_test_bit(i, (char *)&sbi->dirty_device))
			continue;

		do {
			ret = __submit_flush_wait(sbi, FDEV(i).bdev);
			if (ret)
				f2fs_io_schedule_timeout(DEFAULT_IO_TIMEOUT);
		} while (ret && --count);

		if (ret) {
			f2fs_stop_checkpoint(sbi, false,
					STOP_CP_REASON_FLUSH_FAIL);
			break;
		}

		spin_lock(&sbi->dev_lock);
		f2fs_clear_bit(i, (char *)&sbi->dirty_device);
		spin_unlock(&sbi->dev_lock);
	}

	return ret;
}

static void __locate_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno,
		enum dirty_type dirty_type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	/* need not be added */
	if (IS_CURSEG(sbi, segno))
		return;

	if (!test_and_set_bit(segno, dirty_i->dirty_segmap[dirty_type]))
		dirty_i->nr_dirty[dirty_type]++;

	if (dirty_type == DIRTY) {
		struct seg_entry *sentry = get_seg_entry(sbi, segno);
		enum dirty_type t = sentry->type;

		if (unlikely(t >= DIRTY)) {
			f2fs_bug_on(sbi, 1);
			return;
		}
		if (!test_and_set_bit(segno, dirty_i->dirty_segmap[t]))
			dirty_i->nr_dirty[t]++;

		if (__is_large_section(sbi)) {
			unsigned int secno = GET_SEC_FROM_SEG(sbi, segno);
			block_t valid_blocks =
				get_valid_blocks(sbi, segno, true);

			f2fs_bug_on(sbi,
				(!is_sbi_flag_set(sbi, SBI_CP_DISABLED) &&
				!valid_blocks) ||
				valid_blocks == CAP_BLKS_PER_SEC(sbi));

			if (!IS_CURSEC(sbi, secno))
				set_bit(secno, dirty_i->dirty_secmap);
		}
	}
}

static void __remove_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno,
		enum dirty_type dirty_type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	block_t valid_blocks;

	if (test_and_clear_bit(segno, dirty_i->dirty_segmap[dirty_type]))
		dirty_i->nr_dirty[dirty_type]--;

	if (dirty_type == DIRTY) {
		struct seg_entry *sentry = get_seg_entry(sbi, segno);
		enum dirty_type t = sentry->type;

		if (test_and_clear_bit(segno, dirty_i->dirty_segmap[t]))
			dirty_i->nr_dirty[t]--;

		valid_blocks = get_valid_blocks(sbi, segno, true);
		if (valid_blocks == 0) {
			clear_bit(GET_SEC_FROM_SEG(sbi, segno),
						dirty_i->victim_secmap);
#ifdef CONFIG_F2FS_CHECK_FS
			clear_bit(segno, SIT_I(sbi)->invalid_segmap);
#endif
		}
		if (__is_large_section(sbi)) {
			unsigned int secno = GET_SEC_FROM_SEG(sbi, segno);

			if (!valid_blocks ||
					valid_blocks == CAP_BLKS_PER_SEC(sbi)) {
				clear_bit(secno, dirty_i->dirty_secmap);
				return;
			}

			if (!IS_CURSEC(sbi, secno))
				set_bit(secno, dirty_i->dirty_secmap);
		}
	}
}

/*
 * Should not occur error such as -ENOMEM.
 * Adding dirty entry into seglist is not critical operation.
 * If a given segment is one of current working segments, it won't be added.
 */
static void locate_dirty_segment(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned short valid_blocks, ckpt_valid_blocks;
	unsigned int usable_blocks;

	if (segno == NULL_SEGNO || IS_CURSEG(sbi, segno))
		return;

	usable_blocks = f2fs_usable_blks_in_seg(sbi, segno);
	mutex_lock(&dirty_i->seglist_lock);

	valid_blocks = get_valid_blocks(sbi, segno, false);
	ckpt_valid_blocks = get_ckpt_valid_blocks(sbi, segno, false);

	if (valid_blocks == 0 && (!is_sbi_flag_set(sbi, SBI_CP_DISABLED) ||
		ckpt_valid_blocks == usable_blocks)) {
		__locate_dirty_segment(sbi, segno, PRE);
		__remove_dirty_segment(sbi, segno, DIRTY);
	} else if (valid_blocks < usable_blocks) {
		__locate_dirty_segment(sbi, segno, DIRTY);
	} else {
		/* Recovery routine with SSR needs this */
		__remove_dirty_segment(sbi, segno, DIRTY);
	}

	mutex_unlock(&dirty_i->seglist_lock);
}

/* This moves currently empty dirty blocks to prefree. Must hold seglist_lock */
void f2fs_dirty_to_prefree(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int segno;

	mutex_lock(&dirty_i->seglist_lock);
	for_each_set_bit(segno, dirty_i->dirty_segmap[DIRTY], MAIN_SEGS(sbi)) {
		if (get_valid_blocks(sbi, segno, false))
			continue;
		if (IS_CURSEG(sbi, segno))
			continue;
		__locate_dirty_segment(sbi, segno, PRE);
		__remove_dirty_segment(sbi, segno, DIRTY);
	}
	mutex_unlock(&dirty_i->seglist_lock);
}

block_t f2fs_get_unusable_blocks(struct f2fs_sb_info *sbi)
{
	int ovp_hole_segs =
		(overprovision_segments(sbi) - reserved_segments(sbi));
	block_t ovp_holes = SEGS_TO_BLKS(sbi, ovp_hole_segs);
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	block_t holes[2] = {0, 0};	/* DATA and NODE */
	block_t unusable;
	struct seg_entry *se;
	unsigned int segno;

	mutex_lock(&dirty_i->seglist_lock);
	for_each_set_bit(segno, dirty_i->dirty_segmap[DIRTY], MAIN_SEGS(sbi)) {
		se = get_seg_entry(sbi, segno);
		if (IS_NODESEG(se->type))
			holes[NODE] += f2fs_usable_blks_in_seg(sbi, segno) -
							se->valid_blocks;
		else
			holes[DATA] += f2fs_usable_blks_in_seg(sbi, segno) -
							se->valid_blocks;
	}
	mutex_unlock(&dirty_i->seglist_lock);

	unusable = max(holes[DATA], holes[NODE]);
	if (unusable > ovp_holes)
		return unusable - ovp_holes;
	return 0;
}

int f2fs_disable_cp_again(struct f2fs_sb_info *sbi, block_t unusable)
{
	int ovp_hole_segs =
		(overprovision_segments(sbi) - reserved_segments(sbi));

	if (F2FS_OPTION(sbi).unusable_cap_perc == 100)
		return 0;
	if (unusable > F2FS_OPTION(sbi).unusable_cap)
		return -EAGAIN;
	if (is_sbi_flag_set(sbi, SBI_CP_DISABLED_QUICK) &&
		dirty_segments(sbi) > ovp_hole_segs)
		return -EAGAIN;
	if (has_not_enough_free_secs(sbi, 0, 0))
		return -EAGAIN;
	return 0;
}

/* This is only used by SBI_CP_DISABLED */
static unsigned int get_free_segment(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int segno = 0;

	mutex_lock(&dirty_i->seglist_lock);
	for_each_set_bit(segno, dirty_i->dirty_segmap[DIRTY], MAIN_SEGS(sbi)) {
		if (get_valid_blocks(sbi, segno, false))
			continue;
		if (get_ckpt_valid_blocks(sbi, segno, false))
			continue;
		mutex_unlock(&dirty_i->seglist_lock);
		return segno;
	}
	mutex_unlock(&dirty_i->seglist_lock);
	return NULL_SEGNO;
}

static struct discard_cmd *__create_discard_cmd(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t lstart,
		block_t start, block_t len)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *pend_list;
	struct discard_cmd *dc;

	f2fs_bug_on(sbi, !len);

	pend_list = &dcc->pend_list[plist_idx(len)];

	dc = f2fs_kmem_cache_alloc(discard_cmd_slab, GFP_NOFS, true, NULL);
	INIT_LIST_HEAD(&dc->list);
	dc->bdev = bdev;
	dc->di.lstart = lstart;
	dc->di.start = start;
	dc->di.len = len;
	dc->ref = 0;
	dc->state = D_PREP;
	dc->queued = 0;
	dc->error = 0;
	init_completion(&dc->wait);
	list_add_tail(&dc->list, pend_list);
	spin_lock_init(&dc->lock);
	dc->bio_ref = 0;
	atomic_inc(&dcc->discard_cmd_cnt);
	dcc->undiscard_blks += len;

	return dc;
}

static bool f2fs_check_discard_tree(struct f2fs_sb_info *sbi)
{
#ifdef CONFIG_F2FS_CHECK_FS
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct rb_node *cur = rb_first_cached(&dcc->root), *next;
	struct discard_cmd *cur_dc, *next_dc;

	while (cur) {
		next = rb_next(cur);
		if (!next)
			return true;

		cur_dc = rb_entry(cur, struct discard_cmd, rb_node);
		next_dc = rb_entry(next, struct discard_cmd, rb_node);

		if (cur_dc->di.lstart + cur_dc->di.len > next_dc->di.lstart) {
			f2fs_info(sbi, "broken discard_rbtree, "
				"cur(%u, %u) next(%u, %u)",
				cur_dc->di.lstart, cur_dc->di.len,
				next_dc->di.lstart, next_dc->di.len);
			return false;
		}
		cur = next;
	}
#endif
	return true;
}

static struct discard_cmd *__lookup_discard_cmd(struct f2fs_sb_info *sbi,
						block_t blkaddr)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct rb_node *node = dcc->root.rb_root.rb_node;
	struct discard_cmd *dc;

	while (node) {
		dc = rb_entry(node, struct discard_cmd, rb_node);

		if (blkaddr < dc->di.lstart)
			node = node->rb_left;
		else if (blkaddr >= dc->di.lstart + dc->di.len)
			node = node->rb_right;
		else
			return dc;
	}
	return NULL;
}

static struct discard_cmd *__lookup_discard_cmd_ret(struct rb_root_cached *root,
				block_t blkaddr,
				struct discard_cmd **prev_entry,
				struct discard_cmd **next_entry,
				struct rb_node ***insert_p,
				struct rb_node **insert_parent)
{
	struct rb_node **pnode = &root->rb_root.rb_node;
	struct rb_node *parent = NULL, *tmp_node;
	struct discard_cmd *dc;

	*insert_p = NULL;
	*insert_parent = NULL;
	*prev_entry = NULL;
	*next_entry = NULL;

	if (RB_EMPTY_ROOT(&root->rb_root))
		return NULL;

	while (*pnode) {
		parent = *pnode;
		dc = rb_entry(*pnode, struct discard_cmd, rb_node);

		if (blkaddr < dc->di.lstart)
			pnode = &(*pnode)->rb_left;
		else if (blkaddr >= dc->di.lstart + dc->di.len)
			pnode = &(*pnode)->rb_right;
		else
			goto lookup_neighbors;
	}

	*insert_p = pnode;
	*insert_parent = parent;

	dc = rb_entry(parent, struct discard_cmd, rb_node);
	tmp_node = parent;
	if (parent && blkaddr > dc->di.lstart)
		tmp_node = rb_next(parent);
	*next_entry = rb_entry_safe(tmp_node, struct discard_cmd, rb_node);

	tmp_node = parent;
	if (parent && blkaddr < dc->di.lstart)
		tmp_node = rb_prev(parent);
	*prev_entry = rb_entry_safe(tmp_node, struct discard_cmd, rb_node);
	return NULL;

lookup_neighbors:
	/* lookup prev node for merging backward later */
	tmp_node = rb_prev(&dc->rb_node);
	*prev_entry = rb_entry_safe(tmp_node, struct discard_cmd, rb_node);

	/* lookup next node for merging frontward later */
	tmp_node = rb_next(&dc->rb_node);
	*next_entry = rb_entry_safe(tmp_node, struct discard_cmd, rb_node);
	return dc;
}

static void __detach_discard_cmd(struct discard_cmd_control *dcc,
							struct discard_cmd *dc)
{
	if (dc->state == D_DONE)
		atomic_sub(dc->queued, &dcc->queued_discard);

	list_del(&dc->list);
	rb_erase_cached(&dc->rb_node, &dcc->root);
	dcc->undiscard_blks -= dc->di.len;

	kmem_cache_free(discard_cmd_slab, dc);

	atomic_dec(&dcc->discard_cmd_cnt);
}

static void __remove_discard_cmd(struct f2fs_sb_info *sbi,
							struct discard_cmd *dc)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	unsigned long flags;

	trace_f2fs_remove_discard(dc->bdev, dc->di.start, dc->di.len);

	spin_lock_irqsave(&dc->lock, flags);
	if (dc->bio_ref) {
		spin_unlock_irqrestore(&dc->lock, flags);
		return;
	}
	spin_unlock_irqrestore(&dc->lock, flags);

	f2fs_bug_on(sbi, dc->ref);

	if (dc->error == -EOPNOTSUPP)
		dc->error = 0;

	if (dc->error)
		f2fs_info_ratelimited(sbi,
			"Issue discard(%u, %u, %u) failed, ret: %d",
			dc->di.lstart, dc->di.start, dc->di.len, dc->error);
	__detach_discard_cmd(dcc, dc);
}

static void f2fs_submit_discard_endio(struct bio *bio)
{
	struct discard_cmd *dc = (struct discard_cmd *)bio->bi_private;
	unsigned long flags;

	spin_lock_irqsave(&dc->lock, flags);
	if (!dc->error)
		dc->error = blk_status_to_errno(bio->bi_status);
	dc->bio_ref--;
	if (!dc->bio_ref && dc->state == D_SUBMIT) {
		dc->state = D_DONE;
		complete_all(&dc->wait);
	}
	spin_unlock_irqrestore(&dc->lock, flags);
	bio_put(bio);
}

static void __check_sit_bitmap(struct f2fs_sb_info *sbi,
				block_t start, block_t end)
{
#ifdef CONFIG_F2FS_CHECK_FS
	struct seg_entry *sentry;
	unsigned int segno;
	block_t blk = start;
	unsigned long offset, size, *map;

	while (blk < end) {
		segno = GET_SEGNO(sbi, blk);
		sentry = get_seg_entry(sbi, segno);
		offset = GET_BLKOFF_FROM_SEG0(sbi, blk);

		if (end < START_BLOCK(sbi, segno + 1))
			size = GET_BLKOFF_FROM_SEG0(sbi, end);
		else
			size = BLKS_PER_SEG(sbi);
		map = (unsigned long *)(sentry->cur_valid_map);
		offset = __find_rev_next_bit(map, size, offset);
		f2fs_bug_on(sbi, offset != size);
		blk = START_BLOCK(sbi, segno + 1);
	}
#endif
}

static void __init_discard_policy(struct f2fs_sb_info *sbi,
				struct discard_policy *dpolicy,
				int discard_type, unsigned int granularity)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	/* common policy */
	dpolicy->type = discard_type;
	dpolicy->sync = true;
	dpolicy->ordered = false;
	dpolicy->granularity = granularity;

	dpolicy->max_requests = dcc->max_discard_request;
	dpolicy->io_aware_gran = dcc->discard_io_aware_gran;
	dpolicy->timeout = false;

	if (discard_type == DPOLICY_BG) {
		dpolicy->min_interval = dcc->min_discard_issue_time;
		dpolicy->mid_interval = dcc->mid_discard_issue_time;
		dpolicy->max_interval = dcc->max_discard_issue_time;
		if (dcc->discard_io_aware == DPOLICY_IO_AWARE_ENABLE)
			dpolicy->io_aware = true;
		else if (dcc->discard_io_aware == DPOLICY_IO_AWARE_DISABLE)
			dpolicy->io_aware = false;
		dpolicy->sync = false;
		dpolicy->ordered = true;
		if (utilization(sbi) > dcc->discard_urgent_util) {
			dpolicy->granularity = MIN_DISCARD_GRANULARITY;
			if (atomic_read(&dcc->discard_cmd_cnt))
				dpolicy->max_interval =
					dcc->min_discard_issue_time;
		}
	} else if (discard_type == DPOLICY_FORCE) {
		dpolicy->min_interval = dcc->min_discard_issue_time;
		dpolicy->mid_interval = dcc->mid_discard_issue_time;
		dpolicy->max_interval = dcc->max_discard_issue_time;
		dpolicy->io_aware = false;
	} else if (discard_type == DPOLICY_FSTRIM) {
		dpolicy->io_aware = false;
	} else if (discard_type == DPOLICY_UMOUNT) {
		dpolicy->io_aware = false;
		/* we need to issue all to keep CP_TRIMMED_FLAG */
		dpolicy->granularity = MIN_DISCARD_GRANULARITY;
		dpolicy->timeout = true;
	}
}

static void __update_discard_tree_range(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len);

#ifdef CONFIG_BLK_DEV_ZONED
static void __submit_zone_reset_cmd(struct f2fs_sb_info *sbi,
				   struct discard_cmd *dc, blk_opf_t flag,
				   struct list_head *wait_list,
				   unsigned int *issued)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct block_device *bdev = dc->bdev;
	struct bio *bio = bio_alloc(bdev, 0, REQ_OP_ZONE_RESET | flag, GFP_NOFS);
	unsigned long flags;

	trace_f2fs_issue_reset_zone(bdev, dc->di.start);

	spin_lock_irqsave(&dc->lock, flags);
	dc->state = D_SUBMIT;
	dc->bio_ref++;
	spin_unlock_irqrestore(&dc->lock, flags);

	if (issued)
		(*issued)++;

	atomic_inc(&dcc->queued_discard);
	dc->queued++;
	list_move_tail(&dc->list, wait_list);

	/* sanity check on discard range */
	__check_sit_bitmap(sbi, dc->di.lstart, dc->di.lstart + dc->di.len);

	bio->bi_iter.bi_sector = SECTOR_FROM_BLOCK(dc->di.start);
	bio->bi_private = dc;
	bio->bi_end_io = f2fs_submit_discard_endio;
	submit_bio(bio);

	atomic_inc(&dcc->issued_discard);
	f2fs_update_iostat(sbi, NULL, FS_ZONE_RESET_IO, dc->di.len * F2FS_BLKSIZE);
}
#endif

/* this function is copied from blkdev_issue_discard from block/blk-lib.c */
static int __submit_discard_cmd(struct f2fs_sb_info *sbi,
				struct discard_policy *dpolicy,
				struct discard_cmd *dc, int *issued)
{
	struct block_device *bdev = dc->bdev;
	unsigned int max_discard_blocks =
			SECTOR_TO_BLOCK(bdev_max_discard_sectors(bdev));
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *wait_list = (dpolicy->type == DPOLICY_FSTRIM) ?
					&(dcc->fstrim_list) : &(dcc->wait_list);
	blk_opf_t flag = dpolicy->sync ? REQ_SYNC : 0;
	block_t lstart, start, len, total_len;
	int err = 0;

	if (dc->state != D_PREP)
		return 0;

	if (is_sbi_flag_set(sbi, SBI_NEED_FSCK))
		return 0;

#ifdef CONFIG_BLK_DEV_ZONED
	if (f2fs_sb_has_blkzoned(sbi) && bdev_is_zoned(bdev)) {
		int devi = f2fs_bdev_index(sbi, bdev);

		if (devi < 0)
			return -EINVAL;

		if (f2fs_blkz_is_seq(sbi, devi, dc->di.start)) {
			__submit_zone_reset_cmd(sbi, dc, flag,
						wait_list, issued);
			return 0;
		}
	}
#endif

	/*
	 * stop issuing discard for any of below cases:
	 * 1. device is conventional zone, but it doesn't support discard.
	 * 2. device is regulare device, after snapshot it doesn't support
	 * discard.
	 */
	if (!bdev_max_discard_sectors(bdev))
		return -EOPNOTSUPP;

	trace_f2fs_issue_discard(bdev, dc->di.start, dc->di.len);

	lstart = dc->di.lstart;
	start = dc->di.start;
	len = dc->di.len;
	total_len = len;

	dc->di.len = 0;

	while (total_len && *issued < dpolicy->max_requests && !err) {
		struct bio *bio = NULL;
		unsigned long flags;
		bool last = true;

		if (len > max_discard_blocks) {
			len = max_discard_blocks;
			last = false;
		}

		(*issued)++;
		if (*issued == dpolicy->max_requests)
			last = true;

		dc->di.len += len;

		if (time_to_inject(sbi, FAULT_DISCARD)) {
			err = -EIO;
		} else {
			err = __blkdev_issue_discard(bdev,
					SECTOR_FROM_BLOCK(start),
					SECTOR_FROM_BLOCK(len),
					GFP_NOFS, &bio);
		}
		if (err) {
			spin_lock_irqsave(&dc->lock, flags);
			if (dc->state == D_PARTIAL)
				dc->state = D_SUBMIT;
			spin_unlock_irqrestore(&dc->lock, flags);

			break;
		}

		f2fs_bug_on(sbi, !bio);

		/*
		 * should keep before submission to avoid D_DONE
		 * right away
		 */
		spin_lock_irqsave(&dc->lock, flags);
		if (last)
			dc->state = D_SUBMIT;
		else
			dc->state = D_PARTIAL;
		dc->bio_ref++;
		spin_unlock_irqrestore(&dc->lock, flags);

		atomic_inc(&dcc->queued_discard);
		dc->queued++;
		list_move_tail(&dc->list, wait_list);

		/* sanity check on discard range */
		__check_sit_bitmap(sbi, lstart, lstart + len);

		bio->bi_private = dc;
		bio->bi_end_io = f2fs_submit_discard_endio;
		bio->bi_opf |= flag;
		submit_bio(bio);

		atomic_inc(&dcc->issued_discard);

		f2fs_update_iostat(sbi, NULL, FS_DISCARD_IO, len * F2FS_BLKSIZE);

		lstart += len;
		start += len;
		total_len -= len;
		len = total_len;
	}

	if (!err && len) {
		dcc->undiscard_blks -= len;
		__update_discard_tree_range(sbi, bdev, lstart, start, len);
	}
	return err;
}

static void __insert_discard_cmd(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct rb_node **p = &dcc->root.rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct discard_cmd *dc;
	bool leftmost = true;

	/* look up rb tree to find parent node */
	while (*p) {
		parent = *p;
		dc = rb_entry(parent, struct discard_cmd, rb_node);

		if (lstart < dc->di.lstart) {
			p = &(*p)->rb_left;
		} else if (lstart >= dc->di.lstart + dc->di.len) {
			p = &(*p)->rb_right;
			leftmost = false;
		} else {
			/* Let's skip to add, if exists */
			return;
		}
	}

	dc = __create_discard_cmd(sbi, bdev, lstart, start, len);

	rb_link_node(&dc->rb_node, parent, p);
	rb_insert_color_cached(&dc->rb_node, &dcc->root, leftmost);
}

static void __relocate_discard_cmd(struct discard_cmd_control *dcc,
						struct discard_cmd *dc)
{
	list_move_tail(&dc->list, &dcc->pend_list[plist_idx(dc->di.len)]);
}

static void __punch_discard_cmd(struct f2fs_sb_info *sbi,
				struct discard_cmd *dc, block_t blkaddr)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_info di = dc->di;
	bool modified = false;

	if (dc->state == D_DONE || dc->di.len == 1) {
		__remove_discard_cmd(sbi, dc);
		return;
	}

	dcc->undiscard_blks -= di.len;

	if (blkaddr > di.lstart) {
		dc->di.len = blkaddr - dc->di.lstart;
		dcc->undiscard_blks += dc->di.len;
		__relocate_discard_cmd(dcc, dc);
		modified = true;
	}

	if (blkaddr < di.lstart + di.len - 1) {
		if (modified) {
			__insert_discard_cmd(sbi, dc->bdev, blkaddr + 1,
					di.start + blkaddr + 1 - di.lstart,
					di.lstart + di.len - 1 - blkaddr);
		} else {
			dc->di.lstart++;
			dc->di.len--;
			dc->di.start++;
			dcc->undiscard_blks += dc->di.len;
			__relocate_discard_cmd(dcc, dc);
		}
	}
}

static void __update_discard_tree_range(struct f2fs_sb_info *sbi,
				struct block_device *bdev, block_t lstart,
				block_t start, block_t len)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *prev_dc = NULL, *next_dc = NULL;
	struct discard_cmd *dc;
	struct discard_info di = {0};
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	unsigned int max_discard_blocks =
			SECTOR_TO_BLOCK(bdev_max_discard_sectors(bdev));
	block_t end = lstart + len;

	dc = __lookup_discard_cmd_ret(&dcc->root, lstart,
				&prev_dc, &next_dc, &insert_p, &insert_parent);
	if (dc)
		prev_dc = dc;

	if (!prev_dc) {
		di.lstart = lstart;
		di.len = next_dc ? next_dc->di.lstart - lstart : len;
		di.len = min(di.len, len);
		di.start = start;
	}

	while (1) {
		struct rb_node *node;
		bool merged = false;
		struct discard_cmd *tdc = NULL;

		if (prev_dc) {
			di.lstart = prev_dc->di.lstart + prev_dc->di.len;
			if (di.lstart < lstart)
				di.lstart = lstart;
			if (di.lstart >= end)
				break;

			if (!next_dc || next_dc->di.lstart > end)
				di.len = end - di.lstart;
			else
				di.len = next_dc->di.lstart - di.lstart;
			di.start = start + di.lstart - lstart;
		}

		if (!di.len)
			goto next;

		if (prev_dc && prev_dc->state == D_PREP &&
			prev_dc->bdev == bdev &&
			__is_discard_back_mergeable(&di, &prev_dc->di,
							max_discard_blocks)) {
			prev_dc->di.len += di.len;
			dcc->undiscard_blks += di.len;
			__relocate_discard_cmd(dcc, prev_dc);
			di = prev_dc->di;
			tdc = prev_dc;
			merged = true;
		}

		if (next_dc && next_dc->state == D_PREP &&
			next_dc->bdev == bdev &&
			__is_discard_front_mergeable(&di, &next_dc->di,
							max_discard_blocks)) {
			next_dc->di.lstart = di.lstart;
			next_dc->di.len += di.len;
			next_dc->di.start = di.start;
			dcc->undiscard_blks += di.len;
			__relocate_discard_cmd(dcc, next_dc);
			if (tdc)
				__remove_discard_cmd(sbi, tdc);
			merged = true;
		}

		if (!merged)
			__insert_discard_cmd(sbi, bdev,
						di.lstart, di.start, di.len);
 next:
		prev_dc = next_dc;
		if (!prev_dc)
			break;

		node = rb_next(&prev_dc->rb_node);
		next_dc = rb_entry_safe(node, struct discard_cmd, rb_node);
	}
}

#ifdef CONFIG_BLK_DEV_ZONED
static void __queue_zone_reset_cmd(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t lblkstart,
		block_t blklen)
{
	trace_f2fs_queue_reset_zone(bdev, blkstart);

	mutex_lock(&SM_I(sbi)->dcc_info->cmd_lock);
	__insert_discard_cmd(sbi, bdev, lblkstart, blkstart, blklen);
	mutex_unlock(&SM_I(sbi)->dcc_info->cmd_lock);
}
#endif

static void __queue_discard_cmd(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t blklen)
{
	block_t lblkstart = blkstart;

	if (!f2fs_bdev_support_discard(bdev))
		return;

	trace_f2fs_queue_discard(bdev, blkstart, blklen);

	if (f2fs_is_multi_device(sbi)) {
		int devi = f2fs_target_device_index(sbi, blkstart);

		blkstart -= FDEV(devi).start_blk;
	}
	mutex_lock(&SM_I(sbi)->dcc_info->cmd_lock);
	__update_discard_tree_range(sbi, bdev, lblkstart, blkstart, blklen);
	mutex_unlock(&SM_I(sbi)->dcc_info->cmd_lock);
}

static void __issue_discard_cmd_orderly(struct f2fs_sb_info *sbi,
		struct discard_policy *dpolicy, int *issued)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *prev_dc = NULL, *next_dc = NULL;
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	struct discard_cmd *dc;
	struct blk_plug plug;
	bool io_interrupted = false;

	mutex_lock(&dcc->cmd_lock);
	dc = __lookup_discard_cmd_ret(&dcc->root, dcc->next_pos,
				&prev_dc, &next_dc, &insert_p, &insert_parent);
	if (!dc)
		dc = next_dc;

	blk_start_plug(&plug);

	while (dc) {
		struct rb_node *node;
		int err = 0;

		if (dc->state != D_PREP)
			goto next;

		if (dpolicy->io_aware && !is_idle(sbi, DISCARD_TIME)) {
			io_interrupted = true;
			break;
		}

		dcc->next_pos = dc->di.lstart + dc->di.len;
		err = __submit_discard_cmd(sbi, dpolicy, dc, issued);

		if (*issued >= dpolicy->max_requests)
			break;
next:
		node = rb_next(&dc->rb_node);
		if (err)
			__remove_discard_cmd(sbi, dc);
		dc = rb_entry_safe(node, struct discard_cmd, rb_node);
	}

	blk_finish_plug(&plug);

	if (!dc)
		dcc->next_pos = 0;

	mutex_unlock(&dcc->cmd_lock);

	if (!(*issued) && io_interrupted)
		*issued = -1;
}
static unsigned int __wait_all_discard_cmd(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy);

static int __issue_discard_cmd(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *pend_list;
	struct discard_cmd *dc, *tmp;
	struct blk_plug plug;
	int i, issued;
	bool io_interrupted = false;

	if (dpolicy->timeout)
		f2fs_update_time(sbi, UMOUNT_DISCARD_TIMEOUT);

retry:
	issued = 0;
	for (i = MAX_PLIST_NUM - 1; i >= 0; i--) {
		if (dpolicy->timeout &&
				f2fs_time_over(sbi, UMOUNT_DISCARD_TIMEOUT))
			break;

		if (i + 1 < dpolicy->granularity)
			break;

		if (i + 1 < dcc->max_ordered_discard && dpolicy->ordered) {
			__issue_discard_cmd_orderly(sbi, dpolicy, &issued);
			return issued;
		}

		pend_list = &dcc->pend_list[i];

		mutex_lock(&dcc->cmd_lock);
		if (list_empty(pend_list))
			goto next;
		if (unlikely(dcc->rbtree_check))
			f2fs_bug_on(sbi, !f2fs_check_discard_tree(sbi));
		blk_start_plug(&plug);
		list_for_each_entry_safe(dc, tmp, pend_list, list) {
			f2fs_bug_on(sbi, dc->state != D_PREP);

			if (dpolicy->timeout &&
				f2fs_time_over(sbi, UMOUNT_DISCARD_TIMEOUT))
				break;

			if (dpolicy->io_aware && i < dpolicy->io_aware_gran &&
						!is_idle(sbi, DISCARD_TIME)) {
				io_interrupted = true;
				break;
			}

			__submit_discard_cmd(sbi, dpolicy, dc, &issued);

			if (issued >= dpolicy->max_requests)
				break;
		}
		blk_finish_plug(&plug);
next:
		mutex_unlock(&dcc->cmd_lock);

		if (issued >= dpolicy->max_requests || io_interrupted)
			break;
	}

	if (dpolicy->type == DPOLICY_UMOUNT && issued) {
		__wait_all_discard_cmd(sbi, dpolicy);
		goto retry;
	}

	if (!issued && io_interrupted)
		issued = -1;

	return issued;
}

static bool __drop_discard_cmd(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *pend_list;
	struct discard_cmd *dc, *tmp;
	int i;
	bool dropped = false;

	mutex_lock(&dcc->cmd_lock);
	for (i = MAX_PLIST_NUM - 1; i >= 0; i--) {
		pend_list = &dcc->pend_list[i];
		list_for_each_entry_safe(dc, tmp, pend_list, list) {
			f2fs_bug_on(sbi, dc->state != D_PREP);
			__remove_discard_cmd(sbi, dc);
			dropped = true;
		}
	}
	mutex_unlock(&dcc->cmd_lock);

	return dropped;
}

void f2fs_drop_discard_cmd(struct f2fs_sb_info *sbi)
{
	__drop_discard_cmd(sbi);
}

static unsigned int __wait_one_discard_bio(struct f2fs_sb_info *sbi,
							struct discard_cmd *dc)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	unsigned int len = 0;

	wait_for_completion_io(&dc->wait);
	mutex_lock(&dcc->cmd_lock);
	f2fs_bug_on(sbi, dc->state != D_DONE);
	dc->ref--;
	if (!dc->ref) {
		if (!dc->error)
			len = dc->di.len;
		__remove_discard_cmd(sbi, dc);
	}
	mutex_unlock(&dcc->cmd_lock);

	return len;
}

static unsigned int __wait_discard_cmd_range(struct f2fs_sb_info *sbi,
						struct discard_policy *dpolicy,
						block_t start, block_t end)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *wait_list = (dpolicy->type == DPOLICY_FSTRIM) ?
					&(dcc->fstrim_list) : &(dcc->wait_list);
	struct discard_cmd *dc = NULL, *iter, *tmp;
	unsigned int trimmed = 0;

next:
	dc = NULL;

	mutex_lock(&dcc->cmd_lock);
	list_for_each_entry_safe(iter, tmp, wait_list, list) {
		if (iter->di.lstart + iter->di.len <= start ||
					end <= iter->di.lstart)
			continue;
		if (iter->di.len < dpolicy->granularity)
			continue;
		if (iter->state == D_DONE && !iter->ref) {
			wait_for_completion_io(&iter->wait);
			if (!iter->error)
				trimmed += iter->di.len;
			__remove_discard_cmd(sbi, iter);
		} else {
			iter->ref++;
			dc = iter;
			break;
		}
	}
	mutex_unlock(&dcc->cmd_lock);

	if (dc) {
		trimmed += __wait_one_discard_bio(sbi, dc);
		goto next;
	}

	return trimmed;
}

static unsigned int __wait_all_discard_cmd(struct f2fs_sb_info *sbi,
						struct discard_policy *dpolicy)
{
	struct discard_policy dp;
	unsigned int discard_blks;

	if (dpolicy)
		return __wait_discard_cmd_range(sbi, dpolicy, 0, UINT_MAX);

	/* wait all */
	__init_discard_policy(sbi, &dp, DPOLICY_FSTRIM, MIN_DISCARD_GRANULARITY);
	discard_blks = __wait_discard_cmd_range(sbi, &dp, 0, UINT_MAX);
	__init_discard_policy(sbi, &dp, DPOLICY_UMOUNT, MIN_DISCARD_GRANULARITY);
	discard_blks += __wait_discard_cmd_range(sbi, &dp, 0, UINT_MAX);

	return discard_blks;
}

/* This should be covered by global mutex, &sit_i->sentry_lock */
static void f2fs_wait_discard_bio(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *dc;
	bool need_wait = false;

	mutex_lock(&dcc->cmd_lock);
	dc = __lookup_discard_cmd(sbi, blkaddr);
#ifdef CONFIG_BLK_DEV_ZONED
	if (dc && f2fs_sb_has_blkzoned(sbi) && bdev_is_zoned(dc->bdev)) {
		int devi = f2fs_bdev_index(sbi, dc->bdev);

		if (devi < 0) {
			mutex_unlock(&dcc->cmd_lock);
			return;
		}

		if (f2fs_blkz_is_seq(sbi, devi, dc->di.start)) {
			/* force submit zone reset */
			if (dc->state == D_PREP)
				__submit_zone_reset_cmd(sbi, dc, REQ_SYNC,
							&dcc->wait_list, NULL);
			dc->ref++;
			mutex_unlock(&dcc->cmd_lock);
			/* wait zone reset */
			__wait_one_discard_bio(sbi, dc);
			return;
		}
	}
#endif
	if (dc) {
		if (dc->state == D_PREP) {
			__punch_discard_cmd(sbi, dc, blkaddr);
		} else {
			dc->ref++;
			need_wait = true;
		}
	}
	mutex_unlock(&dcc->cmd_lock);

	if (need_wait)
		__wait_one_discard_bio(sbi, dc);
}

void f2fs_stop_discard_thread(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	if (dcc && dcc->f2fs_issue_discard) {
		struct task_struct *discard_thread = dcc->f2fs_issue_discard;

		dcc->f2fs_issue_discard = NULL;
		kthread_stop(discard_thread);
	}
}

/**
 * f2fs_issue_discard_timeout() - Issue all discard cmd within UMOUNT_DISCARD_TIMEOUT
 * @sbi: the f2fs_sb_info data for discard cmd to issue
 *
 * When UMOUNT_DISCARD_TIMEOUT is exceeded, all remaining discard commands will be dropped
 *
 * Return true if issued all discard cmd or no discard cmd need issue, otherwise return false.
 */
bool f2fs_issue_discard_timeout(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_policy dpolicy;
	bool dropped;

	if (!atomic_read(&dcc->discard_cmd_cnt))
		return true;

	__init_discard_policy(sbi, &dpolicy, DPOLICY_UMOUNT,
					dcc->discard_granularity);
	__issue_discard_cmd(sbi, &dpolicy);
	dropped = __drop_discard_cmd(sbi);

	/* just to make sure there is no pending discard commands */
	__wait_all_discard_cmd(sbi, NULL);

	f2fs_bug_on(sbi, atomic_read(&dcc->discard_cmd_cnt));
	return !dropped;
}

static int issue_discard_thread(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	wait_queue_head_t *q = &dcc->discard_wait_queue;
	struct discard_policy dpolicy;
	unsigned int wait_ms = dcc->min_discard_issue_time;
	int issued;

	set_freezable();

	do {
		wait_event_freezable_timeout(*q,
				kthread_should_stop() || dcc->discard_wake,
				msecs_to_jiffies(wait_ms));

		if (sbi->gc_mode == GC_URGENT_HIGH ||
			!f2fs_available_free_memory(sbi, DISCARD_CACHE))
			__init_discard_policy(sbi, &dpolicy, DPOLICY_FORCE,
						MIN_DISCARD_GRANULARITY);
		else
			__init_discard_policy(sbi, &dpolicy, DPOLICY_BG,
						dcc->discard_granularity);

		if (dcc->discard_wake)
			dcc->discard_wake = false;

		/* clean up pending candidates before going to sleep */
		if (atomic_read(&dcc->queued_discard))
			__wait_all_discard_cmd(sbi, NULL);

		if (f2fs_readonly(sbi->sb))
			continue;
		if (kthread_should_stop())
			return 0;
		if (is_sbi_flag_set(sbi, SBI_NEED_FSCK) ||
			!atomic_read(&dcc->discard_cmd_cnt)) {
			wait_ms = dpolicy.max_interval;
			continue;
		}

		sb_start_intwrite(sbi->sb);

		issued = __issue_discard_cmd(sbi, &dpolicy);
		if (issued > 0) {
			__wait_all_discard_cmd(sbi, &dpolicy);
			wait_ms = dpolicy.min_interval;
		} else if (issued == -1) {
			wait_ms = f2fs_time_to_wait(sbi, DISCARD_TIME);
			if (!wait_ms)
				wait_ms = dpolicy.mid_interval;
		} else {
			wait_ms = dpolicy.max_interval;
		}
		if (!atomic_read(&dcc->discard_cmd_cnt))
			wait_ms = dpolicy.max_interval;

		sb_end_intwrite(sbi->sb);

	} while (!kthread_should_stop());
	return 0;
}

#ifdef CONFIG_BLK_DEV_ZONED
static int __f2fs_issue_discard_zone(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t blklen)
{
	sector_t sector, nr_sects;
	block_t lblkstart = blkstart;
	int devi = 0;
	u64 remainder = 0;

	if (f2fs_is_multi_device(sbi)) {
		devi = f2fs_target_device_index(sbi, blkstart);
		if (blkstart < FDEV(devi).start_blk ||
		    blkstart > FDEV(devi).end_blk) {
			f2fs_err(sbi, "Invalid block %x", blkstart);
			return -EIO;
		}
		blkstart -= FDEV(devi).start_blk;
	}

	/* For sequential zones, reset the zone write pointer */
	if (f2fs_blkz_is_seq(sbi, devi, blkstart)) {
		sector = SECTOR_FROM_BLOCK(blkstart);
		nr_sects = SECTOR_FROM_BLOCK(blklen);
		div64_u64_rem(sector, bdev_zone_sectors(bdev), &remainder);

		if (remainder || nr_sects != bdev_zone_sectors(bdev)) {
			f2fs_err(sbi, "(%d) %s: Unaligned zone reset attempted (block %x + %x)",
				 devi, sbi->s_ndevs ? FDEV(devi).path : "",
				 blkstart, blklen);
			return -EIO;
		}

		if (unlikely(is_sbi_flag_set(sbi, SBI_POR_DOING))) {
			unsigned int nofs_flags;
			int ret;

			trace_f2fs_issue_reset_zone(bdev, blkstart);
			nofs_flags = memalloc_nofs_save();
			ret = blkdev_zone_mgmt(bdev, REQ_OP_ZONE_RESET,
						sector, nr_sects);
			memalloc_nofs_restore(nofs_flags);
			return ret;
		}

		__queue_zone_reset_cmd(sbi, bdev, blkstart, lblkstart, blklen);
		return 0;
	}

	/* For conventional zones, use regular discard if supported */
	__queue_discard_cmd(sbi, bdev, lblkstart, blklen);
	return 0;
}
#endif

static int __issue_discard_async(struct f2fs_sb_info *sbi,
		struct block_device *bdev, block_t blkstart, block_t blklen)
{
#ifdef CONFIG_BLK_DEV_ZONED
	if (f2fs_sb_has_blkzoned(sbi) && bdev_is_zoned(bdev))
		return __f2fs_issue_discard_zone(sbi, bdev, blkstart, blklen);
#endif
	__queue_discard_cmd(sbi, bdev, blkstart, blklen);
	return 0;
}

static int f2fs_issue_discard(struct f2fs_sb_info *sbi,
				block_t blkstart, block_t blklen)
{
	sector_t start = blkstart, len = 0;
	struct block_device *bdev;
	struct seg_entry *se;
	unsigned int offset;
	block_t i;
	int err = 0;

	bdev = f2fs_target_device(sbi, blkstart, NULL);

	for (i = blkstart; i < blkstart + blklen; i++, len++) {
		if (i != start) {
			struct block_device *bdev2 =
				f2fs_target_device(sbi, i, NULL);

			if (bdev2 != bdev) {
				err = __issue_discard_async(sbi, bdev,
						start, len);
				if (err)
					return err;
				bdev = bdev2;
				start = i;
				len = 0;
			}
		}

		se = get_seg_entry(sbi, GET_SEGNO(sbi, i));
		offset = GET_BLKOFF_FROM_SEG0(sbi, i);

		if (f2fs_block_unit_discard(sbi) &&
				!f2fs_test_and_set_bit(offset, se->discard_map))
			sbi->discard_blks--;
	}

	if (len)
		err = __issue_discard_async(sbi, bdev, start, len);
	return err;
}

static bool add_discard_addrs(struct f2fs_sb_info *sbi, struct cp_control *cpc,
							bool check_only)
{
	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
	struct seg_entry *se = get_seg_entry(sbi, cpc->trim_start);
	unsigned long *cur_map = (unsigned long *)se->cur_valid_map;
	unsigned long *ckpt_map = (unsigned long *)se->ckpt_valid_map;
	unsigned long *discard_map = (unsigned long *)se->discard_map;
	unsigned long *dmap = SIT_I(sbi)->tmp_map;
	unsigned int start = 0, end = -1;
	bool force = (cpc->reason & CP_DISCARD);
	struct discard_entry *de = NULL;
	struct list_head *head = &SM_I(sbi)->dcc_info->entry_list;
	int i;

	if (se->valid_blocks == BLKS_PER_SEG(sbi) ||
	    !f2fs_hw_support_discard(sbi) ||
	    !f2fs_block_unit_discard(sbi))
		return false;

	if (!force) {
		if (!f2fs_realtime_discard_enable(sbi) || !se->valid_blocks ||
			SM_I(sbi)->dcc_info->nr_discards >=
				SM_I(sbi)->dcc_info->max_discards)
			return false;
	}

	/* SIT_VBLOCK_MAP_SIZE should be multiple of sizeof(unsigned long) */
	for (i = 0; i < entries; i++)
		dmap[i] = force ? ~ckpt_map[i] & ~discard_map[i] :
				(cur_map[i] ^ ckpt_map[i]) & ckpt_map[i];

	while (force || SM_I(sbi)->dcc_info->nr_discards <=
				SM_I(sbi)->dcc_info->max_discards) {
		start = __find_rev_next_bit(dmap, BLKS_PER_SEG(sbi), end + 1);
		if (start >= BLKS_PER_SEG(sbi))
			break;

		end = __find_rev_next_zero_bit(dmap,
						BLKS_PER_SEG(sbi), start + 1);
		if (force && start && end != BLKS_PER_SEG(sbi) &&
		    (end - start) < cpc->trim_minlen)
			continue;

		if (check_only)
			return true;

		if (!de) {
			de = f2fs_kmem_cache_alloc(discard_entry_slab,
						GFP_F2FS_ZERO, true, NULL);
			de->start_blkaddr = START_BLOCK(sbi, cpc->trim_start);
			list_add_tail(&de->list, head);
		}

		for (i = start; i < end; i++)
			__set_bit_le(i, (void *)de->discard_map);

		SM_I(sbi)->dcc_info->nr_discards += end - start;
	}
	return false;
}

static void release_discard_addr(struct discard_entry *entry)
{
	list_del(&entry->list);
	kmem_cache_free(discard_entry_slab, entry);
}

void f2fs_release_discard_addrs(struct f2fs_sb_info *sbi)
{
	struct list_head *head = &(SM_I(sbi)->dcc_info->entry_list);
	struct discard_entry *entry, *this;

	/* drop caches */
	list_for_each_entry_safe(entry, this, head, list)
		release_discard_addr(entry);
}

/*
 * Should call f2fs_clear_prefree_segments after checkpoint is done.
 */
static void set_prefree_as_free_segments(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int segno;

	mutex_lock(&dirty_i->seglist_lock);
	for_each_set_bit(segno, dirty_i->dirty_segmap[PRE], MAIN_SEGS(sbi))
		__set_test_and_free(sbi, segno, false);
	mutex_unlock(&dirty_i->seglist_lock);
}

void f2fs_clear_prefree_segments(struct f2fs_sb_info *sbi,
						struct cp_control *cpc)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct list_head *head = &dcc->entry_list;
	struct discard_entry *entry, *this;
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned long *prefree_map = dirty_i->dirty_segmap[PRE];
	unsigned int start = 0, end = -1;
	unsigned int secno, start_segno;
	bool force = (cpc->reason & CP_DISCARD);
	bool section_alignment = F2FS_OPTION(sbi).discard_unit ==
						DISCARD_UNIT_SECTION;

	if (f2fs_lfs_mode(sbi) && __is_large_section(sbi))
		section_alignment = true;

	mutex_lock(&dirty_i->seglist_lock);

	while (1) {
		int i;

		if (section_alignment && end != -1)
			end--;
		start = find_next_bit(prefree_map, MAIN_SEGS(sbi), end + 1);
		if (start >= MAIN_SEGS(sbi))
			break;
		end = find_next_zero_bit(prefree_map, MAIN_SEGS(sbi),
								start + 1);

		if (section_alignment) {
			start = rounddown(start, SEGS_PER_SEC(sbi));
			end = roundup(end, SEGS_PER_SEC(sbi));
		}

		for (i = start; i < end; i++) {
			if (test_and_clear_bit(i, prefree_map))
				dirty_i->nr_dirty[PRE]--;
		}

		if (!f2fs_realtime_discard_enable(sbi))
			continue;

		if (force && start >= cpc->trim_start &&
					(end - 1) <= cpc->trim_end)
			continue;

		/* Should cover 2MB zoned device for zone-based reset */
		if (!f2fs_sb_has_blkzoned(sbi) &&
		    (!f2fs_lfs_mode(sbi) || !__is_large_section(sbi))) {
			f2fs_issue_discard(sbi, START_BLOCK(sbi, start),
				SEGS_TO_BLKS(sbi, end - start));
			continue;
		}
next:
		secno = GET_SEC_FROM_SEG(sbi, start);
		start_segno = GET_SEG_FROM_SEC(sbi, secno);
		if (!IS_CURSEC(sbi, secno) &&
			!get_valid_blocks(sbi, start, true))
			f2fs_issue_discard(sbi, START_BLOCK(sbi, start_segno),
						BLKS_PER_SEC(sbi));

		start = start_segno + SEGS_PER_SEC(sbi);
		if (start < end)
			goto next;
		else
			end = start - 1;
	}
	mutex_unlock(&dirty_i->seglist_lock);

	if (!f2fs_block_unit_discard(sbi))
		goto wakeup;

	/* send small discards */
	list_for_each_entry_safe(entry, this, head, list) {
		unsigned int cur_pos = 0, next_pos, len, total_len = 0;
		bool is_valid = test_bit_le(0, entry->discard_map);

find_next:
		if (is_valid) {
			next_pos = find_next_zero_bit_le(entry->discard_map,
						BLKS_PER_SEG(sbi), cur_pos);
			len = next_pos - cur_pos;

			if (f2fs_sb_has_blkzoned(sbi) ||
			    (force && len < cpc->trim_minlen))
				goto skip;

			f2fs_issue_discard(sbi, entry->start_blkaddr + cur_pos,
									len);
			total_len += len;
		} else {
			next_pos = find_next_bit_le(entry->discard_map,
						BLKS_PER_SEG(sbi), cur_pos);
		}
skip:
		cur_pos = next_pos;
		is_valid = !is_valid;

		if (cur_pos < BLKS_PER_SEG(sbi))
			goto find_next;

		release_discard_addr(entry);
		dcc->nr_discards -= total_len;
	}

wakeup:
	wake_up_discard_thread(sbi, false);
}

int f2fs_start_discard_thread(struct f2fs_sb_info *sbi)
{
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	int err = 0;

	if (f2fs_sb_has_readonly(sbi)) {
		f2fs_info(sbi,
			"Skip to start discard thread for readonly image");
		return 0;
	}

	if (!f2fs_realtime_discard_enable(sbi))
		return 0;

	dcc->f2fs_issue_discard = kthread_run(issue_discard_thread, sbi,
				"f2fs_discard-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(dcc->f2fs_issue_discard)) {
		err = PTR_ERR(dcc->f2fs_issue_discard);
		dcc->f2fs_issue_discard = NULL;
	}

	return err;
}

static int create_discard_cmd_control(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc;
	int err = 0, i;

	if (SM_I(sbi)->dcc_info) {
		dcc = SM_I(sbi)->dcc_info;
		goto init_thread;
	}

	dcc = f2fs_kzalloc(sbi, sizeof(struct discard_cmd_control), GFP_KERNEL);
	if (!dcc)
		return -ENOMEM;

	dcc->discard_io_aware_gran = MAX_PLIST_NUM;
	dcc->discard_granularity = DEFAULT_DISCARD_GRANULARITY;
	dcc->max_ordered_discard = DEFAULT_MAX_ORDERED_DISCARD_GRANULARITY;
	dcc->discard_io_aware = DPOLICY_IO_AWARE_ENABLE;
	if (F2FS_OPTION(sbi).discard_unit == DISCARD_UNIT_SEGMENT)
		dcc->discard_granularity = BLKS_PER_SEG(sbi);
	else if (F2FS_OPTION(sbi).discard_unit == DISCARD_UNIT_SECTION)
		dcc->discard_granularity = BLKS_PER_SEC(sbi);

	INIT_LIST_HEAD(&dcc->entry_list);
	for (i = 0; i < MAX_PLIST_NUM; i++)
		INIT_LIST_HEAD(&dcc->pend_list[i]);
	INIT_LIST_HEAD(&dcc->wait_list);
	INIT_LIST_HEAD(&dcc->fstrim_list);
	mutex_init(&dcc->cmd_lock);
	atomic_set(&dcc->issued_discard, 0);
	atomic_set(&dcc->queued_discard, 0);
	atomic_set(&dcc->discard_cmd_cnt, 0);
	dcc->nr_discards = 0;
	dcc->max_discards = SEGS_TO_BLKS(sbi, MAIN_SEGS(sbi));
	dcc->max_discard_request = DEF_MAX_DISCARD_REQUEST;
	dcc->min_discard_issue_time = DEF_MIN_DISCARD_ISSUE_TIME;
	dcc->mid_discard_issue_time = DEF_MID_DISCARD_ISSUE_TIME;
	dcc->max_discard_issue_time = DEF_MAX_DISCARD_ISSUE_TIME;
	dcc->discard_urgent_util = DEF_DISCARD_URGENT_UTIL;
	dcc->undiscard_blks = 0;
	dcc->next_pos = 0;
	dcc->root = RB_ROOT_CACHED;
	dcc->rbtree_check = false;

	init_waitqueue_head(&dcc->discard_wait_queue);
	SM_I(sbi)->dcc_info = dcc;
init_thread:
	err = f2fs_start_discard_thread(sbi);
	if (err) {
		kfree(dcc);
		SM_I(sbi)->dcc_info = NULL;
	}

	return err;
}

static void destroy_discard_cmd_control(struct f2fs_sb_info *sbi)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;

	if (!dcc)
		return;

	f2fs_stop_discard_thread(sbi);

	/*
	 * Recovery can cache discard commands, so in error path of
	 * fill_super(), it needs to give a chance to handle them.
	 */
	f2fs_issue_discard_timeout(sbi);

	kfree(dcc);
	SM_I(sbi)->dcc_info = NULL;
}

static bool __mark_sit_entry_dirty(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);

	if (!__test_and_set_bit(segno, sit_i->dirty_sentries_bitmap)) {
		sit_i->dirty_sentries++;
		return false;
	}

	return true;
}

static void __set_sit_entry_type(struct f2fs_sb_info *sbi, int type,
					unsigned int segno, int modified)
{
	struct seg_entry *se = get_seg_entry(sbi, segno);

	se->type = type;
	if (modified)
		__mark_sit_entry_dirty(sbi, segno);
}

static inline unsigned long long get_segment_mtime(struct f2fs_sb_info *sbi,
								block_t blkaddr)
{
	unsigned int segno = GET_SEGNO(sbi, blkaddr);

	if (segno == NULL_SEGNO)
		return 0;
	return get_seg_entry(sbi, segno)->mtime;
}

static void update_segment_mtime(struct f2fs_sb_info *sbi, block_t blkaddr,
						unsigned long long old_mtime)
{
	struct seg_entry *se;
	unsigned int segno = GET_SEGNO(sbi, blkaddr);
	unsigned long long ctime = get_mtime(sbi, false);
	unsigned long long mtime = old_mtime ? old_mtime : ctime;

	if (segno == NULL_SEGNO)
		return;

	se = get_seg_entry(sbi, segno);

	if (!se->mtime)
		se->mtime = mtime;
	else
		se->mtime = div_u64(se->mtime * se->valid_blocks + mtime,
						se->valid_blocks + 1);

	if (ctime > SIT_I(sbi)->max_mtime)
		SIT_I(sbi)->max_mtime = ctime;
}

static void update_sit_entry(struct f2fs_sb_info *sbi, block_t blkaddr, int del)
{
	struct seg_entry *se;
	unsigned int segno, offset;
	long int new_vblocks;
	bool exist;
#ifdef CONFIG_F2FS_CHECK_FS
	bool mir_exist;
#endif

	segno = GET_SEGNO(sbi, blkaddr);
	if (segno == NULL_SEGNO)
		return;

	se = get_seg_entry(sbi, segno);
	new_vblocks = se->valid_blocks + del;
	offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

	f2fs_bug_on(sbi, (new_vblocks < 0 ||
			(new_vblocks > f2fs_usable_blks_in_seg(sbi, segno))));

	se->valid_blocks = new_vblocks;

	/* Update valid block bitmap */
	if (del > 0) {
		exist = f2fs_test_and_set_bit(offset, se->cur_valid_map);
#ifdef CONFIG_F2FS_CHECK_FS
		mir_exist = f2fs_test_and_set_bit(offset,
						se->cur_valid_map_mir);
		if (unlikely(exist != mir_exist)) {
			f2fs_err(sbi, "Inconsistent error when setting bitmap, blk:%u, old bit:%d",
				 blkaddr, exist);
			f2fs_bug_on(sbi, 1);
		}
#endif
		if (unlikely(exist)) {
			f2fs_err(sbi, "Bitmap was wrongly set, blk:%u",
				 blkaddr);
			f2fs_bug_on(sbi, 1);
			se->valid_blocks--;
			del = 0;
		}

		if (f2fs_block_unit_discard(sbi) &&
				!f2fs_test_and_set_bit(offset, se->discard_map))
			sbi->discard_blks--;

		/*
		 * SSR should never reuse block which is checkpointed
		 * or newly invalidated.
		 */
		if (!is_sbi_flag_set(sbi, SBI_CP_DISABLED)) {
			if (!f2fs_test_and_set_bit(offset, se->ckpt_valid_map))
				se->ckpt_valid_blocks++;
		}
	} else {
		exist = f2fs_test_and_clear_bit(offset, se->cur_valid_map);
#ifdef CONFIG_F2FS_CHECK_FS
		mir_exist = f2fs_test_and_clear_bit(offset,
						se->cur_valid_map_mir);
		if (unlikely(exist != mir_exist)) {
			f2fs_err(sbi, "Inconsistent error when clearing bitmap, blk:%u, old bit:%d",
				 blkaddr, exist);
			f2fs_bug_on(sbi, 1);
		}
#endif
		if (unlikely(!exist)) {
			f2fs_err(sbi, "Bitmap was wrongly cleared, blk:%u",
				 blkaddr);
			f2fs_bug_on(sbi, 1);
			se->valid_blocks++;
			del = 0;
		} else if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
			/*
			 * If checkpoints are off, we must not reuse data that
			 * was used in the previous checkpoint. If it was used
			 * before, we must track that to know how much space we
			 * really have.
			 */
			if (f2fs_test_bit(offset, se->ckpt_valid_map)) {
				spin_lock(&sbi->stat_lock);
				sbi->unusable_block_count++;
				spin_unlock(&sbi->stat_lock);
			}
		}

		if (f2fs_block_unit_discard(sbi) &&
			f2fs_test_and_clear_bit(offset, se->discard_map))
			sbi->discard_blks++;
	}
	if (!f2fs_test_bit(offset, se->ckpt_valid_map))
		se->ckpt_valid_blocks += del;

	__mark_sit_entry_dirty(sbi, segno);

	/* update total number of valid blocks to be written in ckpt area */
	SIT_I(sbi)->written_valid_blocks += del;

	if (__is_large_section(sbi))
		get_sec_entry(sbi, segno)->valid_blocks += del;
}

void f2fs_invalidate_blocks(struct f2fs_sb_info *sbi, block_t addr)
{
	unsigned int segno = GET_SEGNO(sbi, addr);
	struct sit_info *sit_i = SIT_I(sbi);

	f2fs_bug_on(sbi, addr == NULL_ADDR);
	if (addr == NEW_ADDR || addr == COMPRESS_ADDR)
		return;

	f2fs_invalidate_internal_cache(sbi, addr);

	/* add it into sit main buffer */
	down_write(&sit_i->sentry_lock);

	update_segment_mtime(sbi, addr, 0);
	update_sit_entry(sbi, addr, -1);

	/* add it into dirty seglist */
	locate_dirty_segment(sbi, segno);

	up_write(&sit_i->sentry_lock);
}

bool f2fs_is_checkpointed_data(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int segno, offset;
	struct seg_entry *se;
	bool is_cp = false;

	if (!__is_valid_data_blkaddr(blkaddr))
		return true;

	down_read(&sit_i->sentry_lock);

	segno = GET_SEGNO(sbi, blkaddr);
	se = get_seg_entry(sbi, segno);
	offset = GET_BLKOFF_FROM_SEG0(sbi, blkaddr);

	if (f2fs_test_bit(offset, se->ckpt_valid_map))
		is_cp = true;

	up_read(&sit_i->sentry_lock);

	return is_cp;
}

static unsigned short f2fs_curseg_valid_blocks(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	if (sbi->ckpt->alloc_type[type] == SSR)
		return BLKS_PER_SEG(sbi);
	return curseg->next_blkoff;
}

/*
 * Calculate the number of current summary pages for writing
 */
int f2fs_npages_for_summary_flush(struct f2fs_sb_info *sbi, bool for_ra)
{
	int valid_sum_count = 0;
	int i, sum_in_page;

	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		if (sbi->ckpt->alloc_type[i] != SSR && for_ra)
			valid_sum_count +=
				le16_to_cpu(F2FS_CKPT(sbi)->cur_data_blkoff[i]);
		else
			valid_sum_count += f2fs_curseg_valid_blocks(sbi, i);
	}

	sum_in_page = (PAGE_SIZE - 2 * SUM_JOURNAL_SIZE -
			SUM_FOOTER_SIZE) / SUMMARY_SIZE;
	if (valid_sum_count <= sum_in_page)
		return 1;
	else if ((valid_sum_count - sum_in_page) <=
		(PAGE_SIZE - SUM_FOOTER_SIZE) / SUMMARY_SIZE)
		return 2;
	return 3;
}

/*
 * Caller should put this summary page
 */
struct page *f2fs_get_sum_page(struct f2fs_sb_info *sbi, unsigned int segno)
{
	if (unlikely(f2fs_cp_error(sbi)))
		return ERR_PTR(-EIO);
	return f2fs_get_meta_page_retry(sbi, GET_SUM_BLOCK(sbi, segno));
}

void f2fs_update_meta_page(struct f2fs_sb_info *sbi,
					void *src, block_t blk_addr)
{
	struct page *page = f2fs_grab_meta_page(sbi, blk_addr);

	memcpy(page_address(page), src, PAGE_SIZE);
	set_page_dirty(page);
	f2fs_put_page(page, 1);
}

static void write_sum_page(struct f2fs_sb_info *sbi,
			struct f2fs_summary_block *sum_blk, block_t blk_addr)
{
	f2fs_update_meta_page(sbi, (void *)sum_blk, blk_addr);
}

static void write_current_sum_page(struct f2fs_sb_info *sbi,
						int type, block_t blk_addr)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	struct page *page = f2fs_grab_meta_page(sbi, blk_addr);
	struct f2fs_summary_block *src = curseg->sum_blk;
	struct f2fs_summary_block *dst;

	dst = (struct f2fs_summary_block *)page_address(page);
	memset(dst, 0, PAGE_SIZE);

	mutex_lock(&curseg->curseg_mutex);

	down_read(&curseg->journal_rwsem);
	memcpy(&dst->journal, curseg->journal, SUM_JOURNAL_SIZE);
	up_read(&curseg->journal_rwsem);

	memcpy(dst->entries, src->entries, SUM_ENTRY_SIZE);
	memcpy(&dst->footer, &src->footer, SUM_FOOTER_SIZE);

	mutex_unlock(&curseg->curseg_mutex);

	set_page_dirty(page);
	f2fs_put_page(page, 1);
}

static int is_next_segment_free(struct f2fs_sb_info *sbi,
				struct curseg_info *curseg)
{
	unsigned int segno = curseg->segno + 1;
	struct free_segmap_info *free_i = FREE_I(sbi);

	if (segno < MAIN_SEGS(sbi) && segno % SEGS_PER_SEC(sbi))
		return !test_bit(segno, free_i->free_segmap);
	return 0;
}

/*
 * Find a new segment from the free segments bitmap to right order
 * This function should be returned with success, otherwise BUG
 */
static int get_new_segment(struct f2fs_sb_info *sbi,
			unsigned int *newseg, bool new_sec, bool pinning)
{
	struct free_segmap_info *free_i = FREE_I(sbi);
	unsigned int segno, secno, zoneno;
	unsigned int total_zones = MAIN_SECS(sbi) / sbi->secs_per_zone;
	unsigned int hint = GET_SEC_FROM_SEG(sbi, *newseg);
	unsigned int old_zoneno = GET_ZONE_FROM_SEG(sbi, *newseg);
	bool init = true;
	int i;
	int ret = 0;

	spin_lock(&free_i->segmap_lock);

	if (time_to_inject(sbi, FAULT_NO_SEGMENT)) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	if (!new_sec && ((*newseg + 1) % SEGS_PER_SEC(sbi))) {
		segno = find_next_zero_bit(free_i->free_segmap,
			GET_SEG_FROM_SEC(sbi, hint + 1), *newseg + 1);
		if (segno < GET_SEG_FROM_SEC(sbi, hint + 1))
			goto got_it;
	}

#ifdef CONFIG_BLK_DEV_ZONED
	/*
	 * If we format f2fs on zoned storage, let's try to get pinned sections
	 * from beginning of the storage, which should be a conventional one.
	 */
	if (f2fs_sb_has_blkzoned(sbi)) {
		/* Prioritize writing to conventional zones */
		if (sbi->blkzone_alloc_policy == BLKZONE_ALLOC_PRIOR_CONV || pinning)
			segno = 0;
		else
			segno = max(sbi->first_seq_zone_segno, *newseg);
		hint = GET_SEC_FROM_SEG(sbi, segno);
	}
#endif

find_other_zone:
	secno = find_next_zero_bit(free_i->free_secmap, MAIN_SECS(sbi), hint);

#ifdef CONFIG_BLK_DEV_ZONED
	if (secno >= MAIN_SECS(sbi) && f2fs_sb_has_blkzoned(sbi)) {
		/* Write only to sequential zones */
		if (sbi->blkzone_alloc_policy == BLKZONE_ALLOC_ONLY_SEQ) {
			hint = GET_SEC_FROM_SEG(sbi, sbi->first_seq_zone_segno);
			secno = find_next_zero_bit(free_i->free_secmap, MAIN_SECS(sbi), hint);
		} else
			secno = find_first_zero_bit(free_i->free_secmap,
								MAIN_SECS(sbi));
		if (secno >= MAIN_SECS(sbi)) {
			ret = -ENOSPC;
			f2fs_bug_on(sbi, 1);
			goto out_unlock;
		}
	}
#endif

	if (secno >= MAIN_SECS(sbi)) {
		secno = find_first_zero_bit(free_i->free_secmap,
							MAIN_SECS(sbi));
		if (secno >= MAIN_SECS(sbi)) {
			ret = -ENOSPC;
			f2fs_bug_on(sbi, 1);
			goto out_unlock;
		}
	}
	segno = GET_SEG_FROM_SEC(sbi, secno);
	zoneno = GET_ZONE_FROM_SEC(sbi, secno);

	/* give up on finding another zone */
	if (!init)
		goto got_it;
	if (sbi->secs_per_zone == 1)
		goto got_it;
	if (zoneno == old_zoneno)
		goto got_it;
	for (i = 0; i < NR_CURSEG_TYPE; i++)
		if (CURSEG_I(sbi, i)->zone == zoneno)
			break;

	if (i < NR_CURSEG_TYPE) {
		/* zone is in user, try another */
		if (zoneno + 1 >= total_zones)
			hint = 0;
		else
			hint = (zoneno + 1) * sbi->secs_per_zone;
		init = false;
		goto find_other_zone;
	}
got_it:
	/* set it as dirty segment in free segmap */
	if (test_bit(segno, free_i->free_segmap)) {
		ret = -EFSCORRUPTED;
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_CORRUPTED_FREE_BITMAP);
		goto out_unlock;
	}

	/* no free section in conventional device or conventional zone */
	if (new_sec && pinning &&
		f2fs_is_sequential_zone_area(sbi, START_BLOCK(sbi, segno))) {
		ret = -EAGAIN;
		goto out_unlock;
	}
	__set_inuse(sbi, segno);
	*newseg = segno;
out_unlock:
	spin_unlock(&free_i->segmap_lock);

	if (ret == -ENOSPC)
		f2fs_stop_checkpoint(sbi, false, STOP_CP_REASON_NO_SEGMENT);
	return ret;
}

static void reset_curseg(struct f2fs_sb_info *sbi, int type, int modified)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	struct summary_footer *sum_footer;
	unsigned short seg_type = curseg->seg_type;

	/* only happen when get_new_segment() fails */
	if (curseg->next_segno == NULL_SEGNO)
		return;

	curseg->inited = true;
	curseg->segno = curseg->next_segno;
	curseg->zone = GET_ZONE_FROM_SEG(sbi, curseg->segno);
	curseg->next_blkoff = 0;
	curseg->next_segno = NULL_SEGNO;

	sum_footer = &(curseg->sum_blk->footer);
	memset(sum_footer, 0, sizeof(struct summary_footer));

	sanity_check_seg_type(sbi, seg_type);

	if (IS_DATASEG(seg_type))
		SET_SUM_TYPE(sum_footer, SUM_TYPE_DATA);
	if (IS_NODESEG(seg_type))
		SET_SUM_TYPE(sum_footer, SUM_TYPE_NODE);
	__set_sit_entry_type(sbi, seg_type, curseg->segno, modified);
}

static unsigned int __get_next_segno(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned short seg_type = curseg->seg_type;

	sanity_check_seg_type(sbi, seg_type);
	if (__is_large_section(sbi)) {
		if (f2fs_need_rand_seg(sbi)) {
			unsigned int hint = GET_SEC_FROM_SEG(sbi, curseg->segno);

			if (GET_SEC_FROM_SEG(sbi, curseg->segno + 1) != hint)
				return curseg->segno;
			return get_random_u32_inclusive(curseg->segno + 1,
					GET_SEG_FROM_SEC(sbi, hint + 1) - 1);
		}
		return curseg->segno;
	} else if (f2fs_need_rand_seg(sbi)) {
		return get_random_u32_below(MAIN_SECS(sbi) * SEGS_PER_SEC(sbi));
	}

	/* inmem log may not locate on any segment after mount */
	if (!curseg->inited)
		return 0;

	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return 0;

	if (seg_type == CURSEG_HOT_DATA || IS_NODESEG(seg_type))
		return 0;

	if (SIT_I(sbi)->last_victim[ALLOC_NEXT])
		return SIT_I(sbi)->last_victim[ALLOC_NEXT];

	/* find segments from 0 to reuse freed segments */
	if (F2FS_OPTION(sbi).alloc_mode == ALLOC_MODE_REUSE)
		return 0;

	return curseg->segno;
}

/*
 * Allocate a current working segment.
 * This function always allocates a free segment in LFS manner.
 */
static int new_curseg(struct f2fs_sb_info *sbi, int type, bool new_sec)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned int segno = curseg->segno;
	bool pinning = type == CURSEG_COLD_DATA_PINNED;
	int ret;

	if (curseg->inited)
		write_sum_page(sbi, curseg->sum_blk, GET_SUM_BLOCK(sbi, segno));

	segno = __get_next_segno(sbi, type);
	ret = get_new_segment(sbi, &segno, new_sec, pinning);
	if (ret) {
		if (ret == -ENOSPC)
			curseg->segno = NULL_SEGNO;
		return ret;
	}

	curseg->next_segno = segno;
	reset_curseg(sbi, type, 1);
	curseg->alloc_type = LFS;
	if (F2FS_OPTION(sbi).fs_mode == FS_MODE_FRAGMENT_BLK)
		curseg->fragment_remained_chunk =
				get_random_u32_inclusive(1, sbi->max_fragment_chunk);
	return 0;
}

static int __next_free_blkoff(struct f2fs_sb_info *sbi,
					int segno, block_t start)
{
	struct seg_entry *se = get_seg_entry(sbi, segno);
	int entries = SIT_VBLOCK_MAP_SIZE / sizeof(unsigned long);
	unsigned long *target_map = SIT_I(sbi)->tmp_map;
	unsigned long *ckpt_map = (unsigned long *)se->ckpt_valid_map;
	unsigned long *cur_map = (unsigned long *)se->cur_valid_map;
	int i;

	for (i = 0; i < entries; i++)
		target_map[i] = ckpt_map[i] | cur_map[i];

	return __find_rev_next_zero_bit(target_map, BLKS_PER_SEG(sbi), start);
}

static int f2fs_find_next_ssr_block(struct f2fs_sb_info *sbi,
		struct curseg_info *seg)
{
	return __next_free_blkoff(sbi, seg->segno, seg->next_blkoff + 1);
}

bool f2fs_segment_has_free_slot(struct f2fs_sb_info *sbi, int segno)
{
	return __next_free_blkoff(sbi, segno, 0) < BLKS_PER_SEG(sbi);
}

/*
 * This function always allocates a used segment(from dirty seglist) by SSR
 * manner, so it should recover the existing segment information of valid blocks
 */
static int change_curseg(struct f2fs_sb_info *sbi, int type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned int new_segno = curseg->next_segno;
	struct f2fs_summary_block *sum_node;
	struct page *sum_page;

	if (curseg->inited)
		write_sum_page(sbi, curseg->sum_blk, GET_SUM_BLOCK(sbi, curseg->segno));

	__set_test_and_inuse(sbi, new_segno);

	mutex_lock(&dirty_i->seglist_lock);
	__remove_dirty_segment(sbi, new_segno, PRE);
	__remove_dirty_segment(sbi, new_segno, DIRTY);
	mutex_unlock(&dirty_i->seglist_lock);

	reset_curseg(sbi, type, 1);
	curseg->alloc_type = SSR;
	curseg->next_blkoff = __next_free_blkoff(sbi, curseg->segno, 0);

	sum_page = f2fs_get_sum_page(sbi, new_segno);
	if (IS_ERR(sum_page)) {
		/* GC won't be able to use stale summary pages by cp_error */
		memset(curseg->sum_blk, 0, SUM_ENTRY_SIZE);
		return PTR_ERR(sum_page);
	}
	sum_node = (struct f2fs_summary_block *)page_address(sum_page);
	memcpy(curseg->sum_blk, sum_node, SUM_ENTRY_SIZE);
	f2fs_put_page(sum_page, 1);
	return 0;
}

static int get_ssr_segment(struct f2fs_sb_info *sbi, int type,
				int alloc_mode, unsigned long long age);

static int get_atssr_segment(struct f2fs_sb_info *sbi, int type,
					int target_type, int alloc_mode,
					unsigned long long age)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	int ret = 0;

	curseg->seg_type = target_type;

	if (get_ssr_segment(sbi, type, alloc_mode, age)) {
		struct seg_entry *se = get_seg_entry(sbi, curseg->next_segno);

		curseg->seg_type = se->type;
		ret = change_curseg(sbi, type);
	} else {
		/* allocate cold segment by default */
		curseg->seg_type = CURSEG_COLD_DATA;
		ret = new_curseg(sbi, type, true);
	}
	stat_inc_seg_type(sbi, curseg);
	return ret;
}

static int __f2fs_init_atgc_curseg(struct f2fs_sb_info *sbi, bool force)
{
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_ALL_DATA_ATGC);
	int ret = 0;

	if (!sbi->am.atgc_enabled && !force)
		return 0;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);

	mutex_lock(&curseg->curseg_mutex);
	down_write(&SIT_I(sbi)->sentry_lock);

	ret = get_atssr_segment(sbi, CURSEG_ALL_DATA_ATGC,
					CURSEG_COLD_DATA, SSR, 0);

	up_write(&SIT_I(sbi)->sentry_lock);
	mutex_unlock(&curseg->curseg_mutex);

	f2fs_up_read(&SM_I(sbi)->curseg_lock);
	return ret;
}

int f2fs_init_inmem_curseg(struct f2fs_sb_info *sbi)
{
	return __f2fs_init_atgc_curseg(sbi, false);
}

int f2fs_reinit_atgc_curseg(struct f2fs_sb_info *sbi)
{
	int ret;

	if (!test_opt(sbi, ATGC))
		return 0;
	if (sbi->am.atgc_enabled)
		return 0;
	if (le64_to_cpu(F2FS_CKPT(sbi)->elapsed_time) <
			sbi->am.age_threshold)
		return 0;

	ret = __f2fs_init_atgc_curseg(sbi, true);
	if (!ret) {
		sbi->am.atgc_enabled = true;
		f2fs_info(sbi, "reenabled age threshold GC");
	}
	return ret;
}

static void __f2fs_save_inmem_curseg(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	mutex_lock(&curseg->curseg_mutex);
	if (!curseg->inited)
		goto out;

	if (get_valid_blocks(sbi, curseg->segno, false)) {
		write_sum_page(sbi, curseg->sum_blk,
				GET_SUM_BLOCK(sbi, curseg->segno));
	} else {
		mutex_lock(&DIRTY_I(sbi)->seglist_lock);
		__set_test_and_free(sbi, curseg->segno, true);
		mutex_unlock(&DIRTY_I(sbi)->seglist_lock);
	}
out:
	mutex_unlock(&curseg->curseg_mutex);
}

void f2fs_save_inmem_curseg(struct f2fs_sb_info *sbi)
{
	__f2fs_save_inmem_curseg(sbi, CURSEG_COLD_DATA_PINNED);

	if (sbi->am.atgc_enabled)
		__f2fs_save_inmem_curseg(sbi, CURSEG_ALL_DATA_ATGC);
}

static void __f2fs_restore_inmem_curseg(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	mutex_lock(&curseg->curseg_mutex);
	if (!curseg->inited)
		goto out;
	if (get_valid_blocks(sbi, curseg->segno, false))
		goto out;

	mutex_lock(&DIRTY_I(sbi)->seglist_lock);
	__set_test_and_inuse(sbi, curseg->segno);
	mutex_unlock(&DIRTY_I(sbi)->seglist_lock);
out:
	mutex_unlock(&curseg->curseg_mutex);
}

void f2fs_restore_inmem_curseg(struct f2fs_sb_info *sbi)
{
	__f2fs_restore_inmem_curseg(sbi, CURSEG_COLD_DATA_PINNED);

	if (sbi->am.atgc_enabled)
		__f2fs_restore_inmem_curseg(sbi, CURSEG_ALL_DATA_ATGC);
}

static int get_ssr_segment(struct f2fs_sb_info *sbi, int type,
				int alloc_mode, unsigned long long age)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned segno = NULL_SEGNO;
	unsigned short seg_type = curseg->seg_type;
	int i, cnt;
	bool reversed = false;

	sanity_check_seg_type(sbi, seg_type);

	/* f2fs_need_SSR() already forces to do this */
	if (!f2fs_get_victim(sbi, &segno, BG_GC, seg_type,
				alloc_mode, age, false)) {
		curseg->next_segno = segno;
		return 1;
	}

	/* For node segments, let's do SSR more intensively */
	if (IS_NODESEG(seg_type)) {
		if (seg_type >= CURSEG_WARM_NODE) {
			reversed = true;
			i = CURSEG_COLD_NODE;
		} else {
			i = CURSEG_HOT_NODE;
		}
		cnt = NR_CURSEG_NODE_TYPE;
	} else {
		if (seg_type >= CURSEG_WARM_DATA) {
			reversed = true;
			i = CURSEG_COLD_DATA;
		} else {
			i = CURSEG_HOT_DATA;
		}
		cnt = NR_CURSEG_DATA_TYPE;
	}

	for (; cnt-- > 0; reversed ? i-- : i++) {
		if (i == seg_type)
			continue;
		if (!f2fs_get_victim(sbi, &segno, BG_GC, i,
					alloc_mode, age, false)) {
			curseg->next_segno = segno;
			return 1;
		}
	}

	/* find valid_blocks=0 in dirty list */
	if (unlikely(is_sbi_flag_set(sbi, SBI_CP_DISABLED))) {
		segno = get_free_segment(sbi);
		if (segno != NULL_SEGNO) {
			curseg->next_segno = segno;
			return 1;
		}
	}
	return 0;
}

static bool need_new_seg(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);

	if (!is_set_ckpt_flags(sbi, CP_CRC_RECOVERY_FLAG) &&
	    curseg->seg_type == CURSEG_WARM_NODE)
		return true;
	if (curseg->alloc_type == LFS && is_next_segment_free(sbi, curseg) &&
	    likely(!is_sbi_flag_set(sbi, SBI_CP_DISABLED)))
		return true;
	if (!f2fs_need_SSR(sbi) || !get_ssr_segment(sbi, type, SSR, 0))
		return true;
	return false;
}

int f2fs_allocate_segment_for_resize(struct f2fs_sb_info *sbi, int type,
					unsigned int start, unsigned int end)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned int segno;
	int ret = 0;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);
	mutex_lock(&curseg->curseg_mutex);
	down_write(&SIT_I(sbi)->sentry_lock);

	segno = CURSEG_I(sbi, type)->segno;
	if (segno < start || segno > end)
		goto unlock;

	if (f2fs_need_SSR(sbi) && get_ssr_segment(sbi, type, SSR, 0))
		ret = change_curseg(sbi, type);
	else
		ret = new_curseg(sbi, type, true);

	stat_inc_seg_type(sbi, curseg);

	locate_dirty_segment(sbi, segno);
unlock:
	up_write(&SIT_I(sbi)->sentry_lock);

	if (segno != curseg->segno)
		f2fs_notice(sbi, "For resize: curseg of type %d: %u ==> %u",
			    type, segno, curseg->segno);

	mutex_unlock(&curseg->curseg_mutex);
	f2fs_up_read(&SM_I(sbi)->curseg_lock);
	return ret;
}

static int __allocate_new_segment(struct f2fs_sb_info *sbi, int type,
						bool new_sec, bool force)
{
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned int old_segno;
	int err = 0;

	if (type == CURSEG_COLD_DATA_PINNED && !curseg->inited)
		goto allocate;

	if (!force && curseg->inited &&
	    !curseg->next_blkoff &&
	    !get_valid_blocks(sbi, curseg->segno, new_sec) &&
	    !get_ckpt_valid_blocks(sbi, curseg->segno, new_sec))
		return 0;

allocate:
	old_segno = curseg->segno;
	err = new_curseg(sbi, type, true);
	if (err)
		return err;
	stat_inc_seg_type(sbi, curseg);
	locate_dirty_segment(sbi, old_segno);
	return 0;
}

int f2fs_allocate_new_section(struct f2fs_sb_info *sbi, int type, bool force)
{
	int ret;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);
	down_write(&SIT_I(sbi)->sentry_lock);
	ret = __allocate_new_segment(sbi, type, true, force);
	up_write(&SIT_I(sbi)->sentry_lock);
	f2fs_up_read(&SM_I(sbi)->curseg_lock);

	return ret;
}

int f2fs_allocate_pinning_section(struct f2fs_sb_info *sbi)
{
	int err;
	bool gc_required = true;

retry:
	f2fs_lock_op(sbi);
	err = f2fs_allocate_new_section(sbi, CURSEG_COLD_DATA_PINNED, false);
	f2fs_unlock_op(sbi);

	if (f2fs_sb_has_blkzoned(sbi) && err == -EAGAIN && gc_required) {
		f2fs_down_write(&sbi->gc_lock);
		err = f2fs_gc_range(sbi, 0, sbi->first_seq_zone_segno - 1,
				true, ZONED_PIN_SEC_REQUIRED_COUNT);
		f2fs_up_write(&sbi->gc_lock);

		gc_required = false;
		if (!err)
			goto retry;
	}

	return err;
}

int f2fs_allocate_new_segments(struct f2fs_sb_info *sbi)
{
	int i;
	int err = 0;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);
	down_write(&SIT_I(sbi)->sentry_lock);
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++)
		err += __allocate_new_segment(sbi, i, false, false);
	up_write(&SIT_I(sbi)->sentry_lock);
	f2fs_up_read(&SM_I(sbi)->curseg_lock);

	return err;
}

bool f2fs_exist_trim_candidates(struct f2fs_sb_info *sbi,
						struct cp_control *cpc)
{
	__u64 trim_start = cpc->trim_start;
	bool has_candidate = false;

	down_write(&SIT_I(sbi)->sentry_lock);
	for (; cpc->trim_start <= cpc->trim_end; cpc->trim_start++) {
		if (add_discard_addrs(sbi, cpc, true)) {
			has_candidate = true;
			break;
		}
	}
	up_write(&SIT_I(sbi)->sentry_lock);

	cpc->trim_start = trim_start;
	return has_candidate;
}

static unsigned int __issue_discard_cmd_range(struct f2fs_sb_info *sbi,
					struct discard_policy *dpolicy,
					unsigned int start, unsigned int end)
{
	struct discard_cmd_control *dcc = SM_I(sbi)->dcc_info;
	struct discard_cmd *prev_dc = NULL, *next_dc = NULL;
	struct rb_node **insert_p = NULL, *insert_parent = NULL;
	struct discard_cmd *dc;
	struct blk_plug plug;
	int issued;
	unsigned int trimmed = 0;

next:
	issued = 0;

	mutex_lock(&dcc->cmd_lock);
	if (unlikely(dcc->rbtree_check))
		f2fs_bug_on(sbi, !f2fs_check_discard_tree(sbi));

	dc = __lookup_discard_cmd_ret(&dcc->root, start,
				&prev_dc, &next_dc, &insert_p, &insert_parent);
	if (!dc)
		dc = next_dc;

	blk_start_plug(&plug);

	while (dc && dc->di.lstart <= end) {
		struct rb_node *node;
		int err = 0;

		if (dc->di.len < dpolicy->granularity)
			goto skip;

		if (dc->state != D_PREP) {
			list_move_tail(&dc->list, &dcc->fstrim_list);
			goto skip;
		}

		err = __submit_discard_cmd(sbi, dpolicy, dc, &issued);

		if (issued >= dpolicy->max_requests) {
			start = dc->di.lstart + dc->di.len;

			if (err)
				__remove_discard_cmd(sbi, dc);

			blk_finish_plug(&plug);
			mutex_unlock(&dcc->cmd_lock);
			trimmed += __wait_all_discard_cmd(sbi, NULL);
			f2fs_io_schedule_timeout(DEFAULT_IO_TIMEOUT);
			goto next;
		}
skip:
		node = rb_next(&dc->rb_node);
		if (err)
			__remove_discard_cmd(sbi, dc);
		dc = rb_entry_safe(node, struct discard_cmd, rb_node);

		if (fatal_signal_pending(current))
			break;
	}

	blk_finish_plug(&plug);
	mutex_unlock(&dcc->cmd_lock);

	return trimmed;
}

int f2fs_trim_fs(struct f2fs_sb_info *sbi, struct fstrim_range *range)
{
	__u64 start = F2FS_BYTES_TO_BLK(range->start);
	__u64 end = start + F2FS_BYTES_TO_BLK(range->len) - 1;
	unsigned int start_segno, end_segno;
	block_t start_block, end_block;
	struct cp_control cpc;
	struct discard_policy dpolicy;
	unsigned long long trimmed = 0;
	int err = 0;
	bool need_align = f2fs_lfs_mode(sbi) && __is_large_section(sbi);

	if (start >= MAX_BLKADDR(sbi) || range->len < sbi->blocksize)
		return -EINVAL;

	if (end < MAIN_BLKADDR(sbi))
		goto out;

	if (is_sbi_flag_set(sbi, SBI_NEED_FSCK)) {
		f2fs_warn(sbi, "Found FS corruption, run fsck to fix.");
		return -EFSCORRUPTED;
	}

	/* start/end segment number in main_area */
	start_segno = (start <= MAIN_BLKADDR(sbi)) ? 0 : GET_SEGNO(sbi, start);
	end_segno = (end >= MAX_BLKADDR(sbi)) ? MAIN_SEGS(sbi) - 1 :
						GET_SEGNO(sbi, end);
	if (need_align) {
		start_segno = rounddown(start_segno, SEGS_PER_SEC(sbi));
		end_segno = roundup(end_segno + 1, SEGS_PER_SEC(sbi)) - 1;
	}

	cpc.reason = CP_DISCARD;
	cpc.trim_minlen = max_t(__u64, 1, F2FS_BYTES_TO_BLK(range->minlen));
	cpc.trim_start = start_segno;
	cpc.trim_end = end_segno;

	if (sbi->discard_blks == 0)
		goto out;

	f2fs_down_write(&sbi->gc_lock);
	stat_inc_cp_call_count(sbi, TOTAL_CALL);
	err = f2fs_write_checkpoint(sbi, &cpc);
	f2fs_up_write(&sbi->gc_lock);
	if (err)
		goto out;

	/*
	 * We filed discard candidates, but actually we don't need to wait for
	 * all of them, since they'll be issued in idle time along with runtime
	 * discard option. User configuration looks like using runtime discard
	 * or periodic fstrim instead of it.
	 */
	if (f2fs_realtime_discard_enable(sbi))
		goto out;

	start_block = START_BLOCK(sbi, start_segno);
	end_block = START_BLOCK(sbi, end_segno + 1);

	__init_discard_policy(sbi, &dpolicy, DPOLICY_FSTRIM, cpc.trim_minlen);
	trimmed = __issue_discard_cmd_range(sbi, &dpolicy,
					start_block, end_block);

	trimmed += __wait_discard_cmd_range(sbi, &dpolicy,
					start_block, end_block);
out:
	if (!err)
		range->len = F2FS_BLK_TO_BYTES(trimmed);
	return err;
}

int f2fs_rw_hint_to_seg_type(struct f2fs_sb_info *sbi, enum rw_hint hint)
{
	if (F2FS_OPTION(sbi).active_logs == 2)
		return CURSEG_HOT_DATA;
	else if (F2FS_OPTION(sbi).active_logs == 4)
		return CURSEG_COLD_DATA;

	/* active_log == 6 */
	switch (hint) {
	case WRITE_LIFE_SHORT:
		return CURSEG_HOT_DATA;
	case WRITE_LIFE_EXTREME:
		return CURSEG_COLD_DATA;
	default:
		return CURSEG_WARM_DATA;
	}
}

/*
 * This returns write hints for each segment type. This hints will be
 * passed down to block layer as below by default.
 *
 * User                  F2FS                     Block
 * ----                  ----                     -----
 *                       META                     WRITE_LIFE_NONE|REQ_META
 *                       HOT_NODE                 WRITE_LIFE_NONE
 *                       WARM_NODE                WRITE_LIFE_MEDIUM
 *                       COLD_NODE                WRITE_LIFE_LONG
 * ioctl(COLD)           COLD_DATA                WRITE_LIFE_EXTREME
 * extension list        "                        "
 *
 * -- buffered io
 *                       COLD_DATA                WRITE_LIFE_EXTREME
 *                       HOT_DATA                 WRITE_LIFE_SHORT
 *                       WARM_DATA                WRITE_LIFE_NOT_SET
 *
 * -- direct io
 * WRITE_LIFE_EXTREME    COLD_DATA                WRITE_LIFE_EXTREME
 * WRITE_LIFE_SHORT      HOT_DATA                 WRITE_LIFE_SHORT
 * WRITE_LIFE_NOT_SET    WARM_DATA                WRITE_LIFE_NOT_SET
 * WRITE_LIFE_NONE       "                        WRITE_LIFE_NONE
 * WRITE_LIFE_MEDIUM     "                        WRITE_LIFE_MEDIUM
 * WRITE_LIFE_LONG       "                        WRITE_LIFE_LONG
 */
enum rw_hint f2fs_io_type_to_rw_hint(struct f2fs_sb_info *sbi,
				enum page_type type, enum temp_type temp)
{
	switch (type) {
	case DATA:
		switch (temp) {
		case WARM:
			return WRITE_LIFE_NOT_SET;
		case HOT:
			return WRITE_LIFE_SHORT;
		case COLD:
			return WRITE_LIFE_EXTREME;
		default:
			return WRITE_LIFE_NONE;
		}
	case NODE:
		switch (temp) {
		case WARM:
			return WRITE_LIFE_MEDIUM;
		case HOT:
			return WRITE_LIFE_NONE;
		case COLD:
			return WRITE_LIFE_LONG;
		default:
			return WRITE_LIFE_NONE;
		}
	case META:
		return WRITE_LIFE_NONE;
	default:
		return WRITE_LIFE_NONE;
	}
}

static int __get_segment_type_2(struct f2fs_io_info *fio)
{
	if (fio->type == DATA)
		return CURSEG_HOT_DATA;
	else
		return CURSEG_HOT_NODE;
}

static int __get_segment_type_4(struct f2fs_io_info *fio)
{
	if (fio->type == DATA) {
		struct inode *inode = fio->page->mapping->host;

		if (S_ISDIR(inode->i_mode))
			return CURSEG_HOT_DATA;
		else
			return CURSEG_COLD_DATA;
	} else {
		if (IS_DNODE(fio->page) && is_cold_node(fio->page))
			return CURSEG_WARM_NODE;
		else
			return CURSEG_COLD_NODE;
	}
}

static int __get_age_segment_type(struct inode *inode, pgoff_t pgofs)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct extent_info ei = {};

	if (f2fs_lookup_age_extent_cache(inode, pgofs, &ei)) {
		if (!ei.age)
			return NO_CHECK_TYPE;
		if (ei.age <= sbi->hot_data_age_threshold)
			return CURSEG_HOT_DATA;
		if (ei.age <= sbi->warm_data_age_threshold)
			return CURSEG_WARM_DATA;
		return CURSEG_COLD_DATA;
	}
	return NO_CHECK_TYPE;
}

static int __get_segment_type_6(struct f2fs_io_info *fio)
{
	if (fio->type == DATA) {
		struct inode *inode = fio->page->mapping->host;
		int type;

		if (is_inode_flag_set(inode, FI_ALIGNED_WRITE))
			return CURSEG_COLD_DATA_PINNED;

		if (page_private_gcing(fio->page)) {
			if (fio->sbi->am.atgc_enabled &&
				(fio->io_type == FS_DATA_IO) &&
				(fio->sbi->gc_mode != GC_URGENT_HIGH) &&
				__is_valid_data_blkaddr(fio->old_blkaddr) &&
				!is_inode_flag_set(inode, FI_OPU_WRITE))
				return CURSEG_ALL_DATA_ATGC;
			else
				return CURSEG_COLD_DATA;
		}
		if (file_is_cold(inode) || f2fs_need_compress_data(inode))
			return CURSEG_COLD_DATA;

		type = __get_age_segment_type(inode,
				page_folio(fio->page)->index);
		if (type != NO_CHECK_TYPE)
			return type;

		if (file_is_hot(inode) ||
				is_inode_flag_set(inode, FI_HOT_DATA) ||
				f2fs_is_cow_file(inode))
			return CURSEG_HOT_DATA;
		return f2fs_rw_hint_to_seg_type(F2FS_I_SB(inode),
						inode->i_write_hint);
	} else {
		if (IS_DNODE(fio->page))
			return is_cold_node(fio->page) ? CURSEG_WARM_NODE :
						CURSEG_HOT_NODE;
		return CURSEG_COLD_NODE;
	}
}

int f2fs_get_segment_temp(int seg_type)
{
	if (IS_HOT(seg_type))
		return HOT;
	else if (IS_WARM(seg_type))
		return WARM;
	return COLD;
}

static int __get_segment_type(struct f2fs_io_info *fio)
{
	int type = 0;

	switch (F2FS_OPTION(fio->sbi).active_logs) {
	case 2:
		type = __get_segment_type_2(fio);
		break;
	case 4:
		type = __get_segment_type_4(fio);
		break;
	case 6:
		type = __get_segment_type_6(fio);
		break;
	default:
		f2fs_bug_on(fio->sbi, true);
	}

	fio->temp = f2fs_get_segment_temp(type);

	return type;
}

static void f2fs_randomize_chunk(struct f2fs_sb_info *sbi,
		struct curseg_info *seg)
{
	/* To allocate block chunks in different sizes, use random number */
	if (--seg->fragment_remained_chunk > 0)
		return;

	seg->fragment_remained_chunk =
		get_random_u32_inclusive(1, sbi->max_fragment_chunk);
	seg->next_blkoff +=
		get_random_u32_inclusive(1, sbi->max_fragment_hole);
}

static void reset_curseg_fields(struct curseg_info *curseg)
{
	curseg->inited = false;
	curseg->segno = NULL_SEGNO;
	curseg->next_segno = 0;
}

int f2fs_allocate_data_block(struct f2fs_sb_info *sbi, struct page *page,
		block_t old_blkaddr, block_t *new_blkaddr,
		struct f2fs_summary *sum, int type,
		struct f2fs_io_info *fio)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, type);
	unsigned long long old_mtime;
	bool from_gc = (type == CURSEG_ALL_DATA_ATGC);
	struct seg_entry *se = NULL;
	bool segment_full = false;
	int ret = 0;

	f2fs_down_read(&SM_I(sbi)->curseg_lock);

	mutex_lock(&curseg->curseg_mutex);
	down_write(&sit_i->sentry_lock);

	if (curseg->segno == NULL_SEGNO) {
		ret = -ENOSPC;
		goto out_err;
	}

	if (from_gc) {
		f2fs_bug_on(sbi, GET_SEGNO(sbi, old_blkaddr) == NULL_SEGNO);
		se = get_seg_entry(sbi, GET_SEGNO(sbi, old_blkaddr));
		sanity_check_seg_type(sbi, se->type);
		f2fs_bug_on(sbi, IS_NODESEG(se->type));
	}
	*new_blkaddr = NEXT_FREE_BLKADDR(sbi, curseg);

	f2fs_bug_on(sbi, curseg->next_blkoff >= BLKS_PER_SEG(sbi));

	f2fs_wait_discard_bio(sbi, *new_blkaddr);

	curseg->sum_blk->entries[curseg->next_blkoff] = *sum;
	if (curseg->alloc_type == SSR) {
		curseg->next_blkoff = f2fs_find_next_ssr_block(sbi, curseg);
	} else {
		curseg->next_blkoff++;
		if (F2FS_OPTION(sbi).fs_mode == FS_MODE_FRAGMENT_BLK)
			f2fs_randomize_chunk(sbi, curseg);
	}
	if (curseg->next_blkoff >= f2fs_usable_blks_in_seg(sbi, curseg->segno))
		segment_full = true;
	stat_inc_block_count(sbi, curseg);

	if (from_gc) {
		old_mtime = get_segment_mtime(sbi, old_blkaddr);
	} else {
		update_segment_mtime(sbi, old_blkaddr, 0);
		old_mtime = 0;
	}
	update_segment_mtime(sbi, *new_blkaddr, old_mtime);

	/*
	 * SIT information should be updated before segment allocation,
	 * since SSR needs latest valid block information.
	 */
	update_sit_entry(sbi, *new_blkaddr, 1);
	update_sit_entry(sbi, old_blkaddr, -1);

	/*
	 * If the current segment is full, flush it out and replace it with a
	 * new segment.
	 */
	if (segment_full) {
		if (type == CURSEG_COLD_DATA_PINNED &&
		    !((curseg->segno + 1) % sbi->segs_per_sec)) {
			write_sum_page(sbi, curseg->sum_blk,
					GET_SUM_BLOCK(sbi, curseg->segno));
			reset_curseg_fields(curseg);
			goto skip_new_segment;
		}

		if (from_gc) {
			ret = get_atssr_segment(sbi, type, se->type,
						AT_SSR, se->mtime);
		} else {
			if (need_new_seg(sbi, type))
				ret = new_curseg(sbi, type, false);
			else
				ret = change_curseg(sbi, type);
			stat_inc_seg_type(sbi, curseg);
		}

		if (ret)
			goto out_err;
	}

skip_new_segment:
	/*
	 * segment dirty status should be updated after segment allocation,
	 * so we just need to update status only one time after previous
	 * segment being closed.
	 */
	locate_dirty_segment(sbi, GET_SEGNO(sbi, old_blkaddr));
	locate_dirty_segment(sbi, GET_SEGNO(sbi, *new_blkaddr));

	if (IS_DATASEG(curseg->seg_type))
		atomic64_inc(&sbi->allocated_data_blocks);

	up_write(&sit_i->sentry_lock);

	if (page && IS_NODESEG(curseg->seg_type)) {
		fill_node_footer_blkaddr(page, NEXT_FREE_BLKADDR(sbi, curseg));

		f2fs_inode_chksum_set(sbi, page);
	}

	if (fio) {
		struct f2fs_bio_info *io;

		INIT_LIST_HEAD(&fio->list);
		fio->in_list = 1;
		io = sbi->write_io[fio->type] + fio->temp;
		spin_lock(&io->io_lock);
		list_add_tail(&fio->list, &io->io_list);
		spin_unlock(&io->io_lock);
	}

	mutex_unlock(&curseg->curseg_mutex);
	f2fs_up_read(&SM_I(sbi)->curseg_lock);
	return 0;

out_err:
	*new_blkaddr = NULL_ADDR;
	up_write(&sit_i->sentry_lock);
	mutex_unlock(&curseg->curseg_mutex);
	f2fs_up_read(&SM_I(sbi)->curseg_lock);
	return ret;
}

void f2fs_update_device_state(struct f2fs_sb_info *sbi, nid_t ino,
					block_t blkaddr, unsigned int blkcnt)
{
	if (!f2fs_is_multi_device(sbi))
		return;

	while (1) {
		unsigned int devidx = f2fs_target_device_index(sbi, blkaddr);
		unsigned int blks = FDEV(devidx).end_blk - blkaddr + 1;

		/* update device state for fsync */
		f2fs_set_dirty_device(sbi, ino, devidx, FLUSH_INO);

		/* update device state for checkpoint */
		if (!f2fs_test_bit(devidx, (char *)&sbi->dirty_device)) {
			spin_lock(&sbi->dev_lock);
			f2fs_set_bit(devidx, (char *)&sbi->dirty_device);
			spin_unlock(&sbi->dev_lock);
		}

		if (blkcnt <= blks)
			break;
		blkcnt -= blks;
		blkaddr += blks;
	}
}

static void do_write_page(struct f2fs_summary *sum, struct f2fs_io_info *fio)
{
	int type = __get_segment_type(fio);
	bool keep_order = (f2fs_lfs_mode(fio->sbi) && type == CURSEG_COLD_DATA);

	if (keep_order)
		f2fs_down_read(&fio->sbi->io_order_lock);

	if (f2fs_allocate_data_block(fio->sbi, fio->page, fio->old_blkaddr,
			&fio->new_blkaddr, sum, type, fio)) {
		if (fscrypt_inode_uses_fs_layer_crypto(fio->page->mapping->host))
			fscrypt_finalize_bounce_page(&fio->encrypted_page);
		end_page_writeback(fio->page);
		if (f2fs_in_warm_node_list(fio->sbi, fio->page))
			f2fs_del_fsync_node_entry(fio->sbi, fio->page);
		goto out;
	}
	if (GET_SEGNO(fio->sbi, fio->old_blkaddr) != NULL_SEGNO)
		f2fs_invalidate_internal_cache(fio->sbi, fio->old_blkaddr);

	/* writeout dirty page into bdev */
	f2fs_submit_page_write(fio);

	f2fs_update_device_state(fio->sbi, fio->ino, fio->new_blkaddr, 1);
out:
	if (keep_order)
		f2fs_up_read(&fio->sbi->io_order_lock);
}

void f2fs_do_write_meta_page(struct f2fs_sb_info *sbi, struct folio *folio,
					enum iostat_type io_type)
{
	struct f2fs_io_info fio = {
		.sbi = sbi,
		.type = META,
		.temp = HOT,
		.op = REQ_OP_WRITE,
		.op_flags = REQ_SYNC | REQ_META | REQ_PRIO,
		.old_blkaddr = folio->index,
		.new_blkaddr = folio->index,
		.page = folio_page(folio, 0),
		.encrypted_page = NULL,
		.in_list = 0,
	};

	if (unlikely(folio->index >= MAIN_BLKADDR(sbi)))
		fio.op_flags &= ~REQ_META;

	folio_start_writeback(folio);
	f2fs_submit_page_write(&fio);

	stat_inc_meta_count(sbi, folio->index);
	f2fs_update_iostat(sbi, NULL, io_type, F2FS_BLKSIZE);
}

void f2fs_do_write_node_page(unsigned int nid, struct f2fs_io_info *fio)
{
	struct f2fs_summary sum;

	set_summary(&sum, nid, 0, 0);
	do_write_page(&sum, fio);

	f2fs_update_iostat(fio->sbi, NULL, fio->io_type, F2FS_BLKSIZE);
}

void f2fs_outplace_write_data(struct dnode_of_data *dn,
					struct f2fs_io_info *fio)
{
	struct f2fs_sb_info *sbi = fio->sbi;
	struct f2fs_summary sum;

	f2fs_bug_on(sbi, dn->data_blkaddr == NULL_ADDR);
	if (fio->io_type == FS_DATA_IO || fio->io_type == FS_CP_DATA_IO)
		f2fs_update_age_extent_cache(dn);
	set_summary(&sum, dn->nid, dn->ofs_in_node, fio->version);
	do_write_page(&sum, fio);
	f2fs_update_data_blkaddr(dn, fio->new_blkaddr);

	f2fs_update_iostat(sbi, dn->inode, fio->io_type, F2FS_BLKSIZE);
}

int f2fs_inplace_write_data(struct f2fs_io_info *fio)
{
	int err;
	struct f2fs_sb_info *sbi = fio->sbi;
	unsigned int segno;

	fio->new_blkaddr = fio->old_blkaddr;
	/* i/o temperature is needed for passing down write hints */
	__get_segment_type(fio);

	segno = GET_SEGNO(sbi, fio->new_blkaddr);

	if (!IS_DATASEG(get_seg_entry(sbi, segno)->type)) {
		set_sbi_flag(sbi, SBI_NEED_FSCK);
		f2fs_warn(sbi, "%s: incorrect segment(%u) type, run fsck to fix.",
			  __func__, segno);
		err = -EFSCORRUPTED;
		f2fs_handle_error(sbi, ERROR_INCONSISTENT_SUM_TYPE);
		goto drop_bio;
	}

	if (f2fs_cp_error(sbi)) {
		err = -EIO;
		goto drop_bio;
	}

	if (fio->meta_gc)
		f2fs_truncate_meta_inode_pages(sbi, fio->new_blkaddr, 1);

	stat_inc_inplace_blocks(fio->sbi);

	if (fio->bio && !IS_F2FS_IPU_NOCACHE(sbi))
		err = f2fs_merge_page_bio(fio);
	else
		err = f2fs_submit_page_bio(fio);
	if (!err) {
		f2fs_update_device_state(fio->sbi, fio->ino,
						fio->new_blkaddr, 1);
		f2fs_update_iostat(fio->sbi, fio->page->mapping->host,
						fio->io_type, F2FS_BLKSIZE);
	}

	return err;
drop_bio:
	if (fio->bio && *(fio->bio)) {
		struct bio *bio = *(fio->bio);

		bio->bi_status = BLK_STS_IOERR;
		bio_endio(bio);
		*(fio->bio) = NULL;
	}
	return err;
}

static inline int __f2fs_get_curseg(struct f2fs_sb_info *sbi,
						unsigned int segno)
{
	int i;

	for (i = CURSEG_HOT_DATA; i < NO_CHECK_TYPE; i++) {
		if (CURSEG_I(sbi, i)->segno == segno)
			break;
	}
	return i;
}

void f2fs_do_replace_block(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
				block_t old_blkaddr, block_t new_blkaddr,
				bool recover_curseg, bool recover_newaddr,
				bool from_gc)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct curseg_info *curseg;
	unsigned int segno, old_cursegno;
	struct seg_entry *se;
	int type;
	unsigned short old_blkoff;
	unsigned char old_alloc_type;

	segno = GET_SEGNO(sbi, new_blkaddr);
	se = get_seg_entry(sbi, segno);
	type = se->type;

	f2fs_down_write(&SM_I(sbi)->curseg_lock);

	if (!recover_curseg) {
		/* for recovery flow */
		if (se->valid_blocks == 0 && !IS_CURSEG(sbi, segno)) {
			if (old_blkaddr == NULL_ADDR)
				type = CURSEG_COLD_DATA;
			else
				type = CURSEG_WARM_DATA;
		}
	} else {
		if (IS_CURSEG(sbi, segno)) {
			/* se->type is volatile as SSR allocation */
			type = __f2fs_get_curseg(sbi, segno);
			f2fs_bug_on(sbi, type == NO_CHECK_TYPE);
		} else {
			type = CURSEG_WARM_DATA;
		}
	}

	curseg = CURSEG_I(sbi, type);
	f2fs_bug_on(sbi, !IS_DATASEG(curseg->seg_type));

	mutex_lock(&curseg->curseg_mutex);
	down_write(&sit_i->sentry_lock);

	old_cursegno = curseg->segno;
	old_blkoff = curseg->next_blkoff;
	old_alloc_type = curseg->alloc_type;

	/* change the current segment */
	if (segno != curseg->segno) {
		curseg->next_segno = segno;
		if (change_curseg(sbi, type))
			goto out_unlock;
	}

	curseg->next_blkoff = GET_BLKOFF_FROM_SEG0(sbi, new_blkaddr);
	curseg->sum_blk->entries[curseg->next_blkoff] = *sum;

	if (!recover_curseg || recover_newaddr) {
		if (!from_gc)
			update_segment_mtime(sbi, new_blkaddr, 0);
		update_sit_entry(sbi, new_blkaddr, 1);
	}
	if (GET_SEGNO(sbi, old_blkaddr) != NULL_SEGNO) {
		f2fs_invalidate_internal_cache(sbi, old_blkaddr);
		if (!from_gc)
			update_segment_mtime(sbi, old_blkaddr, 0);
		update_sit_entry(sbi, old_blkaddr, -1);
	}

	locate_dirty_segment(sbi, GET_SEGNO(sbi, old_blkaddr));
	locate_dirty_segment(sbi, GET_SEGNO(sbi, new_blkaddr));

	locate_dirty_segment(sbi, old_cursegno);

	if (recover_curseg) {
		if (old_cursegno != curseg->segno) {
			curseg->next_segno = old_cursegno;
			if (change_curseg(sbi, type))
				goto out_unlock;
		}
		curseg->next_blkoff = old_blkoff;
		curseg->alloc_type = old_alloc_type;
	}

out_unlock:
	up_write(&sit_i->sentry_lock);
	mutex_unlock(&curseg->curseg_mutex);
	f2fs_up_write(&SM_I(sbi)->curseg_lock);
}

void f2fs_replace_block(struct f2fs_sb_info *sbi, struct dnode_of_data *dn,
				block_t old_addr, block_t new_addr,
				unsigned char version, bool recover_curseg,
				bool recover_newaddr)
{
	struct f2fs_summary sum;

	set_summary(&sum, dn->nid, dn->ofs_in_node, version);

	f2fs_do_replace_block(sbi, &sum, old_addr, new_addr,
					recover_curseg, recover_newaddr, false);

	f2fs_update_data_blkaddr(dn, new_addr);
}

void f2fs_wait_on_page_writeback(struct page *page,
				enum page_type type, bool ordered, bool locked)
{
	if (folio_test_writeback(page_folio(page))) {
		struct f2fs_sb_info *sbi = F2FS_P_SB(page);

		/* submit cached LFS IO */
		f2fs_submit_merged_write_cond(sbi, NULL, page, 0, type);
		/* submit cached IPU IO */
		f2fs_submit_merged_ipu_write(sbi, NULL, page);
		if (ordered) {
			wait_on_page_writeback(page);
			f2fs_bug_on(sbi, locked &&
				folio_test_writeback(page_folio(page)));
		} else {
			wait_for_stable_page(page);
		}
	}
}

void f2fs_wait_on_block_writeback(struct inode *inode, block_t blkaddr)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	struct page *cpage;

	if (!f2fs_meta_inode_gc_required(inode))
		return;

	if (!__is_valid_data_blkaddr(blkaddr))
		return;

	cpage = find_lock_page(META_MAPPING(sbi), blkaddr);
	if (cpage) {
		f2fs_wait_on_page_writeback(cpage, DATA, true, true);
		f2fs_put_page(cpage, 1);
	}
}

void f2fs_wait_on_block_writeback_range(struct inode *inode, block_t blkaddr,
								block_t len)
{
	struct f2fs_sb_info *sbi = F2FS_I_SB(inode);
	block_t i;

	if (!f2fs_meta_inode_gc_required(inode))
		return;

	for (i = 0; i < len; i++)
		f2fs_wait_on_block_writeback(inode, blkaddr + i);

	f2fs_truncate_meta_inode_pages(sbi, blkaddr, len);
}

static int read_compacted_summaries(struct f2fs_sb_info *sbi)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct curseg_info *seg_i;
	unsigned char *kaddr;
	struct page *page;
	block_t start;
	int i, j, offset;

	start = start_sum_block(sbi);

	page = f2fs_get_meta_page(sbi, start++);
	if (IS_ERR(page))
		return PTR_ERR(page);
	kaddr = (unsigned char *)page_address(page);

	/* Step 1: restore nat cache */
	seg_i = CURSEG_I(sbi, CURSEG_HOT_DATA);
	memcpy(seg_i->journal, kaddr, SUM_JOURNAL_SIZE);

	/* Step 2: restore sit cache */
	seg_i = CURSEG_I(sbi, CURSEG_COLD_DATA);
	memcpy(seg_i->journal, kaddr + SUM_JOURNAL_SIZE, SUM_JOURNAL_SIZE);
	offset = 2 * SUM_JOURNAL_SIZE;

	/* Step 3: restore summary entries */
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		unsigned short blk_off;
		unsigned int segno;

		seg_i = CURSEG_I(sbi, i);
		segno = le32_to_cpu(ckpt->cur_data_segno[i]);
		blk_off = le16_to_cpu(ckpt->cur_data_blkoff[i]);
		seg_i->next_segno = segno;
		reset_curseg(sbi, i, 0);
		seg_i->alloc_type = ckpt->alloc_type[i];
		seg_i->next_blkoff = blk_off;

		if (seg_i->alloc_type == SSR)
			blk_off = BLKS_PER_SEG(sbi);

		for (j = 0; j < blk_off; j++) {
			struct f2fs_summary *s;

			s = (struct f2fs_summary *)(kaddr + offset);
			seg_i->sum_blk->entries[j] = *s;
			offset += SUMMARY_SIZE;
			if (offset + SUMMARY_SIZE <= PAGE_SIZE -
						SUM_FOOTER_SIZE)
				continue;

			f2fs_put_page(page, 1);
			page = NULL;

			page = f2fs_get_meta_page(sbi, start++);
			if (IS_ERR(page))
				return PTR_ERR(page);
			kaddr = (unsigned char *)page_address(page);
			offset = 0;
		}
	}
	f2fs_put_page(page, 1);
	return 0;
}

static int read_normal_summaries(struct f2fs_sb_info *sbi, int type)
{
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_summary_block *sum;
	struct curseg_info *curseg;
	struct page *new;
	unsigned short blk_off;
	unsigned int segno = 0;
	block_t blk_addr = 0;
	int err = 0;

	/* get segment number and block addr */
	if (IS_DATASEG(type)) {
		segno = le32_to_cpu(ckpt->cur_data_segno[type]);
		blk_off = le16_to_cpu(ckpt->cur_data_blkoff[type -
							CURSEG_HOT_DATA]);
		if (__exist_node_summaries(sbi))
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_PERSIST_TYPE, type);
		else
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_DATA_TYPE, type);
	} else {
		segno = le32_to_cpu(ckpt->cur_node_segno[type -
							CURSEG_HOT_NODE]);
		blk_off = le16_to_cpu(ckpt->cur_node_blkoff[type -
							CURSEG_HOT_NODE]);
		if (__exist_node_summaries(sbi))
			blk_addr = sum_blk_addr(sbi, NR_CURSEG_NODE_TYPE,
							type - CURSEG_HOT_NODE);
		else
			blk_addr = GET_SUM_BLOCK(sbi, segno);
	}

	new = f2fs_get_meta_page(sbi, blk_addr);
	if (IS_ERR(new))
		return PTR_ERR(new);
	sum = (struct f2fs_summary_block *)page_address(new);

	if (IS_NODESEG(type)) {
		if (__exist_node_summaries(sbi)) {
			struct f2fs_summary *ns = &sum->entries[0];
			int i;

			for (i = 0; i < BLKS_PER_SEG(sbi); i++, ns++) {
				ns->version = 0;
				ns->ofs_in_node = 0;
			}
		} else {
			err = f2fs_restore_node_summary(sbi, segno, sum);
			if (err)
				goto out;
		}
	}

	/* set uncompleted segment to curseg */
	curseg = CURSEG_I(sbi, type);
	mutex_lock(&curseg->curseg_mutex);

	/* update journal info */
	down_write(&curseg->journal_rwsem);
	memcpy(curseg->journal, &sum->journal, SUM_JOURNAL_SIZE);
	up_write(&curseg->journal_rwsem);

	memcpy(curseg->sum_blk->entries, sum->entries, SUM_ENTRY_SIZE);
	memcpy(&curseg->sum_blk->footer, &sum->footer, SUM_FOOTER_SIZE);
	curseg->next_segno = segno;
	reset_curseg(sbi, type, 0);
	curseg->alloc_type = ckpt->alloc_type[type];
	curseg->next_blkoff = blk_off;
	mutex_unlock(&curseg->curseg_mutex);
out:
	f2fs_put_page(new, 1);
	return err;
}

static int restore_curseg_summaries(struct f2fs_sb_info *sbi)
{
	struct f2fs_journal *sit_j = CURSEG_I(sbi, CURSEG_COLD_DATA)->journal;
	struct f2fs_journal *nat_j = CURSEG_I(sbi, CURSEG_HOT_DATA)->journal;
	int type = CURSEG_HOT_DATA;
	int err;

	if (is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG)) {
		int npages = f2fs_npages_for_summary_flush(sbi, true);

		if (npages >= 2)
			f2fs_ra_meta_pages(sbi, start_sum_block(sbi), npages,
							META_CP, true);

		/* restore for compacted data summary */
		err = read_compacted_summaries(sbi);
		if (err)
			return err;
		type = CURSEG_HOT_NODE;
	}

	if (__exist_node_summaries(sbi))
		f2fs_ra_meta_pages(sbi,
				sum_blk_addr(sbi, NR_CURSEG_PERSIST_TYPE, type),
				NR_CURSEG_PERSIST_TYPE - type, META_CP, true);

	for (; type <= CURSEG_COLD_NODE; type++) {
		err = read_normal_summaries(sbi, type);
		if (err)
			return err;
	}

	/* sanity check for summary blocks */
	if (nats_in_cursum(nat_j) > NAT_JOURNAL_ENTRIES ||
			sits_in_cursum(sit_j) > SIT_JOURNAL_ENTRIES) {
		f2fs_err(sbi, "invalid journal entries nats %u sits %u",
			 nats_in_cursum(nat_j), sits_in_cursum(sit_j));
		return -EINVAL;
	}

	return 0;
}

static void write_compacted_summaries(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct page *page;
	unsigned char *kaddr;
	struct f2fs_summary *summary;
	struct curseg_info *seg_i;
	int written_size = 0;
	int i, j;

	page = f2fs_grab_meta_page(sbi, blkaddr++);
	kaddr = (unsigned char *)page_address(page);
	memset(kaddr, 0, PAGE_SIZE);

	/* Step 1: write nat cache */
	seg_i = CURSEG_I(sbi, CURSEG_HOT_DATA);
	memcpy(kaddr, seg_i->journal, SUM_JOURNAL_SIZE);
	written_size += SUM_JOURNAL_SIZE;

	/* Step 2: write sit cache */
	seg_i = CURSEG_I(sbi, CURSEG_COLD_DATA);
	memcpy(kaddr + written_size, seg_i->journal, SUM_JOURNAL_SIZE);
	written_size += SUM_JOURNAL_SIZE;

	/* Step 3: write summary entries */
	for (i = CURSEG_HOT_DATA; i <= CURSEG_COLD_DATA; i++) {
		seg_i = CURSEG_I(sbi, i);
		for (j = 0; j < f2fs_curseg_valid_blocks(sbi, i); j++) {
			if (!page) {
				page = f2fs_grab_meta_page(sbi, blkaddr++);
				kaddr = (unsigned char *)page_address(page);
				memset(kaddr, 0, PAGE_SIZE);
				written_size = 0;
			}
			summary = (struct f2fs_summary *)(kaddr + written_size);
			*summary = seg_i->sum_blk->entries[j];
			written_size += SUMMARY_SIZE;

			if (written_size + SUMMARY_SIZE <= PAGE_SIZE -
							SUM_FOOTER_SIZE)
				continue;

			set_page_dirty(page);
			f2fs_put_page(page, 1);
			page = NULL;
		}
	}
	if (page) {
		set_page_dirty(page);
		f2fs_put_page(page, 1);
	}
}

static void write_normal_summaries(struct f2fs_sb_info *sbi,
					block_t blkaddr, int type)
{
	int i, end;

	if (IS_DATASEG(type))
		end = type + NR_CURSEG_DATA_TYPE;
	else
		end = type + NR_CURSEG_NODE_TYPE;

	for (i = type; i < end; i++)
		write_current_sum_page(sbi, i, blkaddr + (i - type));
}

void f2fs_write_data_summaries(struct f2fs_sb_info *sbi, block_t start_blk)
{
	if (is_set_ckpt_flags(sbi, CP_COMPACT_SUM_FLAG))
		write_compacted_summaries(sbi, start_blk);
	else
		write_normal_summaries(sbi, start_blk, CURSEG_HOT_DATA);
}

void f2fs_write_node_summaries(struct f2fs_sb_info *sbi, block_t start_blk)
{
	write_normal_summaries(sbi, start_blk, CURSEG_HOT_NODE);
}

int f2fs_lookup_journal_in_cursum(struct f2fs_journal *journal, int type,
					unsigned int val, int alloc)
{
	int i;

	if (type == NAT_JOURNAL) {
		for (i = 0; i < nats_in_cursum(journal); i++) {
			if (le32_to_cpu(nid_in_journal(journal, i)) == val)
				return i;
		}
		if (alloc && __has_cursum_space(journal, 1, NAT_JOURNAL))
			return update_nats_in_cursum(journal, 1);
	} else if (type == SIT_JOURNAL) {
		for (i = 0; i < sits_in_cursum(journal); i++)
			if (le32_to_cpu(segno_in_journal(journal, i)) == val)
				return i;
		if (alloc && __has_cursum_space(journal, 1, SIT_JOURNAL))
			return update_sits_in_cursum(journal, 1);
	}
	return -1;
}

static struct page *get_current_sit_page(struct f2fs_sb_info *sbi,
					unsigned int segno)
{
	return f2fs_get_meta_page(sbi, current_sit_addr(sbi, segno));
}

static struct page *get_next_sit_page(struct f2fs_sb_info *sbi,
					unsigned int start)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct page *page;
	pgoff_t src_off, dst_off;

	src_off = current_sit_addr(sbi, start);
	dst_off = next_sit_addr(sbi, src_off);

	page = f2fs_grab_meta_page(sbi, dst_off);
	seg_info_to_sit_page(sbi, page, start);

	set_page_dirty(page);
	set_to_next_sit(sit_i, start);

	return page;
}

static struct sit_entry_set *grab_sit_entry_set(void)
{
	struct sit_entry_set *ses =
			f2fs_kmem_cache_alloc(sit_entry_set_slab,
						GFP_NOFS, true, NULL);

	ses->entry_cnt = 0;
	INIT_LIST_HEAD(&ses->set_list);
	return ses;
}

static void release_sit_entry_set(struct sit_entry_set *ses)
{
	list_del(&ses->set_list);
	kmem_cache_free(sit_entry_set_slab, ses);
}

static void adjust_sit_entry_set(struct sit_entry_set *ses,
						struct list_head *head)
{
	struct sit_entry_set *next = ses;

	if (list_is_last(&ses->set_list, head))
		return;

	list_for_each_entry_continue(next, head, set_list)
		if (ses->entry_cnt <= next->entry_cnt) {
			list_move_tail(&ses->set_list, &next->set_list);
			return;
		}

	list_move_tail(&ses->set_list, head);
}

static void add_sit_entry(unsigned int segno, struct list_head *head)
{
	struct sit_entry_set *ses;
	unsigned int start_segno = START_SEGNO(segno);

	list_for_each_entry(ses, head, set_list) {
		if (ses->start_segno == start_segno) {
			ses->entry_cnt++;
			adjust_sit_entry_set(ses, head);
			return;
		}
	}

	ses = grab_sit_entry_set();

	ses->start_segno = start_segno;
	ses->entry_cnt++;
	list_add(&ses->set_list, head);
}

static void add_sits_in_set(struct f2fs_sb_info *sbi)
{
	struct f2fs_sm_info *sm_info = SM_I(sbi);
	struct list_head *set_list = &sm_info->sit_entry_set;
	unsigned long *bitmap = SIT_I(sbi)->dirty_sentries_bitmap;
	unsigned int segno;

	for_each_set_bit(segno, bitmap, MAIN_SEGS(sbi))
		add_sit_entry(segno, set_list);
}

static void remove_sits_in_journal(struct f2fs_sb_info *sbi)
{
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_COLD_DATA);
	struct f2fs_journal *journal = curseg->journal;
	int i;

	down_write(&curseg->journal_rwsem);
	for (i = 0; i < sits_in_cursum(journal); i++) {
		unsigned int segno;
		bool dirtied;

		segno = le32_to_cpu(segno_in_journal(journal, i));
		dirtied = __mark_sit_entry_dirty(sbi, segno);

		if (!dirtied)
			add_sit_entry(segno, &SM_I(sbi)->sit_entry_set);
	}
	update_sits_in_cursum(journal, -i);
	up_write(&curseg->journal_rwsem);
}

/*
 * CP calls this function, which flushes SIT entries including sit_journal,
 * and moves prefree segs to free segs.
 */
void f2fs_flush_sit_entries(struct f2fs_sb_info *sbi, struct cp_control *cpc)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned long *bitmap = sit_i->dirty_sentries_bitmap;
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_COLD_DATA);
	struct f2fs_journal *journal = curseg->journal;
	struct sit_entry_set *ses, *tmp;
	struct list_head *head = &SM_I(sbi)->sit_entry_set;
	bool to_journal = !is_sbi_flag_set(sbi, SBI_IS_RESIZEFS);
	struct seg_entry *se;

	down_write(&sit_i->sentry_lock);

	if (!sit_i->dirty_sentries)
		goto out;

	/*
	 * add and account sit entries of dirty bitmap in sit entry
	 * set temporarily
	 */
	add_sits_in_set(sbi);

	/*
	 * if there are no enough space in journal to store dirty sit
	 * entries, remove all entries from journal and add and account
	 * them in sit entry set.
	 */
	if (!__has_cursum_space(journal, sit_i->dirty_sentries, SIT_JOURNAL) ||
								!to_journal)
		remove_sits_in_journal(sbi);

	/*
	 * there are two steps to flush sit entries:
	 * #1, flush sit entries to journal in current cold data summary block.
	 * #2, flush sit entries to sit page.
	 */
	list_for_each_entry_safe(ses, tmp, head, set_list) {
		struct page *page = NULL;
		struct f2fs_sit_block *raw_sit = NULL;
		unsigned int start_segno = ses->start_segno;
		unsigned int end = min(start_segno + SIT_ENTRY_PER_BLOCK,
						(unsigned long)MAIN_SEGS(sbi));
		unsigned int segno = start_segno;

		if (to_journal &&
			!__has_cursum_space(journal, ses->entry_cnt, SIT_JOURNAL))
			to_journal = false;

		if (to_journal) {
			down_write(&curseg->journal_rwsem);
		} else {
			page = get_next_sit_page(sbi, start_segno);
			raw_sit = page_address(page);
		}

		/* flush dirty sit entries in region of current sit set */
		for_each_set_bit_from(segno, bitmap, end) {
			int offset, sit_offset;

			se = get_seg_entry(sbi, segno);
#ifdef CONFIG_F2FS_CHECK_FS
			if (memcmp(se->cur_valid_map, se->cur_valid_map_mir,
						SIT_VBLOCK_MAP_SIZE))
				f2fs_bug_on(sbi, 1);
#endif

			/* add discard candidates */
			if (!(cpc->reason & CP_DISCARD)) {
				cpc->trim_start = segno;
				add_discard_addrs(sbi, cpc, false);
			}

			if (to_journal) {
				offset = f2fs_lookup_journal_in_cursum(journal,
							SIT_JOURNAL, segno, 1);
				f2fs_bug_on(sbi, offset < 0);
				segno_in_journal(journal, offset) =
							cpu_to_le32(segno);
				seg_info_to_raw_sit(se,
					&sit_in_journal(journal, offset));
				check_block_count(sbi, segno,
					&sit_in_journal(journal, offset));
			} else {
				sit_offset = SIT_ENTRY_OFFSET(sit_i, segno);
				seg_info_to_raw_sit(se,
						&raw_sit->entries[sit_offset]);
				check_block_count(sbi, segno,
						&raw_sit->entries[sit_offset]);
			}

			__clear_bit(segno, bitmap);
			sit_i->dirty_sentries--;
			ses->entry_cnt--;
		}

		if (to_journal)
			up_write(&curseg->journal_rwsem);
		else
			f2fs_put_page(page, 1);

		f2fs_bug_on(sbi, ses->entry_cnt);
		release_sit_entry_set(ses);
	}

	f2fs_bug_on(sbi, !list_empty(head));
	f2fs_bug_on(sbi, sit_i->dirty_sentries);
out:
	if (cpc->reason & CP_DISCARD) {
		__u64 trim_start = cpc->trim_start;

		for (; cpc->trim_start <= cpc->trim_end; cpc->trim_start++)
			add_discard_addrs(sbi, cpc, false);

		cpc->trim_start = trim_start;
	}
	up_write(&sit_i->sentry_lock);

	set_prefree_as_free_segments(sbi);
}

static int build_sit_info(struct f2fs_sb_info *sbi)
{
	struct f2fs_super_block *raw_super = F2FS_RAW_SUPER(sbi);
	struct sit_info *sit_i;
	unsigned int sit_segs, start;
	char *src_bitmap, *bitmap;
	unsigned int bitmap_size, main_bitmap_size, sit_bitmap_size;
	unsigned int discard_map = f2fs_block_unit_discard(sbi) ? 1 : 0;

	/* allocate memory for SIT information */
	sit_i = f2fs_kzalloc(sbi, sizeof(struct sit_info), GFP_KERNEL);
	if (!sit_i)
		return -ENOMEM;

	SM_I(sbi)->sit_info = sit_i;

	sit_i->sentries =
		f2fs_kvzalloc(sbi, array_size(sizeof(struct seg_entry),
					      MAIN_SEGS(sbi)),
			      GFP_KERNEL);
	if (!sit_i->sentries)
		return -ENOMEM;

	main_bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));
	sit_i->dirty_sentries_bitmap = f2fs_kvzalloc(sbi, main_bitmap_size,
								GFP_KERNEL);
	if (!sit_i->dirty_sentries_bitmap)
		return -ENOMEM;

#ifdef CONFIG_F2FS_CHECK_FS
	bitmap_size = MAIN_SEGS(sbi) * SIT_VBLOCK_MAP_SIZE * (3 + discard_map);
#else
	bitmap_size = MAIN_SEGS(sbi) * SIT_VBLOCK_MAP_SIZE * (2 + discard_map);
#endif
	sit_i->bitmap = f2fs_kvzalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!sit_i->bitmap)
		return -ENOMEM;

	bitmap = sit_i->bitmap;

	for (start = 0; start < MAIN_SEGS(sbi); start++) {
		sit_i->sentries[start].cur_valid_map = bitmap;
		bitmap += SIT_VBLOCK_MAP_SIZE;

		sit_i->sentries[start].ckpt_valid_map = bitmap;
		bitmap += SIT_VBLOCK_MAP_SIZE;

#ifdef CONFIG_F2FS_CHECK_FS
		sit_i->sentries[start].cur_valid_map_mir = bitmap;
		bitmap += SIT_VBLOCK_MAP_SIZE;
#endif

		if (discard_map) {
			sit_i->sentries[start].discard_map = bitmap;
			bitmap += SIT_VBLOCK_MAP_SIZE;
		}
	}

	sit_i->tmp_map = f2fs_kzalloc(sbi, SIT_VBLOCK_MAP_SIZE, GFP_KERNEL);
	if (!sit_i->tmp_map)
		return -ENOMEM;

	if (__is_large_section(sbi)) {
		sit_i->sec_entries =
			f2fs_kvzalloc(sbi, array_size(sizeof(struct sec_entry),
						      MAIN_SECS(sbi)),
				      GFP_KERNEL);
		if (!sit_i->sec_entries)
			return -ENOMEM;
	}

	/* get information related with SIT */
	sit_segs = le32_to_cpu(raw_super->segment_count_sit) >> 1;

	/* setup SIT bitmap from ckeckpoint pack */
	sit_bitmap_size = __bitmap_size(sbi, SIT_BITMAP);
	src_bitmap = __bitmap_ptr(sbi, SIT_BITMAP);

	sit_i->sit_bitmap = kmemdup(src_bitmap, sit_bitmap_size, GFP_KERNEL);
	if (!sit_i->sit_bitmap)
		return -ENOMEM;

#ifdef CONFIG_F2FS_CHECK_FS
	sit_i->sit_bitmap_mir = kmemdup(src_bitmap,
					sit_bitmap_size, GFP_KERNEL);
	if (!sit_i->sit_bitmap_mir)
		return -ENOMEM;

	sit_i->invalid_segmap = f2fs_kvzalloc(sbi,
					main_bitmap_size, GFP_KERNEL);
	if (!sit_i->invalid_segmap)
		return -ENOMEM;
#endif

	sit_i->sit_base_addr = le32_to_cpu(raw_super->sit_blkaddr);
	sit_i->sit_blocks = SEGS_TO_BLKS(sbi, sit_segs);
	sit_i->written_valid_blocks = 0;
	sit_i->bitmap_size = sit_bitmap_size;
	sit_i->dirty_sentries = 0;
	sit_i->sents_per_block = SIT_ENTRY_PER_BLOCK;
	sit_i->elapsed_time = le64_to_cpu(sbi->ckpt->elapsed_time);
	sit_i->mounted_time = ktime_get_boottime_seconds();
	init_rwsem(&sit_i->sentry_lock);
	return 0;
}

static int build_free_segmap(struct f2fs_sb_info *sbi)
{
	struct free_segmap_info *free_i;
	unsigned int bitmap_size, sec_bitmap_size;

	/* allocate memory for free segmap information */
	free_i = f2fs_kzalloc(sbi, sizeof(struct free_segmap_info), GFP_KERNEL);
	if (!free_i)
		return -ENOMEM;

	SM_I(sbi)->free_info = free_i;

	bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));
	free_i->free_segmap = f2fs_kvmalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!free_i->free_segmap)
		return -ENOMEM;

	sec_bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));
	free_i->free_secmap = f2fs_kvmalloc(sbi, sec_bitmap_size, GFP_KERNEL);
	if (!free_i->free_secmap)
		return -ENOMEM;

	/* set all segments as dirty temporarily */
	memset(free_i->free_segmap, 0xff, bitmap_size);
	memset(free_i->free_secmap, 0xff, sec_bitmap_size);

	/* init free segmap information */
	free_i->start_segno = GET_SEGNO_FROM_SEG0(sbi, MAIN_BLKADDR(sbi));
	free_i->free_segments = 0;
	free_i->free_sections = 0;
	spin_lock_init(&free_i->segmap_lock);
	return 0;
}

static int build_curseg(struct f2fs_sb_info *sbi)
{
	struct curseg_info *array;
	int i;

	array = f2fs_kzalloc(sbi, array_size(NR_CURSEG_TYPE,
					sizeof(*array)), GFP_KERNEL);
	if (!array)
		return -ENOMEM;

	SM_I(sbi)->curseg_array = array;

	for (i = 0; i < NO_CHECK_TYPE; i++) {
		mutex_init(&array[i].curseg_mutex);
		array[i].sum_blk = f2fs_kzalloc(sbi, PAGE_SIZE, GFP_KERNEL);
		if (!array[i].sum_blk)
			return -ENOMEM;
		init_rwsem(&array[i].journal_rwsem);
		array[i].journal = f2fs_kzalloc(sbi,
				sizeof(struct f2fs_journal), GFP_KERNEL);
		if (!array[i].journal)
			return -ENOMEM;
		if (i < NR_PERSISTENT_LOG)
			array[i].seg_type = CURSEG_HOT_DATA + i;
		else if (i == CURSEG_COLD_DATA_PINNED)
			array[i].seg_type = CURSEG_COLD_DATA;
		else if (i == CURSEG_ALL_DATA_ATGC)
			array[i].seg_type = CURSEG_COLD_DATA;
		reset_curseg_fields(&array[i]);
	}
	return restore_curseg_summaries(sbi);
}

static int build_sit_entries(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct curseg_info *curseg = CURSEG_I(sbi, CURSEG_COLD_DATA);
	struct f2fs_journal *journal = curseg->journal;
	struct seg_entry *se;
	struct f2fs_sit_entry sit;
	int sit_blk_cnt = SIT_BLK_CNT(sbi);
	unsigned int i, start, end;
	unsigned int readed, start_blk = 0;
	int err = 0;
	block_t sit_valid_blocks[2] = {0, 0};

	do {
		readed = f2fs_ra_meta_pages(sbi, start_blk, BIO_MAX_VECS,
							META_SIT, true);

		start = start_blk * sit_i->sents_per_block;
		end = (start_blk + readed) * sit_i->sents_per_block;

		for (; start < end && start < MAIN_SEGS(sbi); start++) {
			struct f2fs_sit_block *sit_blk;
			struct page *page;

			se = &sit_i->sentries[start];
			page = get_current_sit_page(sbi, start);
			if (IS_ERR(page))
				return PTR_ERR(page);
			sit_blk = (struct f2fs_sit_block *)page_address(page);
			sit = sit_blk->entries[SIT_ENTRY_OFFSET(sit_i, start)];
			f2fs_put_page(page, 1);

			err = check_block_count(sbi, start, &sit);
			if (err)
				return err;
			seg_info_from_raw_sit(se, &sit);

			if (se->type >= NR_PERSISTENT_LOG) {
				f2fs_err(sbi, "Invalid segment type: %u, segno: %u",
							se->type, start);
				f2fs_handle_error(sbi,
						ERROR_INCONSISTENT_SUM_TYPE);
				return -EFSCORRUPTED;
			}

			sit_valid_blocks[SE_PAGETYPE(se)] += se->valid_blocks;

			if (!f2fs_block_unit_discard(sbi))
				goto init_discard_map_done;

			/* build discard map only one time */
			if (is_set_ckpt_flags(sbi, CP_TRIMMED_FLAG)) {
				memset(se->discard_map, 0xff,
						SIT_VBLOCK_MAP_SIZE);
				goto init_discard_map_done;
			}
			memcpy(se->discard_map, se->cur_valid_map,
						SIT_VBLOCK_MAP_SIZE);
			sbi->discard_blks += BLKS_PER_SEG(sbi) -
						se->valid_blocks;
init_discard_map_done:
			if (__is_large_section(sbi))
				get_sec_entry(sbi, start)->valid_blocks +=
							se->valid_blocks;
		}
		start_blk += readed;
	} while (start_blk < sit_blk_cnt);

	down_read(&curseg->journal_rwsem);
	for (i = 0; i < sits_in_cursum(journal); i++) {
		unsigned int old_valid_blocks;

		start = le32_to_cpu(segno_in_journal(journal, i));
		if (start >= MAIN_SEGS(sbi)) {
			f2fs_err(sbi, "Wrong journal entry on segno %u",
				 start);
			err = -EFSCORRUPTED;
			f2fs_handle_error(sbi, ERROR_CORRUPTED_JOURNAL);
			break;
		}

		se = &sit_i->sentries[start];
		sit = sit_in_journal(journal, i);

		old_valid_blocks = se->valid_blocks;

		sit_valid_blocks[SE_PAGETYPE(se)] -= old_valid_blocks;

		err = check_block_count(sbi, start, &sit);
		if (err)
			break;
		seg_info_from_raw_sit(se, &sit);

		if (se->type >= NR_PERSISTENT_LOG) {
			f2fs_err(sbi, "Invalid segment type: %u, segno: %u",
							se->type, start);
			err = -EFSCORRUPTED;
			f2fs_handle_error(sbi, ERROR_INCONSISTENT_SUM_TYPE);
			break;
		}

		sit_valid_blocks[SE_PAGETYPE(se)] += se->valid_blocks;

		if (f2fs_block_unit_discard(sbi)) {
			if (is_set_ckpt_flags(sbi, CP_TRIMMED_FLAG)) {
				memset(se->discard_map, 0xff, SIT_VBLOCK_MAP_SIZE);
			} else {
				memcpy(se->discard_map, se->cur_valid_map,
							SIT_VBLOCK_MAP_SIZE);
				sbi->discard_blks += old_valid_blocks;
				sbi->discard_blks -= se->valid_blocks;
			}
		}

		if (__is_large_section(sbi)) {
			get_sec_entry(sbi, start)->valid_blocks +=
							se->valid_blocks;
			get_sec_entry(sbi, start)->valid_blocks -=
							old_valid_blocks;
		}
	}
	up_read(&curseg->journal_rwsem);

	if (err)
		return err;

	if (sit_valid_blocks[NODE] != valid_node_count(sbi)) {
		f2fs_err(sbi, "SIT is corrupted node# %u vs %u",
			 sit_valid_blocks[NODE], valid_node_count(sbi));
		f2fs_handle_error(sbi, ERROR_INCONSISTENT_NODE_COUNT);
		return -EFSCORRUPTED;
	}

	if (sit_valid_blocks[DATA] + sit_valid_blocks[NODE] >
				valid_user_blocks(sbi)) {
		f2fs_err(sbi, "SIT is corrupted data# %u %u vs %u",
			 sit_valid_blocks[DATA], sit_valid_blocks[NODE],
			 valid_user_blocks(sbi));
		f2fs_handle_error(sbi, ERROR_INCONSISTENT_BLOCK_COUNT);
		return -EFSCORRUPTED;
	}

	return 0;
}

static void init_free_segmap(struct f2fs_sb_info *sbi)
{
	unsigned int start;
	int type;
	struct seg_entry *sentry;

	for (start = 0; start < MAIN_SEGS(sbi); start++) {
		if (f2fs_usable_blks_in_seg(sbi, start) == 0)
			continue;
		sentry = get_seg_entry(sbi, start);
		if (!sentry->valid_blocks)
			__set_free(sbi, start);
		else
			SIT_I(sbi)->written_valid_blocks +=
						sentry->valid_blocks;
	}

	/* set use the current segments */
	for (type = CURSEG_HOT_DATA; type <= CURSEG_COLD_NODE; type++) {
		struct curseg_info *curseg_t = CURSEG_I(sbi, type);

		__set_test_and_inuse(sbi, curseg_t->segno);
	}
}

static void init_dirty_segmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct free_segmap_info *free_i = FREE_I(sbi);
	unsigned int segno = 0, offset = 0, secno;
	block_t valid_blocks, usable_blks_in_seg;

	while (1) {
		/* find dirty segment based on free segmap */
		segno = find_next_inuse(free_i, MAIN_SEGS(sbi), offset);
		if (segno >= MAIN_SEGS(sbi))
			break;
		offset = segno + 1;
		valid_blocks = get_valid_blocks(sbi, segno, false);
		usable_blks_in_seg = f2fs_usable_blks_in_seg(sbi, segno);
		if (valid_blocks == usable_blks_in_seg || !valid_blocks)
			continue;
		if (valid_blocks > usable_blks_in_seg) {
			f2fs_bug_on(sbi, 1);
			continue;
		}
		mutex_lock(&dirty_i->seglist_lock);
		__locate_dirty_segment(sbi, segno, DIRTY);
		mutex_unlock(&dirty_i->seglist_lock);
	}

	if (!__is_large_section(sbi))
		return;

	mutex_lock(&dirty_i->seglist_lock);
	for (segno = 0; segno < MAIN_SEGS(sbi); segno += SEGS_PER_SEC(sbi)) {
		valid_blocks = get_valid_blocks(sbi, segno, true);
		secno = GET_SEC_FROM_SEG(sbi, segno);

		if (!valid_blocks || valid_blocks == CAP_BLKS_PER_SEC(sbi))
			continue;
		if (IS_CURSEC(sbi, secno))
			continue;
		set_bit(secno, dirty_i->dirty_secmap);
	}
	mutex_unlock(&dirty_i->seglist_lock);
}

static int init_victim_secmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));

	dirty_i->victim_secmap = f2fs_kvzalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!dirty_i->victim_secmap)
		return -ENOMEM;

	dirty_i->pinned_secmap = f2fs_kvzalloc(sbi, bitmap_size, GFP_KERNEL);
	if (!dirty_i->pinned_secmap)
		return -ENOMEM;

	dirty_i->pinned_secmap_cnt = 0;
	dirty_i->enable_pin_section = true;
	return 0;
}

static int build_dirty_segmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i;
	unsigned int bitmap_size, i;

	/* allocate memory for dirty segments list information */
	dirty_i = f2fs_kzalloc(sbi, sizeof(struct dirty_seglist_info),
								GFP_KERNEL);
	if (!dirty_i)
		return -ENOMEM;

	SM_I(sbi)->dirty_info = dirty_i;
	mutex_init(&dirty_i->seglist_lock);

	bitmap_size = f2fs_bitmap_size(MAIN_SEGS(sbi));

	for (i = 0; i < NR_DIRTY_TYPE; i++) {
		dirty_i->dirty_segmap[i] = f2fs_kvzalloc(sbi, bitmap_size,
								GFP_KERNEL);
		if (!dirty_i->dirty_segmap[i])
			return -ENOMEM;
	}

	if (__is_large_section(sbi)) {
		bitmap_size = f2fs_bitmap_size(MAIN_SECS(sbi));
		dirty_i->dirty_secmap = f2fs_kvzalloc(sbi,
						bitmap_size, GFP_KERNEL);
		if (!dirty_i->dirty_secmap)
			return -ENOMEM;
	}

	init_dirty_segmap(sbi);
	return init_victim_secmap(sbi);
}

static int sanity_check_curseg(struct f2fs_sb_info *sbi)
{
	int i;

	/*
	 * In LFS/SSR curseg, .next_blkoff should point to an unused blkaddr;
	 * In LFS curseg, all blkaddr after .next_blkoff should be unused.
	 */
	for (i = 0; i < NR_PERSISTENT_LOG; i++) {
		struct curseg_info *curseg = CURSEG_I(sbi, i);
		struct seg_entry *se = get_seg_entry(sbi, curseg->segno);
		unsigned int blkofs = curseg->next_blkoff;

		if (f2fs_sb_has_readonly(sbi) &&
			i != CURSEG_HOT_DATA && i != CURSEG_HOT_NODE)
			continue;

		sanity_check_seg_type(sbi, curseg->seg_type);

		if (curseg->alloc_type != LFS && curseg->alloc_type != SSR) {
			f2fs_err(sbi,
				 "Current segment has invalid alloc_type:%d",
				 curseg->alloc_type);
			f2fs_handle_error(sbi, ERROR_INVALID_CURSEG);
			return -EFSCORRUPTED;
		}

		if (f2fs_test_bit(blkofs, se->cur_valid_map))
			goto out;

		if (curseg->alloc_type == SSR)
			continue;

		for (blkofs += 1; blkofs < BLKS_PER_SEG(sbi); blkofs++) {
			if (!f2fs_test_bit(blkofs, se->cur_valid_map))
				continue;
out:
			f2fs_err(sbi,
				 "Current segment's next free block offset is inconsistent with bitmap, logtype:%u, segno:%u, type:%u, next_blkoff:%u, blkofs:%u",
				 i, curseg->segno, curseg->alloc_type,
				 curseg->next_blkoff, blkofs);
			f2fs_handle_error(sbi, ERROR_INVALID_CURSEG);
			return -EFSCORRUPTED;
		}
	}
	return 0;
}

#ifdef CONFIG_BLK_DEV_ZONED
static int check_zone_write_pointer(struct f2fs_sb_info *sbi,
				    struct f2fs_dev_info *fdev,
				    struct blk_zone *zone)
{
	unsigned int zone_segno;
	block_t zone_block, valid_block_cnt;
	unsigned int log_sectors_per_block = sbi->log_blocksize - SECTOR_SHIFT;
	int ret;
	unsigned int nofs_flags;

	if (zone->type != BLK_ZONE_TYPE_SEQWRITE_REQ)
		return 0;

	zone_block = fdev->start_blk + (zone->start >> log_sectors_per_block);
	zone_segno = GET_SEGNO(sbi, zone_block);

	/*
	 * Skip check of zones cursegs point to, since
	 * fix_curseg_write_pointer() checks them.
	 */
	if (zone_segno >= MAIN_SEGS(sbi))
		return 0;

	/*
	 * Get # of valid block of the zone.
	 */
	valid_block_cnt = get_valid_blocks(sbi, zone_segno, true);
	if (IS_CURSEC(sbi, GET_SEC_FROM_SEG(sbi, zone_segno))) {
		f2fs_notice(sbi, "Open zones: valid block[0x%x,0x%x] cond[%s]",
				zone_segno, valid_block_cnt,
				blk_zone_cond_str(zone->cond));
		return 0;
	}

	if ((!valid_block_cnt && zone->cond == BLK_ZONE_COND_EMPTY) ||
	    (valid_block_cnt && zone->cond == BLK_ZONE_COND_FULL))
		return 0;

	if (!valid_block_cnt) {
		f2fs_notice(sbi, "Zone without valid block has non-zero write "
			    "pointer. Reset the write pointer: cond[%s]",
			    blk_zone_cond_str(zone->cond));
		ret = __f2fs_issue_discard_zone(sbi, fdev->bdev, zone_block,
					zone->len >> log_sectors_per_block);
		if (ret)
			f2fs_err(sbi, "Discard zone failed: %s (errno=%d)",
				 fdev->path, ret);
		return ret;
	}

	/*
	 * If there are valid blocks and the write pointer doesn't match
	 * with them, we need to report the inconsistency and fill
	 * the zone till the end to close the zone. This inconsistency
	 * does not cause write error because the zone will not be
	 * selected for write operation until it get discarded.
	 */
	f2fs_notice(sbi, "Valid blocks are not aligned with write "
		    "pointer: valid block[0x%x,0x%x] cond[%s]",
		    zone_segno, valid_block_cnt, blk_zone_cond_str(zone->cond));

	nofs_flags = memalloc_nofs_save();
	ret = blkdev_zone_mgmt(fdev->bdev, REQ_OP_ZONE_FINISH,
				zone->start, zone->len);
	memalloc_nofs_restore(nofs_flags);
	if (ret == -EOPNOTSUPP) {
		ret = blkdev_issue_zeroout(fdev->bdev, zone->wp,
					zone->len - (zone->wp - zone->start),
					GFP_NOFS, 0);
		if (ret)
			f2fs_err(sbi, "Fill up zone failed: %s (errno=%d)",
					fdev->path, ret);
	} else if (ret) {
		f2fs_err(sbi, "Finishing zone failed: %s (errno=%d)",
				fdev->path, ret);
	}

	return ret;
}

static struct f2fs_dev_info *get_target_zoned_dev(struct f2fs_sb_info *sbi,
						  block_t zone_blkaddr)
{
	int i;

	for (i = 0; i < sbi->s_ndevs; i++) {
		if (!bdev_is_zoned(FDEV(i).bdev))
			continue;
		if (sbi->s_ndevs == 1 || (FDEV(i).start_blk <= zone_blkaddr &&
				zone_blkaddr <= FDEV(i).end_blk))
			return &FDEV(i);
	}

	return NULL;
}

static int report_one_zone_cb(struct blk_zone *zone, unsigned int idx,
			      void *data)
{
	memcpy(data, zone, sizeof(struct blk_zone));
	return 0;
}

static int fix_curseg_write_pointer(struct f2fs_sb_info *sbi, int type)
{
	struct curseg_info *cs = CURSEG_I(sbi, type);
	struct f2fs_dev_info *zbd;
	struct blk_zone zone;
	unsigned int cs_section, wp_segno, wp_blkoff, wp_sector_off;
	block_t cs_zone_block, wp_block;
	unsigned int log_sectors_per_block = sbi->log_blocksize - SECTOR_SHIFT;
	sector_t zone_sector;
	int err;

	cs_section = GET_SEC_FROM_SEG(sbi, cs->segno);
	cs_zone_block = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, cs_section));

	zbd = get_target_zoned_dev(sbi, cs_zone_block);
	if (!zbd)
		return 0;

	/* report zone for the sector the curseg points to */
	zone_sector = (sector_t)(cs_zone_block - zbd->start_blk)
		<< log_sectors_per_block;
	err = blkdev_report_zones(zbd->bdev, zone_sector, 1,
				  report_one_zone_cb, &zone);
	if (err != 1) {
		f2fs_err(sbi, "Report zone failed: %s errno=(%d)",
			 zbd->path, err);
		return err;
	}

	if (zone.type != BLK_ZONE_TYPE_SEQWRITE_REQ)
		return 0;

	/*
	 * When safely unmounted in the previous mount, we could use current
	 * segments. Otherwise, allocate new sections.
	 */
	if (is_set_ckpt_flags(sbi, CP_UMOUNT_FLAG)) {
		wp_block = zbd->start_blk + (zone.wp >> log_sectors_per_block);
		wp_segno = GET_SEGNO(sbi, wp_block);
		wp_blkoff = wp_block - START_BLOCK(sbi, wp_segno);
		wp_sector_off = zone.wp & GENMASK(log_sectors_per_block - 1, 0);

		if (cs->segno == wp_segno && cs->next_blkoff == wp_blkoff &&
				wp_sector_off == 0)
			return 0;

		f2fs_notice(sbi, "Unaligned curseg[%d] with write pointer: "
			    "curseg[0x%x,0x%x] wp[0x%x,0x%x]", type, cs->segno,
			    cs->next_blkoff, wp_segno, wp_blkoff);
	}

	/* Allocate a new section if it's not new. */
	if (cs->next_blkoff ||
	    cs->segno != GET_SEG_FROM_SEC(sbi, GET_ZONE_FROM_SEC(sbi, cs_section))) {
		unsigned int old_segno = cs->segno, old_blkoff = cs->next_blkoff;

		f2fs_allocate_new_section(sbi, type, true);
		f2fs_notice(sbi, "Assign new section to curseg[%d]: "
				"[0x%x,0x%x] -> [0x%x,0x%x]",
				type, old_segno, old_blkoff,
				cs->segno, cs->next_blkoff);
	}

	/* check consistency of the zone curseg pointed to */
	if (check_zone_write_pointer(sbi, zbd, &zone))
		return -EIO;

	/* check newly assigned zone */
	cs_section = GET_SEC_FROM_SEG(sbi, cs->segno);
	cs_zone_block = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, cs_section));

	zbd = get_target_zoned_dev(sbi, cs_zone_block);
	if (!zbd)
		return 0;

	zone_sector = (sector_t)(cs_zone_block - zbd->start_blk)
		<< log_sectors_per_block;
	err = blkdev_report_zones(zbd->bdev, zone_sector, 1,
				  report_one_zone_cb, &zone);
	if (err != 1) {
		f2fs_err(sbi, "Report zone failed: %s errno=(%d)",
			 zbd->path, err);
		return err;
	}

	if (zone.type != BLK_ZONE_TYPE_SEQWRITE_REQ)
		return 0;

	if (zone.wp != zone.start) {
		f2fs_notice(sbi,
			    "New zone for curseg[%d] is not yet discarded. "
			    "Reset the zone: curseg[0x%x,0x%x]",
			    type, cs->segno, cs->next_blkoff);
		err = __f2fs_issue_discard_zone(sbi, zbd->bdev,	cs_zone_block,
					zone.len >> log_sectors_per_block);
		if (err) {
			f2fs_err(sbi, "Discard zone failed: %s (errno=%d)",
				 zbd->path, err);
			return err;
		}
	}

	return 0;
}

int f2fs_fix_curseg_write_pointer(struct f2fs_sb_info *sbi)
{
	int i, ret;

	for (i = 0; i < NR_PERSISTENT_LOG; i++) {
		ret = fix_curseg_write_pointer(sbi, i);
		if (ret)
			return ret;
	}

	return 0;
}

struct check_zone_write_pointer_args {
	struct f2fs_sb_info *sbi;
	struct f2fs_dev_info *fdev;
};

static int check_zone_write_pointer_cb(struct blk_zone *zone, unsigned int idx,
				      void *data)
{
	struct check_zone_write_pointer_args *args;

	args = (struct check_zone_write_pointer_args *)data;

	return check_zone_write_pointer(args->sbi, args->fdev, zone);
}

int f2fs_check_write_pointer(struct f2fs_sb_info *sbi)
{
	int i, ret;
	struct check_zone_write_pointer_args args;

	for (i = 0; i < sbi->s_ndevs; i++) {
		if (!bdev_is_zoned(FDEV(i).bdev))
			continue;

		args.sbi = sbi;
		args.fdev = &FDEV(i);
		ret = blkdev_report_zones(FDEV(i).bdev, 0, BLK_ALL_ZONES,
					  check_zone_write_pointer_cb, &args);
		if (ret < 0)
			return ret;
	}

	return 0;
}

/*
 * Return the number of usable blocks in a segment. The number of blocks
 * returned is always equal to the number of blocks in a segment for
 * segments fully contained within a sequential zone capacity or a
 * conventional zone. For segments partially contained in a sequential
 * zone capacity, the number of usable blocks up to the zone capacity
 * is returned. 0 is returned in all other cases.
 */
static inline unsigned int f2fs_usable_zone_blks_in_seg(
			struct f2fs_sb_info *sbi, unsigned int segno)
{
	block_t seg_start, sec_start_blkaddr, sec_cap_blkaddr;
	unsigned int secno;

	if (!sbi->unusable_blocks_per_sec)
		return BLKS_PER_SEG(sbi);

	secno = GET_SEC_FROM_SEG(sbi, segno);
	seg_start = START_BLOCK(sbi, segno);
	sec_start_blkaddr = START_BLOCK(sbi, GET_SEG_FROM_SEC(sbi, secno));
	sec_cap_blkaddr = sec_start_blkaddr + CAP_BLKS_PER_SEC(sbi);

	/*
	 * If segment starts before zone capacity and spans beyond
	 * zone capacity, then usable blocks are from seg start to
	 * zone capacity. If the segment starts after the zone capacity,
	 * then there are no usable blocks.
	 */
	if (seg_start >= sec_cap_blkaddr)
		return 0;
	if (seg_start + BLKS_PER_SEG(sbi) > sec_cap_blkaddr)
		return sec_cap_blkaddr - seg_start;

	return BLKS_PER_SEG(sbi);
}
#else
int f2fs_fix_curseg_write_pointer(struct f2fs_sb_info *sbi)
{
	return 0;
}

int f2fs_check_write_pointer(struct f2fs_sb_info *sbi)
{
	return 0;
}

static inline unsigned int f2fs_usable_zone_blks_in_seg(struct f2fs_sb_info *sbi,
							unsigned int segno)
{
	return 0;
}

#endif
unsigned int f2fs_usable_blks_in_seg(struct f2fs_sb_info *sbi,
					unsigned int segno)
{
	if (f2fs_sb_has_blkzoned(sbi))
		return f2fs_usable_zone_blks_in_seg(sbi, segno);

	return BLKS_PER_SEG(sbi);
}

unsigned int f2fs_usable_segs_in_sec(struct f2fs_sb_info *sbi)
{
	if (f2fs_sb_has_blkzoned(sbi))
		return CAP_SEGS_PER_SEC(sbi);

	return SEGS_PER_SEC(sbi);
}

/*
 * Update min, max modified time for cost-benefit GC algorithm
 */
static void init_min_max_mtime(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int segno;

	down_write(&sit_i->sentry_lock);

	sit_i->min_mtime = ULLONG_MAX;

	for (segno = 0; segno < MAIN_SEGS(sbi); segno += SEGS_PER_SEC(sbi)) {
		unsigned int i;
		unsigned long long mtime = 0;

		for (i = 0; i < SEGS_PER_SEC(sbi); i++)
			mtime += get_seg_entry(sbi, segno + i)->mtime;

		mtime = div_u64(mtime, SEGS_PER_SEC(sbi));

		if (sit_i->min_mtime > mtime)
			sit_i->min_mtime = mtime;
	}
	sit_i->max_mtime = get_mtime(sbi, false);
	sit_i->dirty_max_mtime = 0;
	up_write(&sit_i->sentry_lock);
}

int f2fs_build_segment_manager(struct f2fs_sb_info *sbi)
{
	struct f2fs_super_block *raw_super = F2FS_RAW_SUPER(sbi);
	struct f2fs_checkpoint *ckpt = F2FS_CKPT(sbi);
	struct f2fs_sm_info *sm_info;
	int err;

	sm_info = f2fs_kzalloc(sbi, sizeof(struct f2fs_sm_info), GFP_KERNEL);
	if (!sm_info)
		return -ENOMEM;

	/* init sm info */
	sbi->sm_info = sm_info;
	sm_info->seg0_blkaddr = le32_to_cpu(raw_super->segment0_blkaddr);
	sm_info->main_blkaddr = le32_to_cpu(raw_super->main_blkaddr);
	sm_info->segment_count = le32_to_cpu(raw_super->segment_count);
	sm_info->reserved_segments = le32_to_cpu(ckpt->rsvd_segment_count);
	sm_info->ovp_segments = le32_to_cpu(ckpt->overprov_segment_count);
	sm_info->main_segments = le32_to_cpu(raw_super->segment_count_main);
	sm_info->ssa_blkaddr = le32_to_cpu(raw_super->ssa_blkaddr);
	sm_info->rec_prefree_segments = sm_info->main_segments *
					DEF_RECLAIM_PREFREE_SEGMENTS / 100;
	if (sm_info->rec_prefree_segments > DEF_MAX_RECLAIM_PREFREE_SEGMENTS)
		sm_info->rec_prefree_segments = DEF_MAX_RECLAIM_PREFREE_SEGMENTS;

	if (!f2fs_lfs_mode(sbi))
		sm_info->ipu_policy = BIT(F2FS_IPU_FSYNC);
	sm_info->min_ipu_util = DEF_MIN_IPU_UTIL;
	sm_info->min_fsync_blocks = DEF_MIN_FSYNC_BLOCKS;
	sm_info->min_seq_blocks = BLKS_PER_SEG(sbi);
	sm_info->min_hot_blocks = DEF_MIN_HOT_BLOCKS;
	sm_info->min_ssr_sections = reserved_sections(sbi);

	INIT_LIST_HEAD(&sm_info->sit_entry_set);

	init_f2fs_rwsem(&sm_info->curseg_lock);

	err = f2fs_create_flush_cmd_control(sbi);
	if (err)
		return err;

	err = create_discard_cmd_control(sbi);
	if (err)
		return err;

	err = build_sit_info(sbi);
	if (err)
		return err;
	err = build_free_segmap(sbi);
	if (err)
		return err;
	err = build_curseg(sbi);
	if (err)
		return err;

	/* reinit free segmap based on SIT */
	err = build_sit_entries(sbi);
	if (err)
		return err;

	init_free_segmap(sbi);
	err = build_dirty_segmap(sbi);
	if (err)
		return err;

	err = sanity_check_curseg(sbi);
	if (err)
		return err;

	init_min_max_mtime(sbi);
	return 0;
}

static void discard_dirty_segmap(struct f2fs_sb_info *sbi,
		enum dirty_type dirty_type)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	mutex_lock(&dirty_i->seglist_lock);
	kvfree(dirty_i->dirty_segmap[dirty_type]);
	dirty_i->nr_dirty[dirty_type] = 0;
	mutex_unlock(&dirty_i->seglist_lock);
}

static void destroy_victim_secmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	kvfree(dirty_i->pinned_secmap);
	kvfree(dirty_i->victim_secmap);
}

static void destroy_dirty_segmap(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	int i;

	if (!dirty_i)
		return;

	/* discard pre-free/dirty segments list */
	for (i = 0; i < NR_DIRTY_TYPE; i++)
		discard_dirty_segmap(sbi, i);

	if (__is_large_section(sbi)) {
		mutex_lock(&dirty_i->seglist_lock);
		kvfree(dirty_i->dirty_secmap);
		mutex_unlock(&dirty_i->seglist_lock);
	}

	destroy_victim_secmap(sbi);
	SM_I(sbi)->dirty_info = NULL;
	kfree(dirty_i);
}

static void destroy_curseg(struct f2fs_sb_info *sbi)
{
	struct curseg_info *array = SM_I(sbi)->curseg_array;
	int i;

	if (!array)
		return;
	SM_I(sbi)->curseg_array = NULL;
	for (i = 0; i < NR_CURSEG_TYPE; i++) {
		kfree(array[i].sum_blk);
		kfree(array[i].journal);
	}
	kfree(array);
}

static void destroy_free_segmap(struct f2fs_sb_info *sbi)
{
	struct free_segmap_info *free_i = SM_I(sbi)->free_info;

	if (!free_i)
		return;
	SM_I(sbi)->free_info = NULL;
	kvfree(free_i->free_segmap);
	kvfree(free_i->free_secmap);
	kfree(free_i);
}

static void destroy_sit_info(struct f2fs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);

	if (!sit_i)
		return;

	if (sit_i->sentries)
		kvfree(sit_i->bitmap);
	kfree(sit_i->tmp_map);

	kvfree(sit_i->sentries);
	kvfree(sit_i->sec_entries);
	kvfree(sit_i->dirty_sentries_bitmap);

	SM_I(sbi)->sit_info = NULL;
	kvfree(sit_i->sit_bitmap);
#ifdef CONFIG_F2FS_CHECK_FS
	kvfree(sit_i->sit_bitmap_mir);
	kvfree(sit_i->invalid_segmap);
#endif
	kfree(sit_i);
}

void f2fs_destroy_segment_manager(struct f2fs_sb_info *sbi)
{
	struct f2fs_sm_info *sm_info = SM_I(sbi);

	if (!sm_info)
		return;
	f2fs_destroy_flush_cmd_control(sbi, true);
	destroy_discard_cmd_control(sbi);
	destroy_dirty_segmap(sbi);
	destroy_curseg(sbi);
	destroy_free_segmap(sbi);
	destroy_sit_info(sbi);
	sbi->sm_info = NULL;
	kfree(sm_info);
}

int __init f2fs_create_segment_manager_caches(void)
{
	discard_entry_slab = f2fs_kmem_cache_create("f2fs_discard_entry",
			sizeof(struct discard_entry));
	if (!discard_entry_slab)
		goto fail;

	discard_cmd_slab = f2fs_kmem_cache_create("f2fs_discard_cmd",
			sizeof(struct discard_cmd));
	if (!discard_cmd_slab)
		goto destroy_discard_entry;

	sit_entry_set_slab = f2fs_kmem_cache_create("f2fs_sit_entry_set",
			sizeof(struct sit_entry_set));
	if (!sit_entry_set_slab)
		goto destroy_discard_cmd;

	revoke_entry_slab = f2fs_kmem_cache_create("f2fs_revoke_entry",
			sizeof(struct revoke_entry));
	if (!revoke_entry_slab)
		goto destroy_sit_entry_set;
	return 0;

destroy_sit_entry_set:
	kmem_cache_destroy(sit_entry_set_slab);
destroy_discard_cmd:
	kmem_cache_destroy(discard_cmd_slab);
destroy_discard_entry:
	kmem_cache_destroy(discard_entry_slab);
fail:
	return -ENOMEM;
}

void f2fs_destroy_segment_manager_caches(void)
{
	kmem_cache_destroy(sit_entry_set_slab);
	kmem_cache_destroy(discard_cmd_slab);
	kmem_cache_destroy(discard_entry_slab);
	kmem_cache_destroy(revoke_entry_slab);
}
