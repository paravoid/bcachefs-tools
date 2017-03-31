/*
 * bcachefs setup/teardown code, and some metadata io - read a superblock and
 * figure out what to do with it.
 *
 * Copyright 2010, 2011 Kent Overstreet <kent.overstreet@gmail.com>
 * Copyright 2012 Google, Inc.
 */

#include "bcachefs.h"
#include "alloc.h"
#include "btree_cache.h"
#include "btree_gc.h"
#include "btree_update.h"
#include "btree_io.h"
#include "chardev.h"
#include "checksum.h"
#include "clock.h"
#include "compress.h"
#include "debug.h"
#include "error.h"
#include "fs.h"
#include "fs-gc.h"
#include "inode.h"
#include "io.h"
#include "journal.h"
#include "keylist.h"
#include "move.h"
#include "migrate.h"
#include "movinggc.h"
#include "super.h"
#include "super-io.h"
#include "sysfs.h"
#include "tier.h"

#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/idr.h>
#include <linux/kthread.h>
#include <linux/module.h>
#include <linux/percpu.h>
#include <linux/random.h>
#include <linux/sysfs.h>
#include <crypto/hash.h>

#include <trace/events/bcachefs.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Kent Overstreet <kent.overstreet@gmail.com>");

#define KTYPE(type)							\
struct kobj_type type ## _ktype = {					\
	.release	= type ## _release,				\
	.sysfs_ops	= &type ## _sysfs_ops,				\
	.default_attrs	= type ## _files				\
}

static void bch2_fs_release(struct kobject *);
static void bch2_dev_release(struct kobject *);

static void bch2_fs_internal_release(struct kobject *k)
{
}

static void bch2_fs_opts_dir_release(struct kobject *k)
{
}

static void bch2_fs_time_stats_release(struct kobject *k)
{
}

static KTYPE(bch2_fs);
static KTYPE(bch2_fs_internal);
static KTYPE(bch2_fs_opts_dir);
static KTYPE(bch2_fs_time_stats);
static KTYPE(bch2_dev);

static struct kset *bcachefs_kset;
static LIST_HEAD(bch_fs_list);
static DEFINE_MUTEX(bch_fs_list_lock);

static DECLARE_WAIT_QUEUE_HEAD(bch_read_only_wait);

static void bch2_dev_free(struct bch_dev *);
static int bch2_dev_alloc(struct bch_fs *, unsigned);
static int bch2_dev_sysfs_online(struct bch_dev *);
static void __bch2_dev_read_only(struct bch_fs *, struct bch_dev *);

struct bch_fs *bch2_bdev_to_fs(struct block_device *bdev)
{
	struct bch_fs *c;
	struct bch_dev *ca;
	unsigned i;

	mutex_lock(&bch_fs_list_lock);
	rcu_read_lock();

	list_for_each_entry(c, &bch_fs_list, list)
		for_each_member_device_rcu(ca, c, i)
			if (ca->disk_sb.bdev == bdev) {
				closure_get(&c->cl);
				goto found;
			}
	c = NULL;
found:
	rcu_read_unlock();
	mutex_unlock(&bch_fs_list_lock);

	return c;
}

static struct bch_fs *__bch2_uuid_to_fs(uuid_le uuid)
{
	struct bch_fs *c;

	lockdep_assert_held(&bch_fs_list_lock);

	list_for_each_entry(c, &bch_fs_list, list)
		if (!memcmp(&c->disk_sb->uuid, &uuid, sizeof(uuid_le)))
			return c;

	return NULL;
}

struct bch_fs *bch2_uuid_to_fs(uuid_le uuid)
{
	struct bch_fs *c;

	mutex_lock(&bch_fs_list_lock);
	c = __bch2_uuid_to_fs(uuid);
	if (c)
		closure_get(&c->cl);
	mutex_unlock(&bch_fs_list_lock);

	return c;
}

int bch2_congested(struct bch_fs *c, int bdi_bits)
{
	struct backing_dev_info *bdi;
	struct bch_dev *ca;
	unsigned i;
	int ret = 0;

	if (bdi_bits & (1 << WB_sync_congested)) {
		/* Reads - check all devices: */
		for_each_readable_member(ca, c, i) {
			bdi = blk_get_backing_dev_info(ca->disk_sb.bdev);

			if (bdi_congested(bdi, bdi_bits)) {
				ret = 1;
				break;
			}
		}
	} else {
		/* Writes prefer fastest tier: */
		struct bch_tier *tier = READ_ONCE(c->fastest_tier);
		struct dev_group *grp = tier ? &tier->devs : &c->all_devs;

		rcu_read_lock();
		group_for_each_dev(ca, grp, i) {
			bdi = blk_get_backing_dev_info(ca->disk_sb.bdev);

			if (bdi_congested(bdi, bdi_bits)) {
				ret = 1;
				break;
			}
		}
		rcu_read_unlock();
	}

	return ret;
}

static int bch2_congested_fn(void *data, int bdi_bits)
{
	struct bch_fs *c = data;

	return bch2_congested(c, bdi_bits);
}

/* Filesystem RO/RW: */

/*
 * For startup/shutdown of RW stuff, the dependencies are:
 *
 * - foreground writes depend on copygc and tiering (to free up space)
 *
 * - copygc and tiering depend on mark and sweep gc (they actually probably
 *   don't because they either reserve ahead of time or don't block if
 *   allocations fail, but allocations can require mark and sweep gc to run
 *   because of generation number wraparound)
 *
 * - all of the above depends on the allocator threads
 *
 * - allocator depends on the journal (when it rewrites prios and gens)
 */

static void __bch2_fs_read_only(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;

	bch2_tiering_stop(c);

	for_each_member_device(ca, c, i)
		bch2_moving_gc_stop(ca);

	bch2_gc_thread_stop(c);

	bch2_btree_flush(c);

	for_each_member_device(ca, c, i)
		bch2_dev_allocator_stop(ca);

	bch2_fs_journal_stop(&c->journal);
}

static void bch2_writes_disabled(struct percpu_ref *writes)
{
	struct bch_fs *c = container_of(writes, struct bch_fs, writes);

	set_bit(BCH_FS_WRITE_DISABLE_COMPLETE, &c->flags);
	wake_up(&bch_read_only_wait);
}

void bch2_fs_read_only(struct bch_fs *c)
{
	mutex_lock(&c->state_lock);
	if (c->state != BCH_FS_STARTING &&
	    c->state != BCH_FS_RW)
		goto out;

	if (test_bit(BCH_FS_ERROR, &c->flags))
		goto out;

	/*
	 * Block new foreground-end write operations from starting - any new
	 * writes will return -EROFS:
	 *
	 * (This is really blocking new _allocations_, writes to previously
	 * allocated space can still happen until stopping the allocator in
	 * bch2_dev_allocator_stop()).
	 */
	percpu_ref_kill(&c->writes);

	del_timer(&c->foreground_write_wakeup);
	cancel_delayed_work(&c->pd_controllers_update);

	c->foreground_write_pd.rate.rate = UINT_MAX;
	bch2_wake_delayed_writes((unsigned long) c);

	/*
	 * If we're not doing an emergency shutdown, we want to wait on
	 * outstanding writes to complete so they don't see spurious errors due
	 * to shutting down the allocator:
	 *
	 * If we are doing an emergency shutdown outstanding writes may
	 * hang until we shutdown the allocator so we don't want to wait
	 * on outstanding writes before shutting everything down - but
	 * we do need to wait on them before returning and signalling
	 * that going RO is complete:
	 */
	wait_event(bch_read_only_wait,
		   test_bit(BCH_FS_WRITE_DISABLE_COMPLETE, &c->flags) ||
		   test_bit(BCH_FS_EMERGENCY_RO, &c->flags));

	__bch2_fs_read_only(c);

	wait_event(bch_read_only_wait,
		   test_bit(BCH_FS_WRITE_DISABLE_COMPLETE, &c->flags));

	clear_bit(BCH_FS_WRITE_DISABLE_COMPLETE, &c->flags);

	if (!bch2_journal_error(&c->journal) &&
	    !test_bit(BCH_FS_ERROR, &c->flags)) {
		mutex_lock(&c->sb_lock);
		SET_BCH_SB_CLEAN(c->disk_sb, true);
		bch2_write_super(c);
		mutex_unlock(&c->sb_lock);
	}

	c->state = BCH_FS_RO;
out:
	mutex_unlock(&c->state_lock);
}

