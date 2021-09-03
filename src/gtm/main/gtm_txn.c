/*-------------------------------------------------------------------------
 *
 * gtm_txn.c
 *	Transaction handling on GTM
 *
 * Portions Copyright (c) 2012-2014, TransLattice, Inc.
 * Portions Copyright (c) 1996-2009, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2010-2012 Postgres-XC Development Group
 *
 *
 * IDENTIFICATION
 *	  src/gtm/main/gtm_txn.c
 *
 *
 * Functions in this file manage the main transaction array (GTMTransactions)
 * and provide API to manage the contents - begin, commit and/or abort global
 * transactions.
 *
 * The rest of this comment is a brief overview of the API. It's by no means
 * exhaustive - you can find more details in comments at each function or
 * in the code itself. But it should explain basic concepts and main functions
 * of the GTM Transaction API.
 *
 * There are additional parts of the GTM, dealing with other types of objects
 * (e.g. sequences or snapshots). Those are managed by functions in other
 * files, and you will need to look into those files for description of that
 * part of the API.
 *
 *
 * Transaction Identifiers
 * -----------------------
 * There are several ways to identify a global transaction. Some identifiers
 * are internal, while other are meant as an interface with users. There are
 * four main types of identifiers in the code:
 *
 * 1) GTM_TransactionHandle (handle) : Index into the internal array of global
 *    transactions (GTMTransactions.gt_transactions_array), so the values are
 *    limited to interval [0, GTM_MAX_GLOBAL_TRANSACTIONS].
 *
 * 2) GlobalTransactionId (GXID) : Sequential ID (uint32), assigned by GTM
 *    to a transaction, just like PostgreSQL assigns XIDs to local transactions.
 *
 * 3) Global Transaction Identifier (GID) : Assigned to transactionss in 2PC
 *    transactions, visible to users.
 *
 * 4) Global Session ID : Not really a transaction identifier, but it's often
 *    necessary to lookup transaction assigned to a global session.
 *
 * One difference between the identifiers is in the cost of looking-up the
 * transaction. Handles are very cheap, as all that's needed is simply
 *
 *     GTMTransactions.gt_transactions_array[handle]
 *
 * All other identifiers may require walking through the currently opened
 * transactions, which is more expensive. This is why the API references to
 * transactions by handles in most places, and provides functions to convert
 * the other identifiers to handles:
 *
 * 	- GTM_GXIDToHandle()            : GXID       -> handle
 * 	- GTM_GlobalSessionIDToHandle() : session ID -> handle
 * 	- GTM_GIDToHandle()             : GID        -> handle
 *
 * Conversion in the other direction is trivial, as the identifiers are
 * stored as fields in GTM_TransactionInfo.
 *
 *
 * Transaction Management
 * ----------------------
 * The basic transaction management commands (BEGIN/PREPARE/COMMIT/ABORT)
 * are implemented in these eight methods:
 *
 * 	- GTM_BeginTransaction()
 * 	- GTM_BeginTransactionMulti()
 *
 * 	- GTM_RollbackTransaction()
 * 	- GTM_RollbackTransactionMulti()
 *
 * 	- GTM_CommitTransaction()
 * 	- GTM_CommitTransactionMulti()
 *
 * 	- GTM_StartPreparedTransaction()
 * 	- GTM_PrepareTransaction()
 *
 * The first three commands have two variants - the first one processes a
 * single transaction (handle), while the "multi" variant operates on an
 * array of handles. This is useful when processing commands grouped by
 * GTM proxy nodes.
 *
 *
 * Message Processing
 * ------------------
 * Most of the transaction management methods are declared as static, and
 * are invoked from functions processing messages arriving from clients
 * over the network. Names of all these meethods start with "Process", and
 * in most cases it's quite clear which transaction management command is
 * invoked by each function:
 *
 * 	- ProcessBeginTransactionCommand()
 * 	- ProcessBeginTransactionGetGXIDCommand()
 * 	- ProcessBeginTransactionGetGXIDAutovacuumCommand()
 * 	- ProcessBeginTransactionGetGXIDCommandMulti()
 *
 * 	- ProcessRollbackTransactionCommand()
 * 	- ProcessRollbackTransactionCommandMulti()
 *
 * 	- ProcessCommitTransactionCommand()
 * 	- ProcessCommitTransactionCommandMulti()
 * 	- ProcessCommitPreparedTransactionCommand()
 *
 * 	- ProcessPrepareTransactionCommand()
 * 	- ProcessStartPreparedTransactionCommand()
 *
 * These function handle communication not only with the GTM clients (that
 * is backends on datanodes/coordinators or proxies), but with a GTM standby
 * nodes. They typically receive a message, execute the command locally
 * and also forward it to the GTM standby node before responding to client.
 *
 * For some methods there are special variants with "Bkup" in the name:
 *
 * 	- ProcessBkupBeginTransactionCommand()
 * 	- ProcessBkupBeginTransactionGetGXIDCommand()
 * 	- ProcessBkupBeginTransactionGetGXIDAutovacuumCommand()
 * 	- ProcessBkupBeginTransactionGetGXIDCommandMulti()
 *
 * Those are handling the commands on standby, in a slightly different way
 * (e.g. without forwarding the messages to GTM standby nodes, etc.).
 *
 *-------------------------------------------------------------------------
 */
#include "gtm/gtm_txn.h"

#include <unistd.h>
#include "gtm/assert.h"
#include "gtm/elog.h"
#include "gtm/gtm.h"
#include "gtm/gtm_time.h"
#include "gtm/gtm_txn.h"
#include "gtm/gtm_serialize.h"
#include "gtm/gtm_standby.h"
#include "gtm/standby_utils.h"
#include "gtm/libpq.h"
#include "gtm/libpq-int.h"
#include "gtm/pqformat.h"
#include "gtm/gtm_backup.h"

extern bool Backup_synchronously;

/* Local functions */
static bool GTM_SetDoVacuum(GTM_TransactionHandle handle);
static void GTM_TransactionInfo_Init(GTM_TransactionInfo *gtm_txninfo,
									 GTM_TransactionHandle txn,
									 GTM_IsolationLevel isolevel,
									 uint32 client_id,
									 GTMProxy_ConnID connid,
									 const char *global_sessionid,
									 bool readonly);
static void GTM_TransactionInfo_Clean(GTM_TransactionInfo *gtm_txninfo);
static GTM_TransactionHandle GTM_GlobalSessionIDToHandle(
									const char *global_sessionid);
static bool GTM_NeedXidRestoreUpdate(void);

GlobalTransactionId ControlXid;  /* last one written to control file */
GTM_Transactions GTMTransactions;

/*
 * GTM_InitTxnManager
 *	Initializes the internal data structures used by GTM.
 *
 * This only resets the data structures to "empty" state, initialized the
 * locks protecting the structures. Restoring the last values from the GTM
 * control file (written on shutdown) is handled elsewhere.
 */
void
GTM_InitTxnManager(void)
{
	int ii;

	memset(&GTMTransactions, 0, sizeof (GTM_Transactions));

	for (ii = 0; ii < GTM_MAX_GLOBAL_TRANSACTIONS; ii++)
	{
		GTM_TransactionInfo *gtm_txninfo = &GTMTransactions.gt_transactions_array[ii];
		gtm_txninfo->gti_in_use = false;
		GTM_RWLockInit(&gtm_txninfo->gti_lock);
	}

	/*
	 * XXX When GTM is stopped and restarted, it must start assinging GXIDs
	 * greater than the previously assigned values. If it was a clean shutdown,
	 * the GTM can store the last assigned value at a known location on
	 * permanent storage and read it back when it's restarted. It will get
	 * trickier for GTM failures.
	 *
	 * Restarts after a clean shutdown is handled by GTM_RestoreTxnInfo.
	 */
	GTMTransactions.gt_nextXid = FirstNormalGlobalTransactionId;

	/*
	 * XXX The gt_oldestXid is the cluster level oldest Xid
	 */
	GTMTransactions.gt_oldestXid = FirstNormalGlobalTransactionId;

	/*
	 * XXX Compute various xid limits to avoid wrap-around related database
	 * corruptions. Again, this is not implemented for the prototype
	 */
	GTMTransactions.gt_xidVacLimit = InvalidGlobalTransactionId;
	GTMTransactions.gt_xidWarnLimit = InvalidGlobalTransactionId;
	GTMTransactions.gt_xidStopLimit = InvalidGlobalTransactionId;
	GTMTransactions.gt_xidWrapLimit = InvalidGlobalTransactionId;

	/*
	 * XXX Newest XID that is committed or aborted
	 */
	GTMTransactions.gt_latestCompletedXid = FirstNormalGlobalTransactionId;

	/* Initialise gt_recent_global_xmin */
	GTMTransactions.gt_recent_global_xmin = FirstNormalGlobalTransactionId;

	/*
	 * Initialize the locks to protect various XID fields as well as the linked
	 * list of transactions
	 */
	GTM_RWLockInit(&GTMTransactions.gt_XidGenLock);
	GTM_RWLockInit(&GTMTransactions.gt_TransArrayLock);

	/*
	 * Initialize the list
	 */
	GTMTransactions.gt_open_transactions = gtm_NIL;
	GTMTransactions.gt_lastslot = -1;

	GTMTransactions.gt_gtm_state = GTM_STARTING;

	ControlXid = FirstNormalGlobalTransactionId;

	return;
}

/*
 * GTM_GXIDToHandle_Internal
 *		Given the GXID, find handle of the corresponding global transaction.
 *
 * We simply walk the list of open transactions until we find a match.
 *
 * XXX I wonder if this might be an issue, as the search is linear and we
 * may have up to 16k global transactions (by default). In that case we
 * should change this to use a hash table (or so) to speed the lookup.
 */
static GTM_TransactionHandle
GTM_GXIDToHandle_Internal(GlobalTransactionId gxid, bool warn)
{
	gtm_ListCell *elem = NULL;
   	GTM_TransactionInfo *gtm_txninfo = NULL;

	if (!GlobalTransactionIdIsValid(gxid))
		return InvalidTransactionHandle;

	GTM_RWLockAcquire(&GTMTransactions.gt_TransArrayLock, GTM_LOCKMODE_READ);

	gtm_foreach(elem, GTMTransactions.gt_open_transactions)
	{
		gtm_txninfo = (GTM_TransactionInfo *)gtm_lfirst(elem);
		if (GlobalTransactionIdEquals(gtm_txninfo->gti_gxid, gxid))
			break;
		gtm_txninfo = NULL;
	}

	GTM_RWLockRelease(&GTMTransactions.gt_TransArrayLock);

	if (gtm_txninfo != NULL)
		return gtm_txninfo->gti_handle;
	else
	{
		if (warn)
			ereport(WARNING,
				(ERANGE, errmsg("No transaction handle for gxid: %d",
								gxid)));
		return InvalidTransactionHandle;
	}
}

/*
 * GTM_GXIDToHandle
 *		Given the GXID, find handle of the corresponding global transaction.
 *
 * If the GXID is not found, returns InvalidTransactionHandle (and emits a
 * warning).
 */
GTM_TransactionHandle
GTM_GXIDToHandle(GlobalTransactionId gxid)
{
	return GTM_GXIDToHandle_Internal(gxid, true);
}

/*
 * GTM_GlobalSessionIDToHandle
 *		Given ID of a global session, find ID of the global transaction.
 *
 * Returns InvalidTransactionHandle for empty session ID (NULL or '\0'),
 * as well as for unknown session IDs.
 *
 * XXX Similarly to GTM_GXIDToHandle_Internal, the search is simply a loop
 * over gt_open_transactions, so it might be causing performance issues.
 * Especially as this is used in GTM_BeginTransactionMulti.
 */
static GTM_TransactionHandle
GTM_GlobalSessionIDToHandle(const char *global_sessionid)
{
	gtm_ListCell *elem = NULL;
	GTM_TransactionInfo	*gtm_txninfo = NULL;

	if (global_sessionid == NULL || global_sessionid[0] == '\0')
		return InvalidTransactionHandle;

	gtm_foreach(elem, GTMTransactions.gt_open_transactions)
	{
		gtm_txninfo = (GTM_TransactionInfo *)gtm_lfirst(elem);
		if (strcmp(gtm_txninfo->gti_global_session_id, global_sessionid) == 0)
			break;
		gtm_txninfo = NULL;
	}
	if (gtm_txninfo != NULL)
		return gtm_txninfo->gti_handle;

	return InvalidTransactionHandle;
}

/*
 * GTM_IsGXIDInProgress
 *	Determines if a global transaction with a given GXID is still in progress.
 *
 * Returns true when the GXID is still in progress (exists in gt_open_transactions),
 * false otherwise.
 */
static bool
GTM_IsGXIDInProgress(GlobalTransactionId gxid)
{
	return (GTM_GXIDToHandle_Internal(gxid, false) !=
			InvalidTransactionHandle);
}
/*
 * GTM_GIDToHandle
 *		Find transaction handle for a given the GID (prepared transaction).
 *
 * XXX Similarly to GTM_GXIDToHandle_Internal the search is simply a loop
 * over gt_open_transactions, so might be subject performance issues.
 */
static GTM_TransactionHandle
GTM_GIDToHandle(const char *gid)
{
	gtm_ListCell *elem = NULL;
	GTM_TransactionInfo *gtm_txninfo = NULL;

	GTM_RWLockAcquire(&GTMTransactions.gt_TransArrayLock, GTM_LOCKMODE_READ);

	gtm_foreach(elem, GTMTransactions.gt_open_transactions)
	{
		gtm_txninfo = (GTM_TransactionInfo *)gtm_lfirst(elem);
		if (gtm_txninfo->gti_gid && strcmp(gid,gtm_txninfo->gti_gid) == 0)
			break;
		gtm_txninfo = NULL;
	}

	GTM_RWLockRelease(&GTMTransactions.gt_TransArrayLock);

	if (gtm_txninfo != NULL)
		return gtm_txninfo->gti_handle;

	/* Print warning for unknown global session IDs. */
	ereport(WARNING,
		(ERANGE, errmsg("No transaction handle for prepared transaction ID: '%s'",
						gid)));

	return InvalidTransactionHandle;
}


/*
 * GTM_HandleToTransactionInfo
 *		Given a transaction handle, find the transaction info structure.
 *
 * The transaction is expected to be still in use, so we emit a WARNING if
 * that's not the case.
 *
 * Note: Since a transaction handle is just an index into the global array,
 * this function should be very quick. We should turn into an inline future
 * for fast path.
 */
