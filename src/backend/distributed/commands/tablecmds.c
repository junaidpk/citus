/*-------------------------------------------------------------------------
 *
 * tablecmds.c
 *    Commands for altering and creating distributed tables.
 *
 * Copyright (c) 2018, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/tablecmds.h"
#include "distributed/citus_ruleutils.h"
#include "distributed/commands/common.h"
#include "distributed/commands/tablecmds.h"
#include "distributed/distributed_planner.h"
#include "distributed/foreign_constraint.h"
#include "distributed/listutils.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_sync.h"
#include "distributed/multi_executor.h"
#include "distributed/multi_join_order.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/reference_table_utils.h"
#include "distributed/resource_lock.h"
#include "distributed/transaction_management.h"
#include "nodes/parsenodes.h"
#include "storage/lmgr.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"


#define LOCK_RELATION_IF_EXISTS "SELECT lock_relation_if_exists('%s', '%s');"


/* Local functions forward declarations for unsupported command checks */
static void ErrorIfUnsupportedTruncateStmt(TruncateStmt *truncateStatement);
static void ExecuteTruncateStmtSequentialIfNecessary(TruncateStmt *command);
static void EnsurePartitionTableNotReplicatedForTruncate(TruncateStmt *truncateStatement);
static void LockTruncatedRelationMetadataInWorkers(TruncateStmt *truncateStatement);
static void AcquireDistributedLockOnRelations(List *relationIdList, LOCKMODE lockMode);


/*
 * ProcessDropTableStmt processes DROP TABLE commands for partitioned tables.
 * If we are trying to DROP partitioned tables, we first need to go to MX nodes
 * and DETACH partitions from their parents. Otherwise, we process DROP command
 * multiple times in MX workers. For shards, we send DROP commands with IF EXISTS
 * parameter which solves problem of processing same command multiple times.
 * However, for distributed table itself, we directly remove related table from
 * Postgres catalogs via performDeletion function, thus we need to be cautious
 * about not processing same DROP command twice.
 */
void
ProcessDropTableStmt(DropStmt *dropTableStatement)
{
	ListCell *dropTableCell = NULL;

	Assert(dropTableStatement->removeType == OBJECT_TABLE);

	foreach(dropTableCell, dropTableStatement->objects)
	{
		List *tableNameList = (List *) lfirst(dropTableCell);
		RangeVar *tableRangeVar = makeRangeVarFromNameList(tableNameList);
		bool missingOK = true;
		List *partitionList = NIL;
		ListCell *partitionCell = NULL;

		Oid relationId = RangeVarGetRelid(tableRangeVar, AccessShareLock, missingOK);

		/* we're not interested in non-valid, non-distributed relations */
		if (relationId == InvalidOid || !IsDistributedTable(relationId))
		{
			continue;
		}

		/* invalidate foreign key cache if the table involved in any foreign key */
		if ((TableReferenced(relationId) || TableReferencing(relationId)))
		{
			MarkInvalidateForeignKeyGraph();
		}

		/* we're only interested in partitioned and mx tables */
		if (!ShouldSyncTableMetadata(relationId) || !PartitionedTable(relationId))
		{
			continue;
		}

		EnsureCoordinator();

		partitionList = PartitionList(relationId);
		if (list_length(partitionList) == 0)
		{
			continue;
		}

		SendCommandToWorkers(WORKERS_WITH_METADATA, DISABLE_DDL_PROPAGATION);

		foreach(partitionCell, partitionList)
		{
			Oid partitionRelationId = lfirst_oid(partitionCell);
			char *detachPartitionCommand =
				GenerateDetachPartitionCommand(partitionRelationId);

			SendCommandToWorkers(WORKERS_WITH_METADATA, detachPartitionCommand);
		}
	}
}


/*
 * ProcessTruncateStatement handles few things that should be
 * done before standard process utility is called for truncate
 * command.
 */