static void bch2_fs_read_only_work(struct work_struct *work)
{
	struct bch_fs *c =
		container_of(work, struct bch_fs, read_only_work);

	bch2_fs_read_only(c);
}

static void bch2_fs_read_only_async(struct bch_fs *c)
{
	queue_work(system_long_wq, &c->read_only_work);
}

bool bch2_fs_emergency_read_only(struct bch_fs *c)
{
	bool ret = !test_and_set_bit(BCH_FS_EMERGENCY_RO, &c->flags);

	bch2_fs_read_only_async(c);
	bch2_journal_halt(&c->journal);

	wake_up(&bch_read_only_wait);
	return ret;
}

const char *bch2_fs_read_write(struct bch_fs *c)
{
	struct bch_dev *ca;
	const char *err = NULL;
	unsigned i;

	mutex_lock(&c->state_lock);
	if (c->state != BCH_FS_STARTING &&
	    c->state != BCH_FS_RO)
		goto out;

	err = "error starting allocator thread";
	for_each_rw_member(ca, c, i)
		if (bch2_dev_allocator_start(ca)) {
			percpu_ref_put(&ca->io_ref);
			goto err;
		}

	err = "error starting btree GC thread";
	if (bch2_gc_thread_start(c))
		goto err;

	err = "error starting moving GC thread";
	for_each_rw_member(ca, c, i)
		if (bch2_moving_gc_start(ca)) {
			percpu_ref_put(&ca->io_ref);
			goto err;
		}

	err = "error starting tiering thread";
	if (bch2_tiering_start(c))
		goto err;

	schedule_delayed_work(&c->pd_controllers_update, 5 * HZ);

	if (c->state != BCH_FS_STARTING)
		percpu_ref_reinit(&c->writes);

	c->state = BCH_FS_RW;
	err = NULL;
out:
	mutex_unlock(&c->state_lock);
	return err;
err:
	__bch2_fs_read_only(c);
	goto out;
}

/* Filesystem startup/shutdown: */

static void bch2_fs_free(struct bch_fs *c)
{
	bch2_fs_encryption_exit(c);
	bch2_fs_btree_exit(c);
	bch2_fs_journal_exit(&c->journal);
	bch2_io_clock_exit(&c->io_clock[WRITE]);
	bch2_io_clock_exit(&c->io_clock[READ]);
	bch2_fs_compress_exit(c);
	bdi_destroy(&c->bdi);
	lg_lock_free(&c->usage_lock);
	free_percpu(c->usage_percpu);
	mempool_exit(&c->btree_bounce_pool);
	mempool_exit(&c->bio_bounce_pages);
	bioset_exit(&c->bio_write);
	bioset_exit(&c->bio_read_split);
	bioset_exit(&c->bio_read);
	bioset_exit(&c->btree_read_bio);
	mempool_exit(&c->btree_interior_update_pool);
	mempool_exit(&c->btree_reserve_pool);
	mempool_exit(&c->fill_iter);
	percpu_ref_exit(&c->writes);

	if (c->copygc_wq)
		destroy_workqueue(c->copygc_wq);
	if (c->wq)
		destroy_workqueue(c->wq);

	free_pages((unsigned long) c->disk_sb, c->disk_sb_order);
	kfree(c);
	module_put(THIS_MODULE);
}

static void bch2_fs_exit(struct bch_fs *c)
{
	unsigned i;

	del_timer_sync(&c->foreground_write_wakeup);
	cancel_delayed_work_sync(&c->pd_controllers_update);
	cancel_work_sync(&c->read_only_work);
	cancel_work_sync(&c->read_retry_work);

	for (i = 0; i < c->sb.nr_devices; i++)
		if (c->devs[i])
			bch2_dev_free(c->devs[i]);

	closure_debug_destroy(&c->cl);
	kobject_put(&c->kobj);
}

static void bch2_fs_offline(struct bch_fs *c)
{
	struct bch_dev *ca;
	unsigned i;

	mutex_lock(&bch_fs_list_lock);
	list_del(&c->list);
	mutex_unlock(&bch_fs_list_lock);

	for_each_member_device(ca, c, i)
		if (ca->kobj.state_in_sysfs &&
		    ca->disk_sb.bdev)
			sysfs_remove_link(&part_to_dev(ca->disk_sb.bdev->bd_part)->kobj,
					  "bcachefs");

	if (c->kobj.state_in_sysfs)
		kobject_del(&c->kobj);

	bch2_fs_debug_exit(c);
	bch2_fs_chardev_exit(c);

	kobject_put(&c->time_stats);
	kobject_put(&c->opts_dir);
	kobject_put(&c->internal);

	__bch2_fs_read_only(c);
}

static void bch2_fs_release(struct kobject *kobj)
{
	struct bch_fs *c = container_of(kobj, struct bch_fs, kobj);

	bch2_fs_free(c);
}

void bch2_fs_stop(struct bch_fs *c)
{
	mutex_lock(&c->state_lock);
	BUG_ON(c->state == BCH_FS_STOPPING);
	c->state = BCH_FS_STOPPING;
	mutex_unlock(&c->state_lock);

	bch2_fs_offline(c);

	closure_sync(&c->cl);

	bch2_fs_exit(c);
}

#define alloc_bucket_pages(gfp, ca)			\
	((void *) __get_free_pages(__GFP_ZERO|gfp, ilog2(bucket_pages(ca))))

static struct bch_fs *bch2_fs_alloc(struct bch_sb *sb, struct bch_opts opts)
{
	struct bch_sb_field_members *mi;
	struct bch_fs *c;
	unsigned i, iter_size, journal_entry_bytes;

	c = kzalloc(sizeof(struct bch_fs), GFP_KERNEL);
	if (!c)
		return NULL;

	__module_get(THIS_MODULE);

	c->minor		= -1;

	mutex_init(&c->state_lock);
	mutex_init(&c->sb_lock);
	mutex_init(&c->btree_cache_lock);
	mutex_init(&c->bucket_lock);
	mutex_init(&c->btree_root_lock);
	INIT_WORK(&c->read_only_work, bch2_fs_read_only_work);