GTM_TransactionInfo *
GTM_HandleToTransactionInfo(GTM_TransactionHandle handle)
{
	GTM_TransactionInfo *gtm_txninfo = NULL;

	if ((handle < 0) || (handle > GTM_MAX_GLOBAL_TRANSACTIONS))
	{
		ereport(WARNING,
				(ERANGE, errmsg("Invalid transaction handle: %d", handle)));
		return NULL;
	}

	gtm_txninfo = &GTMTransactions.gt_transactions_array[handle];

	if (!gtm_txninfo->gti_in_use)
	{
		ereport(WARNING,
				(ERANGE, errmsg("Invalid transaction handle (%d), txn_info not in use",
								handle)));
		return NULL;
	}

	return gtm_txninfo;
}


/*
 * GTM_RemoveTransInfoMulti
 *		Remove multiple transactions from the list of open global transactions.
 *
 * If the calling thread does not have enough cached structures, we in fact keep
 * the structure in the global array and also add it to the list of cached
 * structures for this thread. This ensures that the next transaction starting
 * in this thread can quickly get a free slot in the array of transactions and
 * also avoid repeated malloc/free of the structures.
 *
 * Also updates the gt_latestCompletedXid.
 *
 * XXX We seem to be doing a new linear search for each transaction, which seems
 * rather expensive. We could simply walk gt_open_transactions once and use
 * gtm_list_delete_cell similarly to GTM_RemoveAllTransInfos.
 */
static void
GTM_RemoveTransInfoMulti(GTM_TransactionInfo *gtm_txninfo[], int txn_count)
{
	int ii;

	/*
	 * Remove the transaction structure from the global list of open
	 * transactions
	 */
	GTM_RWLockAcquire(&GTMTransactions.gt_TransArrayLock, GTM_LOCKMODE_WRITE);

	for (ii = 0; ii < txn_count; ii++)
	{
		if (gtm_txninfo[ii] == NULL)
			continue;

		GTMTransactions.gt_open_transactions = gtm_list_delete(GTMTransactions.gt_open_transactions, gtm_txninfo[ii]);

		/*
		 * If this transaction is newer than the current gt_latestCompletedXid,
		 * then use the gti_gxid instead.
		 */
		if (GlobalTransactionIdIsNormal(gtm_txninfo[ii]->gti_gxid) &&
			GlobalTransactionIdFollowsOrEquals(gtm_txninfo[ii]->gti_gxid,
											   GTMTransactions.gt_latestCompletedXid))
			GTMTransactions.gt_latestCompletedXid = gtm_txninfo[ii]->gti_gxid;

		elog(DEBUG1, "GTM_RemoveTransInfoMulti: removing transaction id %u, %u, handle (%d)",
				gtm_txninfo[ii]->gti_gxid, gtm_txninfo[ii]->gti_client_id,
				gtm_txninfo[ii]->gti_handle);

		/*
		 * Do cleanup of objects (in particular sequences) modified by this
		 * transaction. What exactly happens depends on whether the transaction
		 * committed or aborted.
		 */
		GTM_TransactionInfo_Clean(gtm_txninfo[ii]);
	}

	GTM_RWLockRelease(&GTMTransactions.gt_TransArrayLock);
}

/*
 * GTM_RemoveAllTransInfos
 *		Remove information about all transactions associated with a client/backend.
 *
 * Removes all transactions associated with a specified client/backend from
 * the global transaction array (GTMTransactions.gt_open_transactions).
 *
 * Ignores transactions in GTM_TXN_PREPARED and GTM_TXN_PREPARE_IN_PROGRESS
 * states - those must not be removed, and will be committed by a different
 * thread (using a GID).
 *
 * Also updates the gt_latestCompletedXid.
 */
void
GTM_RemoveAllTransInfos(uint32 client_id, int backend_id)
{
	gtm_ListCell *cell, *prev;

	elog(DEBUG1, "GTM_RemoveAllTransInfos: removing transactions for client %u backend %d",
				 client_id, backend_id);

	/*
	 * Scan the global list of open transactions
	 */
	GTM_RWLockAcquire(&GTMTransactions.gt_TransArrayLock, GTM_LOCKMODE_WRITE);

	prev = NULL;
	cell = gtm_list_head(GTMTransactions.gt_open_transactions);
	while (cell != NULL)
	{
		GTM_TransactionInfo *gtm_txninfo = gtm_lfirst(cell);
		/*
		 * Check if current entry is associated with the thread
		 * A transaction in prepared state has to be kept alive in the structure.
		 * It will be committed by another thread than this one.
		 */
		if ((gtm_txninfo->gti_in_use) &&
			(gtm_txninfo->gti_state != GTM_TXN_PREPARED) &&
			(gtm_txninfo->gti_state != GTM_TXN_PREPARE_IN_PROGRESS) &&
			(GTM_CLIENT_ID_EQ(gtm_txninfo->gti_client_id, client_id)) &&
			((gtm_txninfo->gti_proxy_client_id == backend_id) || (backend_id == -1)))
		{
			/* remove the entry */
			GTMTransactions.gt_open_transactions = gtm_list_delete_cell(GTMTransactions.gt_open_transactions, cell, prev);

			/* update the latestCompletedXid */
			if (GlobalTransactionIdIsNormal(gtm_txninfo->gti_gxid) &&
				GlobalTransactionIdFollowsOrEquals(gtm_txninfo->gti_gxid,
												   GTMTransactions.gt_latestCompletedXid))
				GTMTransactions.gt_latestCompletedXid = gtm_txninfo->gti_gxid;

			elog(DEBUG1, "GTM_RemoveAllTransInfos: removing transaction id %u, %u:%u %d:%d",
					gtm_txninfo->gti_gxid, gtm_txninfo->gti_client_id,
					client_id, gtm_txninfo->gti_proxy_client_id, backend_id);
			/*
			 * Now mark the transaction as aborted and mark the structure as not-in-use
			 */
			GTM_TransactionInfo_Clean(gtm_txninfo);

			/* move to next cell in the list */
			if (prev)
				cell = gtm_lnext(prev);
			else
				cell = gtm_list_head(GTMTransactions.gt_open_transactions);
		}
		else
		{
			prev = cell;
			cell = gtm_lnext(cell);
		}
	}

	GTM_RWLockRelease(&GTMTransactions.gt_TransArrayLock);
}

/*
 * GTMGetLastClientIdentifier
 *		Get the latest client identifier assigned to currently open transactions.
 *
 * Remember this may not be the latest identifier issued by the old master, but
 * we won't acknowledge client identifiers larger than what we are about to
 * compute. Any such identifiers will be overwritten new identifiers issued
 * by the new master.
 *
 * XXX Another linear search over gt_open_transactions. Perhaps we could eliminate
 * most of the searches by updating the value whenever we generate a higher value,
 * and only doing the search when the client with the highest ID terminates.
 *
 * XXX What happens when the value wraps around, which is what GTM_CLIENT_ID_NEXT
 * apparently does? If we ignore identifiers higher than the value, isn't that an
 * issue?
 */
uint32
GTM_GetLastClientIdentifier(void)
{
	gtm_ListCell *cell;
	uint32 last_client_id = 0;

	/*
	 * Scan the global list of open transactions
	 */
	GTM_RWLockAcquire(&GTMTransactions.gt_TransArrayLock, GTM_LOCKMODE_WRITE);

	cell = gtm_list_head(GTMTransactions.gt_open_transactions);
	while (cell != NULL)
	{
		GTM_TransactionInfo *gtm_txninfo = gtm_lfirst(cell);

		if (GTM_CLIENT_ID_GT(gtm_txninfo->gti_client_id, last_client_id))
			last_client_id = gtm_txninfo->gti_client_id;

		cell = gtm_lnext(cell);
	}

	GTM_RWLockRelease(&GTMTransactions.gt_TransArrayLock);

	elog(DEBUG1, "GTMGetLastClientIdentifier: last client ID %u", last_client_id);

	return last_client_id;
}

/*
 * GTM_SetDoVacuum
 *		Mark a given transaction (identified by a transaction handle) as VACUUM.
 *
 * Matters for GTM_GetTransactionSnapshot, which ignores lazy vacuums when
 * building transaction snapshot
 *
 * Fails with an ERROR when the transaction handle does not exist.
 */
static bool
GTM_SetDoVacuum(GTM_TransactionHandle handle)
{
	GTM_TransactionInfo *gtm_txninfo = GTM_HandleToTransactionInfo(handle);

	if (gtm_txninfo == NULL)
		ereport(ERROR, (EINVAL, errmsg("Invalid transaction handle")));

	gtm_txninfo->gti_vacuum = true;
	return true;
}

/*
 * GTM_GetGlobalTransactionIdMulti
 *		Allocate GXID for a list of transaction handles.
 *
 * The function accepts an array of transaction handles with txn_count elements,
 * some of which may already have GXID assigned. Such handles (that already had
 * GXID assigned) are skipped and we don't try to assign a new GXID to them.
 *
 * For we handles without a GXID, the function assigns a GXID, and tracks the
 * handle to new_handles, so that the caller can easily identify which handles
 * were modified.
 *
 * The output array 'gxids' should contain GXIDs for all handles (even those
 * that had GXID assigned before calling this function).
 *
 * That means both 'gxids' and 'new_handles' should have space for at least
 * txn_count elements, but 'new_handles' may use only some of the space.
 *
 * Input:
 *		handles			- transactions to assing GXID to
 * 		txn_count		- number of handles in 'handles' array
 *
 * Output:
 * 		gxids			- array of newly assigned GXIDs
 * 		new_handles		- array of handles with newly assigned GXIDs
 * 		new_txn_count	- number of newly assigned GXIDs (and number of elements
 * 						in new_handles)
 */
static bool
GTM_GetGlobalTransactionIdMulti(GTM_TransactionHandle handles[], int txn_count,
		GlobalTransactionId gxids[], GTM_TransactionHandle new_handles[],
		int *new_txn_count)
{
	GlobalTransactionId xid = InvalidGlobalTransactionId;
	GTM_TransactionInfo *gtm_txninfo = NULL;
	int ii;
	int new_handles_count = 0;
	bool save_control = false;

	elog(DEBUG1, "GTM_GetGlobalTransactionIdMulti: generate GXIDs for %d transactions", txn_count);

	/* gxids is required parameter (we always return the GXID) */
	Assert(gxids != NULL);

	/* either both new_handles and new_txn_count, or neiter of them */
	Assert((new_handles && new_txn_count) || (!new_handles && !new_txn_count));

	/* GTM standby can only receive GXID from the GTM master */
	if (Recovery_IsStandby())
	{
		ereport(ERROR, (EINVAL, errmsg("GTM is running in STANDBY mode -- can not issue new transaction ids")));
		return false;
	}

	GTM_RWLockAcquire(&GTMTransactions.gt_XidGenLock, GTM_LOCKMODE_WRITE);

	if (GTMTransactions.gt_gtm_state == GTM_SHUTTING_DOWN)
	{
		GTM_RWLockRelease(&GTMTransactions.gt_XidGenLock);
		ereport(ERROR, (EINVAL, errmsg("GTM shutting down -- can not issue new transaction ids")));
		return false;
	}

	/*
	 * Now generate a GXID to hadles that do now have a GXID assigned yet.
	 */
	for (ii = 0; ii < txn_count; ii++)
	{
		gtm_txninfo = GTM_HandleToTransactionInfo(handles[ii]);
		Assert(gtm_txninfo);

		if (GlobalTransactionIdIsValid(gtm_txninfo->gti_gxid))
		{
			gxids[ii] = gtm_txninfo->gti_gxid;
			elog(DEBUG1, "GTM_TransactionInfo has XID already assgined - %s:%d",
					gtm_txninfo->gti_global_session_id, gxids[ii]);
			continue;
		}

		xid = GTMTransactions.gt_nextXid;

		/*----------
		 * Check to see if it's safe to assign another XID.  This protects against
		 * catastrophic data loss due to XID wraparound.  The basic rules are:
		 *
		 * If we're past xidVacLimit, start trying to force autovacuum cycles.
		 * If we're past xidWarnLimit, start issuing warnings.
		 * If we're past xidStopLimit, refuse to execute transactions, unless
		 * we are running in a standalone backend (which gives an escape hatch
		 * to the DBA who somehow got past the earlier defenses).
		 *
		 * Test is coded to fall out as fast as possible during normal operation,
		 * ie, when the vac limit is set and we haven't violated it.
		 *----------
		 */
		if (GlobalTransactionIdFollowsOrEquals(xid, GTMTransactions.gt_xidVacLimit) &&
			GlobalTransactionIdIsValid(GTMTransactions.gt_xidVacLimit))
		{
			if (GlobalTransactionIdFollowsOrEquals(xid, GTMTransactions.gt_xidStopLimit))
			{
				GTM_RWLockRelease(&GTMTransactions.gt_XidGenLock);
				ereport(ERROR,
						(ERANGE,
						 errmsg("database is not accepting commands to avoid wraparound data loss in database ")));
			}
			else if (GlobalTransactionIdFollowsOrEquals(xid, GTMTransactions.gt_xidWarnLimit))
				ereport(WARNING,
				(errmsg("database must be vacuumed within %u transactions",
						GTMTransactions.gt_xidWrapLimit - xid)));
		}

		GlobalTransactionIdAdvance(GTMTransactions.gt_nextXid);

		elog(DEBUG1, "Assigning new transaction ID = %s:%d",
				gtm_txninfo->gti_global_session_id, xid);

		gxids[ii] = gtm_txninfo->gti_gxid = xid;

		/* only return the new handles when requested */
		if (new_handles)
			new_handles[new_handles_count++] = gtm_txninfo->gti_handle;
	}

	/*
	 * Periodically write the xid and sequence info out to the control file.
	 * Try and handle wrapping, too.
	 */
	if (GlobalTransactionIdIsValid(xid) &&
			(xid - ControlXid > CONTROL_INTERVAL || xid < ControlXid))
	{
		save_control = true;
		ControlXid = xid;
	}

	if (GTM_NeedXidRestoreUpdate())
		GTM_SetNeedBackup();

	GTM_RWLockRelease(&GTMTransactions.gt_XidGenLock);

	/* save control info when not holding the XidGenLock */
	if (save_control)
		SaveControlInfo();

	if (new_txn_count)
		*new_txn_count = new_handles_count;

	elog(DEBUG1, "GTM_GetGlobalTransactionIdMulti: assigned %d new GXIDs for %d handles",
				 new_handles_count, txn_count);

	return true;
}

