/*-------------------------------------------------------------------------
 *
 * procarray.h
 *	  POSTGRES process array definitions.
 *
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/storage/procarray.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PROCARRAY_H
#define PROCARRAY_H

#include "storage/standby.h"
#include "utils/relcache.h"
#include "utils/snapshot.h"


extern void ProcArrayAdd(PGPROC *proc);
extern void ProcArrayRemove(PGPROC *proc, TransactionId latestXid);

extern void ProcArrayEndTransaction(PGPROC *proc, TransactionId latestXid);
extern void ProcArrayClearTransaction(PGPROC *proc);

extern void ProcArrayInitRecovery(FullTransactionId initializedUptoXID);
extern void ProcArrayApplyRecoveryInfo(RunningTransactions running);
extern void ProcArrayApplyXidAssignment(TransactionId topxid,
										int nsubxids, TransactionId *subxids);

extern void RecordKnownAssignedTransactionIds(TransactionId xid);
extern void ExpireTreeKnownAssignedTransactionIds(TransactionId xid,
												  int nsubxids, TransactionId *subxids,
												  TransactionId max_xid);
extern void ExpireAllKnownAssignedTransactionIds(void);
extern void ExpireOldKnownAssignedTransactionIds(FullTransactionId fxid);
extern void KnownAssignedTransactionIdsIdleMaintenance(void);

extern int	GetMaxSnapshotXidCount(void);
extern int	GetMaxSnapshotSubxidCount(void);

extern Snapshot GetSnapshotData(Snapshot snapshot);

extern bool ProcArrayInstallImportedXmin(TransactionId xmin,
										 VirtualTransactionId *sourcevxid);
extern bool ProcArrayInstallRestoredXmin(TransactionId xmin, PGPROC *proc);

extern RunningTransactions GetRunningTransactionData(Oid dbid);

extern bool TransactionIdIsInProgress(TransactionId xid);
extern TransactionId GetOldestNonRemovableTransactionId(Relation rel);
extern TransactionId GetOldestTransactionIdConsideredRunning(void);
extern TransactionId GetOldestActiveTransactionId(bool inCommitOnly,
												  bool allDbs);
extern TransactionId GetOldestSafeDecodingTransactionId(bool catalogOnly);
extern void GetReplicationHorizons(TransactionId *xmin, TransactionId *catalog_xmin);

extern VirtualTransactionId *GetVirtualXIDsDelayingChkpt(int *nvxids, int type);
extern bool HaveVirtualXIDsDelayingChkpt(VirtualTransactionId *vxids,
										 int nvxids, int type);

extern PGPROC *ProcNumberGetProc(int procNumber);
extern void ProcNumberGetTransactionIds(int procNumber, TransactionId *xid,
										TransactionId *xmin, int *nsubxid,
										bool *overflowed);
extern PGPROC *BackendPidGetProc(int pid);
extern PGPROC *BackendPidGetProcWithLock(int pid);
extern int	BackendXidGetPid(TransactionId xid);
extern bool IsBackendPid(int pid);

extern VirtualTransactionId *GetCurrentVirtualXIDs(TransactionId limitXmin,
												   bool excludeXmin0, bool allDbs, int excludeVacuum,
												   int *nvxids);
extern VirtualTransactionId *GetConflictingVirtualXIDs(TransactionId limitXmin, Oid dbOid);

extern bool SignalRecoveryConflict(PGPROC *proc, pid_t pid, RecoveryConflictReason reason);
extern bool SignalRecoveryConflictWithVirtualXID(VirtualTransactionId vxid, RecoveryConflictReason reason);
extern void SignalRecoveryConflictWithDatabase(Oid databaseid, RecoveryConflictReason reason);


extern bool MinimumActiveBackends(int min);
extern int	CountDBBackends(Oid databaseid);
extern int	CountDBConnections(Oid databaseid);
extern int	CountUserBackends(Oid roleid);
extern bool CountOtherDBBackends(Oid databaseId,
								 int *nbackends, int *nprepared);
extern void TerminateOtherDBBackends(Oid databaseId);

extern void XidCacheRemoveRunningXids(TransactionId xid,
									  int nxids, const TransactionId *xids,
									  TransactionId latestXid);

extern void ProcArraySetReplicationSlotXmin(TransactionId xmin,
											TransactionId catalog_xmin, bool already_locked);

extern void ProcArrayGetReplicationSlotXmin(TransactionId *xmin,
											TransactionId *catalog_xmin);

/*
 * Convert a 32 bit transaction id into 64 bit transaction id, by assuming it
 * is within MaxTransactionId / 2 of XidFromFullTransactionId(rel).
 *
 * Be very careful about when to use this function. It can only safely be used
 * when there is a guarantee that xid is within MaxTransactionId / 2 xids of
 * rel. That e.g. can be guaranteed if the caller assures a snapshot is
 * held by the backend and xid is from a table (where vacuum/freezing ensures
 * the xid has to be within that range), or if xid is from the procarray and
 * prevents xid wraparound that way.
 */
static inline FullTransactionId
FullXidRelativeTo(FullTransactionId rel, TransactionId xid)
{
	TransactionId rel_xid = XidFromFullTransactionId(rel);

	Assert(TransactionIdIsValid(xid));
	Assert(TransactionIdIsValid(rel_xid));

	/* not guaranteed to find issues, but likely to catch mistakes */
	AssertTransactionIdInAllowableRange(xid);

	return FullTransactionIdFromU64(U64FromFullTransactionId(rel)
									+ (int32) (xid - rel_xid));
}

#endif							/* PROCARRAY_H */