void
ProcessTruncateStatement(TruncateStmt *truncateStatement)
{
	ErrorIfUnsupportedTruncateStmt(truncateStatement);
	EnsurePartitionTableNotReplicatedForTruncate(truncateStatement);
	ExecuteTruncateStmtSequentialIfNecessary(truncateStatement);
	LockTruncatedRelationMetadataInWorkers(truncateStatement);
}


/*
 * ErrorIfUnsupportedTruncateStmt errors out if the command attempts to
 * truncate a distributed foreign table.
 */
static void
ErrorIfUnsupportedTruncateStmt(TruncateStmt *truncateStatement)
{
	List *relationList = truncateStatement->relations;
	ListCell *relationCell = NULL;
	foreach(relationCell, relationList)
	{
		RangeVar *rangeVar = (RangeVar *) lfirst(relationCell);
		Oid relationId = RangeVarGetRelid(rangeVar, NoLock, true);
		char relationKind = get_rel_relkind(relationId);
		if (IsDistributedTable(relationId) &&
			relationKind == RELKIND_FOREIGN_TABLE)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("truncating distributed foreign tables is "
								   "currently unsupported"),
							errhint("Use master_drop_all_shards to remove "
									"foreign table's shards.")));
		}
	}
}


/*
 * EnsurePartitionTableNotReplicatedForTruncate a simple wrapper around
 * EnsurePartitionTableNotReplicated for TRUNCATE command.
 */
static void
EnsurePartitionTableNotReplicatedForTruncate(TruncateStmt *truncateStatement)
{
	ListCell *relationCell = NULL;

	foreach(relationCell, truncateStatement->relations)
	{
		RangeVar *relationRV = (RangeVar *) lfirst(relationCell);
		Relation relation = heap_openrv(relationRV, NoLock);
		Oid relationId = RelationGetRelid(relation);

		if (!IsDistributedTable(relationId))
		{
			heap_close(relation, NoLock);
			continue;
		}

		EnsurePartitionTableNotReplicated(relationId);

		heap_close(relation, NoLock);
	}
}


/*
 * ExecuteTruncateStmtSequentialIfNecessary decides if the TRUNCATE stmt needs
 * to run sequential. If so, it calls SetLocalMultiShardModifyModeToSequential().
 *
 * If a reference table which has a foreign key from a distributed table is truncated
 * we need to execute the command sequentially to avoid self-deadlock.
 */
static void
ExecuteTruncateStmtSequentialIfNecessary(TruncateStmt *command)
{
	List *relationList = command->relations;
	ListCell *relationCell = NULL;
	bool failOK = false;

	foreach(relationCell, relationList)
	{
		RangeVar *rangeVar = (RangeVar *) lfirst(relationCell);
		Oid relationId = RangeVarGetRelid(rangeVar, NoLock, failOK);

		if (IsDistributedTable(relationId) &&
			PartitionMethod(relationId) == DISTRIBUTE_BY_NONE &&
			TableReferenced(relationId))
		{
			char *relationName = get_rel_name(relationId);

			ereport(DEBUG1, (errmsg("switching to sequential query execution mode"),
							 errdetail(
								 "Reference relation \"%s\" is modified, which might lead "
								 "to data inconsistencies or distributed deadlocks via "
								 "parallel accesses to hash distributed relations due to "
								 "foreign keys. Any parallel modification to "
								 "those hash distributed relations in the same "
								 "transaction can only be executed in sequential query "
								 "execution mode", relationName)));

			SetLocalMultiShardModifyModeToSequential();

			/* nothing to do more */
			return;
		}
	}
}


/*
 * LockTruncatedRelationMetadataInWorkers determines if distributed
 * lock is necessary for truncated relations, and acquire locks.
 *
 * LockTruncatedRelationMetadataInWorkers handles distributed locking
 * of truncated tables before standard utility takes over.
 *
 * Actual distributed truncation occurs inside truncate trigger.
 *
 * This is only for distributed serialization of truncate commands.
 * The function assumes that there is no foreign key relation between
 * non-distributed and distributed relations.
 */