/*
 * GTM_GetGlobalTransactionId
 *		Allocate GXID for a new transaction.
 *
 * The new GXID is stored into the transaction info structure of the given
 * transaction before returning (not just returned).
 */
GlobalTransactionId
GTM_GetGlobalTransactionId(GTM_TransactionHandle handle)
{
	GlobalTransactionId gxid = InvalidGlobalTransactionId;

	GTM_GetGlobalTransactionIdMulti(&handle, 1, &gxid, NULL, NULL);

	elog(DEBUG1, "GTM_GetGlobalTransactionId: assigned new GXID %u", gxid);

	Assert(GlobalTransactionIdIsValid(gxid));

	return gxid;
}

/*
 * GTM_ReadNewGlobalTransactionId
 *		Reads nextXid, but do not allocate it (advance to te next one).
 */
GlobalTransactionId
GTM_ReadNewGlobalTransactionId(void)
{
	GlobalTransactionId xid;

	GTM_RWLockAcquire(&GTMTransactions.gt_XidGenLock, GTM_LOCKMODE_READ);
	xid = GTMTransactions.gt_nextXid;
	GTM_RWLockRelease(&GTMTransactions.gt_XidGenLock);

	return xid;
}

/*
 * GTM_SetNextGlobalTransactionId
 *		Set the next global XID.
 *
 * The GXID is usually read from a control file and set when the GTM is
 * started. When the GTM is finally shutdown, the next to-be-assigned GXID
 * is stored in the control file.
 *
 * The function also switches the GTM from 'starting' to 'running' state.
 *
 * This is handled by gtm_backup.c.  Anyway, because this function is to be
 * called by GTM_RestoreTransactionId() and the backup will be performed
 * afterwards, we don't care the new value of GTMTransactions.gt_nextXid here
 * (it may even be invalid or stale).
 *
 * XXX We don't yet handle any crash recovery. So if the GTM did not shutdown
 * cleanly, it's not quite sure what'll happen.
 */
void
GTM_SetNextGlobalTransactionId(GlobalTransactionId gxid)
{
	/* we should only be calling this during GTM startup */
	Assert(GTMTransactions.gt_gtm_state == GTM_STARTING);

	GTM_RWLockAcquire(&GTMTransactions.gt_XidGenLock, GTM_LOCKMODE_WRITE);
	GTMTransactions.gt_nextXid = gxid;
	GTMTransactions.gt_gtm_state = GTM_RUNNING;
	GTM_RWLockRelease(&GTMTransactions.gt_XidGenLock);

	return;
}

/*
 * GTM_SetControlXid
 *		Sets the control GXID.
 */
void
GTM_SetControlXid(GlobalTransactionId gxid)
{
	elog(DEBUG1, "GTM_SetControlXid: setting control GXID %u", gxid);
	ControlXid = gxid;
}

/*
 * GTM_BeginTransactionMulti
 *		Starts transactions on provided global sessions, if needed.
 *
 * If there already is an open transaction on a global session, the existing
 * transaction handle is reused.
 *
 * The transaction handles are initialized in the txns[] array, and the
 * number of elements is returned (in general it will be equal to txn_count).
 *
 * Input:
 *		isolevel[]			- requested isolation levels
 *		readonly[]			- flags for read-only sessions
 *		global_sessionid[]	- IDs of global sessions
 *		connid[]			- IDs of proxy connections
 *		txn_count			- number of sessions/transactions
 *
 * Output:
 *		txns[]				- initialized transaction handles
 *
 * Returns number of transaction handles returned in txns[] array.
 *
 * The caller is responsible for ensuring the input/output arrays are
 * correctly sized (all should have at least txn_count elements).
 *
 * XXX The transaction handles are allocated in TopMostMemoryContext.
 */
static int
GTM_BeginTransactionMulti(GTM_IsolationLevel isolevel[],
					 bool readonly[],
					 const char *global_sessionid[],
					 GTMProxy_ConnID connid[],
					 int txn_count,
					 GTM_TransactionHandle txns[])
{
	GTM_TransactionInfo *gtm_txninfo[txn_count];
	MemoryContext oldContext;
	int kk;

	/* make sure we received all the required array paremeters */
	Assert(isolevel && readonly && global_sessionid && txns && connid);

	memset(gtm_txninfo, 0, sizeof (gtm_txninfo));

	/*
	 * XXX We should allocate the transaction info structure in the
	 * top-most memory context instead of a thread context. This is
	 * necessary because the transaction may outlive the thread which
	 * started the transaction. Also, since the structures are stored in
	 * the global array, it's dangerous to free the structures themselves
	 * without removing the corresponding references from the global array
	 */
	oldContext = MemoryContextSwitchTo(TopMostMemoryContext);

	GTM_RWLockAcquire(&GTMTransactions.gt_TransArrayLock, GTM_LOCKMODE_WRITE);

	for (kk = 0; kk < txn_count; kk++)
	{
		int ii, jj, startslot;
		GTM_TransactionHandle txn =
				GTM_GlobalSessionIDToHandle(global_sessionid[kk]);

		/*
		 * If tere already is a transaction open on the global session, reuse
		 * it and continue with the next one.
		 */
		if (txn != InvalidTransactionHandle)
		{
			gtm_txninfo[kk] = GTM_HandleToTransactionInfo(txn);
			elog(DEBUG1, "Existing transaction found: %s:%d",
					gtm_txninfo[kk]->gti_global_session_id,
					gtm_txninfo[kk]->gti_gxid);
			txns[kk] = txn;
			continue;
		}

		/*
		 * We had no cached slots. Now find a free slot in the transaction array
		 * and store the new transaction info structure there.
		 *
		 * When looking for a new empty slot in the transactions array, we do not
		 * start at index 0 as the transaction are likely squashed there. Instead
		 * we track the ID of the last assigned slot (gt_lastslot), and start from
		 * that index. We do exactly GTM_MAX_GLOBAL_TRANSACTIONS steps, so we may
		 * walk the whole array in the worst case (everything is full).
		 *
		 * The assumptiion is that the "oldest" slots will be eventually freed, so
		 * when we get back to them (after about GTM_MAX_GLOBAL_TRANSACTIONS
		 * transaction), the slots will be free again.
		 *
		 * XXX This will degrade with many open global transactions, as the array
		 * gets "more full". In that case we could perhaps track the free slots
		 * in a freelist (similarly to gt_open_transactions), or something.
		 *
		 * XXX We could also track the number of assigned slots, to quickly detect
		 * when there are no free slots. But that seems unlikely.
		 */
		startslot = GTMTransactions.gt_lastslot + 1;
		if (startslot >= GTM_MAX_GLOBAL_TRANSACTIONS)
			startslot = 0;

		for (ii = startslot, jj = 0;
			 jj < GTM_MAX_GLOBAL_TRANSACTIONS;
			 ii = (ii + 1) % GTM_MAX_GLOBAL_TRANSACTIONS, jj++)
		{
			if (GTMTransactions.gt_transactions_array[ii].gti_in_use == false)
			{
				gtm_txninfo[kk] = &GTMTransactions.gt_transactions_array[ii];
				break;
			}

			/*
			 * We got back to the starting point, and haven't found any free slot.
			 * That means we have reached GTM_MAX_GLOBAL_TRANSACTIONS.
			 */
			if (ii == GTMTransactions.gt_lastslot)
			{
				GTM_RWLockRelease(&GTMTransactions.gt_TransArrayLock);
				ereport(ERROR,
						(ERANGE, errmsg("Max global transactions limit reached (%d)",
										GTM_MAX_GLOBAL_TRANSACTIONS)));
			}
		}

		GTM_TransactionInfo_Init(gtm_txninfo[kk], ii, isolevel[kk],
				GetMyThreadInfo->thr_client_id, connid[kk],
				global_sessionid[kk],
				readonly[kk]);

		/* remember which slot we used for the next loop */
		GTMTransactions.gt_lastslot = ii;

		txns[kk] = ii;

		/*
		 * Add the structure to the global list of open transactions. We should
		 * call add the element to the list in the context of TopMostMemoryContext
		 * because the list is global and any memory allocation must outlive the
		 * thread context
		 */
		GTMTransactions.gt_open_transactions = gtm_lappend(GTMTransactions.gt_open_transactions, gtm_txninfo[kk]);
	}

	GTM_RWLockRelease(&GTMTransactions.gt_TransArrayLock);

	MemoryContextSwitchTo(oldContext);

	return txn_count;
}

/*
 * GTM_BeginTransaction
 *		Starts transaction on provided global session.
 *
 * If there already is an open transaction on the global session, the
 * existing transaction handle is reused.
 *
 * Input:
 *		isolevel			- requested isolation level
 *		readonly			- should the transaction be read-only
 *		global_sessionid	- ID of theh global session
 *
 * Returns an initialized transaction handle.
 *
 * XXX The transaction handle is allocated in TopMostMemoryContext.
 */
static GTM_TransactionHandle
GTM_BeginTransaction(GTM_IsolationLevel isolevel,
					 bool readonly,
					 const char *global_sessionid)
{
	GTM_TransactionHandle txn;
	GTMProxy_ConnID connid = -1;

	GTM_BeginTransactionMulti(&isolevel, &readonly, &global_sessionid, &connid, 1, &txn);

	return txn;
}

/*
 * GTM_TransactionInfo_Init
 *		Initialize info about a transaction and store it in global array.
 */
static void
GTM_TransactionInfo_Init(GTM_TransactionInfo *gtm_txninfo,
						 GTM_TransactionHandle txn,
						 GTM_IsolationLevel isolevel,
						 uint32 client_id,
						 GTMProxy_ConnID connid,
						 const char *global_sessionid,
						 bool readonly)
{
	gtm_txninfo->gti_gxid = InvalidGlobalTransactionId;
	gtm_txninfo->gti_xmin = InvalidGlobalTransactionId;
	gtm_txninfo->gti_state = GTM_TXN_STARTING;

	gtm_txninfo->gti_isolevel = isolevel;
	gtm_txninfo->gti_readonly = readonly;
	gtm_txninfo->gti_in_use = true;

	if (global_sessionid)
		strncpy(gtm_txninfo->gti_global_session_id, global_sessionid,
				GTM_MAX_SESSION_ID_LEN);
	else
		gtm_txninfo->gti_global_session_id[0] = '\0';

	gtm_txninfo->nodestring = NULL;
	gtm_txninfo->gti_gid = NULL;

	gtm_txninfo->gti_handle = txn;
	gtm_txninfo->gti_vacuum = false;

	/*
	 * For every new transaction that gets created, we track two important
	 * identifiers:
	 *
	 * gt_client_id: is the identifier assigned to the client connected to
	 * GTM. Whenever a connection to GTM is dropped, we must clean up all
	 * transactions opened by that client. Since we track all open transactions
	 * in a global data structure, this identifier helps us to identify
	 * client-specific transactions. Also, the identifier is issued and tracked
	 * irrespective of whether the remote client is a GTM proxy or a PG
	 * backend.
	 *
	 * gti_proxy_client_id: is the identifier assigned by the GTM proxy to its
	 * client. Proxy sends us this identifier and we track it in the list of
	 * open transactions. If a backend disconnects from the proxy, it sends us
	 * a MSG_BACKEND_DISCONNECT message, along with the backend identifier. As
	 * a response to that message, we clean up all the transactions opened by
	 * the backend.
	 */ 
	gtm_txninfo->gti_client_id = client_id;
	gtm_txninfo->gti_proxy_client_id = connid;
}


/*
 * GTM_TransactionInfo_Clean
 *		Mark a transaction slot as empty and release memory.
 *
 * Most of the cleanup is about dealing with sequences modified in the
 * transaction, and what exactly needs to happen depends on whether the
 * transaction is being committed or aborted.
 *
 * XXX We do not pfree the txid array of the snapshot, which may be referenced
 * by by multiple transactions. But we should never really have more than
 * GTM_MAX_GLOBAL_TRANSACTIONS of them (with 16k transactions, that's about
 * 1GB of RAM).
 *
 * XXX Do we expect this being called only for transactions that are currently
 * being aborted/committed, or in other states too (for example "starting")?
 */
static void
GTM_TransactionInfo_Clean(GTM_TransactionInfo *gtm_txninfo)
{
	gtm_ListCell *lc;

	if (gtm_txninfo->gti_state == GTM_TXN_ABORT_IN_PROGRESS)
	{
		/*
		 * First drop any sequences created in this transaction. We must do
		 * this before restoring any dropped sequences because the new sequence
		 * may have reused old name
		 */
		gtm_foreach(lc, gtm_txninfo->gti_created_seqs)
		{
			GTM_SeqRemoveCreated(gtm_lfirst(lc));
		}

		/*
		 * Restore dropped sequences to their original state
		 */
		gtm_foreach(lc, gtm_txninfo->gti_dropped_seqs)
		{
			GTM_SeqRestoreDropped(gtm_lfirst(lc));
		}

		/*
		 * Restore altered sequences to their original state
		 */
		gtm_foreach(lc, gtm_txninfo->gti_altered_seqs)
		{
			GTM_SeqRestoreAltered(gtm_lfirst(lc));
		}

	}
	else if (gtm_txninfo->gti_state == GTM_TXN_COMMIT_IN_PROGRESS)
	{
		/*
		 * Remove sequences dropped in this transaction permanently. No action
		 * needed for sequences created in this transaction
		 */
		gtm_foreach(lc, gtm_txninfo->gti_dropped_seqs)
		{
			GTM_SeqRemoveDropped(gtm_lfirst(lc));
		}
		/*
		 * Remove original copies of sequences altered in this transaction
		 * permanently. The altered copies stay.
		 */
		gtm_foreach(lc, gtm_txninfo->gti_altered_seqs)
		{
			GTM_SeqRemoveAltered(gtm_lfirst(lc));
		}

	}

	gtm_list_free(gtm_txninfo->gti_created_seqs);
	gtm_list_free(gtm_txninfo->gti_dropped_seqs);
	gtm_list_free(gtm_txninfo->gti_altered_seqs);

	gtm_txninfo->gti_dropped_seqs = gtm_NIL;
	gtm_txninfo->gti_created_seqs = gtm_NIL;
	gtm_txninfo->gti_altered_seqs = gtm_NIL;

	gtm_txninfo->gti_state = GTM_TXN_ABORTED;
	gtm_txninfo->gti_in_use = false;
	gtm_txninfo->gti_snapshot_set = false;

	if (gtm_txninfo->gti_gid)
	{
		pfree(gtm_txninfo->gti_gid);
		gtm_txninfo->gti_gid = NULL;
	}
	if (gtm_txninfo->nodestring)
	{
		pfree(gtm_txninfo->nodestring);
		gtm_txninfo->nodestring = NULL;
	}
}

