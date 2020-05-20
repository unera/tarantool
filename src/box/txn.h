#ifndef TARANTOOL_BOX_TXN_H_INCLUDED
#define TARANTOOL_BOX_TXN_H_INCLUDED
/*
 * Copyright 2010-2016, Tarantool AUTHORS, please see AUTHORS file.
 *
 * Redistribution and use in source and binary forms, with or
 * without modification, are permitted provided that the following
 * conditions are met:
 *
 * 1. Redistributions of source code must retain the above
 *    copyright notice, this list of conditions and the
 *    following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL
 * <COPYRIGHT HOLDER> OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
 * LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdbool.h>
#include "salad/stailq.h"
#include "trigger.h"
#include "fiber.h"
#include "space.h"
#include "tuple.h"

#if defined(__cplusplus)
extern "C" {
#endif /* defined(__cplusplus) */

/** box statistics */
extern struct rmean *rmean_box;
/**
 * Global flag that enables mvcc engine.
 * If set, memtx starts to apply statements through txm history mechanism
 * and tx manager itself transaction reads in order to detect conflicts.
 */
extern bool tx_manager_use_mvcc_engine;

struct journal_entry;
struct engine;
struct space;
struct tuple;
struct xrow_header;
struct Vdbe;

enum txn_flag {
	/** Transaction has been processed. */
	TXN_IS_DONE,
	/**
	 * Transaction has been aborted by fiber yield so
	 * should be rolled back at commit.
	 */
	TXN_IS_ABORTED_BY_YIELD,
	/**
	 * fiber_yield() is allowed inside the transaction.
	 * See txn_can_yield() for more details.
	 */
	TXN_CAN_YIELD,
	/** on_commit and/or on_rollback list is not empty. */
	TXN_HAS_TRIGGERS,
	/**
	 * Synchronous transaction touched sync spaces, or an
	 * asynchronous transaction blocked by a sync one until it
	 * is confirmed.
	 */
	TXN_WAIT_SYNC,
	/**
	 * Synchronous transaction 'waiting for ACKs' state before
	 * commit. In this state it waits until it is replicated
	 * onto a quorum of replicas, and only then finishes
	 * commit and returns success to a user.
	 * TXN_WAIT_SYNC is always set, if TXN_WAIT_ACK is set.
	 */
	TXN_WAIT_ACK,
	/**
	 * A transaction may be forced to be asynchronous, not
	 * wait for any ACKs, and not depend on prepending sync
	 * transactions. This happens in a few special cases. For
	 * example, when applier receives snapshot from master.
	 */
	TXN_FORCE_ASYNC,
};

enum {
	/**
	 * Maximum recursion depth for on_replace triggers.
	 * Large numbers may corrupt C stack.
	 */
	TXN_SUB_STMT_MAX = 3
};

enum {
	/** Signature set for empty transactions. */
	TXN_SIGNATURE_NOP = 0,
	/**
	 * The default signature value for failed transactions.
	 * Indicates either write failure or any other failure
	 * not caused by synchronous transaction processing.
	 */
	TXN_SIGNATURE_ROLLBACK = -1,
	/**
	 * A value set for failed synchronous transactions
	 * on master, when not enough acks were collected.
	 */
	TXN_SIGNATURE_QUORUM_TIMEOUT = -2,
	/**
	 * A value set for failed synchronous transactions
	 * on replica (or any instance during recovery), when a
	 * transaction is rolled back because ROLLBACK message was
	 * read.
	 */
	TXN_SIGNATURE_SYNC_ROLLBACK = -3,
};

/**
 * Status of a transaction.
 */
