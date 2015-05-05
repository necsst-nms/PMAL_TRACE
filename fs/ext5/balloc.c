/*
 *  linux/fs/ext5/balloc.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  Enhanced block allocation by Stephen Tweedie (sct@redhat.com), 1993
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 */

#include <linux/time.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/quotaops.h>
#include <linux/buffer_head.h>
#include "ext5.h"
#include "ext5_jbd2.h"
#include "mballoc.h"

#include <trace/events/ext5.h>

static unsigned ext5_num_base_meta_clusters(struct super_block *sb,
					    ext5_group_t block_group);
/*
 * balloc.c contains the blocks allocation and deallocation routines
 */

/*
 * Calculate block group number for a given block number
 */
ext5_group_t ext5_get_group_number(struct super_block *sb,
				   ext5_fsblk_t block)
{
	ext5_group_t group;

	if (test_opt2(sb, STD_GROUP_SIZE))
		group = (block -
			 le32_to_cpu(EXT5_SB(sb)->s_es->s_first_data_block)) >>
			(EXT5_BLOCK_SIZE_BITS(sb) + EXT5_CLUSTER_BITS(sb) + 3);
	else
		ext5_get_group_no_and_offset(sb, block, &group, NULL);
	return group;
}

/*
 * Calculate the block group number and offset into the block/cluster
 * allocation bitmap, given a block number
 */
void ext5_get_group_no_and_offset(struct super_block *sb, ext5_fsblk_t blocknr,
		ext5_group_t *blockgrpp, ext5_grpblk_t *offsetp)
{
	struct ext5_super_block *es = EXT5_SB(sb)->s_es;
	ext5_grpblk_t offset;

	blocknr = blocknr - le32_to_cpu(es->s_first_data_block);
	offset = do_div(blocknr, EXT5_BLOCKS_PER_GROUP(sb)) >>
		EXT5_SB(sb)->s_cluster_bits;
	if (offsetp)
		*offsetp = offset;
	if (blockgrpp)
		*blockgrpp = blocknr;

}

/*
 * Check whether the 'block' lives within the 'block_group'. Returns 1 if so
 * and 0 otherwise.
 */
static inline int ext5_block_in_group(struct super_block *sb,
				      ext5_fsblk_t block,
				      ext5_group_t block_group)
{
	ext5_group_t actual_group;

	actual_group = ext5_get_group_number(sb, block);
	return (actual_group == block_group) ? 1 : 0;
}

/* Return the number of clusters used for file system metadata; this
 * represents the overhead needed by the file system.
 */
unsigned ext5_num_overhead_clusters(struct super_block *sb,
				    ext5_group_t block_group,
				    struct ext5_group_desc *gdp)
{
	unsigned num_clusters;
	int block_cluster = -1, inode_cluster = -1, itbl_cluster = -1, i, c;
	ext5_fsblk_t start = ext5_group_first_block_no(sb, block_group);
	ext5_fsblk_t itbl_blk;
	struct ext5_sb_info *sbi = EXT5_SB(sb);

	/* This is the number of clusters used by the superblock,
	 * block group descriptors, and reserved block group
	 * descriptor blocks */
	num_clusters = ext5_num_base_meta_clusters(sb, block_group);

	/*
	 * For the allocation bitmaps and inode table, we first need
	 * to check to see if the block is in the block group.  If it
	 * is, then check to see if the cluster is already accounted
	 * for in the clusters used for the base metadata cluster, or
	 * if we can increment the base metadata cluster to include
	 * that block.  Otherwise, we will have to track the cluster
	 * used for the allocation bitmap or inode table explicitly.
	 * Normally all of these blocks are contiguous, so the special
	 * case handling shouldn't be necessary except for *very*
	 * unusual file system layouts.
	 */
	if (ext5_block_in_group(sb, ext5_block_bitmap(sb, gdp), block_group)) {
		block_cluster = EXT5_B2C(sbi,
					 ext5_block_bitmap(sb, gdp) - start);
		if (block_cluster < num_clusters)
			block_cluster = -1;
		else if (block_cluster == num_clusters) {
			num_clusters++;
			block_cluster = -1;
		}
	}

	if (ext5_block_in_group(sb, ext5_inode_bitmap(sb, gdp), block_group)) {
		inode_cluster = EXT5_B2C(sbi,
					 ext5_inode_bitmap(sb, gdp) - start);
		if (inode_cluster < num_clusters)
			inode_cluster = -1;
		else if (inode_cluster == num_clusters) {
			num_clusters++;
			inode_cluster = -1;
		}
	}