	init_rwsem(&c->gc_lock);

#define BCH_TIME_STAT(name, frequency_units, duration_units)		\
	spin_lock_init(&c->name##_time.lock);
	BCH_TIME_STATS()
#undef BCH_TIME_STAT

	bch2_fs_allocator_init(c);
	bch2_fs_tiering_init(c);

	INIT_LIST_HEAD(&c->list);
	INIT_LIST_HEAD(&c->btree_cache);
	INIT_LIST_HEAD(&c->btree_cache_freeable);
	INIT_LIST_HEAD(&c->btree_cache_freed);

	INIT_LIST_HEAD(&c->btree_interior_update_list);
	mutex_init(&c->btree_reserve_cache_lock);
	mutex_init(&c->btree_interior_update_lock);

	mutex_init(&c->bio_bounce_pages_lock);
	bio_list_init(&c->read_retry_list);
	spin_lock_init(&c->read_retry_lock);
	INIT_WORK(&c->read_retry_work, bch2_read_retry_work);
	mutex_init(&c->zlib_workspace_lock);

	seqcount_init(&c->gc_pos_lock);

	c->prio_clock[READ].hand = 1;
	c->prio_clock[READ].min_prio = 0;
	c->prio_clock[WRITE].hand = 1;
	c->prio_clock[WRITE].min_prio = 0;

	init_waitqueue_head(&c->writeback_wait);
	c->writeback_pages_max = (256 << 10) / PAGE_SIZE;

	c->copy_gc_enabled = 1;
	c->tiering_enabled = 1;
	c->tiering_percent = 10;

	c->foreground_target_percent = 20;

	c->journal.write_time	= &c->journal_write_time;
	c->journal.delay_time	= &c->journal_delay_time;
	c->journal.blocked_time	= &c->journal_blocked_time;
	c->journal.flush_seq_time = &c->journal_flush_seq_time;

	mutex_lock(&c->sb_lock);

	if (bch2_sb_to_fs(c, sb)) {
		mutex_unlock(&c->sb_lock);
		goto err;
	}

	mutex_unlock(&c->sb_lock);

	scnprintf(c->name, sizeof(c->name), "%pU", &c->sb.user_uuid);

	bch2_opts_apply(&c->opts, bch2_sb_opts(sb));
	bch2_opts_apply(&c->opts, opts);

	c->opts.nochanges	|= c->opts.noreplay;
	c->opts.read_only	|= c->opts.nochanges;

	c->block_bits		= ilog2(c->sb.block_size);

	if (bch2_fs_init_fault("fs_alloc"))
		goto err;

	iter_size = (btree_blocks(c) + 1) * 2 *
		sizeof(struct btree_node_iter_set);

	journal_entry_bytes = 512U << BCH_SB_JOURNAL_ENTRY_SIZE(sb);

	if (!(c->wq = alloc_workqueue("bcachefs",
				WQ_FREEZABLE|WQ_MEM_RECLAIM|WQ_HIGHPRI, 1)) ||
	    !(c->copygc_wq = alloc_workqueue("bcache_copygc",
				WQ_FREEZABLE|WQ_MEM_RECLAIM|WQ_HIGHPRI, 1)) ||
	    percpu_ref_init(&c->writes, bch2_writes_disabled, 0, GFP_KERNEL) ||
	    mempool_init_kmalloc_pool(&c->btree_reserve_pool, 1,
				      sizeof(struct btree_reserve)) ||
	    mempool_init_kmalloc_pool(&c->btree_interior_update_pool, 1,
				      sizeof(struct btree_interior_update)) ||
	    mempool_init_kmalloc_pool(&c->fill_iter, 1, iter_size) ||
	    bioset_init(&c->btree_read_bio, 1, 0) ||
	    bioset_init(&c->bio_read, 1, offsetof(struct bch_read_bio, bio)) ||
	    bioset_init(&c->bio_read_split, 1, offsetof(struct bch_read_bio, bio)) ||
	    bioset_init(&c->bio_write, 1, offsetof(struct bch_write_bio, bio)) ||
	    mempool_init_page_pool(&c->bio_bounce_pages,
				   max_t(unsigned,
					 c->sb.btree_node_size,
					 BCH_ENCODED_EXTENT_MAX) /
				   PAGE_SECTORS, 0) ||
	    !(c->usage_percpu = alloc_percpu(struct bch_fs_usage)) ||
	    lg_lock_init(&c->usage_lock) ||
	    mempool_init_page_pool(&c->btree_bounce_pool, 1,
				   ilog2(btree_pages(c))) ||
	    bdi_setup_and_register(&c->bdi, "bcachefs") ||
	    bch2_io_clock_init(&c->io_clock[READ]) ||
	    bch2_io_clock_init(&c->io_clock[WRITE]) ||
	    bch2_fs_journal_init(&c->journal, journal_entry_bytes) ||
	    bch2_fs_btree_init(c) ||
	    bch2_fs_encryption_init(c) ||
	    bch2_fs_compress_init(c) ||
	    bch2_check_set_has_compressed_data(c, c->opts.compression))
		goto err;

	c->bdi.ra_pages		= VM_MAX_READAHEAD * 1024 / PAGE_SIZE;
	c->bdi.congested_fn	= bch2_congested_fn;
	c->bdi.congested_data	= c;

	mi = bch2_sb_get_members(c->disk_sb);
	for (i = 0; i < c->sb.nr_devices; i++)
		if (!bch2_is_zero(mi->members[i].uuid.b, sizeof(uuid_le)) &&
		    bch2_dev_alloc(c, i))
			goto err;

	/*
	 * Now that all allocations have succeeded, init various refcounty
	 * things that let us shutdown:
	 */
	closure_init(&c->cl, NULL);

	c->kobj.kset = bcachefs_kset;
	kobject_init(&c->kobj, &bch2_fs_ktype);
	kobject_init(&c->internal, &bch2_fs_internal_ktype);
	kobject_init(&c->opts_dir, &bch2_fs_opts_dir_ktype);
	kobject_init(&c->time_stats, &bch2_fs_time_stats_ktype);
	return c;
err:
	bch2_fs_free(c);
	return NULL;
}

static const char *__bch2_fs_online(struct bch_fs *c)
{
	struct bch_dev *ca;
	const char *err = NULL;
	unsigned i;
	int ret;

	lockdep_assert_held(&bch_fs_list_lock);

	if (!list_empty(&c->list))
		return NULL;

	if (__bch2_uuid_to_fs(c->sb.uuid))
		return "filesystem UUID already open";

	ret = bch2_fs_chardev_init(c);
	if (ret)
		return "error creating character device";

	bch2_fs_debug_init(c);

	if (kobject_add(&c->kobj, NULL, "%pU", c->sb.user_uuid.b) ||
	    kobject_add(&c->internal, &c->kobj, "internal") ||
	    kobject_add(&c->opts_dir, &c->kobj, "options") ||
	    kobject_add(&c->time_stats, &c->kobj, "time_stats"))
		return "error creating sysfs objects";

	mutex_lock(&c->state_lock);

	err = "error creating sysfs objects";
	__for_each_member_device(ca, c, i)
		if (bch2_dev_sysfs_online(ca))
			goto err;

	list_add(&c->list, &bch_fs_list);
	err = NULL;
err:
	mutex_unlock(&c->state_lock);
	return err;
}

static const char *bch2_fs_online(struct bch_fs *c)
{
	const char *err;

	mutex_lock(&bch_fs_list_lock);
	err = __bch2_fs_online(c);
	mutex_unlock(&bch_fs_list_lock);

	return err;
}

static const char *__bch2_fs_start(struct bch_fs *c)
{
	const char *err = "cannot allocate memory";
	struct bch_sb_field_members *mi;
	struct bch_dev *ca;
	unsigned i, id;
	time64_t now;
	LIST_HEAD(journal);
	struct jset *j;
	int ret = -EINVAL;

	BUG_ON(c->state != BCH_FS_STARTING);

	mutex_lock(&c->sb_lock);
	for_each_online_member(ca, c, i)
		bch2_sb_from_fs(c, ca);
	mutex_unlock(&c->sb_lock);

	if (BCH_SB_INITIALIZED(c->disk_sb)) {
		ret = bch2_journal_read(c, &journal);
		if (ret)
			goto err;

		j = &list_entry(journal.prev, struct journal_replay, list)->j;

		c->prio_clock[READ].hand = le16_to_cpu(j->read_clock);
		c->prio_clock[WRITE].hand = le16_to_cpu(j->write_clock);

		err = "error reading priorities";
		for_each_readable_member(ca, c, i) {
			ret = bch2_prio_read(ca);
			if (ret) {
				percpu_ref_put(&ca->io_ref);
				goto err;
			}
		}

		for (id = 0; id < BTREE_ID_NR; id++) {
			unsigned level;
			struct bkey_i *k;

			err = "bad btree root";
			k = bch2_journal_find_btree_root(c, j, id, &level);
			if (!k && id == BTREE_ID_EXTENTS)
				goto err;
			if (!k) {
				pr_debug("missing btree root: %d", id);
				continue;
			}

			err = "error reading btree root";
			if (bch2_btree_root_read(c, id, k, level))
				goto err;
		}

		bch_verbose(c, "starting mark and sweep:");

		err = "error in recovery";
		ret = bch2_initial_gc(c, &journal);
		if (ret)
			goto err;

		if (c->opts.noreplay)
			goto recovery_done;

		bch_verbose(c, "mark and sweep done");

		/*
		 * bch2_journal_start() can't happen sooner, or btree_gc_finish()
		 * will give spurious errors about oldest_gen > bucket_gen -
		 * this is a hack but oh well.
		 */
		bch2_journal_start(c);

		err = "error starting allocator thread";
		for_each_rw_member(ca, c, i)
			if (bch2_dev_allocator_start(ca)) {
				percpu_ref_put(&ca->io_ref);
				goto err;
			}

		bch_verbose(c, "starting journal replay:");

		err = "journal replay failed";
		ret = bch2_journal_replay(c, &journal);
		if (ret)
			goto err;

		bch_verbose(c, "journal replay done");

		if (c->opts.norecovery)
			goto recovery_done;

		bch_verbose(c, "starting fsck:");
		err = "error in fsck";
		ret = bch2_fsck(c, !c->opts.nofsck);
		if (ret)
			goto err;

		bch_verbose(c, "fsck done");
	} else {
		struct bch_inode_unpacked inode;
		struct bkey_inode_buf packed_inode;
		struct closure cl;

		closure_init_stack(&cl);

		bch_notice(c, "initializing new filesystem");

		ret = bch2_initial_gc(c, &journal);
		if (ret)
			goto err;

		err = "unable to allocate journal buckets";
		for_each_rw_member(ca, c, i)
			if (bch2_dev_journal_alloc(ca)) {
				percpu_ref_put(&ca->io_ref);
				goto err;
			}

		/*
		 * journal_res_get() will crash if called before this has
		 * set up the journal.pin FIFO and journal.cur pointer:
		 */
		bch2_journal_start(c);
		bch2_journal_set_replay_done(&c->journal);

		err = "error starting allocator thread";
		for_each_rw_member(ca, c, i)
			if (bch2_dev_allocator_start(ca)) {
				percpu_ref_put(&ca->io_ref);
				goto err;
			}

		err = "cannot allocate new btree root";
		for (id = 0; id < BTREE_ID_NR; id++)
			if (bch2_btree_root_alloc(c, id, &cl)) {
				closure_sync(&cl);
				goto err;
			}

		/* Wait for new btree roots to be written: */
		closure_sync(&cl);

		bch2_inode_init(c, &inode, 0, 0,
			       S_IFDIR|S_IRWXU|S_IRUGO|S_IXUGO, 0);
		inode.inum = BCACHE_ROOT_INO;

		bch2_inode_pack(&packed_inode, &inode);

		err = "error creating root directory";
		if (bch2_btree_insert(c, BTREE_ID_INODES,
				     &packed_inode.inode.k_i,
				     NULL, NULL, NULL, 0))
			goto err;

		err = "error writing first journal entry";
		if (bch2_journal_meta(&c->journal))
			goto err;
	}
recovery_done:
	err = "dynamic fault";
	if (bch2_fs_init_fault("fs_start"))
		goto err;

	if (c->opts.read_only) {
		bch2_fs_read_only(c);
	} else {
		err = bch2_fs_read_write(c);
		if (err)
			goto err;
	}

	mutex_lock(&c->sb_lock);
	mi = bch2_sb_get_members(c->disk_sb);
	now = ktime_get_seconds();

	for_each_member_device(ca, c, i)
		mi->members[ca->dev_idx].last_mount = cpu_to_le64(now);

	SET_BCH_SB_INITIALIZED(c->disk_sb, true);
	SET_BCH_SB_CLEAN(c->disk_sb, false);
	c->disk_sb->version = BCACHE_SB_VERSION_CDEV;

	bch2_write_super(c);
	mutex_unlock(&c->sb_lock);

	err = NULL;
out:
	bch2_journal_entries_free(&journal);
	return err;
err:
	switch (ret) {
	case BCH_FSCK_ERRORS_NOT_FIXED:
		bch_err(c, "filesystem contains errors: please report this to the developers");
		pr_cont("mount with -o fix_errors to repair");
		err = "fsck error";
		break;
	case BCH_FSCK_REPAIR_UNIMPLEMENTED:
		bch_err(c, "filesystem contains errors: please report this to the developers");
		pr_cont("repair unimplemented: inform the developers so that it can be added");
		err = "fsck error";
		break;
	case BCH_FSCK_REPAIR_IMPOSSIBLE:
		bch_err(c, "filesystem contains errors, but repair impossible");
		err = "fsck error";
		break;
	case BCH_FSCK_UNKNOWN_VERSION:
		err = "unknown metadata version";;
		break;
	case -ENOMEM:
		err = "cannot allocate memory";
		break;
	case -EIO:
		err = "IO error";
		break;
	}

	BUG_ON(!err);
	set_bit(BCH_FS_ERROR, &c->flags);
	goto out;
}

const char *bch2_fs_start(struct bch_fs *c)
{
	return __bch2_fs_start(c) ?: bch2_fs_online(c);
}

static const char *bch2_dev_may_add(struct bch_sb *sb, struct bch_fs *c)
{
	struct bch_sb_field_members *sb_mi;

	sb_mi = bch2_sb_get_members(sb);
	if (!sb_mi)
		return "Invalid superblock: member info area missing";

	if (le16_to_cpu(sb->block_size) != c->sb.block_size)
		return "mismatched block size";

	if (le16_to_cpu(sb_mi->members[sb->dev_idx].bucket_size) <
	    BCH_SB_BTREE_NODE_SIZE(c->disk_sb))
		return "new cache bucket size is too small";

	return NULL;
}

static const char *bch2_dev_in_fs(struct bch_sb *fs, struct bch_sb *sb)
{
	struct bch_sb *newest =
		le64_to_cpu(fs->seq) > le64_to_cpu(sb->seq) ? fs : sb;
	struct bch_sb_field_members *mi = bch2_sb_get_members(newest);

	if (uuid_le_cmp(fs->uuid, sb->uuid))
		return "device not a member of filesystem";

	if (sb->dev_idx >= newest->nr_devices)
		return "device has invalid dev_idx";

	if (bch2_is_zero(mi->members[sb->dev_idx].uuid.b, sizeof(uuid_le)))
		return "device has been removed";

	if (fs->block_size != sb->block_size)
		return "mismatched block size";

	return NULL;
}

/* Device startup/shutdown: */

static void bch2_dev_release(struct kobject *kobj)
{
	struct bch_dev *ca = container_of(kobj, struct bch_dev, kobj);

	kfree(ca);
}

static void bch2_dev_free(struct bch_dev *ca)
{
	unsigned i;

	cancel_work_sync(&ca->io_error_work);

	if (ca->kobj.state_in_sysfs &&
	    ca->disk_sb.bdev)
		sysfs_remove_link(&part_to_dev(ca->disk_sb.bdev->bd_part)->kobj,
				  "bcachefs");

	if (ca->kobj.state_in_sysfs)
		kobject_del(&ca->kobj);

	bch2_free_super(&ca->disk_sb);
	bch2_dev_journal_exit(ca);

	free_percpu(ca->sectors_written);
	bioset_exit(&ca->replica_set);
	free_percpu(ca->usage_percpu);
	free_pages((unsigned long) ca->disk_buckets, ilog2(bucket_pages(ca)));
	kfree(ca->prio_buckets);
	kfree(ca->bio_prio);
	vfree(ca->buckets);
	vfree(ca->oldest_gens);
	free_heap(&ca->heap);
	free_fifo(&ca->free_inc);

	for (i = 0; i < RESERVE_NR; i++)
		free_fifo(&ca->free[i]);

	percpu_ref_exit(&ca->io_ref);
	percpu_ref_exit(&ca->ref);
	kobject_put(&ca->kobj);
}

static void bch2_dev_io_ref_release(struct percpu_ref *ref)
{
	struct bch_dev *ca = container_of(ref, struct bch_dev, io_ref);

	complete(&ca->offline_complete);
}

static void __bch2_dev_offline(struct bch_dev *ca)
{
	struct bch_fs *c = ca->fs;

	lockdep_assert_held(&c->state_lock);

	__bch2_dev_read_only(ca->fs, ca);

	reinit_completion(&ca->offline_complete);
	percpu_ref_kill(&ca->io_ref);
	wait_for_completion(&ca->offline_complete);

	if (ca->kobj.state_in_sysfs) {
		struct kobject *block =
			&part_to_dev(ca->disk_sb.bdev->bd_part)->kobj;

		sysfs_remove_link(block, "bcachefs");
		sysfs_remove_link(&ca->kobj, "block");
	}

	bch2_free_super(&ca->disk_sb);
	bch2_dev_journal_exit(ca);
}

static void bch2_dev_ref_release(struct percpu_ref *ref)
{
	struct bch_dev *ca = container_of(ref, struct bch_dev, ref);

	complete(&ca->stop_complete);
}

static void bch2_dev_stop(struct bch_dev *ca)
{
	struct bch_fs *c = ca->fs;

	lockdep_assert_held(&c->state_lock);

	BUG_ON(rcu_access_pointer(c->devs[ca->dev_idx]) != ca);
	rcu_assign_pointer(c->devs[ca->dev_idx], NULL);

	synchronize_rcu();

	reinit_completion(&ca->stop_complete);
	percpu_ref_kill(&ca->ref);
	wait_for_completion(&ca->stop_complete);
}

static int bch2_dev_sysfs_online(struct bch_dev *ca)
{
	struct bch_fs *c = ca->fs;
	int ret;

	if (!c->kobj.state_in_sysfs)
		return 0;

	if (!ca->kobj.state_in_sysfs) {
		ret = kobject_add(&ca->kobj, &ca->fs->kobj,
				  "dev-%u", ca->dev_idx);
		if (ret)
			return ret;
	}

	if (ca->disk_sb.bdev) {
		struct kobject *block =
			&part_to_dev(ca->disk_sb.bdev->bd_part)->kobj;

		ret = sysfs_create_link(block, &ca->kobj, "bcachefs");
		if (ret)
			return ret;
		ret = sysfs_create_link(&ca->kobj, block, "block");
		if (ret)
			return ret;
	}

	return 0;
}

static int bch2_dev_alloc(struct bch_fs *c, unsigned dev_idx)
{
	struct bch_member *member;
	size_t reserve_none, movinggc_reserve, free_inc_reserve, total_reserve;
	size_t heap_size;
	unsigned i;
	struct bch_dev *ca;

	if (bch2_fs_init_fault("dev_alloc"))
		return -ENOMEM;

	ca = kzalloc(sizeof(*ca), GFP_KERNEL);
	if (!ca)
		return -ENOMEM;

	kobject_init(&ca->kobj, &bch2_dev_ktype);
	init_completion(&ca->stop_complete);
	init_completion(&ca->offline_complete);

	spin_lock_init(&ca->self.lock);
	ca->self.nr = 1;
	rcu_assign_pointer(ca->self.d[0].dev, ca);
	ca->dev_idx = dev_idx;

	spin_lock_init(&ca->freelist_lock);
	spin_lock_init(&ca->prio_buckets_lock);
	mutex_init(&ca->heap_lock);
	bch2_dev_moving_gc_init(ca);

	INIT_WORK(&ca->io_error_work, bch2_nonfatal_io_error_work);

	if (bch2_fs_init_fault("dev_alloc"))
		goto err;

	member = bch2_sb_get_members(c->disk_sb)->members + dev_idx;

	ca->mi = bch2_mi_to_cpu(member);
	ca->uuid = member->uuid;
	ca->bucket_bits = ilog2(ca->mi.bucket_size);
	scnprintf(ca->name, sizeof(ca->name), "dev-%u", dev_idx);

	/* XXX: tune these */
	movinggc_reserve = max_t(size_t, 16, ca->mi.nbuckets >> 7);
	reserve_none = max_t(size_t, 4, ca->mi.nbuckets >> 9);
	/*
	 * free_inc must be smaller than the copygc reserve: if it was bigger,
	 * one copygc iteration might not make enough buckets available to fill
	 * up free_inc and allow the allocator to make forward progress
	 */
	free_inc_reserve = movinggc_reserve / 2;
	heap_size = movinggc_reserve * 8;

	if (percpu_ref_init(&ca->ref, bch2_dev_ref_release,
			    0, GFP_KERNEL) ||
	    percpu_ref_init(&ca->io_ref, bch2_dev_io_ref_release,
			    PERCPU_REF_INIT_DEAD, GFP_KERNEL) ||
	    !init_fifo(&ca->free[RESERVE_PRIO], prio_buckets(ca), GFP_KERNEL) ||
	    !init_fifo(&ca->free[RESERVE_BTREE], BTREE_NODE_RESERVE, GFP_KERNEL) ||
	    !init_fifo(&ca->free[RESERVE_MOVINGGC],
		       movinggc_reserve, GFP_KERNEL) ||
	    !init_fifo(&ca->free[RESERVE_NONE], reserve_none, GFP_KERNEL) ||
	    !init_fifo(&ca->free_inc,	free_inc_reserve, GFP_KERNEL) ||
	    !init_heap(&ca->heap,	heap_size, GFP_KERNEL) ||
	    !(ca->oldest_gens	= vzalloc(sizeof(u8) *
					  ca->mi.nbuckets)) ||
	    !(ca->buckets	= vzalloc(sizeof(struct bucket) *
					  ca->mi.nbuckets)) ||
	    !(ca->prio_buckets	= kzalloc(sizeof(u64) * prio_buckets(ca) *
					  2, GFP_KERNEL)) ||
	    !(ca->disk_buckets	= alloc_bucket_pages(GFP_KERNEL, ca)) ||
	    !(ca->usage_percpu = alloc_percpu(struct bch_dev_usage)) ||
	    !(ca->bio_prio = bio_kmalloc(GFP_NOIO, bucket_pages(ca))) ||
	    bioset_init(&ca->replica_set, 4,
			offsetof(struct bch_write_bio, bio)) ||
	    !(ca->sectors_written = alloc_percpu(*ca->sectors_written)))
		goto err;

	ca->prio_last_buckets = ca->prio_buckets + prio_buckets(ca);

	total_reserve = ca->free_inc.size;
	for (i = 0; i < RESERVE_NR; i++)
		total_reserve += ca->free[i].size;

	ca->copygc_write_point.group = &ca->self;
	ca->tiering_write_point.group = &ca->self;

	ca->fs = c;
	rcu_assign_pointer(c->devs[ca->dev_idx], ca);

	if (bch2_dev_sysfs_online(ca))
		pr_warn("error creating sysfs objects");

	return 0;
err:
	bch2_dev_free(ca);
	return -ENOMEM;
}

static int __bch2_dev_online(struct bch_fs *c, struct bcache_superblock *sb)
{
	struct bch_dev *ca;
	int ret;

	lockdep_assert_held(&c->sb_lock);

	if (le64_to_cpu(sb->sb->seq) >
	    le64_to_cpu(c->disk_sb->seq))
		bch2_sb_to_fs(c, sb->sb);

	BUG_ON(sb->sb->dev_idx >= c->sb.nr_devices ||
	       !c->devs[sb->sb->dev_idx]);

	ca = c->devs[sb->sb->dev_idx];
	if (ca->disk_sb.bdev) {
		bch_err(c, "already have device online in slot %u",
			sb->sb->dev_idx);
		return -EINVAL;
	}

	ret = bch2_dev_journal_init(ca, sb->sb);
	if (ret)
		return ret;

	/*
	 * Increase journal write timeout if flushes to this device are
	 * expensive:
	 */
	if (!blk_queue_nonrot(bdev_get_queue(sb->bdev)) &&
	    journal_flushes_device(ca))
		c->journal.write_delay_ms =
			max(c->journal.write_delay_ms, 1000U);

	/* Commit: */
	ca->disk_sb = *sb;
	if (sb->mode & FMODE_EXCL)
		ca->disk_sb.bdev->bd_holder = ca;
	memset(sb, 0, sizeof(*sb));

	if (c->sb.nr_devices == 1)
		bdevname(ca->disk_sb.bdev, c->name);
	bdevname(ca->disk_sb.bdev, ca->name);

	if (bch2_dev_sysfs_online(ca))
		pr_warn("error creating sysfs objects");

	lg_local_lock(&c->usage_lock);
	if (!gc_will_visit(c, gc_phase(GC_PHASE_SB_METADATA)))
		bch2_mark_dev_metadata(ca->fs, ca);
	lg_local_unlock(&c->usage_lock);

	percpu_ref_reinit(&ca->io_ref);
	return 0;
}

/* Device management: */

bool bch2_fs_may_start(struct bch_fs *c, int flags)
{
	struct bch_sb_field_members *mi;
	unsigned meta_missing = 0;
	unsigned data_missing = 0;
	bool degraded = false;
	unsigned i;

	mutex_lock(&c->sb_lock);
	mi = bch2_sb_get_members(c->disk_sb);

	for (i = 0; i < c->disk_sb->nr_devices; i++)
		if (!c->devs[i] &&
		    !bch2_is_zero(mi->members[i].uuid.b, sizeof(uuid_le))) {
			degraded = true;
			if (BCH_MEMBER_HAS_METADATA(&mi->members[i]))
				meta_missing++;
			if (BCH_MEMBER_HAS_DATA(&mi->members[i]))
				data_missing++;
		}
	mutex_unlock(&c->sb_lock);

	if (degraded &&
	    !(flags & BCH_FORCE_IF_DEGRADED))
		return false;

	if (meta_missing &&
	    !(flags & BCH_FORCE_IF_METADATA_DEGRADED))
		return false;

	if (meta_missing >= BCH_SB_META_REPLICAS_HAVE(c->disk_sb) &&
	    !(flags & BCH_FORCE_IF_METADATA_LOST))
		return false;

	if (data_missing && !(flags & BCH_FORCE_IF_DATA_DEGRADED))
		return false;

	if (data_missing >= BCH_SB_DATA_REPLICAS_HAVE(c->disk_sb) &&
	    !(flags & BCH_FORCE_IF_DATA_LOST))
		return false;

	return true;
}

bool bch2_dev_state_allowed(struct bch_fs *c, struct bch_dev *ca,
			    enum bch_member_state new_state, int flags)
{
	lockdep_assert_held(&c->state_lock);