enum txn_status {
	/**
	 * Initial state of TX. The only state of a TX that allowed to do
	 * read or write actions.
	 */
	TXN_INPROGRESS,
	/**
	 * The TX have passed conflict checks and is ready to be committed.
	 */
	TXN_PREPARED,
	/**
	 * The TX was aborted when other TX was committed due to conflict.
	 */
	TXN_CONFLICTED,
	/**
	 * The TX was read_only, has a conflict and was sent to read view.
	 * Read-only and does not participate in conflict resolution ever more.
	 * This transaction can only see a state of the database at some fixed
	 * point in the past.
	 */
	TXN_IN_READ_VIEW,
	/**
	 * The TX was committed.
	 */
	TXN_COMMITTED,
	/**
	 * The TX was aborted.
	 */
	TXN_ABORTED,
};

/**
 * A single statement of a multi-statement
 * transaction: undo and redo info.
 */
struct txn_stmt {
	/* (!) Please update txn_stmt_new() after changing members */

	/** A linked list of all statements. */
	struct stailq_entry next;
	/** Owner of that statement. */
	struct txn *txn;
	/** Undo info. */
	struct space *space;
	struct tuple *old_tuple;
	struct tuple *new_tuple;
	/**
	 * If new_tuple != NULL and this transaction was not prepared,
	 * this member holds added story of the new_tuple.
	 */
	struct txm_story *add_story;
	/**
	 * If new_tuple == NULL and this transaction was not prepared,
	 * this member holds added story of the old_tuple.
	 */
	struct txm_story *del_story;
	/**
	 * Link in txm_story::del_stmt linked list.
	 * Only one prepared TX can delete a tuple and a story. But
	 * when there are several in-progress transactions and they delete
	 * the same tuple we have to store several delete statements in one
	 * story. It's implemented in that way: story has a pointer to the first
	 * deleting statement, that statement has a pointer to the next etc,
	 * with NULL in the end.
	 * That member is that the pointer to next deleting statement.
	 */
	struct txn_stmt *next_in_del_list;
	/** Engine savepoint for the start of this statement. */
	void *engine_savepoint;
	/** Redo info: the binary log row */
	struct xrow_header *row;
	/** on_commit and/or on_rollback list is not empty. */
	bool has_triggers;
	/**
	 * Whether the stmt requires to replace exactly old_tuple (member).
	 * That flag is needed for transactional conflict manager - if any
	 * other transaction replaces old_tuple before current one and the flag
	 * is set - the current transaction will be aborted.
	 * For example REPLACE just replaces a key, no matter what tuple
	 * lays in the index and thus does_require_old_tuple = false.
	 * In contrast, UPDATE makes new tuple using old_tuple and thus
	 * the statement will require old_tuple (does_require_old_tuple = true).
	 * INSERT also does_require_old_tuple = true because it requires
	 * old_tuple to be NULL.
	 */
	bool does_require_old_tuple;
	/** Commit/rollback triggers associated with this statement. */
	struct rlist on_commit;
	struct rlist on_rollback;
};

/**
 * Transaction savepoint object. Allocated on a transaction
 * region and becames invalid after the transaction's end.
 * Allows to rollback a transaction partially.
 */
struct txn_savepoint {
	/**
	 * Saved substatement level at the time of a savepoint
	 * creation.
	 */
	int in_sub_stmt;
	/**
	 * Statement, on which a savepoint is created. On rollback
	 * to this savepoint all newer statements are rolled back.
	 * Initialized to NULL in case a savepoint is created in
	 * an empty transaction.
	 */
	struct stailq_entry *stmt;
	/**
	 * Each foreign key constraint is classified as either
	 * immediate (by default) or deferred. In autocommit mode
	 * they mean the same. Inside separate transaction,
	 * deferred FK constraints are not checked until the
	 * transaction tries to commit. For as long as
	 * a transaction is open, it is allowed to exist in a
	 * state violating any number of deferred FK constraints.
	 */
	uint32_t fk_deferred_count;
	/** Organize savepoints into linked list. */
	struct rlist link;
	/**
	 * Optional name of savepoint. If savepoint lacks
	 * name (i.e. anonymous savepoint available only by
	 * reference to the object), name[0] == ''. Otherwise,
	 * memory for name is reserved in the same memory chunk
	 * as struct txn_savepoint itself - name is placed
	 * right after structure (see txn_savepoint_new()).
	 */
	char name[1];
};