	itbl_blk = ext5_inode_table(sb, gdp);
	for (i = 0; i < sbi->s_itb_per_group; i++) {
		if (ext5_block_in_group(sb, itbl_blk + i, block_group)) {
			c = EXT5_B2C(sbi, itbl_blk + i - start);
			if ((c < num_clusters) || (c == inode_cluster) ||
			    (c == block_cluster) || (c == itbl_cluster))
				continue;
			if (c == num_clusters) {
				num_clusters++;
				continue;
			}
			num_clusters++;
			itbl_cluster = c;
		}
	}

	if (block_cluster != -1)
		num_clusters++;
	if (inode_cluster != -1)
		num_clusters++;

	return num_clusters;
}

static unsigned int num_clusters_in_group(struct super_block *sb,
					  ext5_group_t block_group)
{
	unsigned int blocks;

	if (block_group == ext5_get_groups_count(sb) - 1) {
		/*
		 * Even though mke2fs always initializes the first and
		 * last group, just in case some other tool was used,
		 * we need to make sure we calculate the right free
		 * blocks.
		 */
		blocks = ext5_blocks_count(EXT5_SB(sb)->s_es) -
			ext5_group_first_block_no(sb, block_group);
	} else
		blocks = EXT5_BLOCKS_PER_GROUP(sb);
	return EXT5_NUM_B2C(EXT5_SB(sb), blocks);
}

/* Initializes an uninitialized block bitmap */
void ext5_init_block_bitmap(struct super_block *sb, struct buffer_head *bh,
			    ext5_group_t block_group,
			    struct ext5_group_desc *gdp)
{
	unsigned int bit, bit_max;
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	ext5_fsblk_t start, tmp;
	int flex_bg = 0;

	J_ASSERT_BH(bh, buffer_locked(bh));

	/* If checksum is bad mark all blocks used to prevent allocation
	 * essentially implementing a per-group read-only flag. */
	if (!ext5_group_desc_csum_verify(sb, block_group, gdp)) {
		ext5_error(sb, "Checksum bad for group %u", block_group);
		ext5_free_group_clusters_set(sb, gdp, 0);
		ext5_free_inodes_set(sb, gdp, 0);
		ext5_itable_unused_set(sb, gdp, 0);
		memset(bh->b_data, 0xff, sb->s_blocksize);
		ext5_block_bitmap_csum_set(sb, block_group, gdp, bh);
		return;
	}
	memset(bh->b_data, 0, sb->s_blocksize);

	bit_max = ext5_num_base_meta_clusters(sb, block_group);
	for (bit = 0; bit < bit_max; bit++)
		ext5_set_bit(bit, bh->b_data);

	start = ext5_group_first_block_no(sb, block_group);

	if (EXT5_HAS_INCOMPAT_FEATURE(sb, EXT5_FEATURE_INCOMPAT_FLEX_BG))
		flex_bg = 1;

	/* Set bits for block and inode bitmaps, and inode table */
	tmp = ext5_block_bitmap(sb, gdp);
	if (!flex_bg || ext5_block_in_group(sb, tmp, block_group))
		ext5_set_bit(EXT5_B2C(sbi, tmp - start), bh->b_data);

	tmp = ext5_inode_bitmap(sb, gdp);
	if (!flex_bg || ext5_block_in_group(sb, tmp, block_group))
		ext5_set_bit(EXT5_B2C(sbi, tmp - start), bh->b_data);

	tmp = ext5_inode_table(sb, gdp);
	for (; tmp < ext5_inode_table(sb, gdp) +
		     sbi->s_itb_per_group; tmp++) {
		if (!flex_bg || ext5_block_in_group(sb, tmp, block_group))
			ext5_set_bit(EXT5_B2C(sbi, tmp - start), bh->b_data);
	}

	/*
	 * Also if the number of blocks within the group is less than
	 * the blocksize * 8 ( which is the size of bitmap ), set rest
	 * of the block bitmap to 1
	 */
	ext5_mark_bitmap_end(num_clusters_in_group(sb, block_group),
			     sb->s_blocksize * 8, bh->b_data);
	ext5_block_bitmap_csum_set(sb, block_group, gdp, bh);
	ext5_group_desc_csum_set(sb, block_group, gdp);
}