	if (new_state == BCH_MEMBER_STATE_RW)
		return true;

	if (ca->mi.has_data &&
	    !(flags & BCH_FORCE_IF_DATA_DEGRADED))
		return false;

	if (ca->mi.has_data &&
	    c->sb.data_replicas_have <= 1 &&
	    !(flags & BCH_FORCE_IF_DATA_LOST))
		return false;

	if (ca->mi.has_metadata &&
	    !(flags & BCH_FORCE_IF_METADATA_DEGRADED))
		return false;

	if (ca->mi.has_metadata &&
	    c->sb.meta_replicas_have <= 1 &&
	    !(flags & BCH_FORCE_IF_METADATA_LOST))
		return false;

	return true;
}

static void __bch2_dev_read_only(struct bch_fs *c, struct bch_dev *ca)
{
	bch2_moving_gc_stop(ca);

	/*
	 * This stops new data writes (e.g. to existing open data
	 * buckets) and then waits for all existing writes to
	 * complete.
	 */
	bch2_dev_allocator_stop(ca);

	bch2_dev_group_remove(&c->journal.devs, ca);
}

static const char *__bch2_dev_read_write(struct bch_fs *c, struct bch_dev *ca)
{
	lockdep_assert_held(&c->state_lock);

	BUG_ON(ca->mi.state != BCH_MEMBER_STATE_RW);

	if (bch2_dev_allocator_start(ca))
		return "error starting allocator thread";

	if (bch2_moving_gc_start(ca))
		return "error starting moving GC thread";

	if (bch2_tiering_start(c))
		return "error starting tiering thread";

	return NULL;
}

int __bch2_dev_set_state(struct bch_fs *c, struct bch_dev *ca,
			 enum bch_member_state new_state, int flags)
{
	struct bch_sb_field_members *mi;