/*
 * GTM_BkupBeginTransactionMulti
 *		Open multiple transactions on provided global sessions.
 *
 * XXX I'm not sure why we need this when GTM_BeginTransactionMulti does the
 * same thing (and it allocates everything in TopMostMemoryContext too).
 * Maybe that we fail if some of the transactions fail to start?
 */
static void
GTM_BkupBeginTransactionMulti(GTM_IsolationLevel isolevel[],
							  bool readonly[],
							  const char *global_sessionid[],
							  uint32 client_id[],
							  GTMProxy_ConnID connid[],
							  int txn_count)
{
	GTM_TransactionHandle txn[GTM_MAX_GLOBAL_TRANSACTIONS];
	MemoryContext oldContext;
	int count;

	oldContext = MemoryContextSwitchTo(TopMostMemoryContext);

	count = GTM_BeginTransactionMulti(isolevel, readonly,
									  global_sessionid, connid,
									  txn_count, txn);
	if (count != txn_count)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to start %d new transactions", txn_count)));

	MemoryContextSwitchTo(oldContext);
}

/*
 * GTM_BkupBeginTransaction
 *		Starts transaction on provided global session.
 *
 * XXX I'm not sure why we need this when GTM_BeginTransaction does the
 * same thing (and it allocates everything in TopMostMemoryContext too).
 */
static void
GTM_BkupBeginTransaction(GTM_IsolationLevel isolevel,
						 bool readonly,
						 const char *global_sessionid,
						 uint32 client_id)
{
	GTMProxy_ConnID connid = -1;

	GTM_BkupBeginTransactionMulti(&isolevel, &readonly,
			&global_sessionid,
			&client_id, &connid, 1);
}

/*
 * GTM_RollbackTransactionMulti
 *		Rollback multiple global transactions (handles) in one go.
 *
 * The function expects txn_count handles to be supplied in the txn[] array.
 * We first mark all transactions as GTM_TXN_ABORT_IN_PROGRESS and then
 * remove them.
 *
 * Rollback status for each of supplied transaction handle is returned in the
 * status[] array (so it has to have space for at least txn_count elements).
 * If a handle is not provided (it's NULL in txn[]), the matching status will
 * be set to STATUS_ERROR.
 *
 * The function returns txn_count, that is the number of supplied handles.
 */
static int
GTM_RollbackTransactionMulti(GTM_TransactionHandle txn[], int txn_count, int status[])
{
	GTM_TransactionInfo *gtm_txninfo[txn_count];
	int ii;

	ereport(DEBUG1,
		(ERANGE, errmsg("GTM_RollbackTransactionMulti: rollbing back %d transactions",
						txn_count)));

	for (ii = 0; ii < txn_count; ii++)
	{
		gtm_txninfo[ii] = GTM_HandleToTransactionInfo(txn[ii]);

		if (gtm_txninfo[ii] == NULL)
		{
			status[ii] = STATUS_ERROR;
			continue;
		}

		/*
		 * Mark the transaction as being aborted. We need to acquire the lock
		 * on that transaction to do that.
		 */
		GTM_RWLockAcquire(&gtm_txninfo[ii]->gti_lock, GTM_LOCKMODE_WRITE);
		gtm_txninfo[ii]->gti_state = GTM_TXN_ABORT_IN_PROGRESS;
		GTM_RWLockRelease(&gtm_txninfo[ii]->gti_lock);

		status[ii] = STATUS_OK;
	}

	GTM_RemoveTransInfoMulti(gtm_txninfo, txn_count);

	return txn_count;
}

/*
 * GTM_RollbackTransaction
 *		Rollback a single global transaction, identified by a handle.
 */
static int
GTM_RollbackTransaction(GTM_TransactionHandle txn)
{
	int status;
	GTM_RollbackTransactionMulti(&txn, 1, &status);
	return status;
}

/*
 * GTM_CommitTransactionMulti
 *		Commit multiple global transactions in one go.
 *
 * Commits txn_count transactions identified by handles passed in txn[] array,
 * and returns the status for each of them in status[] array.
 *
 * It is also possible to provide an array of transactions that have to finish
 * before txn[] transactions can be committed. If some of the transactions in
 * waited_xids[] (with waited_xid_count elements) are still in progress, the
 * transactions will not be committed and will be marked as delayed.
 *
 * Input:
 *		txn[]				- array of transaction handles to commit
 *		txn_count			- number of transaction handles in txn[]
 *		waited_xid_count	- number of GIXDs in waited_xids[]
 *		waited_xids[]		- GIXDs to wait for before the commit
 *
 * Output:
 *		status[]			- outcome of the commit for each txn[] handle
 *
 * The function returns the number of successfully committed transactions
 * (and removed from the global array).
 *
 * The status[] array contains the commit status for each txn[] element, i.e.
 * txn_count elements. There are three possible values:
 *
 *  - STATUS_OK			transaction was committed (and removed)
 *	- STATUS_DELAYED	commit is delayed due to in-progress transactions
 *	- STATUS_ERROR		invalid (NULL) transaction handle
 *
 * XXX Do we need to repeat the loop over waited_xids for every transaction?
 * Maybe we could check it once at the beginning. The only case why that might
 * fail is probably when waited_xids[] and txn[] overlap, some of the GXIDs
 * we're waiting for are also on the list of transactions to commit. But maybe
 * that's not allowed, as such transaction would get delayed by itself.
 */
static int
GTM_CommitTransactionMulti(GTM_TransactionHandle txn[], int txn_count,
		int waited_xid_count, GlobalTransactionId waited_xids[],
		int status[])
{
	GTM_TransactionInfo *gtm_txninfo[txn_count];
	GTM_TransactionInfo *remove_txninfo[txn_count];
	int remove_count = 0;
	int ii;

	for (ii = 0; ii < txn_count; ii++)
	{
		int jj;
		bool waited_xid_running = false;

		gtm_txninfo[ii] = GTM_HandleToTransactionInfo(txn[ii]);

		/* We should not be committing handles that are not initialized. */
		if (gtm_txninfo[ii] == NULL)
		{
			elog(WARNING, "GTM_CommitTransactionMulti: can not commit non-initialized handle");
			status[ii] = STATUS_ERROR;
			continue;
		}

		/*
		 * See if the current transaction depends on other transactions that are
		 * still running (possiby one of those we're currently committing?). In
		 * that case we have to delay commit of this transaction until after those
		 * transaction finish.
		 */
		for (jj = 0; jj < waited_xid_count; jj++)
		{
			if (GTM_IsGXIDInProgress(waited_xids[jj]))
			{
				elog(DEBUG1, "Xact %d not yet finished, xact %d will be delayed",
						waited_xids[jj], gtm_txninfo[ii]->gti_gxid);
				waited_xid_running = true;
				break;
			}
		}

		/* We're waiting for in-progress transactions, so let's delay the commit. */
		if (waited_xid_running) 
		{
			elog(WARNING, "GTM_CommitTransactionMulti: delaying commit of handle %d",
						  gtm_txninfo[ii]->gti_gxid);

			status[ii] = STATUS_DELAYED;
			continue;
		}

		/*
		 * Mark the transaction as being aborted
		 */
		GTM_RWLockAcquire(&gtm_txninfo[ii]->gti_lock, GTM_LOCKMODE_WRITE);
		gtm_txninfo[ii]->gti_state = GTM_TXN_COMMIT_IN_PROGRESS;
		GTM_RWLockRelease(&gtm_txninfo[ii]->gti_lock);

		status[ii] = STATUS_OK;

		/* Keep track of transactions to remove from global array. */
		remove_txninfo[remove_count++] = gtm_txninfo[ii];
	}

	/*
	 * Remove the transaction from the global array, but only those that we
	 * managed to switch to GTM_TXN_COMMIT_IN_PROGRESS state.
	 */
	GTM_RemoveTransInfoMulti(remove_txninfo, remove_count);

	return remove_count;
}

/*
 * GTM_CommitTransaction
 *		Commit a single global transaction handle.
 *
 * Similaarly to GTM_CommitTransactionMulti, it's possible to specify an array
 * of GIXDs that should have completed before the transaction gets committed.
 *
 * Returns STATUS_OK (committed), STATUS_DELAYED (waiting by in-progress
 * transactions) or STATUS_ERROR (txninfo for the handle not found).
 */
static int
GTM_CommitTransaction(GTM_TransactionHandle txn, int waited_xid_count,
		GlobalTransactionId *waited_xids)
{
	int status;
	GTM_CommitTransactionMulti(&txn, 1, waited_xid_count, waited_xids, &status);
	return status;
}

/*
 * GTM_PrepareTransaction
 *		Prepare transaction for commit (in 2PC protocol).
 *
 * Prepare a transaction for commit, and returns STATUS_OK or STATUS_ERROR.
 *
 * XXX This should probably check the initial gti_state (at least by assert).
 * I assume we can only see transactions in GTM_TXN_PREPARE_IN_PROGRESS.
 */
static int
GTM_PrepareTransaction(GTM_TransactionHandle txn)
{
	int	state;
	GTM_TransactionInfo *gtm_txninfo = NULL;

	gtm_txninfo = GTM_HandleToTransactionInfo(txn);

	if (gtm_txninfo == NULL)
	{
		elog(WARNING, "GTM_PrepareTransaction: can't prepare transaction handle %d (txninfo is NULL)",
			 txn);
		return STATUS_ERROR;
	}

	/*
	 * Mark the transaction as prepared
	 */
	GTM_RWLockAcquire(&gtm_txninfo->gti_lock, GTM_LOCKMODE_WRITE);
	state = gtm_txninfo->gti_state;
	gtm_txninfo->gti_state = GTM_TXN_PREPARED;
	GTM_RWLockRelease(&gtm_txninfo->gti_lock);

	/* The initial state should have been PREPARE_IN_PROGRESS. */
	Assert(state == GTM_TXN_PREPARE_IN_PROGRESS);

	return STATUS_OK;
}

/*
 * GTM_StartPreparedTransaction
 *		Start preparing a transaction (set GTM_TXN_PREPARE_IN_PROGRESS).
 *
 * Returns either STATUS_OK when the transaction was succesfully switched to
 * GTM_TXN_PREPARE_IN_PROGRESS, or STATUS_ERROR when the state change fails
 * for some reason (unknown transaction handle, duplicate GID).
 */
static int
GTM_StartPreparedTransaction(GTM_TransactionHandle txn,
							 char *gid,
							 char *nodestring)
{
	GTM_TransactionInfo *gtm_txninfo = GTM_HandleToTransactionInfo(txn);

	if (gtm_txninfo == NULL)
	{
		elog(WARNING, "GTM_StartPreparedTransaction: unknown handle %d", txn);
		return STATUS_ERROR;
	}

	/*
	 * Check if given GID is already in use by another transaction.
	 */
	if (GTM_GIDToHandle(gid) != InvalidTransactionHandle)
	{
		elog(WARNING, "GTM_StartPreparedTransaction: GID %s already exists", gid);
		return STATUS_ERROR;
	}

	/*
	 * Mark the transaction as being prepared
	 */
	GTM_RWLockAcquire(&gtm_txninfo->gti_lock, GTM_LOCKMODE_WRITE);

	gtm_txninfo->gti_state = GTM_TXN_PREPARE_IN_PROGRESS;
	if (gtm_txninfo->nodestring == NULL)
		gtm_txninfo->nodestring = (char *)MemoryContextAlloc(TopMostMemoryContext,
															 GTM_MAX_NODESTRING_LEN);
	memcpy(gtm_txninfo->nodestring, nodestring, strlen(nodestring) + 1);

	/* It is possible that no Datanode is involved in a transaction */
	if (gtm_txninfo->gti_gid == NULL)
		gtm_txninfo->gti_gid = (char *)MemoryContextAlloc(TopMostMemoryContext, GTM_MAX_GID_LEN);
	memcpy(gtm_txninfo->gti_gid, gid, strlen(gid) + 1);

	GTM_RWLockRelease(&gtm_txninfo->gti_lock);

	return STATUS_OK;
}

/*
 * GTM_GetGIDData
 *		Returns gti_gxid and nodestring for a transaction handle.
 *
 * The nodestring (if available) is allocated in TopMostMemoryContext.
 * If there is no matching transaction info (no open transaction for the
 * handle), the rertur value is STATUS_ERROR.
 *
 * In case of success the return value is STATUS_OK.
 */
static int
GTM_GetGIDData(GTM_TransactionHandle prepared_txn,
			   GlobalTransactionId *prepared_gxid,
			   char **nodestring)
{
	GTM_TransactionInfo	*gtm_txninfo = NULL;
	MemoryContext		oldContext;

	Assert(prepared_gxid);

	oldContext = MemoryContextSwitchTo(TopMostMemoryContext);

	gtm_txninfo = GTM_HandleToTransactionInfo(prepared_txn);
	if (gtm_txninfo == NULL)
		return STATUS_ERROR;

	/* then get the necessary Data */
	*prepared_gxid = gtm_txninfo->gti_gxid;
	if (gtm_txninfo->nodestring)
	{
		*nodestring = (char *) palloc(strlen(gtm_txninfo->nodestring) + 1);
		memcpy(*nodestring, gtm_txninfo->nodestring, strlen(gtm_txninfo->nodestring) + 1);
		(*nodestring)[strlen(gtm_txninfo->nodestring)] = '\0';
	}
	else
		*nodestring = NULL;

	MemoryContextSwitchTo(oldContext);

	return STATUS_OK;
}

/*
 * GTM_BkupBeginTransactionGetGXIDMulti
 *
 * XXX Not sure what this does.
 */