/* Return the number of free blocks in a block group.  It is used when
 * the block bitmap is uninitialized, so we can't just count the bits
 * in the bitmap. */
unsigned ext5_free_clusters_after_init(struct super_block *sb,
				       ext5_group_t block_group,
				       struct ext5_group_desc *gdp)
{
	return num_clusters_in_group(sb, block_group) - 
		ext5_num_overhead_clusters(sb, block_group, gdp);
}

/*
 * The free blocks are managed by bitmaps.  A file system contains several
 * blocks groups.  Each group contains 1 bitmap block for blocks, 1 bitmap
 * block for inodes, N blocks for the inode table and data blocks.
 *
 * The file system contains group descriptors which are located after the
 * super block.  Each descriptor contains the number of the bitmap block and
 * the free blocks count in the block.  The descriptors are loaded in memory
 * when a file system is mounted (see ext5_fill_super).
 */

/**
 * ext5_get_group_desc() -- load group descriptor from disk
 * @sb:			super block
 * @block_group:	given block group
 * @bh:			pointer to the buffer head to store the block
 *			group descriptor
 */
struct ext5_group_desc * ext5_get_group_desc(struct super_block *sb,
					     ext5_group_t block_group,
					     struct buffer_head **bh)
{
	unsigned int group_desc;
	unsigned int offset;
	ext5_group_t ngroups = ext5_get_groups_count(sb);
	struct ext5_group_desc *desc;
	struct ext5_sb_info *sbi = EXT5_SB(sb);

	if (block_group >= ngroups) {
		ext5_error(sb, "block_group >= groups_count - block_group = %u,"
			   " groups_count = %u", block_group, ngroups);

		return NULL;
	}

	group_desc = block_group >> EXT5_DESC_PER_BLOCK_BITS(sb);
	offset = block_group & (EXT5_DESC_PER_BLOCK(sb) - 1);
	if (!sbi->s_group_desc[group_desc]) {
		ext5_error(sb, "Group descriptor not loaded - "
			   "block_group = %u, group_desc = %u, desc = %u",
			   block_group, group_desc, offset);
		return NULL;
	}

	desc = (struct ext5_group_desc *)(
		(__u8 *)sbi->s_group_desc[group_desc]->b_data +
		offset * EXT5_DESC_SIZE(sb));
	if (bh)
		*bh = sbi->s_group_desc[group_desc];
	return desc;
}

/*
 * Return the block number which was discovered to be invalid, or 0 if
 * the block bitmap is valid.
 */
static ext5_fsblk_t ext5_valid_block_bitmap(struct super_block *sb,
					    struct ext5_group_desc *desc,
					    unsigned int block_group,
					    struct buffer_head *bh)
{
	ext5_grpblk_t offset;
	ext5_grpblk_t next_zero_bit;
	ext5_fsblk_t blk;
	ext5_fsblk_t group_first_block;

	if (EXT5_HAS_INCOMPAT_FEATURE(sb, EXT5_FEATURE_INCOMPAT_FLEX_BG)) {
		/* with FLEX_BG, the inode/block bitmaps and itable
		 * blocks may not be in the group at all
		 * so the bitmap validation will be skipped for those groups
		 * or it has to also read the block group where the bitmaps
		 * are located to verify they are set.
		 */
		return 0;
	}
	group_first_block = ext5_group_first_block_no(sb, block_group);

	/* check whether block bitmap block number is set */
	blk = ext5_block_bitmap(sb, desc);
	offset = blk - group_first_block;
	if (!ext5_test_bit(offset, bh->b_data))
		/* bad block bitmap */
		return blk;

	/* check whether the inode bitmap block number is set */
	blk = ext5_inode_bitmap(sb, desc);
	offset = blk - group_first_block;
	if (!ext5_test_bit(offset, bh->b_data))
		/* bad block bitmap */
		return blk;

	/* check whether the inode table block number is set */
	blk = ext5_inode_table(sb, desc);
	offset = blk - group_first_block;
	next_zero_bit = ext5_find_next_zero_bit(bh->b_data,
				offset + EXT5_SB(sb)->s_itb_per_group,
				offset);
	if (next_zero_bit < offset + EXT5_SB(sb)->s_itb_per_group)
		/* bad bitmap for inode tables */
		return blk;
	return 0;
}

void ext5_validate_block_bitmap(struct super_block *sb,
			       struct ext5_group_desc *desc,
			       unsigned int block_group,
			       struct buffer_head *bh)
{
	ext5_fsblk_t	blk;