	if (ca->mi.state == new_state)
		return 0;

	if (!bch2_dev_state_allowed(c, ca, new_state, flags))
		return -EINVAL;

	if (new_state == BCH_MEMBER_STATE_RW) {
		if (__bch2_dev_read_write(c, ca))
			return -ENOMEM;
	} else {
		__bch2_dev_read_only(c, ca);
	}

	bch_notice(ca, "%s", bch2_dev_state[new_state]);

	mutex_lock(&c->sb_lock);
	mi = bch2_sb_get_members(c->disk_sb);
	SET_BCH_MEMBER_STATE(&mi->members[ca->dev_idx], new_state);
	bch2_write_super(c);
	mutex_unlock(&c->sb_lock);

	return 0;
}

int bch2_dev_set_state(struct bch_fs *c, struct bch_dev *ca,
		       enum bch_member_state new_state, int flags)
{
	int ret;

	mutex_lock(&c->state_lock);
	ret = __bch2_dev_set_state(c, ca, new_state, flags);
	mutex_unlock(&c->state_lock);

	return ret;
}

/* Device add/removal: */

int bch2_dev_remove(struct bch_fs *c, struct bch_dev *ca, int flags)
{
	struct bch_sb_field_members *mi;
	unsigned dev_idx = ca->dev_idx;
	int ret = -EINVAL;

	mutex_lock(&c->state_lock);

	percpu_ref_put(&ca->ref); /* XXX */

	if (ca->mi.state == BCH_MEMBER_STATE_RW) {
		bch_err(ca, "Cannot remove RW device");
		goto err;
	}

	if (!bch2_dev_state_allowed(c, ca, BCH_MEMBER_STATE_FAILED, flags)) {
		bch_err(ca, "Cannot remove without losing data");
		goto err;
	}

	/*
	 * XXX: verify that dev_idx is really not in use anymore, anywhere
	 *
	 * flag_data_bad() does not check btree pointers
	 */
	ret = bch2_flag_data_bad(ca);
	if (ret) {
		bch_err(ca, "Remove failed");
		goto err;
	}

	if (ca->mi.has_data || ca->mi.has_metadata) {
		bch_err(ca, "Remove failed, still has data");
		goto err;
	}

	/*
	 * Ok, really doing the remove:
	 * Drop device's prio pointer before removing it from superblock:
	 */
	spin_lock(&c->journal.lock);
	c->journal.prio_buckets[dev_idx] = 0;
	spin_unlock(&c->journal.lock);

	bch2_journal_meta(&c->journal);

	__bch2_dev_offline(ca);
	bch2_dev_stop(ca);
	bch2_dev_free(ca);

	/*
	 * Free this device's slot in the bch_member array - all pointers to
	 * this device must be gone:
	 */
	mutex_lock(&c->sb_lock);
	mi = bch2_sb_get_members(c->disk_sb);
	memset(&mi->members[dev_idx].uuid, 0, sizeof(mi->members[dev_idx].uuid));

	bch2_write_super(c);

	mutex_unlock(&c->sb_lock);
	mutex_unlock(&c->state_lock);
	return 0;
err:
	mutex_unlock(&c->state_lock);
	return ret;
}

int bch2_dev_add(struct bch_fs *c, const char *path)
{
	struct bcache_superblock sb;
	const char *err;
	struct bch_dev *ca = NULL;
	struct bch_sb_field_members *mi, *dev_mi;
	struct bch_member saved_mi;
	unsigned dev_idx, nr_devices, u64s;
	int ret = -EINVAL;

	err = bch2_read_super(&sb, bch2_opts_empty(), path);
	if (err)
		return -EINVAL;

	err = bch2_validate_cache_super(&sb);
	if (err)
		return -EINVAL;

	err = bch2_dev_may_add(sb.sb, c);
	if (err)
		return -EINVAL;

	mutex_lock(&c->state_lock);
	mutex_lock(&c->sb_lock);

	/*
	 * Preserve the old cache member information (esp. tier)
	 * before we start bashing the disk stuff.
	 */
	dev_mi = bch2_sb_get_members(sb.sb);
	saved_mi = dev_mi->members[sb.sb->dev_idx];
	saved_mi.last_mount = cpu_to_le64(ktime_get_seconds());

	if (dynamic_fault("bcachefs:add:no_slot"))
		goto no_slot;

	mi = bch2_sb_get_members(c->disk_sb);
	for (dev_idx = 0; dev_idx < BCH_SB_MEMBERS_MAX; dev_idx++)
		if (dev_idx >= c->sb.nr_devices ||
		    bch2_is_zero(mi->members[dev_idx].uuid.b,
				 sizeof(uuid_le)))
			goto have_slot;
no_slot:
	err = "no slots available in superblock";
	ret = -ENOSPC;
	goto err_unlock;

have_slot:
	nr_devices = max_t(unsigned, dev_idx + 1, c->sb.nr_devices);
	u64s = (sizeof(struct bch_sb_field_members) +
		sizeof(struct bch_member) * nr_devices) / sizeof(u64);
	err = "no space in superblock for member info";

	mi = bch2_fs_sb_resize_members(c, u64s);
	if (!mi)
		goto err_unlock;

	dev_mi = bch2_sb_resize_members(&sb, u64s);
	if (!dev_mi)
		goto err_unlock;

	memcpy(dev_mi, mi, u64s * sizeof(u64));
	dev_mi->members[dev_idx] = saved_mi;

	sb.sb->uuid		= c->disk_sb->uuid;
	sb.sb->dev_idx		= dev_idx;
	sb.sb->nr_devices	= nr_devices;

	/* commit new member info */
	memcpy(mi, dev_mi, u64s * sizeof(u64));
	c->disk_sb->nr_devices	= nr_devices;
	c->sb.nr_devices	= nr_devices;

	if (bch2_dev_alloc(c, dev_idx)) {
		err = "cannot allocate memory";
		ret = -ENOMEM;
		goto err_unlock;
	}

	if (__bch2_dev_online(c, &sb)) {
		err = "bch2_dev_online() error";
		ret = -ENOMEM;
		goto err_unlock;
	}

	bch2_write_super(c);
	mutex_unlock(&c->sb_lock);

	ca = c->devs[dev_idx];
	if (ca->mi.state == BCH_MEMBER_STATE_RW) {
		err = "journal alloc failed";
		if (bch2_dev_journal_alloc(ca))
			goto err;

		err = __bch2_dev_read_write(c, ca);
		if (err)
			goto err;
	}

	mutex_unlock(&c->state_lock);
	return 0;
err_unlock:
	mutex_unlock(&c->sb_lock);
err:
	mutex_unlock(&c->state_lock);
	bch2_free_super(&sb);

	bch_err(c, "Unable to add device: %s", err);
	return ret ?: -EINVAL;
}

int bch2_dev_online(struct bch_fs *c, const char *path)
{
	struct bcache_superblock sb = { 0 };
	struct bch_dev *ca;
	unsigned dev_idx;
	const char *err;
	int ret;

	mutex_lock(&c->state_lock);

	err = bch2_read_super(&sb, bch2_opts_empty(), path);
	if (err)
		goto err;

	dev_idx = sb.sb->dev_idx;

	err = bch2_dev_in_fs(c->disk_sb, sb.sb);
	if (err)
		goto err;

	mutex_lock(&c->sb_lock);
	if (__bch2_dev_online(c, &sb)) {
		err = "__bch2_dev_online() error";
		mutex_unlock(&c->sb_lock);
		goto err;
	}
	mutex_unlock(&c->sb_lock);

	ca = c->devs[dev_idx];
	ret = bch2_prio_read(ca);
	if (ret) {
		err = "error reading priorities";
		goto err;
	}

	if (ca->mi.state == BCH_MEMBER_STATE_RW) {
		err = __bch2_dev_read_write(c, ca);
		if (err)
			goto err;
	}

	mutex_unlock(&c->state_lock);
	return 0;
err:
	mutex_unlock(&c->state_lock);
	bch2_free_super(&sb);
	bch_err(c, "error bringing %s online: %s", path, err);
	return -EINVAL;
}

int bch2_dev_offline(struct bch_fs *c, struct bch_dev *ca, int flags)
{
	mutex_lock(&c->state_lock);

	if (!bch2_dev_state_allowed(c, ca, BCH_MEMBER_STATE_FAILED, flags)) {
		bch_err(ca, "Cannot offline required disk");
		mutex_unlock(&c->state_lock);
		return -EINVAL;
	}

	__bch2_dev_read_only(c, ca);
	__bch2_dev_offline(ca);

	mutex_unlock(&c->state_lock);
	return 0;
}

int bch2_dev_evacuate(struct bch_fs *c, struct bch_dev *ca)
{
	int ret;

	mutex_lock(&c->state_lock);

	if (ca->mi.state == BCH_MEMBER_STATE_RW) {
		bch_err(ca, "Cannot migrate data off RW device");
		mutex_unlock(&c->state_lock);
		return -EINVAL;
	}

	mutex_unlock(&c->state_lock);

	ret = bch2_move_data_off_device(ca);
	if (ret) {
		bch_err(ca, "Error migrating data: %i", ret);
		return ret;
	}

	ret = bch2_move_metadata_off_device(ca);
	if (ret) {
		bch_err(ca, "Error migrating metadata: %i", ret);
		return ret;
	}

	if (ca->mi.has_data || ca->mi.has_metadata) {
		bch_err(ca, "Migrate error: data still present");
		return -EINVAL;
	}

	return 0;
}

/* Filesystem open: */

const char *bch2_fs_open(char * const *devices, unsigned nr_devices,
			 struct bch_opts opts, struct bch_fs **ret)
{
	const char *err;
	struct bch_fs *c = NULL;
	struct bcache_superblock *sb;
	unsigned i, best_sb = 0;

