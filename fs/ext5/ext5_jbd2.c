/*
 * Interface between ext5 and JBD
 */

#include "ext5_jbd2.h"

#include <trace/events/ext5.h>

/* Just increment the non-pointer handle value */
static handle_t *ext5_get_nojournal(void)
{
	handle_t *handle = current->journal_info;
	unsigned long ref_cnt = (unsigned long)handle;

	BUG_ON(ref_cnt >= EXT5_NOJOURNAL_MAX_REF_COUNT);

	ref_cnt++;
	handle = (handle_t *)ref_cnt;

	current->journal_info = handle;
	return handle;
}


/* Decrement the non-pointer handle value */
static void ext5_put_nojournal(handle_t *handle)
{
	unsigned long ref_cnt = (unsigned long)handle;

	BUG_ON(ref_cnt == 0);

	ref_cnt--;
	handle = (handle_t *)ref_cnt;

	current->journal_info = handle;
}

/*
 * Wrappers for jbd2_journal_start/end.
 */
handle_t *__ext5_journal_start_sb(struct super_block *sb, unsigned int line,
				  int type, int nblocks)
{
	journal_t *journal;

	might_sleep();

	trace_ext5_journal_start(sb, nblocks, _RET_IP_);
	if (sb->s_flags & MS_RDONLY)
		return ERR_PTR(-EROFS);

	WARN_ON(sb->s_writers.frozen == SB_FREEZE_COMPLETE);
	journal = EXT5_SB(sb)->s_journal;
	if (!journal)
		return ext5_get_nojournal();
	/*
	 * Special case here: if the journal has aborted behind our
	 * backs (eg. EIO in the commit thread), then we still need to
	 * take the FS itself readonly cleanly.
	 */
	if (is_journal_aborted(journal)) {
		ext5_abort(sb, "Detected aborted journal");
		return ERR_PTR(-EROFS);
	}
	return jbd2__journal_start(journal, nblocks, GFP_NOFS, type, line);
}

int __ext5_journal_stop(const char *where, unsigned int line, handle_t *handle)
{
	struct super_block *sb;
	int err;
	int rc;

	if (!ext5_handle_valid(handle)) {
		ext5_put_nojournal(handle);
		return 0;
	}
	sb = handle->h_transaction->t_journal->j_private;
	err = handle->h_err;
	rc = jbd2_journal_stop(handle);

	if (!err)
		err = rc;
	if (err)
		__ext5_std_error(sb, where, line, err);
	return err;
}

void ext5_journal_abort_handle(const char *caller, unsigned int line,
			       const char *err_fn, struct buffer_head *bh,
			       handle_t *handle, int err)
{
	char nbuf[16];
	const char *errstr = ext5_decode_error(NULL, err, nbuf);

	BUG_ON(!ext5_handle_valid(handle));

	if (bh)
		BUFFER_TRACE(bh, "abort");

	if (!handle->h_err)
		handle->h_err = err;

	if (is_handle_aborted(handle))
		return;

	printk(KERN_ERR "EXT5-fs: %s:%d: aborting transaction: %s in %s\n",
	       caller, line, errstr, err_fn);

	jbd2_journal_abort_handle(handle);
}

int __ext5_journal_get_write_access(const char *where, unsigned int line,
				    handle_t *handle, struct buffer_head *bh)
{
	int err = 0;

	might_sleep();

	if (ext5_handle_valid(handle)) {
		err = jbd2_journal_get_write_access(handle, bh);
		if (err)
			ext5_journal_abort_handle(where, line, __func__, bh,
						  handle, err);
	}
	return err;
}

/*
 * The ext5 forget function must perform a revoke if we are freeing data
 * which has been journaled.  Metadata (eg. indirect blocks) must be
 * revoked in all cases.
 *
 * "bh" may be NULL: a metadata block may have been freed from memory
 * but there may still be a record of it in the journal, and that record
 * still needs to be revoked.
 *
 * If the handle isn't valid we're not journaling, but we still need to
 * call into ext5_journal_revoke() to put the buffer head.
 */