extern double too_long_threshold;

/**
 * An element of list of autogenerated ids, being returned as SQL
 * response metadata.
 */
struct autoinc_id_entry {
	struct stailq_entry link;
	int64_t id;
};

struct txn {
	/** A stailq_entry to hold a txn in a cache. */
	struct stailq_entry in_txn_cache;
	/**
	 * A memory region to put all transaction relative data in.
	 * Detaching transaction data from a fiber temporary storage
	 * is required to allow an applier fiber to manage multiple
	 * transactions simultaneously. Also interactive and autonomous
	 * transactions will require this.
	 */
	struct region region;
	/**
	 * A sequentially growing transaction id, assigned when
	 * a transaction is initiated. Used to identify
	 * a transaction after it has possibly been destroyed.
	 *
	 * Valid IDs start from 1.
	 */
	int64_t id;
	/**
	 * A sequential ID that is assigned when the TX become prepared.
	 * Transactions are committed in that order.
	 */
	int64_t psn;
	/**
	 * Read view of that TX. The TX can see only changes with ps < rv_psn.
	 * Is nonzero if and only if status = TXN_IN_READ_VIEW.
	 */
	int64_t rv_psn;
	/** Status of the TX */
	enum txn_status status;
	/** List of statements in a transaction. */
	struct stailq stmts;
	/** Number of new rows without an assigned LSN. */
	int n_new_rows;
	/**
	 * Number of local new rows, no assigned LSN and
	 * replication group_id=local (not replicated anywhere).
	 */
	int n_local_rows;
	/**
	 * Number of rows coming from the applier, with an
	 * already assigned LSN.
	 */
	int n_applier_rows;
	/** Bit mask of transaction flags, see txn_flag. */
	unsigned flags;
	/** The number of active nested statement-level transactions. */
	int8_t in_sub_stmt;
	/**
	 * First statement at each statement-level.
	 * Needed to rollback sub statements.
	 */
	struct stailq_entry *sub_stmt_begin[TXN_SUB_STMT_MAX + 1];
	/** LSN of this transaction when written to WAL. */
	int64_t signature;
	/** Engine involved in multi-statement transaction. */
	struct engine *engine;
	/** Engine-specific transaction data */
	void *engine_tx;
	/* A fiber to wake up when transaction is finished. */
	struct fiber *fiber;
	/** Timestampt of entry write start. */
	double start_tm;
	/**
	 * Triggers on fiber yield to abort transaction for
	 * for in-memory engine.
	 */
	struct trigger fiber_on_yield;
	/**
	 * Trigger on fiber stop, to rollback transaction
	 * in case a fiber stops (all engines).
	 */
	struct trigger fiber_on_stop;
	/** Commit and rollback triggers. */
	struct rlist on_commit, on_rollback, on_wal_write;
	/**
	 * This member represents counter of deferred foreign key
	 * violations within transaction. DEFERRED mode means
	 * that until transaction is committed violations are
	 * allowed to appear. However, transaction can't be
	 * committed in presence of violations, i.e. if this
	 * counter is not equal to zero. In the normal mode
	 * violations of FK are checked at the end of each
	 * statement processing. Note that at the moment it is
	 * SQL specific property.
	 */
	uint32_t fk_deferred_count;
	/** List of savepoints to find savepoint by name. */
	struct rlist savepoints;
	/**
	 * List of tx_conflict_tracker records where .breaker is the current
	 * transaction and .victim is the transactions that must be aborted
	 * if the current transaction is committed.
	 */
	struct rlist conflict_list;
	/**
	 * List of tx_conflict_tracker records where .victim is the current
	 * transaction and .breaker is the transactions that, if committed,
	 * will abort the current transaction.
	 */
	struct rlist conflicted_by_list;
	/**
	 * Link in tx_manager::read_view_txs.
	 */
	struct rlist in_read_view_txs;
};

/**
 * Pointer to tuple or story.
 */