static void
GTM_BkupBeginTransactionGetGXIDMulti(GlobalTransactionId *gxid,
									 GTM_IsolationLevel *isolevel,
									 bool *readonly,
									 const char **global_sessionid,
									 uint32 *client_id,
									 GTMProxy_ConnID *connid,
									 int txn_count)
{
	GTM_TransactionHandle txn[GTM_MAX_GLOBAL_TRANSACTIONS];
	GTM_TransactionInfo *gtm_txninfo;
	int ii;
	int count;
	MemoryContext oldContext;

	bool save_control = false;
	GlobalTransactionId xid = InvalidGlobalTransactionId;

	oldContext = MemoryContextSwitchTo(TopMostMemoryContext);

	count = GTM_BeginTransactionMulti(isolevel, readonly, global_sessionid,
									  connid, txn_count, txn);
	if (count != txn_count)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to start %d new transactions", txn_count)));

	elog(DEBUG2, "GTM_BkupBeginTransactionGetGXIDMulti - count %d", count);

	//XCPTODO check oldContext = MemoryContextSwitchTo(TopMemoryContext);
	GTM_RWLockAcquire(&GTMTransactions.gt_TransArrayLock, GTM_LOCKMODE_WRITE);

	for (ii = 0; ii < txn_count; ii++)
	{
		gtm_txninfo = GTM_HandleToTransactionInfo(txn[ii]);
		gtm_txninfo->gti_gxid = gxid[ii];
		if (global_sessionid[ii])
			strncpy(gtm_txninfo->gti_global_session_id, global_sessionid[ii],
					GTM_MAX_SESSION_ID_LEN);

		elog(DEBUG2, "GTM_BkupBeginTransactionGetGXIDMulti: xid(%u), handle(%u)",
				gxid[ii], txn[ii]);

		/*
		 * Advance next gxid -- because this is called at slave only, we don't care the restoration point
		 * here.  Restoration point will be created at promotion.
		 */
		if (GlobalTransactionIdPrecedesOrEquals(GTMTransactions.gt_nextXid, gxid[ii]))
			GTMTransactions.gt_nextXid = gxid[ii] + 1;
		if (!GlobalTransactionIdIsValid(GTMTransactions.gt_nextXid))	/* Handle wrap around too */
			GTMTransactions.gt_nextXid = FirstNormalGlobalTransactionId;
		xid = GTMTransactions.gt_nextXid;
	}

	/*
	 * Periodically write the xid and sequence info out to the control file.
	 * Try and handle wrapping, too.
	 */
	if (GlobalTransactionIdIsValid(xid) &&
			(xid - ControlXid > CONTROL_INTERVAL || xid < ControlXid))
	{
		save_control = true;
		ControlXid = xid;
	}

	GTM_RWLockRelease(&GTMTransactions.gt_TransArrayLock);

	/* save control info when not holding the XidGenLock */
	if (save_control)
		SaveControlInfo();

	MemoryContextSwitchTo(oldContext);
}

/*
 * GTM_BkupBeginTransactionGetGXID
 *
 * XXX Not sure what this does.
 */
static void
GTM_BkupBeginTransactionGetGXID(GlobalTransactionId gxid,
								GTM_IsolationLevel isolevel,
								bool readonly,
								const char *global_sessionid,
								uint32 client_id)
{
	GTMProxy_ConnID connid = -1;

	GTM_BkupBeginTransactionGetGXIDMulti(&gxid, &isolevel,
			&readonly, &global_sessionid, &client_id, &connid, 1);
}

/*
 * Process MSG_TXN_BEGIN message
 */
void
ProcessBeginTransactionCommand(Port *myport, StringInfo message)
{
	GTM_IsolationLevel txn_isolation_level;
	bool txn_read_only;
	StringInfoData buf;
	GTM_TransactionHandle txn;
	GTM_Timestamp timestamp;
	MemoryContext oldContext;
	uint32 global_sessionid_len;
	const char *global_sessionid;

	txn_isolation_level = pq_getmsgint(message, sizeof (GTM_IsolationLevel));
	txn_read_only = pq_getmsgbyte(message);
	global_sessionid_len = pq_getmsgint(message, sizeof (uint32));
	global_sessionid = pq_getmsgbytes(message, global_sessionid_len);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Start a new transaction
	 */
	txn = GTM_BeginTransaction(txn_isolation_level, txn_read_only,
			global_sessionid);
	if (txn == InvalidTransactionHandle)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to start a new transaction")));

	MemoryContextSwitchTo(oldContext);

	/* GXID has been received, now it's time to get a GTM timestamp */
	timestamp = GTM_TimestampGetCurrent();

	/* Backup first */
	if (GetMyThreadInfo->thr_conn->standby)
	{
		bkup_begin_transaction(GetMyThreadInfo->thr_conn->standby,
				txn_isolation_level, txn_read_only,
				global_sessionid,
				GetMyThreadInfo->thr_client_id, timestamp);
		/* Synch. with standby */
		if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
			gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);
	}

	pq_beginmessage(&buf, 'S');
	pq_sendint(&buf, TXN_BEGIN_RESULT, 4);
	if (myport->remote_type == GTM_NODE_GTM_PROXY)
	{
		GTM_ProxyMsgHeader proxyhdr;
		proxyhdr.ph_conid = myport->conn_id;
		pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
	}
	pq_sendbytes(&buf, (char *)&txn, sizeof(txn));
	pq_sendbytes(&buf, (char *)&timestamp, sizeof (GTM_Timestamp));
	pq_endmessage(myport, &buf);

	if (myport->remote_type != GTM_NODE_GTM_PROXY)
	{
		/* Flush standby first */
		if (GetMyThreadInfo->thr_conn->standby)
			gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
		pq_flush(myport);
	}
	return;
}

/*
 * Process MSG_BKUP_TXN_BEGIN message
 */
void
ProcessBkupBeginTransactionCommand(Port *myport, StringInfo message)
{
	GTM_IsolationLevel txn_isolation_level;
	bool txn_read_only;
	GTM_Timestamp timestamp;
	MemoryContext oldContext;
	uint32 client_id;
	uint32 global_sessionid_len;
	const char *global_sessionid;

	txn_isolation_level = pq_getmsgint(message, sizeof(GTM_IsolationLevel));
	txn_read_only = pq_getmsgbyte(message);
	global_sessionid_len = pq_getmsgint(message, sizeof (uint32));
	global_sessionid = pq_getmsgbytes(message, global_sessionid_len);
	client_id = pq_getmsgint(message, sizeof (uint32));
	memcpy(&timestamp, pq_getmsgbytes(message, sizeof(GTM_Timestamp)), sizeof(GTM_Timestamp));
	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	GTM_BkupBeginTransaction(txn_isolation_level, txn_read_only,
			global_sessionid,
			client_id);

	MemoryContextSwitchTo(oldContext);
}

/*
 * Process MSG_TXN_BEGIN_GETGXID message
 */
void
ProcessBeginTransactionGetGXIDCommand(Port *myport, StringInfo message)
{
	GTM_IsolationLevel txn_isolation_level;
	bool txn_read_only;
	StringInfoData buf;
	GTM_TransactionHandle txn;
	GlobalTransactionId gxid;
	GTM_Timestamp timestamp;
	MemoryContext oldContext;
	uint32 global_sessionid_len;
	const char *global_sessionid;

	txn_isolation_level = pq_getmsgint(message, sizeof (GTM_IsolationLevel));
	txn_read_only = pq_getmsgbyte(message);
	global_sessionid_len = pq_getmsgint(message, sizeof (uint32));
	global_sessionid = pq_getmsgbytes(message, global_sessionid_len);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/* GXID has been received, now it's time to get a GTM timestamp */
	timestamp = GTM_TimestampGetCurrent();

	/*
	 * Start a new transaction
	 */
	txn = GTM_BeginTransaction(txn_isolation_level, txn_read_only,
			global_sessionid);
	if (txn == InvalidTransactionHandle)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to start a new transaction")));

	gxid = GTM_GetGlobalTransactionId(txn);
	if (gxid == InvalidGlobalTransactionId)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to get a new transaction id")));

	MemoryContextSwitchTo(oldContext);

	elog(DEBUG1, "Sending transaction id %u", gxid);

	/* Backup first */
	if (GetMyThreadInfo->thr_conn->standby)
	{
		GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
		int count = 0;

		elog(DEBUG1, "calling begin_transaction() for standby GTM %p.", GetMyThreadInfo->thr_conn->standby);

retry:
		bkup_begin_transaction_gxid(GetMyThreadInfo->thr_conn->standby,
									gxid, txn_isolation_level,
									txn_read_only,
									global_sessionid,
									GetMyThreadInfo->thr_client_id,
									timestamp);

		if (gtm_standby_check_communication_error(&count, oldconn))
			goto retry;

		/* Sync */
		if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
			gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

	}
	/* Respond to the client */
	pq_beginmessage(&buf, 'S');
	pq_sendint(&buf, TXN_BEGIN_GETGXID_RESULT, 4);
	if (myport->remote_type == GTM_NODE_GTM_PROXY)
	{
		GTM_ProxyMsgHeader proxyhdr;
		proxyhdr.ph_conid = myport->conn_id;
		pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
	}
	pq_sendbytes(&buf, (char *)&gxid, sizeof(gxid));
	pq_sendbytes(&buf, (char *)&timestamp, sizeof (GTM_Timestamp));
	pq_endmessage(myport, &buf);

	if (myport->remote_type != GTM_NODE_GTM_PROXY)
	{
		/* Flush standby */
		if (GetMyThreadInfo->thr_conn->standby)
			gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
		pq_flush(myport);
	}


	return;
}

/*
 * Process MSG_BKUP_TXN_BEGIN_GETGXID message
 */
void
ProcessBkupBeginTransactionGetGXIDCommand(Port *myport, StringInfo message)
{
	GlobalTransactionId gxid;
	GTM_IsolationLevel txn_isolation_level;
	bool txn_read_only;
	uint32 txn_client_id;
	GTM_Timestamp timestamp;
	uint32 txn_global_sessionid_len;
	const char *txn_global_sessionid;

	gxid = pq_getmsgint(message, sizeof(GlobalTransactionId));
	txn_isolation_level = pq_getmsgint(message, sizeof(GTM_IsolationLevel));
	txn_read_only = pq_getmsgbyte(message);
	txn_global_sessionid_len = pq_getmsgint(message, sizeof (uint32));
	txn_global_sessionid = pq_getmsgbytes(message,
			txn_global_sessionid_len);
	txn_client_id = pq_getmsgint(message, sizeof (uint32));
	memcpy(&timestamp, pq_getmsgbytes(message, sizeof(GTM_Timestamp)), sizeof(GTM_Timestamp));
	pq_getmsgend(message);

	GTM_BkupBeginTransactionGetGXID(gxid, txn_isolation_level,
			txn_read_only, txn_global_sessionid, txn_client_id);
}

/*
 * Process MSG_BKUP_TXN_BEGIN_GETGXID_AUTOVACUUM message
 */
void
ProcessBkupBeginTransactionGetGXIDAutovacuumCommand(Port *myport, StringInfo message)
{
	GlobalTransactionId gxid;
	GTM_IsolationLevel txn_isolation_level;
	uint32 txn_client_id;

	gxid = pq_getmsgint(message, sizeof(GlobalTransactionId));
	txn_isolation_level = pq_getmsgint(message, sizeof(GTM_IsolationLevel));
	txn_client_id = pq_getmsgint(message, sizeof (uint32));
	pq_getmsgend(message);

	GTM_BkupBeginTransactionGetGXID(gxid, txn_isolation_level,
			false, NULL, txn_client_id);
	GTM_SetDoVacuum(GTM_GXIDToHandle(gxid));
}

/*
 * Process MSG_TXN_BEGIN_GETGXID_AUTOVACUUM message
 */