	if (buffer_verified(bh))
		return;

	ext5_lock_group(sb, block_group);
	blk = ext5_valid_block_bitmap(sb, desc, block_group, bh);
	if (unlikely(blk != 0)) {
		ext5_unlock_group(sb, block_group);
		ext5_error(sb, "bg %u: block %llu: invalid block bitmap",
			   block_group, blk);
		return;
	}
	if (unlikely(!ext5_block_bitmap_csum_verify(sb, block_group,
			desc, bh))) {
		ext5_unlock_group(sb, block_group);
		ext5_error(sb, "bg %u: bad block bitmap checksum", block_group);
		return;
	}
	set_buffer_verified(bh);
	ext5_unlock_group(sb, block_group);
}

/**
 * ext5_read_block_bitmap_nowait()
 * @sb:			super block
 * @block_group:	given block group
 *
 * Read the bitmap for a given block_group,and validate the
 * bits for block/inode/inode tables are set in the bitmaps
 *
 * Return buffer_head on success or NULL in case of failure.
 */
struct buffer_head *
ext5_read_block_bitmap_nowait(struct super_block *sb, ext5_group_t block_group)
{
	struct ext5_group_desc *desc;
	struct buffer_head *bh;
	ext5_fsblk_t bitmap_blk;

	desc = ext5_get_group_desc(sb, block_group, NULL);
	if (!desc)
		return NULL;
	bitmap_blk = ext5_block_bitmap(sb, desc);
	bh = sb_getblk(sb, bitmap_blk);
	if (unlikely(!bh)) {
		ext5_error(sb, "Cannot get buffer for block bitmap - "
			   "block_group = %u, block_bitmap = %llu",
			   block_group, bitmap_blk);
		return NULL;
	}

	if (bitmap_uptodate(bh))
		goto verify;

	lock_buffer(bh);
	if (bitmap_uptodate(bh)) {
		unlock_buffer(bh);
		goto verify;
	}
	ext5_lock_group(sb, block_group);
	if (desc->bg_flags & cpu_to_le16(EXT5_BG_BLOCK_UNINIT)) {
		ext5_init_block_bitmap(sb, bh, block_group, desc);
		set_bitmap_uptodate(bh);
		set_buffer_uptodate(bh);
		ext5_unlock_group(sb, block_group);
		unlock_buffer(bh);
		return bh;
	}
	ext5_unlock_group(sb, block_group);
	if (buffer_uptodate(bh)) {
		/*
		 * if not uninit if bh is uptodate,
		 * bitmap is also uptodate
		 */
		set_bitmap_uptodate(bh);
		unlock_buffer(bh);
		goto verify;
	}
	/*
	 * submit the buffer_head for reading
	 */
	set_buffer_new(bh);
	trace_ext5_read_block_bitmap_load(sb, block_group);
	bh->b_end_io = ext5_end_bitmap_read;
	get_bh(bh);
	submit_bh(READ | REQ_META | REQ_PRIO, bh);
	return bh;
verify:
	ext5_validate_block_bitmap(sb, desc, block_group, bh);
	return bh;
}

/* Returns 0 on success, 1 on error */
int ext5_wait_block_bitmap(struct super_block *sb, ext5_group_t block_group,
			   struct buffer_head *bh)
{
	struct ext5_group_desc *desc;

	if (!buffer_new(bh))
		return 0;
	desc = ext5_get_group_desc(sb, block_group, NULL);
	if (!desc)
		return 1;
	wait_on_buffer(bh);
	if (!buffer_uptodate(bh)) {
		ext5_error(sb, "Cannot read block bitmap - "
			   "block_group = %u, block_bitmap = %llu",
			   block_group, (unsigned long long) bh->b_blocknr);
		return 1;
	}
	clear_buffer_new(bh);
	/* Panic or remount fs read-only if block bitmap is invalid */
	ext5_validate_block_bitmap(sb, desc, block_group, bh);
	return 0;
}

struct buffer_head *
ext5_read_block_bitmap(struct super_block *sb, ext5_group_t block_group)
{
	struct buffer_head *bh;

	bh = ext5_read_block_bitmap_nowait(sb, block_group);
	if (!bh)
		return NULL;
	if (ext5_wait_block_bitmap(sb, block_group, bh)) {
		put_bh(bh);
		return NULL;
	}
	return bh;
}