struct txm_story_or_tuple {
	/** Flag whether it's a story. */
	bool is_story;
	union {
		/** Pointer to story, it must be reverse liked. */
		struct txm_story *story;
		/** Smart pointer to tuple: the tuple is referenced if set. */
		struct tuple *tuple;
	};
};

/**
 * Link that connects a txm_story with older and newer stories of the same
 * key in index.
 */
struct txm_story_link {
	/** Story that was happened after that story was ended. */
	struct txm_story *newer_story;
	/**
	 * Older story or ancient tuple (so old that its story was lost).
	 * In case of tuple is can also be NULL.
	 */
	struct txm_story_or_tuple older;
};

/**
 * A part of a history of a value in space.
 * It's a story about a tuple, from the point it was added to space to the
 * point when it was deleted from a space.
 * All stories are linked into a list of stories of the same key of each index.
 */
struct txm_story {
	/** The story is about this tuple. The tuple is referenced. */

	struct tuple *tuple;
	/**
	 * Statement that told this story. Is set to NULL when the statement's
	 * transaction becomes committed. Can also be NULL if we don't know who
	 * introduced that story, the tuple was added by a transaction that
	 * was completed and destroyed some time ago.
	 */
	struct txn_stmt *add_stmt;
	/**
	 * Prepare sequence number of add_stmt's transaction. Is set when
	 * the transactions is prepared. Can be 0 if the transaction is
	 * in progress or we don't know who introduced that story.
	 */
	int64_t add_psn;
	/**
	 * Statement that ended this story. Is set to NULL when the statement's
	 * transaction becomes committed. Can also be NULL if the tuple has not
	 * been deleted yet.
	 */
	struct txn_stmt *del_stmt;
	/**
	 * Prepare sequence number of del_stmt's transaction. Is set when
	 * the transactions is prepared. Can be 0 if the transaction is
	 * in progress or if nobody has deleted the tuple.
	 */
	int64_t del_psn;
	/**
	 * List of trackers - transactions that has read this tuple.
	 */
	struct rlist reader_list;
	/**
	 * Link in tx_manager::all_stories
	 */
	struct rlist in_all_stories;
	/**
	 * Link in space::txm_stories.
	 */
	struct rlist in_space_stories;
	/**
	 * The space where the tuple is supposed to be.
	 */
	struct space *space;
	/**
	 * Number of indexes in this space - and the count of link[].
	 */
	uint32_t index_count;
	/**
	 * Link with older and newer stories (and just tuples) for each
	 * index respectively.
	 */
	struct txm_story_link link[];
};

static inline bool
txn_has_flag(struct txn *txn, enum txn_flag flag)
{
	return (txn->flags & (1 << flag)) != 0;
}

static inline void
txn_set_flag(struct txn *txn, enum txn_flag flag)
{
	txn->flags |= 1 << flag;
}

static inline void
txn_clear_flag(struct txn *txn, enum txn_flag flag)
{
	txn->flags &= ~(1 << flag);
}

/* Pointer to the current transaction (if any) */
static inline struct txn *
in_txn(void)
{
	return fiber()->storage.txn;
}

/* Set to the current transaction (if any) */
static inline void
fiber_set_txn(struct fiber *fiber, struct txn *txn)
{
	fiber->storage.txn = txn;
}

/**
 * Start a transaction explicitly.
 * @pre no transaction is active
 */
struct txn *
txn_begin(void);

/**
 * Complete transaction processing.
 */
void
txn_complete(struct txn *txn);

/**
 * Commit a transaction.
 * @pre txn == in_txn()
 *
 * Return 0 on success. On error, rollback
 * the transaction and return -1.
 */
int
txn_commit(struct txn *txn);

/**
 * Rollback a transaction.
 * @pre txn == in_txn()
 */
void
txn_rollback(struct txn *txn);

/**
 * Complete asynchronous transaction.
 */
void
txn_complete_async(struct journal_entry *entry);