void
ProcessBeginTransactionGetGXIDAutovacuumCommand(Port *myport, StringInfo message)
{
	GTM_IsolationLevel txn_isolation_level;
	bool txn_read_only;
	StringInfoData buf;
	GTM_TransactionHandle txn;
	GlobalTransactionId gxid;
	MemoryContext oldContext;

	txn_isolation_level = pq_getmsgint(message, sizeof (GTM_IsolationLevel));
	txn_read_only = pq_getmsgbyte(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Start a new transaction
	 */
	txn = GTM_BeginTransaction(txn_isolation_level, txn_read_only, NULL);
	if (txn == InvalidTransactionHandle)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to start a new transaction")));

	gxid = GTM_GetGlobalTransactionId(txn);
	if (gxid == InvalidGlobalTransactionId)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to get a new transaction id")));

	/* Indicate that it is for autovacuum */
	GTM_SetDoVacuum(txn);

	MemoryContextSwitchTo(oldContext);

	/* Backup first */
	if (GetMyThreadInfo->thr_conn->standby)
	{
		GlobalTransactionId _gxid;
		GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
		int count = 0;

		elog(DEBUG1, "calling begin_transaction_autovacuum() for standby GTM %p.",
			 GetMyThreadInfo->thr_conn->standby);

	retry:
		_gxid = bkup_begin_transaction_autovacuum(GetMyThreadInfo->thr_conn->standby,
												  gxid,
												  txn_isolation_level,
												  GetMyThreadInfo->thr_client_id);

		if (gtm_standby_check_communication_error(&count, oldconn))
			goto retry;

		/* Sync */
		if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
			gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

		elog(DEBUG1, "begin_transaction_autovacuum() GXID=%d done.", _gxid);
	}
	/* Respond to the client */
	pq_beginmessage(&buf, 'S');
	pq_sendint(&buf, TXN_BEGIN_GETGXID_AUTOVACUUM_RESULT, 4);
	if (myport->remote_type == GTM_NODE_GTM_PROXY)
	{
		GTM_ProxyMsgHeader proxyhdr;
		proxyhdr.ph_conid = myport->conn_id;
		pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
	}
	pq_sendbytes(&buf, (char *)&gxid, sizeof(gxid));
	pq_endmessage(myport, &buf);

	if (myport->remote_type != GTM_NODE_GTM_PROXY)
	{
		/* Flush standby */
		if (GetMyThreadInfo->thr_conn->standby)
			gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
		pq_flush(myport);
	}

	return;
}

/*
 * Process MSG_TXN_BEGIN_GETGXID_MULTI message
 */
void
ProcessBeginTransactionGetGXIDCommandMulti(Port *myport, StringInfo message)
{
	GTM_IsolationLevel txn_isolation_level[GTM_MAX_GLOBAL_TRANSACTIONS];
	bool txn_read_only[GTM_MAX_GLOBAL_TRANSACTIONS];
	uint32 txn_global_sessionid_len;
	const char *txn_global_sessionid[GTM_MAX_GLOBAL_TRANSACTIONS];
	int txn_count, new_txn_count;
	StringInfoData buf;
	GTM_TransactionHandle txn[GTM_MAX_GLOBAL_TRANSACTIONS];
	GTM_TransactionHandle new_txn[GTM_MAX_GLOBAL_TRANSACTIONS];
	GlobalTransactionId txn_gxid[GTM_MAX_GLOBAL_TRANSACTIONS];
	GTM_Timestamp timestamp;
	GTMProxy_ConnID txn_connid[GTM_MAX_GLOBAL_TRANSACTIONS];
	uint32 txn_client_id[GTM_MAX_GLOBAL_TRANSACTIONS];
	MemoryContext oldContext;
	int count;
	int ii;

	txn_count = pq_getmsgint(message, sizeof (int));

	if (txn_count <= 0)
		elog(PANIC, "Zero or less transaction count");

	for (ii = 0; ii < txn_count; ii++)
	{
		txn_isolation_level[ii] = pq_getmsgint(message, sizeof (GTM_IsolationLevel));
		txn_read_only[ii] = pq_getmsgbyte(message);
		txn_global_sessionid_len = pq_getmsgint(message, sizeof (uint32));
		txn_global_sessionid[ii] = pq_getmsgbytes(message,
				txn_global_sessionid_len);
		txn_connid[ii] = pq_getmsgint(message, sizeof (GTMProxy_ConnID));
		txn_client_id[ii] = GetMyThreadInfo->thr_client_id;
	}

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Start a new transaction
	 *
	 * XXX Port should contain Coordinator name - replace NULL with that
	 */
	count = GTM_BeginTransactionMulti(txn_isolation_level, txn_read_only,
									  txn_global_sessionid, txn_connid,
									  txn_count, txn);
	if (count != txn_count)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to start %d new transactions", txn_count)));

	if (!GTM_GetGlobalTransactionIdMulti(txn, txn_count, txn_gxid, new_txn,
			&new_txn_count))
		elog(ERROR, "Failed to get global transaction identifiers");

	MemoryContextSwitchTo(oldContext);

	/* GXID has been received, now it's time to get a GTM timestamp */
	timestamp = GTM_TimestampGetCurrent();

	/* Backup first */
	if (GetMyThreadInfo->thr_conn->standby)
	{
		int _rc;
		GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
		int count = 0;

		elog(DEBUG1, "calling begin_transaction_multi() for standby GTM %p.",
		     GetMyThreadInfo->thr_conn->standby);

retry:
		_rc = bkup_begin_transaction_multi(GetMyThreadInfo->thr_conn->standby,
										   txn_count,
										   txn_gxid,
										   txn_isolation_level,
										   txn_read_only,
										   txn_global_sessionid,
										   txn_client_id,
										   txn_connid);

		if (gtm_standby_check_communication_error(&count, oldconn))
			goto retry;

		/* Sync */
		if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
			gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

		elog(DEBUG1, "begin_transaction_multi() rc=%d done.", _rc);
	}
	/* Respond to the client */
	pq_beginmessage(&buf, 'S');
	pq_sendint(&buf, TXN_BEGIN_GETGXID_MULTI_RESULT, 4);
	if (myport->remote_type == GTM_NODE_GTM_PROXY)
	{
		GTM_ProxyMsgHeader proxyhdr;
		proxyhdr.ph_conid = myport->conn_id;
		pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
	}
	pq_sendbytes(&buf, (char *)&txn_count, sizeof(txn_count));
	pq_sendbytes(&buf, (char *)txn_gxid, sizeof(GlobalTransactionId) * txn_count);
	pq_sendbytes(&buf, (char *)&(timestamp), sizeof (GTM_Timestamp));
	pq_endmessage(myport, &buf);

	if (myport->remote_type != GTM_NODE_GTM_PROXY)
	{
		/* Flush standby */
		if (GetMyThreadInfo->thr_conn->standby)
			gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
		pq_flush(myport);
	}

	return;
}

/*
 * Process MSG_BKUP_BEGIN_TXN_GETGXID_MULTI message
 */
void
ProcessBkupBeginTransactionGetGXIDCommandMulti(Port *myport, StringInfo message)
{
	int txn_count;
	GlobalTransactionId gxid[GTM_MAX_GLOBAL_TRANSACTIONS];
	GTM_IsolationLevel txn_isolation_level[GTM_MAX_GLOBAL_TRANSACTIONS];
	bool txn_read_only[GTM_MAX_GLOBAL_TRANSACTIONS];
	uint32 txn_global_sessionid_len;
	const char *txn_global_sessionid[GTM_MAX_GLOBAL_TRANSACTIONS];
	GTMProxy_ConnID txn_connid[GTM_MAX_GLOBAL_TRANSACTIONS];
	uint32 txn_client_id[GTM_MAX_GLOBAL_TRANSACTIONS];
	int ii;

	txn_count = pq_getmsgint(message, sizeof(int));
	if (txn_count <= 0)
		elog(PANIC, "Zero or less transaction count.");

	for (ii = 0; ii < txn_count; ii++)
	{
		gxid[ii] = pq_getmsgint(message, sizeof(GlobalTransactionId));
		txn_isolation_level[ii] = pq_getmsgint(message, sizeof(GTM_IsolationLevel));
		txn_read_only[ii] = pq_getmsgbyte(message);
		txn_global_sessionid_len = pq_getmsgint(message, sizeof (uint32));
		txn_global_sessionid[ii] = pq_getmsgbytes(message,
				txn_global_sessionid_len);
		txn_client_id[ii] = pq_getmsgint(message, sizeof(uint32));
		txn_connid[ii] = pq_getmsgint(message, sizeof(GTMProxy_ConnID));
	}

	GTM_BkupBeginTransactionGetGXIDMulti(gxid, txn_isolation_level,
			txn_read_only, txn_global_sessionid,
			txn_client_id, txn_connid, txn_count);

}
/*
 * Process MSG_TXN_COMMIT/MSG_BKUP_TXN_COMMIT message
 *
 * is_backup indicates the message is MSG_BKUP_TXN_COMMIT
 */
void
ProcessCommitTransactionCommand(Port *myport, StringInfo message, bool is_backup)
{
	StringInfoData buf;
	GTM_TransactionHandle txn;
	GlobalTransactionId gxid;
	MemoryContext oldContext;
	int status = STATUS_OK;
	int waited_xid_count;
	GlobalTransactionId *waited_xids = NULL;

	const char *data = pq_getmsgbytes(message, sizeof (gxid));

	if (data == NULL)
		ereport(ERROR,
				(EPROTO,
				 errmsg("Message does not contain valid GXID")));
	memcpy(&gxid, data, sizeof (gxid));
	txn = GTM_GXIDToHandle(gxid);

	waited_xid_count = pq_getmsgint(message, sizeof (int));
	if (waited_xid_count > 0)
	{
		waited_xids = (GlobalTransactionId *) pq_getmsgbytes(message,
				waited_xid_count * sizeof (GlobalTransactionId));
	}

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Commit the transaction
	 */
	status = GTM_CommitTransaction(txn, waited_xid_count, waited_xids);

	MemoryContextSwitchTo(oldContext);

	if(!is_backup)
	{
		/*
		 * If the transaction is successfully committed on the GTM master then
		 * send a backup message to the GTM slave to redo the action locally
		 */
		if ((GetMyThreadInfo->thr_conn->standby) && (status == STATUS_OK))
		{
			/*
			 * Backup first
			 */
			int _rc;
			GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
			int count = 0;

			elog(DEBUG1, "calling commit_transaction() for standby GTM %p.", GetMyThreadInfo->thr_conn->standby);

		retry:
			_rc = bkup_commit_transaction(GetMyThreadInfo->thr_conn->standby, gxid);

			if (gtm_standby_check_communication_error(&count, oldconn))
				goto retry;

			/* Sync */
			if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
				gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

			elog(DEBUG1, "commit_transaction() rc=%d done.", _rc);
		}

		pq_beginmessage(&buf, 'S');
		pq_sendint(&buf, TXN_COMMIT_RESULT, 4);
		if (myport->remote_type == GTM_NODE_GTM_PROXY)
		{
			GTM_ProxyMsgHeader proxyhdr;
			proxyhdr.ph_conid = myport->conn_id;
			pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
		}
		pq_sendbytes(&buf, (char *)&gxid, sizeof(gxid));
		pq_sendbytes(&buf, (char *)&status, sizeof(status));
		pq_endmessage(myport, &buf);

		if (myport->remote_type != GTM_NODE_GTM_PROXY)
		{
			/* Flush standby */
			if (GetMyThreadInfo->thr_conn->standby)
				gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
			pq_flush(myport);
		}

	}
	return;
}

/*
 * Process MSG_TXN_COMMIT_PREPARED/MSG_BKUP_TXN_COMMIT_PREPARED message
 * Commit a prepared transaction
 * Here the GXID used for PREPARE and COMMIT PREPARED are both committed
 *
 * is_backup indicates the message is MSG_BKUP_TXN_COMMIT_PREPARED
 */
void
ProcessCommitPreparedTransactionCommand(Port *myport, StringInfo message, bool is_backup)
{
	StringInfoData buf;
	int	txn_count = 2; /* PREPARE and COMMIT PREPARED gxid's */
	GTM_TransactionHandle txn[txn_count];
	GlobalTransactionId gxid[txn_count];
	MemoryContext oldContext;
	int status[txn_count];
	int ii;
	int waited_xid_count;
	GlobalTransactionId *waited_xids = NULL;

	for (ii = 0; ii < txn_count; ii++)
	{
		const char *data = pq_getmsgbytes(message, sizeof (gxid[ii]));
		if (data == NULL)
			ereport(ERROR,
					(EPROTO,
					 errmsg("Message does not contain valid GXID")));
		memcpy(&gxid[ii], data, sizeof (gxid[ii]));
		txn[ii] = GTM_GXIDToHandle(gxid[ii]);
		elog(DEBUG1, "ProcessCommitTransactionCommandMulti: gxid(%u), handle(%u)", gxid[ii], txn[ii]);
	}

	waited_xid_count = pq_getmsgint(message, sizeof (int));
	if (waited_xid_count > 0)
	{
		waited_xids = (GlobalTransactionId *) pq_getmsgbytes(message,
				waited_xid_count * sizeof (GlobalTransactionId));
	}

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	elog(DEBUG1, "Committing: prepared id %u and commit prepared id %u ", gxid[0], gxid[1]);

	/*
	 * Commit the prepared transaction.
	 */
	GTM_CommitTransactionMulti(txn, txn_count, waited_xid_count,
			waited_xids, status);

	MemoryContextSwitchTo(oldContext);

	if (!is_backup)
	{
		/*
		 * If we successfully committed the transaction on the GTM master, then
		 * also send a backup message to the GTM slave to redo the action
		 * locally
		 *
		 * GTM_CommitTransactionMulti() above is used to only commit the main
		 * and the auxiliary GXID. Since we either commit or delay both of
		 * these GXIDs together, its enough to just test for one of the GXIDs.
		 * If the transaction commit is delayed, the backup message will be
		 * sent when the GTM master receives COMMIT message again and
		 * successfully commits the transaction
		 */
		if ((GetMyThreadInfo->thr_conn->standby) && (status[0] == STATUS_OK))
		{
			/* Backup first */
			int _rc;
			GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
			int count = 0;

			elog(DEBUG1, "calling commit_prepared_transaction() for standby GTM %p.",
				 GetMyThreadInfo->thr_conn->standby);

		retry:
			_rc = bkup_commit_prepared_transaction(GetMyThreadInfo->thr_conn->standby,
												   gxid[0], gxid[1] /* prepared GXID */);

			if (gtm_standby_check_communication_error(&count, oldconn))
				goto retry;

			/* Sync */
			if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
				gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

			elog(DEBUG1, "commit_prepared_transaction() rc=%d done.", _rc);
		}
		/* Respond to the client */
		pq_beginmessage(&buf, 'S');
		pq_sendint(&buf, TXN_COMMIT_PREPARED_RESULT, 4);
		if (myport->remote_type == GTM_NODE_GTM_PROXY)
		{
			GTM_ProxyMsgHeader proxyhdr;
			proxyhdr.ph_conid = myport->conn_id;
			pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
		}
		pq_sendbytes(&buf, (char *)&gxid[0], sizeof(GlobalTransactionId));
		pq_sendbytes(&buf, (char *)&status[0], 4);
		pq_endmessage(myport, &buf);

		if (myport->remote_type != GTM_NODE_GTM_PROXY)
		{
			/* Flush standby */
			if (GetMyThreadInfo->thr_conn->standby)
				gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
			pq_flush(myport);
		}

	}
	return;
}


/*
 * Process MSG_TXN_GET_GID_DATA
 * This message is used after at the beginning of a COMMIT PREPARED
 * or a ROLLBACK PREPARED.
 * For a given GID the following info is returned:
 * - a fresh GXID,
 * - GXID of the transaction that made the prepare
 * - Datanode and Coordinator node list involved in the prepare
 */
void
ProcessGetGIDDataTransactionCommand(Port *myport, StringInfo message)
{
	StringInfoData buf;
	char *gid;
	char *nodestring = NULL;
	int gidlen;
	GTM_IsolationLevel txn_isolation_level;
	bool txn_read_only;
	GTM_TransactionHandle txn, prepared_txn;
	/* Data to be sent back to client */
	GlobalTransactionId gxid, prepared_gxid;

	/* take the isolation level and read_only instructions */
	txn_isolation_level = pq_getmsgint(message, sizeof (GTM_IsolationLevel));
	txn_read_only = pq_getmsgbyte(message);

	/* receive GID */
	gidlen = pq_getmsgint(message, sizeof (GTM_StrLen));
	gid = (char *) palloc(gidlen + 1);
	memcpy(gid, (char *)pq_getmsgbytes(message, gidlen), gidlen);
	gid[gidlen] = '\0';

	pq_getmsgend(message);

	/* Get the prepared Transaction for given GID */
	prepared_txn = GTM_GIDToHandle(gid);
	if (prepared_txn == InvalidTransactionHandle)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to get GID Data for prepared transaction")));

	/* First get the GXID for the new transaction */
	txn = GTM_BeginTransaction(txn_isolation_level, txn_read_only, NULL);
	if (txn == InvalidTransactionHandle)
		ereport(ERROR,
			(EINVAL,
			 errmsg("Failed to start a new transaction")));

	gxid = GTM_GetGlobalTransactionId(txn);
	if (gxid == InvalidGlobalTransactionId)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to get a new transaction id")));

	/*
	 * Make the internal process, get the prepared information from GID.
	 */
	if (GTM_GetGIDData(prepared_txn, &prepared_gxid, &nodestring) != STATUS_OK)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to get the information of prepared transaction")));


	if (GetMyThreadInfo->thr_conn->standby)
	{
		GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
		int count = 0;
		GTM_Timestamp timestamp = 0;

		elog(DEBUG1, "calling bkup_begin_transaction_gxid() for auxiliary transaction for standby GTM %p.",
			GetMyThreadInfo->thr_conn->standby);

retry:
		/*
		 * The main XID was already backed up on the standby when it was
		 * started. Now also backup the new GXID we obtained above for running
		 * COMMIT/ROLLBACK PREPARED statements. This is necessary because GTM
		 * will later receive a COMMIT/ABORT message for this XID and the
		 * standby must be prepared to handle those messages as well
		 *
		 * Note: We use the same routine used to backup a new transaction
		 * instead of writing a routine specific to MSG_TXN_GET_GID_DATA
		 * message
		 */ 
		bkup_begin_transaction_gxid(GetMyThreadInfo->thr_conn->standby,
				   gxid,
				   txn_isolation_level,
				   false,
				   NULL,
				   -1,
				   timestamp);

		if (gtm_standby_check_communication_error(&count, oldconn))
			goto retry;

	}

	/*
	 * Send a SUCCESS message back to the client
	 */
	pq_beginmessage(&buf, 'S');
	pq_sendint(&buf, TXN_GET_GID_DATA_RESULT, 4);
	if (myport->remote_type == GTM_NODE_GTM_PROXY)
	{
		GTM_ProxyMsgHeader proxyhdr;
		proxyhdr.ph_conid = myport->conn_id;
		pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
	}

	/* Send the two GXIDs */
	pq_sendbytes(&buf, (char *)&gxid, sizeof(GlobalTransactionId));
	pq_sendbytes(&buf, (char *)&prepared_gxid, sizeof(GlobalTransactionId));

	/* Node string list */
	if (nodestring)
	{
		pq_sendint(&buf, strlen(nodestring), 4);
		pq_sendbytes(&buf, nodestring, strlen(nodestring));
	}
	else
		pq_sendint(&buf, 0, 4);

	/* End of message */
	pq_endmessage(myport, &buf);

	/* No backup to the standby because this does not change internal status */
	if (myport->remote_type != GTM_NODE_GTM_PROXY)
		pq_flush(myport);
	pfree(gid);
	return;
}
/*
 * Process MSG_TXN_GXID_LIST
 */
void
ProcessGXIDListCommand(Port *myport, StringInfo message)
{
	MemoryContext oldContext;
	StringInfoData buf;
	char *data;
	size_t estlen, actlen; /* estimated length and actual length */

	pq_getmsgend(message);

	if (Recovery_IsStandby())
		ereport(ERROR,
			(EPERM,
			 errmsg("Operation not permitted under the standby mode.")));

	/*
	 * Do something here.
	 */
	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	GTM_RWLockAcquire(&GTMTransactions.gt_XidGenLock, GTM_LOCKMODE_WRITE);

	estlen = gtm_get_transactions_size(&GTMTransactions);
	data = malloc(estlen+1);

	actlen = gtm_serialize_transactions(&GTMTransactions, data, estlen);

	elog(DEBUG1, "gtm_serialize_transactions: estlen=%ld, actlen=%ld", estlen, actlen);

	GTM_RWLockRelease(&GTMTransactions.gt_XidGenLock);

	MemoryContextSwitchTo(oldContext);

	/*
	 * Send a SUCCESS message back to the client
	 */
	pq_beginmessage(&buf, 'S');
	pq_sendint(&buf, TXN_GXID_LIST_RESULT, 4);
	if (myport->remote_type == GTM_NODE_GTM_PROXY)
	{
		GTM_ProxyMsgHeader proxyhdr;
		proxyhdr.ph_conid = myport->conn_id;
		pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
	}

	pq_sendint(&buf, actlen, sizeof(int32));	/* size of serialized GTM_Transactions */
	pq_sendbytes(&buf, data, actlen);			/* serialized GTM_Transactions */
	pq_endmessage(myport, &buf);

	/* No backup to the standby because this does not change internal state */
	if (myport->remote_type != GTM_NODE_GTM_PROXY)
	{
		pq_flush(myport);
		elog(DEBUG1, "pq_flush()");
	}

	elog(DEBUG1, "ProcessGXIDListCommand() ok. %ld bytes sent. len=%d", actlen, buf.len);
	free(data);

	return;
}


/*
 * Process MSG_TXN_ROLLBACK/MSG_BKUP_TXN_ROLLBACK message
 *
 * is_backup indicates the message is MSG_BKUP_TXN_ROLLBACK
 */
void
ProcessRollbackTransactionCommand(Port *myport, StringInfo message, bool is_backup)
{
	StringInfoData buf;
	GTM_TransactionHandle txn;
	GlobalTransactionId gxid;
	MemoryContext oldContext;
	int status = STATUS_OK;
	const char *data = pq_getmsgbytes(message, sizeof (gxid));

	if (data == NULL)
		ereport(ERROR,
				(EPROTO,
				 errmsg("Message does not contain valid GXID")));
	memcpy(&gxid, data, sizeof (gxid));
	txn = GTM_GXIDToHandle(gxid);

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	elog(DEBUG1, "Cancelling transaction id %u", gxid);

	/*
	 * Commit the transaction
	 */
	status = GTM_RollbackTransaction(txn);

	MemoryContextSwitchTo(oldContext);

	if (!is_backup)
	{
		/* Backup first */
		if (GetMyThreadInfo->thr_conn->standby)
		{
			GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
			int count = 0;

			elog(DEBUG1, "calling abort_transaction() for standby GTM %p.", GetMyThreadInfo->thr_conn->standby);

		retry:
			bkup_abort_transaction(GetMyThreadInfo->thr_conn->standby, gxid);

			if (gtm_standby_check_communication_error(&count, oldconn))
				goto retry;

			/* Sync */
			if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
				gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

			elog(DEBUG1, "abort_transaction() GXID=%d done.", gxid);
		}
		/* Respond to the client */
		pq_beginmessage(&buf, 'S');
		pq_sendint(&buf, TXN_ROLLBACK_RESULT, 4);
		if (myport->remote_type == GTM_NODE_GTM_PROXY)
		{
			GTM_ProxyMsgHeader proxyhdr;
			proxyhdr.ph_conid = myport->conn_id;
			pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
		}
		pq_sendbytes(&buf, (char *)&gxid, sizeof(gxid));
		pq_sendint(&buf, status, sizeof(status));
		pq_endmessage(myport, &buf);

		if (myport->remote_type != GTM_NODE_GTM_PROXY)
		{
			/* Flush standby first */
			if (GetMyThreadInfo->thr_conn->standby)
				gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
			pq_flush(myport);
		}

	}
	return;
}


/*
 * Process MSG_TXN_COMMIT_MULTI/MSG_BKUP_TXN_COMMIT_MULTI message
 *
 * is_backup indicates the message is MSG_BKUP_TXN_COMMIT_MULTI
 */
void
ProcessCommitTransactionCommandMulti(Port *myport, StringInfo message, bool is_backup)
{
	StringInfoData buf;
	GTM_TransactionHandle txn[GTM_MAX_GLOBAL_TRANSACTIONS];
	GlobalTransactionId gxid[GTM_MAX_GLOBAL_TRANSACTIONS];
	MemoryContext oldContext;
	int status[GTM_MAX_GLOBAL_TRANSACTIONS];
	int txn_count;
	int ii;

	txn_count = pq_getmsgint(message, sizeof (int));

	for (ii = 0; ii < txn_count; ii++)
	{
		const char *data = pq_getmsgbytes(message, sizeof (gxid[ii]));
		if (data == NULL)
			ereport(ERROR,
					(EPROTO,
					 errmsg("Message does not contain valid GXID")));
		memcpy(&gxid[ii], data, sizeof (gxid[ii]));
		txn[ii] = GTM_GXIDToHandle(gxid[ii]);
		elog(DEBUG1, "ProcessCommitTransactionCommandMulti: gxid(%u), handle(%u)", gxid[ii], txn[ii]);
	}

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Commit the transaction
	 */
	GTM_CommitTransactionMulti(txn, txn_count, 0, NULL, status);

	MemoryContextSwitchTo(oldContext);

	if (!is_backup)
	{
		if (GetMyThreadInfo->thr_conn->standby)
		{
			/* Backup first */
			int _rc;
			GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
			int count = 0;

			elog(DEBUG1, "calling commit_transaction_multi() for standby GTM %p.",
				 GetMyThreadInfo->thr_conn->standby);

		retry:
			_rc =
				bkup_commit_transaction_multi(GetMyThreadInfo->thr_conn->standby,
						txn_count, gxid);

			if (gtm_standby_check_communication_error(&count, oldconn))
				goto retry;
			/* Sync */
			if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
				gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

			elog(DEBUG1, "commit_transaction_multi() rc=%d done.", _rc);
		}
		/* Respond to the client */
		pq_beginmessage(&buf, 'S');
		pq_sendint(&buf, TXN_COMMIT_MULTI_RESULT, 4);
		if (myport->remote_type == GTM_NODE_GTM_PROXY)
		{
			GTM_ProxyMsgHeader proxyhdr;
			proxyhdr.ph_conid = myport->conn_id;
			pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
		}
		pq_sendbytes(&buf, (char *)&txn_count, sizeof(txn_count));
		pq_sendbytes(&buf, (char *)status, sizeof(int) * txn_count);
		pq_endmessage(myport, &buf);

		if (myport->remote_type != GTM_NODE_GTM_PROXY)
		{
			/* Flush the standby */
			if (GetMyThreadInfo->thr_conn->standby)
				gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
			pq_flush(myport);
		}
	}
	return;
}

/*
 * Process MSG_TXN_ROLLBACK_MULTI/MSG_BKUP_TXN_ROLLBACK_MULTI message
 *
 * is_backup indicates the message is MSG_BKUP_TXN_ROLLBACK_MULTI
 */
void
ProcessRollbackTransactionCommandMulti(Port *myport, StringInfo message, bool is_backup)
{
	StringInfoData buf;
	GTM_TransactionHandle txn[GTM_MAX_GLOBAL_TRANSACTIONS];
	GlobalTransactionId gxid[GTM_MAX_GLOBAL_TRANSACTIONS];
	MemoryContext oldContext;
	int status[GTM_MAX_GLOBAL_TRANSACTIONS];
	int txn_count;
	int ii;

	txn_count = pq_getmsgint(message, sizeof (int));

	for (ii = 0; ii < txn_count; ii++)
	{
		const char *data = pq_getmsgbytes(message, sizeof (gxid[ii]));
		if (data == NULL)
			ereport(ERROR,
					(EPROTO,
					 errmsg("Message does not contain valid GXID")));
		memcpy(&gxid[ii], data, sizeof (gxid[ii]));
		txn[ii] = GTM_GXIDToHandle(gxid[ii]);
		elog(DEBUG1, "ProcessRollbackTransactionCommandMulti: gxid(%u), handle(%u)", gxid[ii], txn[ii]);
	}

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Commit the transaction
	 */
	GTM_RollbackTransactionMulti(txn, txn_count, status);

	MemoryContextSwitchTo(oldContext);

	if (!is_backup)
	{
		/* Backup first */
		if (GetMyThreadInfo->thr_conn->standby)
		{
			int _rc;
			GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
			int count = 0;

			elog(DEBUG1, "calling abort_transaction_multi() for standby GTM %p.",
				 GetMyThreadInfo->thr_conn->standby);

		retry:
			_rc = bkup_abort_transaction_multi(GetMyThreadInfo->thr_conn->standby, txn_count, gxid);

			if (gtm_standby_check_communication_error(&count, oldconn))
				goto retry;

			/* Sync */
			if (Backup_synchronously &&(myport->remote_type != GTM_NODE_GTM_PROXY))
				gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

			elog(DEBUG1, "abort_transaction_multi() rc=%d done.", _rc);
		}
		/* Respond to the client */
		pq_beginmessage(&buf, 'S');
		pq_sendint(&buf, TXN_ROLLBACK_MULTI_RESULT, 4);
		if (myport->remote_type == GTM_NODE_GTM_PROXY)
		{
			GTM_ProxyMsgHeader proxyhdr;
			proxyhdr.ph_conid = myport->conn_id;
			pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
		}
		pq_sendbytes(&buf, (char *)&txn_count, sizeof(txn_count));
		pq_sendbytes(&buf, (char *)status, sizeof(int) * txn_count);
		pq_endmessage(myport, &buf);

		if (myport->remote_type != GTM_NODE_GTM_PROXY)
		{
			/* Flush the standby */
			if (GetMyThreadInfo->thr_conn->standby)
				gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
			pq_flush(myport);
		}

	}
	return;
}

/*
 * Process MSG_TXN_START_PREPARED/MSG_BKUP_TXN_START_PREPARED message
 *
 * is_backup indicates if the message is MSG_BKUP_TXN_START_PREPARED.
 */
void
ProcessStartPreparedTransactionCommand(Port *myport, StringInfo message, bool is_backup)
{
	StringInfoData buf;
	GTM_TransactionHandle txn;
	GlobalTransactionId gxid;
	GTM_StrLen gidlen, nodelen;
	char nodestring[1024];
	MemoryContext oldContext;
	char *gid;
	const char *data = pq_getmsgbytes(message, sizeof (gxid));

	if (data == NULL)
		ereport(ERROR,
				(EPROTO,
				 errmsg("Message does not contain valid GXID")));
	memcpy(&gxid, data, sizeof (gxid));
	txn = GTM_GXIDToHandle(gxid);

	/* get GID */
	gidlen = pq_getmsgint(message, sizeof (GTM_StrLen));
	gid = (char *) palloc(gidlen + 1);
	memcpy(gid, (char *)pq_getmsgbytes(message, gidlen), gidlen);
	gid[gidlen] = '\0';

	/* get node string list */
	nodelen = pq_getmsgint(message, sizeof (GTM_StrLen));
	memcpy(nodestring, (char *)pq_getmsgbytes(message, nodelen), nodelen);
	nodestring[nodelen] = '\0';

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMostMemoryContext);

	/*
	 * Prepare the transaction
	 */
	if (GTM_StartPreparedTransaction(txn, gid, nodestring) != STATUS_OK)
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to prepare the transaction")));

	MemoryContextSwitchTo(oldContext);

	if (!is_backup)
	{
		/*
		 * Backup first
		 */
		if (GetMyThreadInfo->thr_conn->standby)
		{
			int _rc;
			GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
			int count = 0;

			elog(DEBUG1, "calling start_prepared_transaction() for standby GTM %p.",
				 GetMyThreadInfo->thr_conn->standby);

		retry:
			_rc = backup_start_prepared_transaction(GetMyThreadInfo->thr_conn->standby,
													gxid, gid,
													nodestring);

			if (gtm_standby_check_communication_error(&count, oldconn))
				goto retry;

			/* Sync */
			if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
				gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

			elog(DEBUG1, "start_prepared_transaction() rc=%d done.", _rc);
		}
		pq_beginmessage(&buf, 'S');
		pq_sendint(&buf, TXN_START_PREPARED_RESULT, 4);
		if (myport->remote_type == GTM_NODE_GTM_PROXY)
		{
			GTM_ProxyMsgHeader proxyhdr;
			proxyhdr.ph_conid = myport->conn_id;
			pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
		}
		pq_sendbytes(&buf, (char *)&gxid, sizeof(GlobalTransactionId));
		pq_endmessage(myport, &buf);

		if (myport->remote_type != GTM_NODE_GTM_PROXY)
		{
			/* Flush the standby */
			if (GetMyThreadInfo->thr_conn->standby)
				gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
			pq_flush(myport);
		}

	}
	pfree(gid);
	return;
}