/**
 * ext5_has_free_clusters()
 * @sbi:	in-core super block structure.
 * @nclusters:	number of needed blocks
 * @flags:	flags from ext5_mb_new_blocks()
 *
 * Check if filesystem has nclusters free & available for allocation.
 * On success return 1, return 0 on failure.
 */
static int ext5_has_free_clusters(struct ext5_sb_info *sbi,
				  s64 nclusters, unsigned int flags)
{
	s64 free_clusters, dirty_clusters, rsv, resv_clusters;
	struct percpu_counter *fcc = &sbi->s_freeclusters_counter;
	struct percpu_counter *dcc = &sbi->s_dirtyclusters_counter;

	free_clusters  = percpu_counter_read_positive(fcc);
	dirty_clusters = percpu_counter_read_positive(dcc);
	resv_clusters = atomic64_read(&sbi->s_resv_clusters);

	/*
	 * r_blocks_count should always be multiple of the cluster ratio so
	 * we are safe to do a plane bit shift only.
	 */
	rsv = (ext5_r_blocks_count(sbi->s_es) >> sbi->s_cluster_bits) +
	      resv_clusters;

	if (free_clusters - (nclusters + rsv + dirty_clusters) <
					EXT5_FREECLUSTERS_WATERMARK) {
		free_clusters  = percpu_counter_sum_positive(fcc);
		dirty_clusters = percpu_counter_sum_positive(dcc);
	}
	/* Check whether we have space after accounting for current
	 * dirty clusters & root reserved clusters.
	 */
	if (free_clusters >= (rsv + nclusters + dirty_clusters))
		return 1;

	/* Hm, nope.  Are (enough) root reserved clusters available? */
	if (uid_eq(sbi->s_resuid, current_fsuid()) ||
	    (!gid_eq(sbi->s_resgid, GLOBAL_ROOT_GID) && in_group_p(sbi->s_resgid)) ||
	    capable(CAP_SYS_RESOURCE) ||
	    (flags & EXT5_MB_USE_ROOT_BLOCKS)) {

		if (free_clusters >= (nclusters + dirty_clusters +
				      resv_clusters))
			return 1;
	}
	/* No free blocks. Let's see if we can dip into reserved pool */
	if (flags & EXT5_MB_USE_RESERVED) {
		if (free_clusters >= (nclusters + dirty_clusters))
			return 1;
	}

	return 0;
}

int ext5_claim_free_clusters(struct ext5_sb_info *sbi,
			     s64 nclusters, unsigned int flags)
{
	if (ext5_has_free_clusters(sbi, nclusters, flags)) {
		percpu_counter_add(&sbi->s_dirtyclusters_counter, nclusters);
		return 0;
	} else
		return -ENOSPC;
}

/**
 * ext5_should_retry_alloc()
 * @sb:			super block
 * @retries		number of attemps has been made
 *
 * ext5_should_retry_alloc() is called when ENOSPC is returned, and if
 * it is profitable to retry the operation, this function will wait
 * for the current or committing transaction to complete, and then
 * return TRUE.
 *
 * if the total number of retries exceed three times, return FALSE.
 */
int ext5_should_retry_alloc(struct super_block *sb, int *retries)
{
	if (!ext5_has_free_clusters(EXT5_SB(sb), 1, 0) ||
	    (*retries)++ > 3 ||
	    !EXT5_SB(sb)->s_journal)
		return 0;

	jbd_debug(1, "%s: retrying operation after ENOSPC\n", sb->s_id);

	return jbd2_journal_force_commit_nested(EXT5_SB(sb)->s_journal);
}

/*
 * ext5_new_meta_blocks() -- allocate block for meta data (indexing) blocks
 *
 * @handle:             handle to this transaction
 * @inode:              file inode
 * @goal:               given target block(filesystem wide)
 * @count:		pointer to total number of clusters needed
 * @errp:               error code
 *
 * Return 1st allocated block number on success, *count stores total account
 * error stores in errp pointer
 */