/**
 * Submit a transaction to the journal.
 * @pre txn == in_txn()
 *
 * On success 0 is returned, and the transaction will be freed upon
 * journal write completion. Note, the journal write may still fail.
 * To track transaction status, one is supposed to use on_commit and
 * on_rollback triggers.
 *
 * On failure -1 is returned and the transaction is rolled back and
 * freed.
 */
int
txn_commit_async(struct txn *txn);

/**
 * Most txns don't have triggers, and txn objects
 * are created on every access to data, so txns
 * are partially initialized.
 */
static inline void
txn_init_triggers(struct txn *txn)
{
	if (!txn_has_flag(txn, TXN_HAS_TRIGGERS)) {
		rlist_create(&txn->on_commit);
		rlist_create(&txn->on_rollback);
		rlist_create(&txn->on_wal_write);
		txn_set_flag(txn, TXN_HAS_TRIGGERS);
	}
}

static inline void
txn_on_commit(struct txn *txn, struct trigger *trigger)
{
	txn_init_triggers(txn);
	trigger_add(&txn->on_commit, trigger);
}

static inline void
txn_on_rollback(struct txn *txn, struct trigger *trigger)
{
	txn_init_triggers(txn);
	trigger_add(&txn->on_rollback, trigger);
}

static inline void
txn_on_wal_write(struct txn *txn, struct trigger *trigger)
{
	txn_init_triggers(txn);
	trigger_add(&txn->on_wal_write, trigger);
}

/**
 * Most statements don't have triggers, and txn objects
 * are created on every access to data, so statements
 * are partially initialized.
 */
static inline void
txn_stmt_init_triggers(struct txn_stmt *stmt)
{
	if (!stmt->has_triggers) {
		rlist_create(&stmt->on_commit);
		rlist_create(&stmt->on_rollback);
		stmt->has_triggers = true;
	}
}

static inline void
txn_stmt_on_commit(struct txn_stmt *stmt, struct trigger *trigger)
{
	txn_stmt_init_triggers(stmt);
	trigger_add(&stmt->on_commit, trigger);
}

static inline void
txn_stmt_on_rollback(struct txn_stmt *stmt, struct trigger *trigger)
{
	txn_stmt_init_triggers(stmt);
	trigger_add(&stmt->on_rollback, trigger);
}

/*
 * Return the total number of rows committed in the txn.
 */
static inline int
txn_n_rows(struct txn *txn)
{
	return txn->n_new_rows + txn->n_applier_rows;
}

/**
 * Start a new statement.
 */
int
txn_begin_stmt(struct txn *txn, struct space *space);

int
txn_begin_in_engine(struct engine *engine, struct txn *txn);

/**
 * This is an optimization, which exists to speed up selects
 * in autocommit mode. For such selects, we only need to
 * manage fiber garbage heap. If autocommit mode is
 * off, however, we must start engine transaction with the first
 * select.
 */
static inline int
txn_begin_ro_stmt(struct space *space, struct txn **txn)
{
	*txn = in_txn();
	if (*txn != NULL) {
		struct engine *engine = space->engine;
		return txn_begin_in_engine(engine, *txn);
	}
	return 0;
}

static inline void
txn_commit_ro_stmt(struct txn *txn)
{
	assert(txn == in_txn());
	if (txn) {
		/* nothing to do */
	} else {
		fiber_gc();
	}
}

/**
 * Check whether a transaction which is used to apply
 * remote master rows generated some local changes.
 * Such transaction must be aborted, since we wouldn't
 * be able to *consistently* apply the local changes
 * to the remote master.
 */
bool
txn_is_distributed(struct txn *txn);

/**
 * End a statement. In autocommit mode, end
 * the current transaction as well.
 *
 * Return 0 on success. On error, rollback
 * the statement and return -1.
 */
int
txn_commit_stmt(struct txn *txn, struct request *request);

/**
 * Rollback a statement. In autocommit mode,
 * rolls back the entire transaction.
 */
void
txn_rollback_stmt(struct txn *txn);