/*
 * Process MSG_TXN_PREPARE/MSG_BKUP_TXN_PREPARE message
 *
 * is_backup indicates the message is MSG_BKUP_TXN_PREPARE
 */
void
ProcessPrepareTransactionCommand(Port *myport, StringInfo message, bool is_backup)
{
	StringInfoData buf;
	GTM_TransactionHandle txn;
	GlobalTransactionId gxid;
	MemoryContext oldContext;
	const char *data = pq_getmsgbytes(message, sizeof (gxid));

	if (data == NULL)
		ereport(ERROR,
				(EPROTO,
				 errmsg("Message does not contain valid GXID")));
	memcpy(&gxid, data, sizeof (gxid));
	txn = GTM_GXIDToHandle(gxid);

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMostMemoryContext);

	/*
	 * Commit the transaction
	 */
	GTM_PrepareTransaction(txn);

	MemoryContextSwitchTo(oldContext);

	elog(DEBUG1, "Preparing transaction id %u", gxid);

	if (!is_backup)
	{
		/* Backup first */
		if (GetMyThreadInfo->thr_conn->standby)
		{
			GTM_Conn *oldconn = GetMyThreadInfo->thr_conn->standby;
			int count = 0;

			elog(DEBUG1, "calling prepare_transaction() for standby GTM %p.", GetMyThreadInfo->thr_conn->standby);

		retry:
			bkup_prepare_transaction(GetMyThreadInfo->thr_conn->standby, gxid);

			if (gtm_standby_check_communication_error(&count, oldconn))
				goto retry;

			/* Sync */
			if (Backup_synchronously && (myport->remote_type != GTM_NODE_GTM_PROXY))
				gtm_sync_standby(GetMyThreadInfo->thr_conn->standby);

			elog(DEBUG1, "prepare_transaction() GXID=%d done.", gxid);
		}
		/* Respond to the client */
		pq_beginmessage(&buf, 'S');
		pq_sendint(&buf, TXN_PREPARE_RESULT, 4);
		if (myport->remote_type == GTM_NODE_GTM_PROXY)
		{
			GTM_ProxyMsgHeader proxyhdr;
			proxyhdr.ph_conid = myport->conn_id;
			pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
		}
		pq_sendbytes(&buf, (char *)&gxid, sizeof(gxid));
		pq_endmessage(myport, &buf);

		if (myport->remote_type != GTM_NODE_GTM_PROXY)
		{
			/* Flush the standby */
			if (GetMyThreadInfo->thr_conn->standby)
				gtmpqFlush(GetMyThreadInfo->thr_conn->standby);
			pq_flush(myport);
		}
	}
	return;

}