	if (!nr_devices)
		return "need at least one device";

	if (!try_module_get(THIS_MODULE))
		return "module unloading";

	err = "cannot allocate memory";
	sb = kcalloc(nr_devices, sizeof(*sb), GFP_KERNEL);
	if (!sb)
		goto err;

	for (i = 0; i < nr_devices; i++) {
		err = bch2_read_super(&sb[i], opts, devices[i]);
		if (err)
			goto err;

		err = "attempting to register backing device";
		if (__SB_IS_BDEV(le64_to_cpu(sb[i].sb->version)))
			goto err;

		err = bch2_validate_cache_super(&sb[i]);
		if (err)
			goto err;
	}

	for (i = 1; i < nr_devices; i++)
		if (le64_to_cpu(sb[i].sb->seq) >
		    le64_to_cpu(sb[best_sb].sb->seq))
			best_sb = i;

	for (i = 0; i < nr_devices; i++) {
		err = bch2_dev_in_fs(sb[best_sb].sb, sb[i].sb);
		if (err)
			goto err;
	}

	err = "cannot allocate memory";
	c = bch2_fs_alloc(sb[best_sb].sb, opts);
	if (!c)
		goto err;

	err = "bch2_dev_online() error";
	mutex_lock(&c->sb_lock);
	for (i = 0; i < nr_devices; i++)
		if (__bch2_dev_online(c, &sb[i])) {
			mutex_unlock(&c->sb_lock);
			goto err;
		}
	mutex_unlock(&c->sb_lock);