/**
 * Raise an error if this is a multi-statement transaction:
 * a yielding DDL operation, such as index build or space format
 * check, can not be part of a multi-statement transaction,
 * because there may be uncommitted objects in the schema cache,
 * which would be revealed to other fibers on yield.
 */
int
txn_check_singlestatement(struct txn *txn, const char *where);

/**
 * Enables or disables fiber yields inside the current transaction
 * depending on the value of the given flag. Yields are disabled
 * by installing a fiber-on-yield trigger that marks the transaction
 * as aborted, which results in rolling back the transaction on
 * commit.
 *
 * This function is used by the memtx engine, because it doesn't
 * support yields inside transactions. It is also used to temporarily
 * enable yields for long DDL operations such as building an index
 * or checking a space format.
 */
void
txn_can_yield(struct txn *txn, bool set);

/**
 * Returns true if the transaction has a single statement.
 * Supposed to be used from a space on_replace trigger to
 * detect transaction boundaries.
 */
static inline bool
txn_is_first_statement(struct txn *txn)
{
	return stailq_last(&txn->stmts) == stailq_first(&txn->stmts);
}

/** The current statement of the transaction. */
static inline struct txn_stmt *
txn_current_stmt(struct txn *txn)
{
	if (txn->in_sub_stmt == 0)
		return NULL;
	struct stailq_entry *stmt = txn->sub_stmt_begin[txn->in_sub_stmt - 1];
	stmt = stmt != NULL ? stailq_next(stmt) : stailq_first(&txn->stmts);
	return stailq_entry(stmt, struct txn_stmt, next);
}

/**
 * Allocate new savepoint object using region allocator.
 * Savepoint is allowed to be anonymous (i.e. without
 * name).
 */
struct txn_savepoint *
txn_savepoint_new(struct txn *txn, const char *name);

/** Find savepoint by its name in savepoint list. */
struct txn_savepoint *
txn_savepoint_by_name(struct txn *txn, const char *name);

/** Remove given and all newer entries from savepoint list. */
void
txn_savepoint_release(struct txn_savepoint *svp);

/**
 * FFI bindings: do not throw exceptions, do not accept extra
 * arguments
 */

/** \cond public */

/**
 * Transaction id - a non-persistent unique identifier
 * of the current transaction. -1 if there is no current
 * transaction.
 */
API_EXPORT int64_t
box_txn_id(void);

/**
 * Return true if there is an active transaction.
 */
API_EXPORT bool
box_txn(void);

/**
 * Begin a transaction in the current fiber.
 *
 * A transaction is attached to caller fiber, therefore one fiber can have
 * only one active transaction.
 *
 * @retval 0 - success
 * @retval -1 - failed, perhaps a transaction has already been
 * started
 */
API_EXPORT int
box_txn_begin(void);

/**
 * Commit the current transaction.
 * @retval 0 - success
 * @retval -1 - failed, perhaps a disk write failure.
 * started
 */
API_EXPORT int
box_txn_commit(void);

/**
 * Rollback the current transaction.
 * May fail if called from a nested
 * statement.
 */
API_EXPORT int
box_txn_rollback(void);

/**
 * Allocate memory on txn memory pool.
 * The memory is automatically deallocated when the transaction
 * is committed or rolled back.
 *
 * @retval NULL out of memory
 */
API_EXPORT void *
box_txn_alloc(size_t size);

/** \endcond public */

typedef struct txn_savepoint box_txn_savepoint_t;

/**
 * Create a new savepoint.
 * @retval not NULL Savepoint object.
 * @retval     NULL Client or memory error.
 */
API_EXPORT box_txn_savepoint_t *
box_txn_savepoint(void);

/**
 * Rollback to @a savepoint. Rollback all statements newer than a
 * saved statement. @A savepoint can be rolled back multiple
 * times. All existing savepoints, newer than @a savepoint, are
 * deleted and can not be used.
 * @A savepoint must be from a current transaction, else the
 * rollback crashes. To validate savepoints store transaction id
 * together with @a savepoint.
 * @retval  0 Success.
 * @retval -1 Client error.
 */