/*
 * Process MSG_TXN_GET_GXID message
 *
 * Notice: we don't have corresponding functions in gtm_client.c which
 * generates a command for this function.
 *
 * Because of this, GTM-standby extension is not included in this function.
 */
void
ProcessGetGXIDTransactionCommand(Port *myport, StringInfo message)
{
	StringInfoData buf;
	GTM_TransactionHandle txn;
	GlobalTransactionId gxid;
	const char *data;
	MemoryContext oldContext;

	elog(DEBUG3, "Inside ProcessGetGXIDTransactionCommand");

	data = pq_getmsgbytes(message, sizeof (txn));
	if (data == NULL)
		ereport(ERROR,
				(EPROTO,
				 errmsg("Message does not contain valid Transaction Handle")));
	memcpy(&txn, data, sizeof (txn));

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Get the transaction id for the given global transaction
	 */
	gxid = GTM_GetGlobalTransactionId(txn);
	if (GlobalTransactionIdIsValid(gxid))
		ereport(ERROR,
				(EINVAL,
				 errmsg("Failed to get the transaction id")));

	MemoryContextSwitchTo(oldContext);

	elog(DEBUG3, "Sending transaction id %d", gxid);

	pq_beginmessage(&buf, 'S');
	pq_sendint(&buf, TXN_GET_GXID_RESULT, 4);
	if (myport->remote_type == GTM_NODE_GTM_PROXY)
	{
		GTM_ProxyMsgHeader proxyhdr;
		proxyhdr.ph_conid = myport->conn_id;
		pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
	}
	pq_sendbytes(&buf, (char *)&txn, sizeof(txn));
	pq_sendbytes(&buf, (char *)&gxid, sizeof(gxid));
	pq_endmessage(myport, &buf);

	if (myport->remote_type != GTM_NODE_GTM_PROXY)
		pq_flush(myport);
	return;
}


/*
 * Process MSG_TXN_GET_NEXT_GXID message
 *
 * This does not need backup to the standby because no internal state changes.
 */
void
ProcessGetNextGXIDTransactionCommand(Port *myport, StringInfo message)
{
	StringInfoData buf;
	GlobalTransactionId next_gxid;
	MemoryContext oldContext;

	elog(DEBUG3, "Inside ProcessGetNextGXIDTransactionCommand");

	pq_getmsgend(message);

	oldContext = MemoryContextSwitchTo(TopMemoryContext);

	/*
	 * Get the next gxid.
	 */
	GTM_RWLockAcquire(&GTMTransactions.gt_XidGenLock, GTM_LOCKMODE_WRITE);
	next_gxid = GTMTransactions.gt_nextXid;

	GTM_RWLockRelease(&GTMTransactions.gt_XidGenLock);

	MemoryContextSwitchTo(oldContext);

	elog(DEBUG3, "Sending next gxid %d", next_gxid);

	pq_beginmessage(&buf, 'S');
	pq_sendint(&buf, TXN_GET_NEXT_GXID_RESULT, 4);
	if (myport->remote_type == GTM_NODE_GTM_PROXY)
	{
		GTM_ProxyMsgHeader proxyhdr;
		proxyhdr.ph_conid = myport->conn_id;
		pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
	}
	pq_sendint(&buf, next_gxid, sizeof(GlobalTransactionId));
	pq_endmessage(myport, &buf);

	if (myport->remote_type != GTM_NODE_GTM_PROXY)
		pq_flush(myport);
	return;
}

void
ProcessReportXminCommand(Port *myport, StringInfo message, bool is_backup)
{
	StringInfoData buf;
	GlobalTransactionId gxid;
	GTM_StrLen nodelen;
	char node_name[NI_MAXHOST];
	GTM_PGXCNodeType    type;
	GlobalTransactionId	global_xmin;
	int errcode;

	const char *data = pq_getmsgbytes(message, sizeof (gxid));

	if (data == NULL)
		ereport(ERROR,
				(EPROTO,
				 errmsg("Message does not contain valid GXID")));
	memcpy(&gxid, data, sizeof (gxid));

	/* Read Node Type */
	type = pq_getmsgint(message, sizeof (GTM_PGXCNodeType));

	/* get node name */
	nodelen = pq_getmsgint(message, sizeof (GTM_StrLen));
	memcpy(node_name, (char *)pq_getmsgbytes(message, nodelen), nodelen);
	node_name[nodelen] = '\0';
	pq_getmsgend(message);

	global_xmin = GTM_HandleGlobalXmin(type, node_name, gxid, &errcode);

	{
		/*
		 * Send a SUCCESS message back to the client
		 */
		pq_beginmessage(&buf, 'S');
		pq_sendint(&buf, REPORT_XMIN_RESULT, 4);
		if (myport->remote_type == GTM_NODE_GTM_PROXY)
		{
			GTM_ProxyMsgHeader proxyhdr;
			proxyhdr.ph_conid = myport->conn_id;
			pq_sendbytes(&buf, (char *)&proxyhdr, sizeof (GTM_ProxyMsgHeader));
		}
		pq_sendbytes(&buf, (char *)&GTMTransactions.gt_latestCompletedXid, sizeof (GlobalTransactionId));
		pq_sendbytes(&buf, (char *)&global_xmin, sizeof (GlobalTransactionId));
		pq_sendbytes(&buf, (char *)&errcode, sizeof (errcode));
		pq_endmessage(myport, &buf);
		pq_flush(myport);
	}
}

/*
 * Mark GTM as shutting down. This point onwards no new GXID are issued to
 * ensure that the last GXID recorded in the control file remains sane
 */
void
GTM_SetShuttingDown(void)
{
	GTM_RWLockAcquire(&GTMTransactions.gt_XidGenLock, GTM_LOCKMODE_WRITE);
	GTMTransactions.gt_gtm_state = GTM_SHUTTING_DOWN;
	GTM_RWLockRelease(&GTMTransactions.gt_XidGenLock);
}

static
bool GTM_NeedXidRestoreUpdate(void)
{
	return(GlobalTransactionIdPrecedesOrEquals(GTMTransactions.gt_backedUpXid, GTMTransactions.gt_nextXid));
}

/*
 * GTM_RememberCreatedSequence
 *		Remember a sequence created by a given transaction (GXID).
 *
 * When creating a sequence in a transaction, we need to remember it so thah
 * we can deal with it in case of commit/abort, or when it's later dropped in
 * the same transaction.
 *
 * - If the transaction aborts, we simply remove it from the global structure
 * (see GTM_SeqRemoveCreated).
 *
 * - If the sequence gets dropped in the same transaction (GXID), we can just
 * remove it from the global structure and also stop tracking it in the
 * transaction-specific list (see GTM_ForgetCreatedSequence).
 *
 * - If the transaction commits, just forget about this tracked sequence.
 *
 * See GTM_TransactionInfo_Clean for what happens with the tracked sequences
 * in case of commit/abort of the global transaction.
 */
void
GTM_RememberCreatedSequence(GlobalTransactionId gxid, void *seq)
{
	GTM_TransactionInfo *gtm_txninfo;
	GTM_TransactionHandle txn = GTM_GXIDToHandle(gxid);
   
	if (txn == InvalidTransactionHandle)
		return;

	gtm_txninfo = GTM_HandleToTransactionInfo(txn);
	gtm_txninfo->gti_created_seqs =
		gtm_lappend(gtm_txninfo->gti_created_seqs, seq);
}

/*
 * GTM_ForgetCreatedSequence
 *		Stop tracking a sequence created in a given transaction (GXID).
 */
void
GTM_ForgetCreatedSequence(GlobalTransactionId gxid, void *seq)
{
	GTM_TransactionInfo *gtm_txninfo;
	GTM_TransactionHandle txn = GTM_GXIDToHandle(gxid);
   
	if (txn == InvalidTransactionHandle)
		return;

	gtm_txninfo = GTM_HandleToTransactionInfo(txn);
	gtm_txninfo->gti_created_seqs =
		gtm_list_delete(gtm_txninfo->gti_created_seqs, seq);
}

/*
 * GTM_RememberDroppedSequence
 *		Remember that transaction GXID modified a given sequence.
 *
 * We need to track this, so that we can properly respond to commit/abort of
 * the global transaction (and either undo or alter the sequence).
 *
 * See GTM_TransactionInfo_Clean for what happens with the tracked sequences
 * in case of commit/abort of the global transaction.
 */
void
GTM_RememberDroppedSequence(GlobalTransactionId gxid, void *seq)
{
	GTM_TransactionInfo *gtm_txninfo;
	GTM_TransactionHandle txn = GTM_GXIDToHandle(gxid);
   
	if (txn == InvalidTransactionHandle)
		return;

	gtm_txninfo = GTM_HandleToTransactionInfo(txn);
	gtm_txninfo->gti_dropped_seqs =
		gtm_lappend(gtm_txninfo->gti_dropped_seqs, seq);
}

/*
 * GTM_RememberDroppedSequence
 *		Remember that transaction GXID dropped a given sequence.
 *
 * We need to track this, so that we can properly respond to commit/abort of
 * the global transaction (and either reinstate or definitely remove the
 * sequence).
 *
 * See GTM_TransactionInfo_Clean for what happens with the tracked sequences
 * in case of commit/abort of the global transaction.
 */
void
GTM_RememberAlteredSequence(GlobalTransactionId gxid, void *seq)
{
	GTM_TransactionInfo *gtm_txninfo;
	GTM_TransactionHandle txn = GTM_GXIDToHandle(gxid);
   
	if (txn == InvalidTransactionHandle)
		return;

	gtm_txninfo = GTM_HandleToTransactionInfo(txn);
	gtm_txninfo->gti_altered_seqs = gtm_lcons(seq,
			gtm_txninfo->gti_altered_seqs);
}