ext5_fsblk_t ext5_new_meta_blocks(handle_t *handle, struct inode *inode,
				  ext5_fsblk_t goal, unsigned int flags,
				  unsigned long *count, int *errp)
{
	struct ext5_allocation_request ar;
	ext5_fsblk_t ret;

	memset(&ar, 0, sizeof(ar));
	/* Fill with neighbour allocated blocks */
	ar.inode = inode;
	ar.goal = goal;
	ar.len = count ? *count : 1;
	ar.flags = flags;

	ret = ext5_mb_new_blocks(handle, &ar, errp);
	if (count)
		*count = ar.len;
	/*
	 * Account for the allocated meta blocks.  We will never
	 * fail EDQUOT for metdata, but we do account for it.
	 */
	if (!(*errp) &&
	    ext5_test_inode_state(inode, EXT5_STATE_DELALLOC_RESERVED)) {
		spin_lock(&EXT5_I(inode)->i_block_reservation_lock);
		EXT5_I(inode)->i_allocated_meta_blocks += ar.len;
		spin_unlock(&EXT5_I(inode)->i_block_reservation_lock);
		dquot_alloc_block_nofail(inode,
				EXT5_C2B(EXT5_SB(inode->i_sb), ar.len));
	}
	return ret;
}

/**
 * ext5_count_free_clusters() -- count filesystem free clusters
 * @sb:		superblock
 *
 * Adds up the number of free clusters from each block group.
 */
ext5_fsblk_t ext5_count_free_clusters(struct super_block *sb)
{
	ext5_fsblk_t desc_count;
	struct ext5_group_desc *gdp;
	ext5_group_t i;
	ext5_group_t ngroups = ext5_get_groups_count(sb);
#ifdef EXT5FS_DEBUG
	struct ext5_super_block *es;
	ext5_fsblk_t bitmap_count;
	unsigned int x;
	struct buffer_head *bitmap_bh = NULL;

	es = EXT5_SB(sb)->s_es;
	desc_count = 0;
	bitmap_count = 0;
	gdp = NULL;

	for (i = 0; i < ngroups; i++) {
		gdp = ext5_get_group_desc(sb, i, NULL);
		if (!gdp)
			continue;
		desc_count += ext5_free_group_clusters(sb, gdp);
		brelse(bitmap_bh);
		bitmap_bh = ext5_read_block_bitmap(sb, i);
		if (bitmap_bh == NULL)
			continue;

		x = ext5_count_free(bitmap_bh->b_data,
				    EXT5_BLOCKS_PER_GROUP(sb) / 8);
		printk(KERN_DEBUG "group %u: stored = %d, counted = %u\n",
			i, ext5_free_group_clusters(sb, gdp), x);
		bitmap_count += x;
	}
	brelse(bitmap_bh);
	printk(KERN_DEBUG "ext5_count_free_clusters: stored = %llu"
	       ", computed = %llu, %llu\n",
	       EXT5_NUM_B2C(EXT5_SB(sb), ext5_free_blocks_count(es)),
	       desc_count, bitmap_count);
	return bitmap_count;
#else
	desc_count = 0;
	for (i = 0; i < ngroups; i++) {
		gdp = ext5_get_group_desc(sb, i, NULL);
		if (!gdp)
			continue;
		desc_count += ext5_free_group_clusters(sb, gdp);
	}

	return desc_count;
#endif
}

static inline int test_root(ext5_group_t a, int b)
{
	int num = b;

	while (a > num)
		num *= b;
	return num == a;
}

static int ext5_group_sparse(ext5_group_t group)
{
	if (group <= 1)
		return 1;
	if (!(group & 1))
		return 0;
	return (test_root(group, 7) || test_root(group, 5) ||
		test_root(group, 3));
}

/**
 *	ext5_bg_has_super - number of blocks used by the superblock in group
 *	@sb: superblock for filesystem
 *	@group: group number to check
 *
 *	Return the number of blocks used by the superblock (primary or backup)
 *	in this group.  Currently this will be only 0 or 1.
 */
int ext5_bg_has_super(struct super_block *sb, ext5_group_t group)
{
	if (EXT5_HAS_RO_COMPAT_FEATURE(sb,
				EXT5_FEATURE_RO_COMPAT_SPARSE_SUPER) &&
			!ext5_group_sparse(group))
		return 0;
	return 1;
}

static unsigned long ext5_bg_num_gdb_meta(struct super_block *sb,
					ext5_group_t group)
{
	unsigned long metagroup = group / EXT5_DESC_PER_BLOCK(sb);
	ext5_group_t first = metagroup * EXT5_DESC_PER_BLOCK(sb);
	ext5_group_t last = first + EXT5_DESC_PER_BLOCK(sb) - 1;

	if (group == first || group == first + 1 || group == last)
		return 1;
	return 0;
}