API_EXPORT int
box_txn_rollback_to_savepoint(box_txn_savepoint_t *savepoint);

void
tx_manager_init();

void
tx_manager_free();

/**
 * Notify TX manager that if transaction @breaker is committed then the
 * transactions @victim must be aborted due to conflict.
 * For example: there's two rw transaction in progress, one have read
 * some value while the second is about to overwrite it. If the second
 * is committed first, the first must be aborted.
 * @return 0 on success, -1 on memory error.
 */
int
txm_cause_conflict(struct txn *breaker, struct txn *victim);

/**
 * @brief Add a statement to transaction manager's history.
 * Until unlinking or releasing the space could internally contain
 * wrong tuples and must be cleaned through txm_tuple_clarify call.
 * With that clarifying the statement will be visible to current transaction,
 * but invisible to all others.
 * Follows signature of @sa memtx_space_replace_all_keys .
 *
 * @param stmt current statement.
 * @param old_tuple the tuple that should be removed (can be NULL).
 * @param new_tuple the tuple that should be inserted (can be NULL).
 * @param mode      dup_replace_mode, used only if new_tuple is not
 *                  NULL and old_tuple is NULL, and only for the
 *                  primary key.
 * @param result - old or replaced tuple.
 * @return 0 on success, -1 on error (diag is set).
 */
int
txm_history_add_stmt(struct txn_stmt *stmt, struct tuple *old_tuple,
		     struct tuple *new_tuple, enum dup_replace_mode mode,
		     struct tuple **result);

/**
 * @brief Rollback (undo) a statement from transaction manager's history.
 * It's just make the statement invisible to all.
 * Prepared statements could be also removed, but for consistency all latter
 * prepared statement must be also rolled back.
 *
 * @param stmt current statement.
 */
void
txm_history_rollback_stmt(struct txn_stmt *stmt);

/**
 * @brief Prepare statement in history for further commit.
 * Prepared statements are still invisible for read-only transactions
 * but are visible to all read-write transactions.
 * Prepared and in-progress transactions use the same links for creating
 * chains of stories in history. The difference is that the order of
 * prepared transactions is fixed while in-progress transactions are
 * added to the end of list in any order. Thus to switch to prepared
 * we have to reorder story in such a way that current story will be
 * between earlier prepared stories and in-progress stories. That's what
 * this function does.
 *
 * @param stmt current statement.
 */
void
txm_history_prepare_stmt(struct txn_stmt *stmt);

/**
 * @brief Commit statement in history.
 * Make the statement's changes permanent. It becomes visible to all.
 *
 * @param stmt current statement.
 * @return the change in space bsize.
 */
ssize_t
txm_history_commit_stmt(struct txn_stmt *stmt);

/** Helper of txm_tuple_clarify */
struct tuple *
txm_tuple_clarify_slow(struct txn *txn, struct space *space,
		       struct tuple *tuple, uint32_t index,
		       uint32_t mk_index, bool is_prepared_ok);

/**
 * Cleans a tuple if it's dirty - finds a visible tuple in history.
 * @param txn - current transactions.
 * @param space - space in which the tuple was found.
 * @param tuple - tuple to clean.
 * @param index - index number.
 * @param mk_index - multikey index (iа the index is multikey).
 * @param is_prepared_ok - allow to return prepared tuples.
 * @return clean tuple (can be NULL).
 */
static inline struct tuple *
txm_tuple_clarify(struct txn *txn, struct space *space,
		  struct tuple *tuple, uint32_t index,
		  uint32_t mk_index, bool is_prepared_ok)
{
	if (!tx_manager_use_mvcc_engine)
		return tuple;
	if (!tuple->is_dirty)
		return tuple;
	return txm_tuple_clarify_slow(txn, space, tuple, index, mk_index,
				      is_prepared_ok);
}

#if defined(__cplusplus)
} /* extern "C" */
#endif /* defined(__cplusplus) */

#endif /* TARANTOOL_BOX_TXN_H_INCLUDED */