	err = "insufficient devices";
	if (!bch2_fs_may_start(c, 0))
		goto err;

	if (!c->opts.nostart) {
		err = __bch2_fs_start(c);
		if (err)
			goto err;
	}

	err = bch2_fs_online(c);
	if (err)
		goto err;

	if (ret)
		*ret = c;
	else
		closure_put(&c->cl);

	err = NULL;
out:
	kfree(sb);
	module_put(THIS_MODULE);
	if (err)
		c = NULL;
	return err;
err:
	if (c)
		bch2_fs_stop(c);

	for (i = 0; i < nr_devices; i++)
		bch2_free_super(&sb[i]);
	goto out;
}

static const char *__bch2_fs_open_incremental(struct bcache_superblock *sb,
					      struct bch_opts opts)
{
	const char *err;
	struct bch_fs *c;
	bool allocated_fs = false;

	err = bch2_validate_cache_super(sb);
	if (err)
		return err;

	mutex_lock(&bch_fs_list_lock);
	c = __bch2_uuid_to_fs(sb->sb->uuid);
	if (c) {
		closure_get(&c->cl);

		err = bch2_dev_in_fs(c->disk_sb, sb->sb);
		if (err)
			goto err;
	} else {
		c = bch2_fs_alloc(sb->sb, opts);
		err = "cannot allocate memory";
		if (!c)
			goto err;

		allocated_fs = true;
	}