static unsigned long ext5_bg_num_gdb_nometa(struct super_block *sb,
					ext5_group_t group)
{
	if (!ext5_bg_has_super(sb, group))
		return 0;

	if (EXT5_HAS_INCOMPAT_FEATURE(sb,EXT5_FEATURE_INCOMPAT_META_BG))
		return le32_to_cpu(EXT5_SB(sb)->s_es->s_first_meta_bg);
	else
		return EXT5_SB(sb)->s_gdb_count;
}

/**
 *	ext5_bg_num_gdb - number of blocks used by the group table in group
 *	@sb: superblock for filesystem
 *	@group: group number to check
 *
 *	Return the number of blocks used by the group descriptor table
 *	(primary or backup) in this group.  In the future there may be a
 *	different number of descriptor blocks in each group.
 */
unsigned long ext5_bg_num_gdb(struct super_block *sb, ext5_group_t group)
{
	unsigned long first_meta_bg =
			le32_to_cpu(EXT5_SB(sb)->s_es->s_first_meta_bg);
	unsigned long metagroup = group / EXT5_DESC_PER_BLOCK(sb);

	if (!EXT5_HAS_INCOMPAT_FEATURE(sb,EXT5_FEATURE_INCOMPAT_META_BG) ||
			metagroup < first_meta_bg)
		return ext5_bg_num_gdb_nometa(sb, group);

	return ext5_bg_num_gdb_meta(sb,group);

}

/*
 * This function returns the number of file system metadata clusters at
 * the beginning of a block group, including the reserved gdt blocks.
 */
static unsigned ext5_num_base_meta_clusters(struct super_block *sb,
				     ext5_group_t block_group)
{
	struct ext5_sb_info *sbi = EXT5_SB(sb);
	unsigned num;

	/* Check for superblock and gdt backups in this group */
	num = ext5_bg_has_super(sb, block_group);

	if (!EXT5_HAS_INCOMPAT_FEATURE(sb, EXT5_FEATURE_INCOMPAT_META_BG) ||
	    block_group < le32_to_cpu(sbi->s_es->s_first_meta_bg) *
			  sbi->s_desc_per_block) {
		if (num) {
			num += ext5_bg_num_gdb(sb, block_group);
			num += le16_to_cpu(sbi->s_es->s_reserved_gdt_blocks);
		}
	} else { /* For META_BG_BLOCK_GROUPS */
		num += ext5_bg_num_gdb(sb, block_group);
	}
	return EXT5_NUM_B2C(sbi, num);
}
/**
 *	ext5_inode_to_goal_block - return a hint for block allocation
 *	@inode: inode for block allocation
 *
 *	Return the ideal location to start allocating blocks for a
 *	newly created inode.
 */
ext5_fsblk_t ext5_inode_to_goal_block(struct inode *inode)
{
	struct ext5_inode_info *ei = EXT5_I(inode);
	ext5_group_t block_group;
	ext5_grpblk_t colour;
	int flex_size = ext5_flex_bg_size(EXT5_SB(inode->i_sb));
	ext5_fsblk_t bg_start;
	ext5_fsblk_t last_block;

	block_group = ei->i_block_group;
	if (flex_size >= EXT5_FLEX_SIZE_DIR_ALLOC_SCHEME) {
		/*
		 * If there are at least EXT5_FLEX_SIZE_DIR_ALLOC_SCHEME
		 * block groups per flexgroup, reserve the first block
		 * group for directories and special files.  Regular
		 * files will start at the second block group.  This
		 * tends to speed up directory access and improves
		 * fsck times.
		 */
		block_group &= ~(flex_size-1);
		if (S_ISREG(inode->i_mode))
			block_group++;
	}
	bg_start = ext5_group_first_block_no(inode->i_sb, block_group);
	last_block = ext5_blocks_count(EXT5_SB(inode->i_sb)->s_es) - 1;

	/*
	 * If we are doing delayed allocation, we don't need take
	 * colour into account.
	 */
	if (test_opt(inode->i_sb, DELALLOC))
		return bg_start;

	if (bg_start + EXT5_BLOCKS_PER_GROUP(inode->i_sb) <= last_block)
		colour = (current->pid % 16) *
			(EXT5_BLOCKS_PER_GROUP(inode->i_sb) / 16);
	else
		colour = (current->pid % 16) * ((last_block - bg_start) / 16);
	return bg_start + colour;
}