static void
LockTruncatedRelationMetadataInWorkers(TruncateStmt *truncateStatement)
{
	List *distributedRelationList = NIL;
	ListCell *relationCell = NULL;

	/* nothing to do if there is no metadata at worker nodes */
	if (!ClusterHasKnownMetadataWorkers())
	{
		return;
	}

	foreach(relationCell, truncateStatement->relations)
	{
		RangeVar *relationRV = (RangeVar *) lfirst(relationCell);
		Relation relation = heap_openrv(relationRV, NoLock);
		Oid relationId = RelationGetRelid(relation);
		DistTableCacheEntry *cacheEntry = NULL;
		List *referencingTableList = NIL;
		ListCell *referencingTableCell = NULL;

		if (!IsDistributedTable(relationId))
		{
			heap_close(relation, NoLock);
			continue;
		}

		if (list_member_oid(distributedRelationList, relationId))
		{
			heap_close(relation, NoLock);
			continue;
		}

		distributedRelationList = lappend_oid(distributedRelationList, relationId);

		cacheEntry = DistributedTableCacheEntry(relationId);
		Assert(cacheEntry != NULL);

		referencingTableList = cacheEntry->referencingRelationsViaForeignKey;
		foreach(referencingTableCell, referencingTableList)
		{
			Oid referencingRelationId = lfirst_oid(referencingTableCell);
			distributedRelationList = list_append_unique_oid(distributedRelationList,
															 referencingRelationId);
		}

		heap_close(relation, NoLock);
	}

	if (distributedRelationList != NIL)
	{
		AcquireDistributedLockOnRelations(distributedRelationList, AccessExclusiveLock);
	}
}


/*
 * AcquireDistributedLockOnRelations acquire a distributed lock on worker nodes
 * for given list of relations ids. Relation id list and worker node list
 * sorted so that the lock is acquired in the same order regardless of which
 * node it was run on. Notice that no lock is acquired on coordinator node.
 *
 * Notice that the locking functions is sent to all workers regardless of if
 * it has metadata or not. This is because a worker node only knows itself
 * and previous workers that has metadata sync turned on. The node does not
 * know about other nodes that have metadata sync turned on afterwards.
 */
static void
AcquireDistributedLockOnRelations(List *relationIdList, LOCKMODE lockMode)
{
	ListCell *relationIdCell = NULL;
	List *workerNodeList = ActivePrimaryNodeList();
	const char *lockModeText = LockModeToLockModeText(lockMode);

	/*
	 * We want to acquire locks in the same order accross the nodes.
	 * Although relation ids may change, their ordering will not.
	 */
	relationIdList = SortList(relationIdList, CompareOids);
	workerNodeList = SortList(workerNodeList, CompareWorkerNodes);

	BeginOrContinueCoordinatedTransaction();

	foreach(relationIdCell, relationIdList)
	{
		Oid relationId = lfirst_oid(relationIdCell);

		/*
		 * We only acquire distributed lock on relation if
		 * the relation is sync'ed between mx nodes.
		 */
		if (ShouldSyncTableMetadata(relationId))
		{
			char *qualifiedRelationName = generate_qualified_relation_name(relationId);
			StringInfo lockRelationCommand = makeStringInfo();
			ListCell *workerNodeCell = NULL;

			appendStringInfo(lockRelationCommand, LOCK_RELATION_IF_EXISTS,
							 qualifiedRelationName, lockModeText);

			foreach(workerNodeCell, workerNodeList)
			{
				WorkerNode *workerNode = (WorkerNode *) lfirst(workerNodeCell);
				char *nodeName = workerNode->workerName;
				int nodePort = workerNode->workerPort;

				/* if local node is one of the targets, acquire the lock locally */
				if (workerNode->groupId == GetLocalGroupId())
				{
					LockRelationOid(relationId, lockMode);
					continue;
				}

				SendCommandToWorker(nodeName, nodePort, lockRelationCommand->data);
			}
		}
	}
}