int __ext5_forget(const char *where, unsigned int line, handle_t *handle,
		  int is_metadata, struct inode *inode,
		  struct buffer_head *bh, ext5_fsblk_t blocknr)
{
	int err;

	might_sleep();

	trace_ext5_forget(inode, is_metadata, blocknr);
	BUFFER_TRACE(bh, "enter");

	jbd_debug(4, "forgetting bh %p: is_metadata = %d, mode %o, "
		  "data mode %x\n",
		  bh, is_metadata, inode->i_mode,
		  test_opt(inode->i_sb, DATA_FLAGS));

	/* In the no journal case, we can just do a bforget and return */
	if (!ext5_handle_valid(handle)) {
		bforget(bh);
		return 0;
	}

	/* Never use the revoke function if we are doing full data
	 * journaling: there is no need to, and a V1 superblock won't
	 * support it.  Otherwise, only skip the revoke on un-journaled
	 * data blocks. */

	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT5_MOUNT_JOURNAL_DATA ||
	    (!is_metadata && !ext5_should_journal_data(inode))) {
		if (bh) {
			BUFFER_TRACE(bh, "call jbd2_journal_forget");
			err = jbd2_journal_forget(handle, bh);
			if (err)
				ext5_journal_abort_handle(where, line, __func__,
							  bh, handle, err);
			return err;
		}
		return 0;
	}

	/*
	 * data!=journal && (is_metadata || should_journal_data(inode))
	 */
	BUFFER_TRACE(bh, "call jbd2_journal_revoke");
	err = jbd2_journal_revoke(handle, blocknr, bh);
	if (err) {
		ext5_journal_abort_handle(where, line, __func__,
					  bh, handle, err);
		__ext5_abort(inode->i_sb, where, line,
			   "error %d when attempting revoke", err);
	}
	BUFFER_TRACE(bh, "exit");
	return err;
}

int __ext5_journal_get_create_access(const char *where, unsigned int line,
				handle_t *handle, struct buffer_head *bh)
{
	int err = 0;

	if (ext5_handle_valid(handle)) {
		err = jbd2_journal_get_create_access(handle, bh);
		if (err)
			ext5_journal_abort_handle(where, line, __func__,
						  bh, handle, err);
	}
	return err;
}

int __ext5_handle_dirty_metadata(const char *where, unsigned int line,
				 handle_t *handle, struct inode *inode,
				 struct buffer_head *bh)
{
	int err = 0;

	might_sleep();

	set_buffer_meta(bh);
	set_buffer_prio(bh);
	if (ext5_handle_valid(handle)) {
		err = jbd2_journal_dirty_metadata(handle, bh);
		if (err) {
			/* Errors can only happen if there is a bug */
			handle->h_err = err;
			__ext5_journal_stop(where, line, handle);
		}
	} else {
		if (inode)
			mark_buffer_dirty_inode(bh, inode);
		else
			mark_buffer_dirty(bh);
		if (inode && inode_needs_sync(inode)) {
			sync_dirty_buffer(bh);
			if (buffer_req(bh) && !buffer_uptodate(bh)) {
				struct ext5_super_block *es;

				es = EXT5_SB(inode->i_sb)->s_es;
				es->s_last_error_block =
					cpu_to_le64(bh->b_blocknr);
				ext5_error_inode(inode, where, line,
						 bh->b_blocknr,
					"IO error syncing itable block");
				err = -EIO;
			}
		}
	}
	return err;
}

int __ext5_handle_dirty_super(const char *where, unsigned int line,
			      handle_t *handle, struct super_block *sb)
{
	struct buffer_head *bh = EXT5_SB(sb)->s_sbh;
	int err = 0;

	ext5_superblock_csum_set(sb);
	if (ext5_handle_valid(handle)) {
		err = jbd2_journal_dirty_metadata(handle, bh);
		if (err)
			ext5_journal_abort_handle(where, line, __func__,
						  bh, handle, err);
	} else
		mark_buffer_dirty(bh);
	return err;
}