	err = "bch2_dev_online() error";

	mutex_lock(&c->sb_lock);
	if (__bch2_dev_online(c, sb)) {
		mutex_unlock(&c->sb_lock);
		goto err;
	}
	mutex_unlock(&c->sb_lock);

	if (!c->opts.nostart && bch2_fs_may_start(c, 0)) {
		err = __bch2_fs_start(c);
		if (err)
			goto err;
	}

	err = __bch2_fs_online(c);
	if (err)
		goto err;

	closure_put(&c->cl);
	mutex_unlock(&bch_fs_list_lock);

	return NULL;
err:
	mutex_unlock(&bch_fs_list_lock);

	if (allocated_fs)
		bch2_fs_stop(c);
	else if (c)
		closure_put(&c->cl);

	return err;
}

const char *bch2_fs_open_incremental(const char *path)
{
	struct bcache_superblock sb;
	struct bch_opts opts = bch2_opts_empty();
	const char *err;

	err = bch2_read_super(&sb, opts, path);
	if (err)
		return err;

	if (!__SB_IS_BDEV(le64_to_cpu(sb.sb->version)))
		err = __bch2_fs_open_incremental(&sb, opts);
	else
		err = "not a bcachefs superblock";

	bch2_free_super(&sb);

	return err;
}

/* Global interfaces/init */

static void bcachefs_exit(void)
{
	bch2_debug_exit();
	bch2_vfs_exit();
	bch2_chardev_exit();
	if (bcachefs_kset)
		kset_unregister(bcachefs_kset);
}

static int __init bcachefs_init(void)
{
	bch2_bkey_pack_test();

	if (!(bcachefs_kset = kset_create_and_add("bcachefs", NULL, fs_kobj)) ||
	    bch2_chardev_init() ||
	    bch2_vfs_init() ||
	    bch2_debug_init())
		goto err;

	return 0;
err:
	bcachefs_exit();
	return -ENOMEM;
}

#define BCH_DEBUG_PARAM(name, description)			\
	bool bch2_##name;					\
	module_param_named(name, bch2_##name, bool, 0644);	\
	MODULE_PARM_DESC(name, description);
BCH_DEBUG_PARAMS()
#undef BCH_DEBUG_PARAM

module_exit(bcachefs_exit);
module_init(bcachefs_init);
