/*-------------------------------------------------------------------------
 * utility_hook.c
 *	  Citus utility hook and related functionality.
 *
 * The utility hook is called by PostgreSQL when processing any command
 * that is not SELECT, UPDATE, DELETE, INSERT, in place of the regular
 * ProcessUtility function. We use this primarily to implement (or in
 * some cases prevent) DDL commands and COPY on distributed tables.
 *
 * For DDL commands that affect distributed tables, we check whether
 * they are valid (and implemented) for the distributed table and then
 * propagate the command to all shards and, in case of MX, to distributed
 * tables on other nodes. We still call the original ProcessUtility
 * function to apply catalog changes on the coordinator.
 *
 * For COPY into a distributed table, we provide an alternative
 * implementation in ProcessCopyStmt that sends rows to shards based
 * on their distribution column value instead of writing it to the local
 * table on the coordinator. For COPY from a distributed table, we
 * replace the table with a SELECT * FROM table and pass it back to
 * ProcessUtility, which will plan the query via the distributed planner
 * hook.
 *
 * Copyright (c) 2012-2018, Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "c.h"
#include "libpq-fe.h"
#include "miscadmin.h"
#include "port.h"

#include <string.h>

#include "access/attnum.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/tupdesc.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/index.h"
#include "catalog/indexing.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_class.h"
#include "citus_version.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "commands/prepare.h"
#include "distributed/citus_ruleutils.h"
#include "distributed/colocation_utils.h"
#include "distributed/commands/indexcmds.h"
#include "distributed/commands/schemacmds.h"
#include "distributed/commands/sequence.h"
#include "distributed/commands/tablecmds.h"
#include "distributed/foreign_constraint.h"
#include "distributed/intermediate_results.h"
#include "distributed/listutils.h"
#include "distributed/maintenanced.h"
#include "distributed/master_metadata_utility.h"
#include "distributed/master_protocol.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_sync.h"
#include "distributed/multi_copy.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/distributed_planner.h"
#include "distributed/multi_router_executor.h"
#include "distributed/multi_router_planner.h"
#include "distributed/multi_shard_transaction.h"
#include "distributed/utility_hook.h" /* IWYU pragma: keep */
#include "distributed/pg_dist_partition.h"
#include "distributed/policy.h"
#include "distributed/reference_table_utils.h"
#include "distributed/relation_access_tracking.h"
#include "distributed/resource_lock.h"
#include "distributed/transaction_management.h"
#include "distributed/transmit.h"
#include "distributed/worker_transaction.h"
#include "distributed/version_compat.h"
#include "executor/executor.h"
#include "lib/stringinfo.h"
#include "nodes/bitmapset.h"
#include "nodes/memnodes.h"
#include "nodes/nodes.h"
#include "nodes/params.h"
#include "nodes/parsenodes.h"
#include "nodes/pg_list.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "parser/analyze.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "tcop/dest.h"
#include "tcop/utility.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/elog.h"
#include "utils/errcodes.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/palloc.h"
#include "utils/syscache.h"


bool EnableDDLPropagation = true; /* ddl propagation is enabled */


static bool shouldInvalidateForeignKeyGraph = false;


/* Local functions forward declarations for deciding when to perform processing/checks */
static bool IsCitusExtensionStmt(Node *parsetree);

/* Local functions forward declarations for processing distributed table commands */
static void ProcessCreateTableStmtPartitionOf(CreateStmt *createStatement);
static void ProcessAlterTableStmtAttachPartition(AlterTableStmt *alterTableStatement);
static List * PlanAlterTableStmt(AlterTableStmt *alterTableStatement,
								 const char *alterTableCommand);
static List * PlanRenameStmt(RenameStmt *renameStmt, const char *renameCommand);
static Node * WorkerProcessAlterTableStmt(AlterTableStmt *alterTableStatement,
										  const char *alterTableCommand);
static List * PlanAlterObjectSchemaStmt(AlterObjectSchemaStmt *alterObjectSchemaStmt,
										const char *alterObjectSchemaCommand);
static void ProcessVacuumStmt(VacuumStmt *vacuumStmt, const char *vacuumCommand);
static bool IsDistributedVacuumStmt(VacuumStmt *vacuumStmt, List *vacuumRelationIdList);
static List * VacuumTaskList(Oid relationId, int vacuumOptions, List *vacuumColumnList);
static StringInfo DeparseVacuumStmtPrefix(int vacuumFlags);
static char * DeparseVacuumColumnNames(List *columnNameList);


/* Local functions forward declarations for unsupported command checks */
static void ErrorIfUnstableCreateOrAlterExtensionStmt(Node *parsetree);
static void ErrorIfUnsupportedAlterTableStmt(AlterTableStmt *alterTableStatement);
static void ErrorIfUnsupportedAlterIndexStmt(AlterTableStmt *alterTableStatement);
static void ErrorIfAlterDropsPartitionColumn(AlterTableStmt *alterTableStatement);
static void ErrorIfUnsupportedRenameStmt(RenameStmt *renameStmt);
static void ErrorIfUnsupportedAlterAddConstraintStmt(AlterTableStmt *alterTableStatement);

/* Local functions forward declarations for helper functions */
static char * ExtractNewExtensionVersion(Node *parsetree);
static bool IsAlterTableRenameStmt(RenameStmt *renameStmt);
static bool IsIndexRenameStmt(RenameStmt *renameStmt);
static bool AlterInvolvesPartitionColumn(AlterTableStmt *alterTableStatement,
										 AlterTableCmd *command);
static void ExecuteDistributedDDLJob(DDLJob *ddlJob);
static char * SetSearchPathToCurrentSearchPathCommand(void);
static char * CurrentSearchPath(void);
static List * DDLTaskList(Oid relationId, const char *commandString);
static List * InterShardDDLTaskList(Oid leftRelationId, Oid rightRelationId,
									const char *commandString);
static void PostProcessUtility(Node *parsetree);
extern List * PlanGrantStmt(GrantStmt *grantStmt);
static List * CollectGrantTableIdList(GrantStmt *grantStmt);
static void InvalidateForeignKeyGraphForDDL(void);

static void ErrorUnsupportedAlterTableAddColumn(Oid relationId, AlterTableCmd *command,
												Constraint *constraint);

/*
 * We need to run some of the commands sequentially if there is a foreign constraint
 * from/to reference table.
 */
static bool SetupExecutionModeForAlterTable(Oid relationId, AlterTableCmd *command);


/*
 * multi_ProcessUtility9x is the 9.x-compatible wrapper for Citus' main utility
 * hook. It simply adapts the old-style hook to call into the new-style (10+)
 * hook, which is what now houses all actual logic.
 */
void
multi_ProcessUtility9x(Node *parsetree,
					   const char *queryString,
					   ProcessUtilityContext context,
					   ParamListInfo params,
					   DestReceiver *dest,
					   char *completionTag)
{
	PlannedStmt *plannedStmt = makeNode(PlannedStmt);
	plannedStmt->commandType = CMD_UTILITY;
	plannedStmt->utilityStmt = parsetree;

	multi_ProcessUtility(plannedStmt, queryString, context, params, NULL, dest,
						 completionTag);
}


/*
 * CitusProcessUtility is a version-aware wrapper of ProcessUtility to account
 * for argument differences between the 9.x and 10+ PostgreSQL versions.
 */
void
CitusProcessUtility(Node *node, const char *queryString, ProcessUtilityContext context,
					ParamListInfo params, DestReceiver *dest, char *completionTag)
{
#if (PG_VERSION_NUM >= 100000)
	PlannedStmt *plannedStmt = makeNode(PlannedStmt);
	plannedStmt->commandType = CMD_UTILITY;
	plannedStmt->utilityStmt = node;

	ProcessUtility(plannedStmt, queryString, context, params, NULL, dest,
				   completionTag);
#else
	ProcessUtility(node, queryString, context, params, dest, completionTag);
#endif
}


/*
 * multi_ProcessUtility is the main entry hook for implementing Citus-specific
 * utility behavior. Its primary responsibilities are intercepting COPY and DDL
 * commands and augmenting the coordinator's command with corresponding tasks
 * to be run on worker nodes, after suitably ensuring said commands' options
 * are fully supported by Citus. Much of the DDL behavior is toggled by Citus'
 * enable_ddl_propagation GUC. In addition to DDL and COPY, utilities such as
 * TRUNCATE and VACUUM are also supported.
 */
void
multi_ProcessUtility(PlannedStmt *pstmt,
					 const char *queryString,
					 ProcessUtilityContext context,
					 ParamListInfo params,
					 struct QueryEnvironment *queryEnv,
					 DestReceiver *dest,
					 char *completionTag)
{
	Node *parsetree = pstmt->utilityStmt;
	bool commandMustRunAsOwner = false;
	Oid savedUserId = InvalidOid;
	int savedSecurityContext = 0;
	List *ddlJobs = NIL;
	bool checkExtensionVersion = false;

	if (IsA(parsetree, TransactionStmt))
	{
		/*
		 * Transaction statements (e.g. ABORT, COMMIT) can be run in aborted
		 * transactions in which case a lot of checks cannot be done safely in
		 * that state. Since we never need to intercept transaction statements,
		 * skip our checks and immediately fall into standard_ProcessUtility.
		 */
#if (PG_VERSION_NUM >= 100000)
		standard_ProcessUtility(pstmt, queryString, context,
								params, queryEnv, dest, completionTag);
#else
		standard_ProcessUtility(parsetree, queryString, context,
								params, dest, completionTag);
#endif

		return;
	}

	checkExtensionVersion = IsCitusExtensionStmt(parsetree);
	if (EnableVersionChecks && checkExtensionVersion)
	{
		ErrorIfUnstableCreateOrAlterExtensionStmt(parsetree);
	}


	if (!CitusHasBeenLoaded())
	{
		/*
		 * Ensure that utility commands do not behave any differently until CREATE
		 * EXTENSION is invoked.
		 */
#if (PG_VERSION_NUM >= 100000)
		standard_ProcessUtility(pstmt, queryString, context,
								params, queryEnv, dest, completionTag);
#else
		standard_ProcessUtility(parsetree, queryString, context,
								params, dest, completionTag);
#endif

		return;
	}

#if (PG_VERSION_NUM >= 110000)
	if (IsA(parsetree, CallStmt))
	{
		/*
		 * Stored procedures are a bit strange in the sense that some statements
		 * are not in a transaction block, but can be rolled back. We need to
		 * make sure we send all statements in a transaction block. The
		 * StoredProcedureLevel variable signals this to the router executor
		 * and indicates how deep in the call stack we are in case of nested
		 * stored procedures.
		 */
		StoredProcedureLevel += 1;

		PG_TRY();
		{
			standard_ProcessUtility(pstmt, queryString, context,
									params, queryEnv, dest, completionTag);

			StoredProcedureLevel -= 1;
		}
		PG_CATCH();
		{
			StoredProcedureLevel -= 1;
			PG_RE_THROW();
		}
		PG_END_TRY();

		return;
	}
#endif

	/*
	 * TRANSMIT used to be separate command, but to avoid patching the grammar
	 * it's no overlaid onto COPY, but with FORMAT = 'transmit' instead of the
	 * normal FORMAT options.
	 */
	if (IsTransmitStmt(parsetree))
	{
		CopyStmt *copyStatement = (CopyStmt *) parsetree;

		VerifyTransmitStmt(copyStatement);

		/* ->relation->relname is the target file in our overloaded COPY */
		if (copyStatement->is_from)
		{
			RedirectCopyDataToRegularFile(copyStatement->relation->relname);
		}
		else
		{
			SendRegularFile(copyStatement->relation->relname);
		}

		/* Don't execute the faux copy statement */
		return;
	}

	if (IsA(parsetree, CopyStmt))
	{
		MemoryContext planContext = GetMemoryChunkContext(parsetree);
		MemoryContext previousContext;

		parsetree = copyObject(parsetree);
		parsetree = ProcessCopyStmt((CopyStmt *) parsetree, completionTag,
									&commandMustRunAsOwner);

		previousContext = MemoryContextSwitchTo(planContext);
		parsetree = copyObject(parsetree);
		MemoryContextSwitchTo(previousContext);

		if (parsetree == NULL)
		{
			return;
		}
	}

	/* we're mostly in DDL (and VACUUM/TRUNCATE) territory at this point... */

	if (IsA(parsetree, CreateSeqStmt))
	{
		ErrorIfUnsupportedSeqStmt((CreateSeqStmt *) parsetree);
	}

	if (IsA(parsetree, AlterSeqStmt))
	{
		ErrorIfDistributedAlterSeqOwnedBy((AlterSeqStmt *) parsetree);
	}

	if (IsA(parsetree, TruncateStmt))
	{
		ProcessTruncateStatement((TruncateStmt *) parsetree);
	}

	/* only generate worker DDLJobs if propagation is enabled */
	if (EnableDDLPropagation)
	{
		if (IsA(parsetree, IndexStmt))
		{
			MemoryContext oldContext = MemoryContextSwitchTo(GetMemoryChunkContext(
																 parsetree));

			/* copy parse tree since we might scribble on it to fix the schema name */
			parsetree = copyObject(parsetree);

			MemoryContextSwitchTo(oldContext);

			ddlJobs = PlanIndexStmt((IndexStmt *) parsetree, queryString);
		}

		if (IsA(parsetree, DropStmt))
		{
			DropStmt *dropStatement = (DropStmt *) parsetree;
			if (dropStatement->removeType == OBJECT_INDEX)
			{
				ddlJobs = PlanDropIndexStmt(dropStatement, queryString);
			}

			if (dropStatement->removeType == OBJECT_TABLE)
			{
				ProcessDropTableStmt(dropStatement);
			}

			if (dropStatement->removeType == OBJECT_SCHEMA)
			{
				ProcessDropSchemaStmt(dropStatement);
			}

			if (dropStatement->removeType == OBJECT_POLICY)
			{
				ddlJobs = PlanDropPolicyStmt(dropStatement, queryString);
			}
		}

		if (IsA(parsetree, AlterTableStmt))
		{
			AlterTableStmt *alterTableStmt = (AlterTableStmt *) parsetree;
			if (alterTableStmt->relkind == OBJECT_TABLE ||
				alterTableStmt->relkind == OBJECT_INDEX)
			{
				ddlJobs = PlanAlterTableStmt(alterTableStmt, queryString);
			}
		}

		/*
		 * ALTER TABLE ... RENAME statements have their node type as RenameStmt and
		 * not AlterTableStmt. So, we intercept RenameStmt to tackle these commands.
		 */
		if (IsA(parsetree, RenameStmt))
		{
			ddlJobs = PlanRenameStmt((RenameStmt *) parsetree, queryString);
		}

		/*
		 * ALTER ... SET SCHEMA statements have their node type as AlterObjectSchemaStmt.
		 * So, we intercept AlterObjectSchemaStmt to tackle these commands.
		 */
		if (IsA(parsetree, AlterObjectSchemaStmt))
		{
			AlterObjectSchemaStmt *setSchemaStmt = (AlterObjectSchemaStmt *) parsetree;
			ddlJobs = PlanAlterObjectSchemaStmt(setSchemaStmt, queryString);
		}

		if (IsA(parsetree, CreatePolicyStmt))
		{
			ddlJobs = PlanCreatePolicyStmt((CreatePolicyStmt *) parsetree);
		}

		if (IsA(parsetree, AlterPolicyStmt))
		{
			ddlJobs = PlanAlterPolicyStmt((AlterPolicyStmt *) parsetree);
		}

		/*
		 * ALTER TABLE ALL IN TABLESPACE statements have their node type as
		 * AlterTableMoveAllStmt. At the moment we do not support this functionality in
		 * the distributed environment. We warn out here.
		 */
		if (IsA(parsetree, AlterTableMoveAllStmt))
		{
			ereport(WARNING, (errmsg("not propagating ALTER TABLE ALL IN TABLESPACE "
									 "commands to worker nodes"),
							  errhint("Connect to worker nodes directly to manually "
									  "move all tables.")));
		}
	}
	else
	{
		/*
		 * citus.enable_ddl_propagation is disabled, which means that PostgreSQL
		 * should handle the DDL command on a distributed table directly, without
		 * Citus intervening. The only exception is partition column drop, in
		 * which case we error out. Advanced Citus users use this to implement their
		 * own DDL propagation. We also use it to avoid re-propagating DDL commands
		 * when changing MX tables on workers. Below, we also make sure that DDL
		 * commands don't run queries that might get intercepted by Citus and error
		 * out, specifically we skip validation in foreign keys.
		 */

		if (IsA(parsetree, AlterTableStmt))
		{
			AlterTableStmt *alterTableStmt = (AlterTableStmt *) parsetree;
			if (alterTableStmt->relkind == OBJECT_TABLE)
			{
				ErrorIfAlterDropsPartitionColumn(alterTableStmt);

				/*
				 * When issuing an ALTER TABLE ... ADD FOREIGN KEY command, the
				 * the validation step should be skipped on the distributed table.
				 * Therefore, we check whether the given ALTER TABLE statement is a
				 * FOREIGN KEY constraint and if so disable the validation step.
				 * Note that validation is done on the shard level when DDL
				 * propagation is enabled. Unlike the preceeding Plan* calls, the
				 * following eagerly executes some tasks on workers.
				 */
				parsetree = WorkerProcessAlterTableStmt(alterTableStmt, queryString);
			}
		}
	}

	/* inform the user about potential caveats */
	if (IsA(parsetree, CreatedbStmt))
	{
		ereport(NOTICE, (errmsg("Citus partially supports CREATE DATABASE for "
								"distributed databases"),
						 errdetail("Citus does not propagate CREATE DATABASE "
								   "command to workers"),
						 errhint("You can manually create a database and its "
								 "extensions on workers.")));
	}
	else if (IsA(parsetree, CreateRoleStmt))
	{
		ereport(NOTICE, (errmsg("not propagating CREATE ROLE/USER commands to worker"
								" nodes"),
						 errhint("Connect to worker nodes directly to manually create all"
								 " necessary users and roles.")));
	}

	/*
	 * Make sure that on DROP DATABASE we terminate the background deamon
	 * associated with it.
	 */
	if (IsA(parsetree, DropdbStmt))
	{
		const bool missingOK = true;
		DropdbStmt *dropDbStatement = (DropdbStmt *) parsetree;
		char *dbname = dropDbStatement->dbname;
		Oid databaseOid = get_database_oid(dbname, missingOK);

		if (OidIsValid(databaseOid))
		{
			StopMaintenanceDaemon(databaseOid);
		}
	}

	/* set user if needed and go ahead and run local utility using standard hook */
	if (commandMustRunAsOwner)
	{
		GetUserIdAndSecContext(&savedUserId, &savedSecurityContext);
		SetUserIdAndSecContext(CitusExtensionOwner(), SECURITY_LOCAL_USERID_CHANGE);
	}

#if (PG_VERSION_NUM >= 100000)
	pstmt->utilityStmt = parsetree;
	standard_ProcessUtility(pstmt, queryString, context,
							params, queryEnv, dest, completionTag);
#else
	standard_ProcessUtility(parsetree, queryString, context,
							params, dest, completionTag);
#endif


	/*
	 * We only process CREATE TABLE ... PARTITION OF commands in the function below
	 * to handle the case when user creates a table as a partition of distributed table.
	 */
	if (IsA(parsetree, CreateStmt))
	{
		CreateStmt *createStatement = (CreateStmt *) parsetree;

		ProcessCreateTableStmtPartitionOf(createStatement);
	}

	/*
	 * We only process ALTER TABLE ... ATTACH PARTITION commands in the function below
	 * and distribute the partition if necessary.
	 */
	if (IsA(parsetree, AlterTableStmt))
	{
		AlterTableStmt *alterTableStatement = (AlterTableStmt *) parsetree;

		ProcessAlterTableStmtAttachPartition(alterTableStatement);
	}

	/* don't run post-process code for local commands */
	if (ddlJobs != NIL)
	{
		PostProcessUtility(parsetree);
	}

	if (commandMustRunAsOwner)
	{
		SetUserIdAndSecContext(savedUserId, savedSecurityContext);
	}

	/*
	 * Re-forming the foreign key graph relies on the command being executed
	 * on the local table first. However, in order to decide whether the
	 * command leads to an invalidation, we need to check before the command
	 * is being executed since we read pg_constraint table. Thus, we maintain a
	 * local flag and do the invalidation after multi_ProcessUtility,
	 * before ExecuteDistributedDDLJob().
	 */
	InvalidateForeignKeyGraphForDDL();

	/* after local command has completed, finish by executing worker DDLJobs, if any */
	if (ddlJobs != NIL)
	{
		ListCell *ddlJobCell = NULL;

		/*
		 * At this point, ALTER TABLE command has already run on the master, so we
		 * are checking constraints over the table with constraints already defined
		 * (to make the constraint check process same for ALTER TABLE and CREATE
		 * TABLE). If constraints do not fulfill the rules we defined, they will
		 * be removed and the table will return back to the state before the ALTER
		 * TABLE command.
		 */
		if (IsA(parsetree, AlterTableStmt))
		{
			AlterTableStmt *alterTableStatement = (AlterTableStmt *) parsetree;
			List *commandList = alterTableStatement->cmds;
			ListCell *commandCell = NULL;

			foreach(commandCell, commandList)
			{
				AlterTableCmd *command = (AlterTableCmd *) lfirst(commandCell);
				AlterTableType alterTableType = command->subtype;

				if (alterTableType == AT_AddConstraint)
				{
					LOCKMODE lockmode = NoLock;
					Oid relationId = InvalidOid;
					Constraint *constraint = NULL;

					Assert(list_length(commandList) == 1);

					ErrorIfUnsupportedAlterAddConstraintStmt(alterTableStatement);

					lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
					relationId = AlterTableLookupRelation(alterTableStatement, lockmode);

					if (!OidIsValid(relationId))
					{
						continue;
					}

					constraint = (Constraint *) command->def;
					if (constraint->contype == CONSTR_FOREIGN)
					{
						InvalidateForeignKeyGraph();
					}
				}
				else if (alterTableType == AT_AddColumn)
				{
					List *columnConstraints = NIL;
					ListCell *columnConstraint = NULL;
					Oid relationId = InvalidOid;
					LOCKMODE lockmode = NoLock;

					ColumnDef *columnDefinition = (ColumnDef *) command->def;
					columnConstraints = columnDefinition->constraints;
					if (columnConstraints)
					{
						ErrorIfUnsupportedAlterAddConstraintStmt(alterTableStatement);
					}

					lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
					relationId = AlterTableLookupRelation(alterTableStatement, lockmode);
					if (!OidIsValid(relationId))
					{
						continue;
					}

					foreach(columnConstraint, columnConstraints)
					{
						Constraint *constraint = (Constraint *) lfirst(columnConstraint);

						if (constraint->conname == NULL &&
							(constraint->contype == CONSTR_PRIMARY ||
							 constraint->contype == CONSTR_UNIQUE ||
							 constraint->contype == CONSTR_FOREIGN ||
							 constraint->contype == CONSTR_CHECK))
						{
							ErrorUnsupportedAlterTableAddColumn(relationId, command,
																constraint);
						}
					}
				}
			}
		}

		foreach(ddlJobCell, ddlJobs)
		{
			DDLJob *ddlJob = (DDLJob *) lfirst(ddlJobCell);

			ExecuteDistributedDDLJob(ddlJob);
		}
	}

	/* TODO: fold VACUUM's processing into the above block */
	if (IsA(parsetree, VacuumStmt))
	{
		VacuumStmt *vacuumStmt = (VacuumStmt *) parsetree;

		ProcessVacuumStmt(vacuumStmt, queryString);
	}

	/* warn for CLUSTER command on distributed tables */
	if (IsA(parsetree, ClusterStmt))
	{
		ClusterStmt *clusterStmt = (ClusterStmt *) parsetree;
		bool showPropagationWarning = false;

		/* CLUSTER all */
		if (clusterStmt->relation == NULL)
		{
			showPropagationWarning = true;
		}
		else
		{
			Oid relationId = InvalidOid;
			bool missingOK = false;

			relationId = RangeVarGetRelid(clusterStmt->relation, AccessShareLock,
										  missingOK);

			if (OidIsValid(relationId))
			{
				showPropagationWarning = IsDistributedTable(relationId);
			}
		}

		if (showPropagationWarning)
		{
			ereport(WARNING, (errmsg("not propagating CLUSTER command to worker nodes")));
		}
	}

	/*
	 * Ensure value is valid, we can't do some checks during CREATE
	 * EXTENSION. This is important to register some invalidation callbacks.
	 */
	CitusHasBeenLoaded();
}


static void
ErrorUnsupportedAlterTableAddColumn(Oid relationId, AlterTableCmd *command,
									Constraint *constraint)
{
	ColumnDef *columnDefinition = (ColumnDef *) command->def;
	char *colName = columnDefinition->colname;
	char *errMsg =
		"cannot execute ADD COLUMN command with PRIMARY KEY, UNIQUE, FOREIGN and CHECK constraints";
	StringInfo errHint = makeStringInfo();
	appendStringInfo(errHint, "You can issue each command separately such as ");
	appendStringInfo(errHint,
					 "ALTER TABLE %s ADD COLUMN %s data_type; ALTER TABLE %s ADD CONSTRAINT constraint_name ",
					 get_rel_name(relationId),
					 colName, get_rel_name(relationId));

	if (constraint->contype == CONSTR_UNIQUE)
	{
		appendStringInfo(errHint, "UNIQUE (%s)", colName);
	}
	else if (constraint->contype == CONSTR_PRIMARY)
	{
		appendStringInfo(errHint, "PRIMARY KEY (%s)", colName);
	}
	else if (constraint->contype == CONSTR_CHECK)
	{
		appendStringInfo(errHint, "CHECK (check_expression)");
	}
	else if (constraint->contype == CONSTR_FOREIGN)
	{
		RangeVar *referencedTable = constraint->pktable;
		char *referencedColumn = strVal(lfirst(list_head(constraint->pk_attrs)));
		Oid referencedRelationId = RangeVarGetRelid(referencedTable, NoLock, false);

		appendStringInfo(errHint, "FOREIGN KEY (%s) REFERENCES %s(%s)", colName,
						 get_rel_name(referencedRelationId), referencedColumn);

		if (constraint->fk_del_action == FKCONSTR_ACTION_SETNULL)
		{
			appendStringInfo(errHint, " %s", "ON DELETE SET NULL");
		}
		else if (constraint->fk_del_action == FKCONSTR_ACTION_CASCADE)
		{
			appendStringInfo(errHint, " %s", "ON DELETE CASCADE");
		}
		else if (constraint->fk_del_action == FKCONSTR_ACTION_SETDEFAULT)
		{
			appendStringInfo(errHint, " %s", "ON DELETE SET DEFAULT");
		}
		else if (constraint->fk_del_action == FKCONSTR_ACTION_RESTRICT)
		{
			appendStringInfo(errHint, " %s", "ON DELETE RESTRICT");
		}

		if (constraint->fk_upd_action == FKCONSTR_ACTION_SETNULL)
		{
			appendStringInfo(errHint, " %s", "ON UPDATE SET NULL");
		}
		else if (constraint->fk_upd_action == FKCONSTR_ACTION_CASCADE)
		{
			appendStringInfo(errHint, " %s", "ON UPDATE CASCADE");
		}
		else if (constraint->fk_upd_action == FKCONSTR_ACTION_SETDEFAULT)
		{
			appendStringInfo(errHint, " %s", "ON UPDATE SET DEFAULT");
		}
		else if (constraint->fk_upd_action == FKCONSTR_ACTION_RESTRICT)
		{
			appendStringInfo(errHint, " %s", "ON UPDATE RESTRICT");
		}
	}

	appendStringInfo(errHint, "%s", ";");

	ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					errmsg("%s", errMsg),
					errhint("%s", errHint->data),
					errdetail("Adding a column with a constraint in "
							  "one command is not supported because "
							  "all constraints in Citus must have "
							  "explicit names")));
}


/*
 * MarkInvalidateForeignKeyGraph marks whether the foreign key graph should be
 * invalidated due to a DDL.
 */
void
MarkInvalidateForeignKeyGraph()
{
	shouldInvalidateForeignKeyGraph = true;
}


/*
 * InvalidateForeignKeyGraphForDDL simply keeps track of whether
 * the foreign key graph should be invalidated due to a DDL.
 */
static void
InvalidateForeignKeyGraphForDDL(void)
{
	if (shouldInvalidateForeignKeyGraph)
	{
		InvalidateForeignKeyGraph();

		shouldInvalidateForeignKeyGraph = false;
	}
}


/*
 * IsCitusExtensionStmt returns whether a given utility is a CREATE or ALTER
 * EXTENSION statement which references the citus extension. This function
 * returns false for all other inputs.
 */
static bool
IsCitusExtensionStmt(Node *parsetree)
{
	char *extensionName = "";

	if (IsA(parsetree, CreateExtensionStmt))
	{
		extensionName = ((CreateExtensionStmt *) parsetree)->extname;
	}
	else if (IsA(parsetree, AlterExtensionStmt))
	{
		extensionName = ((AlterExtensionStmt *) parsetree)->extname;
	}

	return (strcmp(extensionName, "citus") == 0);
}


/*
 * ProcessCreateTableStmtPartitionOf takes CreateStmt object as a parameter but
 * it only processes CREATE TABLE ... PARTITION OF statements and it checks if
 * user creates the table as a partition of a distributed table. In that case,
 * it distributes partition as well. Since the table itself is a partition,
 * CreateDistributedTable will attach it to its parent table automatically after
 * distributing it.
 *
 * This function does nothing if PostgreSQL's version is less then 10 and given
 * CreateStmt is not a CREATE TABLE ... PARTITION OF command.
 */
static void
ProcessCreateTableStmtPartitionOf(CreateStmt *createStatement)
{
#if (PG_VERSION_NUM >= 100000)
	if (createStatement->inhRelations != NIL && createStatement->partbound != NULL)
	{
		RangeVar *parentRelation = linitial(createStatement->inhRelations);
		bool parentMissingOk = false;
		Oid parentRelationId = RangeVarGetRelid(parentRelation, NoLock,
												parentMissingOk);

		/* a partition can only inherit from single parent table */
		Assert(list_length(createStatement->inhRelations) == 1);

		Assert(parentRelationId != InvalidOid);

		/*
		 * If a partition is being created and if its parent is a distributed
		 * table, we will distribute this table as well.
		 */
		if (IsDistributedTable(parentRelationId))
		{
			bool missingOk = false;
			Oid relationId = RangeVarGetRelid(createStatement->relation, NoLock,
											  missingOk);
			Var *parentDistributionColumn = DistPartitionKey(parentRelationId);
			char parentDistributionMethod = DISTRIBUTE_BY_HASH;
			char *parentRelationName = generate_qualified_relation_name(parentRelationId);
			bool viaDeprecatedAPI = false;

			CreateDistributedTable(relationId, parentDistributionColumn,
								   parentDistributionMethod, parentRelationName,
								   viaDeprecatedAPI);
		}
	}
#endif
}


/*
 * ProcessAlterTableStmtAttachPartition takes AlterTableStmt object as parameter
 * but it only processes into ALTER TABLE ... ATTACH PARTITION commands and
 * distributes the partition if necessary. There are four cases to consider;
 *
 * Parent is not distributed, partition is not distributed: We do not need to
 * do anything in this case.
 *
 * Parent is not distributed, partition is distributed: This can happen if
 * user first distributes a table and tries to attach it to a non-distributed
 * table. Non-distributed tables cannot have distributed partitions, thus we
 * simply error out in this case.
 *
 * Parent is distributed, partition is not distributed: We should distribute
 * the table and attach it to its parent in workers. CreateDistributedTable
 * perform both of these operations. Thus, we will not propagate ALTER TABLE
 * ... ATTACH PARTITION command to workers.
 *
 * Parent is distributed, partition is distributed: Partition is already
 * distributed, we only need to attach it to its parent in workers. Attaching
 * operation will be performed via propagating this ALTER TABLE ... ATTACH
 * PARTITION command to workers.
 *
 * This function does nothing if PostgreSQL's version is less then 10 and given
 * CreateStmt is not a ALTER TABLE ... ATTACH PARTITION OF command.
 */
static void
ProcessAlterTableStmtAttachPartition(AlterTableStmt *alterTableStatement)
{
#if (PG_VERSION_NUM >= 100000)
	List *commandList = alterTableStatement->cmds;
	ListCell *commandCell = NULL;

	foreach(commandCell, commandList)
	{
		AlterTableCmd *alterTableCommand = (AlterTableCmd *) lfirst(commandCell);

		if (alterTableCommand->subtype == AT_AttachPartition)
		{
			Oid relationId = AlterTableLookupRelation(alterTableStatement, NoLock);
			PartitionCmd *partitionCommand = (PartitionCmd *) alterTableCommand->def;
			bool partitionMissingOk = false;
			Oid partitionRelationId = RangeVarGetRelid(partitionCommand->name, NoLock,
													   partitionMissingOk);

			/*
			 * If user first distributes the table then tries to attach it to non
			 * distributed table, we error out.
			 */
			if (!IsDistributedTable(relationId) &&
				IsDistributedTable(partitionRelationId))
			{
				char *parentRelationName = get_rel_name(partitionRelationId);

				ereport(ERROR, (errmsg("non-distributed tables cannot have "
									   "distributed partitions"),
								errhint("Distribute the partitioned table \"%s\" "
										"instead", parentRelationName)));
			}

			/* if parent of this table is distributed, distribute this table too */
			if (IsDistributedTable(relationId) &&
				!IsDistributedTable(partitionRelationId))
			{
				Var *distributionColumn = DistPartitionKey(relationId);
				char distributionMethod = DISTRIBUTE_BY_HASH;
				char *parentRelationName = generate_qualified_relation_name(relationId);
				bool viaDeprecatedAPI = false;

				CreateDistributedTable(partitionRelationId, distributionColumn,
									   distributionMethod, parentRelationName,
									   viaDeprecatedAPI);
			}
		}
	}
#endif
}


/*
 * PlanAlterTableStmt determines whether a given ALTER TABLE statement involves
 * a distributed table. If so (and if the statement does not use unsupported
 * options), it modifies the input statement to ensure proper execution against
 * the master node table and creates a DDLJob to encapsulate information needed
 * during the worker node portion of DDL execution before returning that DDLJob
 * in a List. If no distributed table is involved, this function returns NIL.
 */
static List *
PlanAlterTableStmt(AlterTableStmt *alterTableStatement, const char *alterTableCommand)
{
	List *ddlJobs = NIL;
	DDLJob *ddlJob = NULL;
	LOCKMODE lockmode = 0;
	Oid leftRelationId = InvalidOid;
	Oid rightRelationId = InvalidOid;
	char leftRelationKind;
	bool isDistributedRelation = false;
	List *commandList = NIL;
	ListCell *commandCell = NULL;
	bool executeSequentially = false;

	/* first check whether a distributed relation is affected */
	if (alterTableStatement->relation == NULL)
	{
		return NIL;
	}

	lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
	leftRelationId = AlterTableLookupRelation(alterTableStatement, lockmode);
	if (!OidIsValid(leftRelationId))
	{
		return NIL;
	}

	/*
	 * AlterTableStmt applies also to INDEX relations, and we have support for
	 * SET/SET storage parameters in Citus, so we might have to check for
	 * another relation here.
	 */
	leftRelationKind = get_rel_relkind(leftRelationId);
	if (leftRelationKind == RELKIND_INDEX)
	{
		leftRelationId = IndexGetRelation(leftRelationId, false);
	}

	isDistributedRelation = IsDistributedTable(leftRelationId);
	if (!isDistributedRelation)
	{
		return NIL;
	}

	/*
	 * The PostgreSQL parser dispatches several commands into the node type
	 * AlterTableStmt, from ALTER INDEX to ALTER SEQUENCE or ALTER VIEW. Here
	 * we have a special implementation for ALTER INDEX, and a specific error
	 * message in case of unsupported sub-command.
	 */
	if (leftRelationKind == RELKIND_INDEX)
	{
		ErrorIfUnsupportedAlterIndexStmt(alterTableStatement);
	}
	else
	{
		/* this function also accepts more than just RELKIND_RELATION... */
		ErrorIfUnsupportedAlterTableStmt(alterTableStatement);
	}

	/*
	 * We check if there is a ADD/DROP FOREIGN CONSTRAINT command in sub commands list.
	 * If there is we assign referenced relation id to rightRelationId and we also
	 * set skip_validation to true to prevent PostgreSQL to verify validity of the
	 * foreign constraint in master. Validity will be checked in workers anyway.
	 */
	commandList = alterTableStatement->cmds;

	foreach(commandCell, commandList)
	{
		AlterTableCmd *command = (AlterTableCmd *) lfirst(commandCell);
		AlterTableType alterTableType = command->subtype;

		if (alterTableType == AT_AddConstraint)
		{
			Constraint *constraint = (Constraint *) command->def;
			if (constraint->contype == CONSTR_FOREIGN)
			{
				/*
				 * We only support ALTER TABLE ADD CONSTRAINT ... FOREIGN KEY, if it is
				 * only subcommand of ALTER TABLE. It was already checked in
				 * ErrorIfUnsupportedAlterTableStmt.
				 */
				Assert(list_length(commandList) <= 1);

				rightRelationId = RangeVarGetRelid(constraint->pktable, lockmode,
												   alterTableStatement->missing_ok);

				/*
				 * Foreign constraint validations will be done in workers. If we do not
				 * set this flag, PostgreSQL tries to do additional checking when we drop
				 * to standard_ProcessUtility. standard_ProcessUtility tries to open new
				 * connections to workers to verify foreign constraints while original
				 * transaction is in process, which causes deadlock.
				 */
				constraint->skip_validation = true;
			}
		}
		else if (alterTableType == AT_AddColumn)
		{
			/*
			 * TODO: This code path is nothing beneficial since we do not
			 * support ALTER TABLE %s ADD COLUMN %s [constraint] for foreign keys.
			 * However, the code is kept in case we fix the constraint
			 * creation without a name and allow foreign key creation with the mentioned
			 * command.
			 */
			ColumnDef *columnDefinition = (ColumnDef *) command->def;
			List *columnConstraints = columnDefinition->constraints;

			ListCell *columnConstraint = NULL;
			foreach(columnConstraint, columnConstraints)
			{
				Constraint *constraint = (Constraint *) lfirst(columnConstraint);
				if (constraint->contype == CONSTR_FOREIGN)
				{
					rightRelationId = RangeVarGetRelid(constraint->pktable, lockmode,
													   alterTableStatement->missing_ok);

					/*
					 * Foreign constraint validations will be done in workers. If we do not
					 * set this flag, PostgreSQL tries to do additional checking when we drop
					 * to standard_ProcessUtility. standard_ProcessUtility tries to open new
					 * connections to workers to verify foreign constraints while original
					 * transaction is in process, which causes deadlock.
					 */
					constraint->skip_validation = true;
					break;
				}
			}
		}
#if (PG_VERSION_NUM >= 100000)
		else if (alterTableType == AT_AttachPartition)
		{
			PartitionCmd *partitionCommand = (PartitionCmd *) command->def;

			/*
			 * We only support ALTER TABLE ATTACH PARTITION, if it is only subcommand of
			 * ALTER TABLE. It was already checked in ErrorIfUnsupportedAlterTableStmt.
			 */
			Assert(list_length(commandList) <= 1);

			rightRelationId = RangeVarGetRelid(partitionCommand->name, NoLock, false);

			/*
			 * Do not generate tasks if relation is distributed and the partition
			 * is not distributed. Because, we'll manually convert the partition into
			 * distributed table and co-locate with its parent.
			 */
			if (!IsDistributedTable(rightRelationId))
			{
				return NIL;
			}
		}
		else if (alterTableType == AT_DetachPartition)
		{
			PartitionCmd *partitionCommand = (PartitionCmd *) command->def;

			/*
			 * We only support ALTER TABLE DETACH PARTITION, if it is only subcommand of
			 * ALTER TABLE. It was already checked in ErrorIfUnsupportedAlterTableStmt.
			 */
			Assert(list_length(commandList) <= 1);

			rightRelationId = RangeVarGetRelid(partitionCommand->name, NoLock, false);
		}
#endif
		executeSequentially |= SetupExecutionModeForAlterTable(leftRelationId,
															   command);
	}

	ddlJob = palloc0(sizeof(DDLJob));
	ddlJob->targetRelationId = leftRelationId;
	ddlJob->concurrentIndexCmd = false;
	ddlJob->commandString = alterTableCommand;
	ddlJob->executeSequentially = executeSequentially;

	if (rightRelationId)
	{
		if (!IsDistributedTable(rightRelationId))
		{
			ddlJob->taskList = NIL;
		}
		else
		{
			/* if foreign key related, use specialized task list function ... */
			ddlJob->taskList = InterShardDDLTaskList(leftRelationId, rightRelationId,
													 alterTableCommand);
		}
	}
	else
	{
		/* ... otherwise use standard DDL task list function */
		ddlJob->taskList = DDLTaskList(leftRelationId, alterTableCommand);
	}

	ddlJobs = list_make1(ddlJob);

	return ddlJobs;
}


/*
 * PlanRenameStmt first determines whether a given rename statement involves
 * a distributed table. If so (and if it is supported, i.e. renames a column),
 * it creates a DDLJob to encapsulate information needed during the worker node
 * portion of DDL execution before returning that DDLJob in a List. If no dis-
 * tributed table is involved, this function returns NIL.
 */
static List *
PlanRenameStmt(RenameStmt *renameStmt, const char *renameCommand)
{
	Oid objectRelationId = InvalidOid; /* SQL Object OID */
	Oid tableRelationId = InvalidOid; /* Relation OID, maybe not the same. */
	bool isDistributedRelation = false;
	DDLJob *ddlJob = NULL;

	/*
	 * We only support some of the PostgreSQL supported RENAME statements, and
	 * our list include only renaming table and index (related) objects.
	 */
	if (!IsAlterTableRenameStmt(renameStmt) &&
		!IsIndexRenameStmt(renameStmt) &&
		!IsPolicyRenameStmt(renameStmt))
	{
		return NIL;
	}

	/*
	 * The lock levels here should be same as the ones taken in
	 * RenameRelation(), renameatt() and RenameConstraint(). However, since all
	 * three statements have identical lock levels, we just use a single statement.
	 */
	objectRelationId = RangeVarGetRelid(renameStmt->relation,
										AccessExclusiveLock,
										renameStmt->missing_ok);

	/*
	 * If the table does not exist, don't do anything here to allow PostgreSQL
	 * to throw the appropriate error or notice message later.
	 */
	if (!OidIsValid(objectRelationId))
	{
		return NIL;
	}

	/* we have no planning to do unless the table is distributed */
	switch (renameStmt->renameType)
	{
		case OBJECT_TABLE:
		case OBJECT_COLUMN:
		case OBJECT_TABCONSTRAINT:
		case OBJECT_POLICY:
		{
			/* the target object is our tableRelationId. */
			tableRelationId = objectRelationId;
			break;
		}

		case OBJECT_INDEX:
		{
			/*
			 * here, objRelationId points to the index relation entry, and we
			 * are interested into the entry of the table on which the index is
			 * defined.
			 */
			tableRelationId = IndexGetRelation(objectRelationId, false);
			break;
		}

		default:

			/*
			 * Nodes that are not supported by Citus: we pass-through to the
			 * main PostgreSQL executor. Any Citus-supported RenameStmt
			 * renameType must appear above in the switch, explicitly.
			 */
			return NIL;
	}

	isDistributedRelation = IsDistributedTable(tableRelationId);
	if (!isDistributedRelation)
	{
		return NIL;
	}

	/*
	 * We might ERROR out on some commands, but only for Citus tables where
	 * isDistributedRelation is true. That's why this test comes this late in
	 * the function.
	 */
	ErrorIfUnsupportedRenameStmt(renameStmt);

	ddlJob = palloc0(sizeof(DDLJob));
	ddlJob->targetRelationId = tableRelationId;
	ddlJob->concurrentIndexCmd = false;
	ddlJob->commandString = renameCommand;
	ddlJob->taskList = DDLTaskList(tableRelationId, renameCommand);

	return list_make1(ddlJob);
}


/*
 * WorkerProcessAlterTableStmt checks and processes the alter table statement to be
 * worked on the distributed table of the worker node. Currently, it only processes
 * ALTER TABLE ... ADD FOREIGN KEY command to skip the validation step.
 */
static Node *
WorkerProcessAlterTableStmt(AlterTableStmt *alterTableStatement,
							const char *alterTableCommand)
{
	LOCKMODE lockmode = 0;
	Oid leftRelationId = InvalidOid;
	bool isDistributedRelation = false;
	List *commandList = NIL;
	ListCell *commandCell = NULL;

	/* first check whether a distributed relation is affected */
	if (alterTableStatement->relation == NULL)
	{
		return (Node *) alterTableStatement;
	}

	lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
	leftRelationId = AlterTableLookupRelation(alterTableStatement, lockmode);
	if (!OidIsValid(leftRelationId))
	{
		return (Node *) alterTableStatement;
	}

	isDistributedRelation = IsDistributedTable(leftRelationId);
	if (!isDistributedRelation)
	{
		return (Node *) alterTableStatement;
	}

	/*
	 * We check if there is a ADD FOREIGN CONSTRAINT command in sub commands list.
	 * If there is we assign referenced releation id to rightRelationId and we also
	 * set skip_validation to true to prevent PostgreSQL to verify validity of the
	 * foreign constraint in master. Validity will be checked in workers anyway.
	 */
	commandList = alterTableStatement->cmds;

	foreach(commandCell, commandList)
	{
		AlterTableCmd *command = (AlterTableCmd *) lfirst(commandCell);
		AlterTableType alterTableType = command->subtype;

		if (alterTableType == AT_AddConstraint)
		{
			Constraint *constraint = (Constraint *) command->def;
			if (constraint->contype == CONSTR_FOREIGN)
			{
				/* foreign constraint validations will be done in shards. */
				constraint->skip_validation = true;
			}
		}
	}

	return (Node *) alterTableStatement;
}


/*
 * PlanAlterObjectSchemaStmt determines whether a given ALTER ... SET SCHEMA
 * statement involves a distributed table and issues a warning if so. Because
 * we do not support distributed ALTER ... SET SCHEMA, this function always
 * returns NIL.
 */
static List *
PlanAlterObjectSchemaStmt(AlterObjectSchemaStmt *alterObjectSchemaStmt,
						  const char *alterObjectSchemaCommand)
{
	Oid relationId = InvalidOid;

	if (alterObjectSchemaStmt->relation == NULL)
	{
		return NIL;
	}

	relationId = RangeVarGetRelid(alterObjectSchemaStmt->relation,
								  AccessExclusiveLock,
								  alterObjectSchemaStmt->missing_ok);

	/* first check whether a distributed relation is affected */
	if (!OidIsValid(relationId) || !IsDistributedTable(relationId))
	{
		return NIL;
	}

	/* emit a warning if a distributed relation is affected */
	ereport(WARNING, (errmsg("not propagating ALTER ... SET SCHEMA commands to "
							 "worker nodes"),
					  errhint("Connect to worker nodes directly to manually "
							  "change schemas of affected objects.")));

	return NIL;
}


/*
 * ProcessVacuumStmt processes vacuum statements that may need propagation to
 * distributed tables. If a VACUUM or ANALYZE command references a distributed
 * table, it is propagated to all involved nodes; otherwise, this function will
 * immediately exit after some error checking.
 *
 * Unlike most other Process functions within this file, this function does not
 * return a modified parse node, as it is expected that the local VACUUM or
 * ANALYZE has already been processed.
 */
static void
ProcessVacuumStmt(VacuumStmt *vacuumStmt, const char *vacuumCommand)
{
	int relationIndex = 0;
	bool distributedVacuumStmt = false;
	List *vacuumRelationList = ExtractVacuumTargetRels(vacuumStmt);
	ListCell *vacuumRelationCell = NULL;
	List *relationIdList = NIL;
	ListCell *relationIdCell = NULL;
	LOCKMODE lockMode = (vacuumStmt->options & VACOPT_FULL) ? AccessExclusiveLock :
						ShareUpdateExclusiveLock;
	int executedVacuumCount = 0;

	foreach(vacuumRelationCell, vacuumRelationList)
	{
		RangeVar *vacuumRelation = (RangeVar *) lfirst(vacuumRelationCell);
		Oid relationId = RangeVarGetRelid(vacuumRelation, lockMode, false);
		relationIdList = lappend_oid(relationIdList, relationId);
	}

	distributedVacuumStmt = IsDistributedVacuumStmt(vacuumStmt, relationIdList);
	if (!distributedVacuumStmt)
	{
		return;
	}

	/* execute vacuum on distributed tables */
	foreach(relationIdCell, relationIdList)
	{
		Oid relationId = lfirst_oid(relationIdCell);
		if (IsDistributedTable(relationId))
		{
			List *vacuumColumnList = NIL;
			List *taskList = NIL;

			/*
			 * VACUUM commands cannot run inside a transaction block, so we use
			 * the "bare" commit protocol without BEGIN/COMMIT. However, ANALYZE
			 * commands can run inside a transaction block. Notice that we do this
			 * once even if there are multiple distributed tables to be vacuumed.
			 */
			if (executedVacuumCount == 0 && (vacuumStmt->options & VACOPT_VACUUM) != 0)
			{
				/* save old commit protocol to restore at xact end */
				Assert(SavedMultiShardCommitProtocol == COMMIT_PROTOCOL_BARE);
				SavedMultiShardCommitProtocol = MultiShardCommitProtocol;
				MultiShardCommitProtocol = COMMIT_PROTOCOL_BARE;
			}

			vacuumColumnList = VacuumColumnList(vacuumStmt, relationIndex);
			taskList = VacuumTaskList(relationId, vacuumStmt->options, vacuumColumnList);

			ExecuteModifyTasksWithoutResults(taskList);

			executedVacuumCount++;
		}
		relationIndex++;
	}
}


/*
 * IsSupportedDistributedVacuumStmt returns whether distributed execution of a
 * given VacuumStmt is supported. The provided relationId list represents
 * the list of tables targeted by the provided statement.
 *
 * Returns true if the statement requires distributed execution and returns
 * false otherwise.
 */
static bool
IsDistributedVacuumStmt(VacuumStmt *vacuumStmt, List *vacuumRelationIdList)
{
	const char *stmtName = (vacuumStmt->options & VACOPT_VACUUM) ? "VACUUM" : "ANALYZE";
	bool distributeStmt = false;
	ListCell *relationIdCell = NULL;
	int distributedRelationCount = 0;
	int vacuumedRelationCount = 0;

	/*
	 * No table in the vacuum statement means vacuuming all relations
	 * which is not supported by citus.
	 */
	vacuumedRelationCount = list_length(vacuumRelationIdList);
	if (vacuumedRelationCount == 0)
	{
		/* WARN for unqualified VACUUM commands */
		ereport(WARNING, (errmsg("not propagating %s command to worker nodes", stmtName),
						  errhint("Provide a specific table in order to %s "
								  "distributed tables.", stmtName)));
	}

	foreach(relationIdCell, vacuumRelationIdList)
	{
		Oid relationId = lfirst_oid(relationIdCell);
		if (OidIsValid(relationId) && IsDistributedTable(relationId))
		{
			distributedRelationCount++;
		}
	}

	if (distributedRelationCount == 0)
	{
		/* nothing to do here */
	}
	else if (!EnableDDLPropagation)
	{
		/* WARN if DDL propagation is not enabled */
		ereport(WARNING, (errmsg("not propagating %s command to worker nodes", stmtName),
						  errhint("Set citus.enable_ddl_propagation to true in order to "
								  "send targeted %s commands to worker nodes.",
								  stmtName)));
	}
	else
	{
		distributeStmt = true;
	}

	return distributeStmt;
}


/*
 * VacuumTaskList returns a list of tasks to be executed as part of processing
 * a VacuumStmt which targets a distributed relation.
 */
static List *
VacuumTaskList(Oid relationId, int vacuumOptions, List *vacuumColumnList)
{
	List *taskList = NIL;
	List *shardIntervalList = NIL;
	ListCell *shardIntervalCell = NULL;
	uint64 jobId = INVALID_JOB_ID;
	int taskId = 1;
	StringInfo vacuumString = DeparseVacuumStmtPrefix(vacuumOptions);
	const char *columnNames = NULL;
	const int vacuumPrefixLen = vacuumString->len;
	Oid schemaId = get_rel_namespace(relationId);
	char *schemaName = get_namespace_name(schemaId);
	char *tableName = get_rel_name(relationId);

	columnNames = DeparseVacuumColumnNames(vacuumColumnList);

	/*
	 * We obtain ShareUpdateExclusiveLock here to not conflict with INSERT's
	 * RowExclusiveLock. However if VACUUM FULL is used, we already obtain
	 * AccessExclusiveLock before reaching to that point and INSERT's will be
	 * blocked anyway. This is inline with PostgreSQL's own behaviour.
	 */
	LockRelationOid(relationId, ShareUpdateExclusiveLock);

	shardIntervalList = LoadShardIntervalList(relationId);

	/* grab shard lock before getting placement list */
	LockShardListMetadata(shardIntervalList, ShareLock);

	foreach(shardIntervalCell, shardIntervalList)
	{
		ShardInterval *shardInterval = (ShardInterval *) lfirst(shardIntervalCell);
		uint64 shardId = shardInterval->shardId;
		Task *task = NULL;

		char *shardName = pstrdup(tableName);
		AppendShardIdToName(&shardName, shardInterval->shardId);
		shardName = quote_qualified_identifier(schemaName, shardName);

		vacuumString->len = vacuumPrefixLen;
		appendStringInfoString(vacuumString, shardName);
		appendStringInfoString(vacuumString, columnNames);

		task = CitusMakeNode(Task);
		task->jobId = jobId;
		task->taskId = taskId++;
		task->taskType = VACUUM_ANALYZE_TASK;
		task->queryString = pstrdup(vacuumString->data);
		task->dependedTaskList = NULL;
		task->replicationModel = REPLICATION_MODEL_INVALID;
		task->anchorShardId = shardId;
		task->taskPlacementList = FinalizedShardPlacementList(shardId);

		taskList = lappend(taskList, task);
	}

	return taskList;
}


/*
 * DeparseVacuumStmtPrefix returns a StringInfo appropriate for use as a prefix
 * during distributed execution of a VACUUM or ANALYZE statement. Callers may
 * reuse this prefix within a loop to generate shard-specific VACUUM or ANALYZE
 * statements.
 */
static StringInfo
DeparseVacuumStmtPrefix(int vacuumFlags)
{
	StringInfo vacuumPrefix = makeStringInfo();
	const int unsupportedFlags PG_USED_FOR_ASSERTS_ONLY = ~(
		VACOPT_ANALYZE |
		VACOPT_DISABLE_PAGE_SKIPPING |
		VACOPT_FREEZE |
		VACOPT_FULL |
		VACOPT_VERBOSE
		);

	/* determine actual command and block out its bit */
	if (vacuumFlags & VACOPT_VACUUM)
	{
		appendStringInfoString(vacuumPrefix, "VACUUM ");
		vacuumFlags &= ~VACOPT_VACUUM;
	}
	else
	{
		appendStringInfoString(vacuumPrefix, "ANALYZE ");
		vacuumFlags &= ~VACOPT_ANALYZE;

		if (vacuumFlags & VACOPT_VERBOSE)
		{
			appendStringInfoString(vacuumPrefix, "VERBOSE ");
			vacuumFlags &= ~VACOPT_VERBOSE;
		}
	}

	/* unsupported flags should have already been rejected */
	Assert((vacuumFlags & unsupportedFlags) == 0);

	/* if no flags remain, exit early */
	if (vacuumFlags == 0)
	{
		return vacuumPrefix;
	}

	/* otherwise, handle options */
	appendStringInfoChar(vacuumPrefix, '(');

	if (vacuumFlags & VACOPT_ANALYZE)
	{
		appendStringInfoString(vacuumPrefix, "ANALYZE,");
	}

	if (vacuumFlags & VACOPT_DISABLE_PAGE_SKIPPING)
	{
		appendStringInfoString(vacuumPrefix, "DISABLE_PAGE_SKIPPING,");
	}

	if (vacuumFlags & VACOPT_FREEZE)
	{
		appendStringInfoString(vacuumPrefix, "FREEZE,");
	}

	if (vacuumFlags & VACOPT_FULL)
	{
		appendStringInfoString(vacuumPrefix, "FULL,");
	}

	if (vacuumFlags & VACOPT_VERBOSE)
	{
		appendStringInfoString(vacuumPrefix, "VERBOSE,");
	}

	vacuumPrefix->data[vacuumPrefix->len - 1] = ')';

	appendStringInfoChar(vacuumPrefix, ' ');

	return vacuumPrefix;
}


/*
 * DeparseVacuumColumnNames joins the list of strings using commas as a
 * delimiter. The whole thing is placed in parenthesis and set off with a
 * single space in order to facilitate appending it to the end of any VACUUM
 * or ANALYZE command which uses explicit column names. If the provided list
 * is empty, this function returns an empty string to keep the calling code
 * simplest.
 */
static char *
DeparseVacuumColumnNames(List *columnNameList)
{
	StringInfo columnNames = makeStringInfo();
	ListCell *columnNameCell = NULL;

	if (columnNameList == NIL)
	{
		return columnNames->data;
	}

	appendStringInfoString(columnNames, " (");

	foreach(columnNameCell, columnNameList)
	{
		char *columnName = strVal(lfirst(columnNameCell));

		appendStringInfo(columnNames, "%s,", columnName);
	}

	columnNames->data[columnNames->len - 1] = ')';

	return columnNames->data;
}


/*
 * ErrorIfUnstableCreateOrAlterExtensionStmt compares CITUS_EXTENSIONVERSION
 * and version given CREATE/ALTER EXTENSION statement will create/update to. If
 * they are not same in major or minor version numbers, this function errors
 * out. It ignores the schema version.
 */
static void
ErrorIfUnstableCreateOrAlterExtensionStmt(Node *parsetree)
{
	char *newExtensionVersion = ExtractNewExtensionVersion(parsetree);

	if (newExtensionVersion != NULL)
	{
		/*  explicit version provided in CREATE or ALTER EXTENSION UPDATE; verify */
		if (!MajorVersionsCompatible(newExtensionVersion, CITUS_EXTENSIONVERSION))
		{
			ereport(ERROR, (errmsg("specified version incompatible with loaded "
								   "Citus library"),
							errdetail("Loaded library requires %s, but %s was specified.",
									  CITUS_MAJORVERSION, newExtensionVersion),
							errhint("If a newer library is present, restart the database "
									"and try the command again.")));
		}
	}
	else
	{
		/*
		 * No version was specified, so PostgreSQL will use the default_version
		 * from the citus.control file.
		 */
		CheckAvailableVersion(ERROR);
	}
}


/*
 * ExtractNewExtensionVersion returns the new extension version specified by
 * a CREATE or ALTER EXTENSION statement. Other inputs are not permitted. This
 * function returns NULL for statements with no explicit version specified.
 */
static char *
ExtractNewExtensionVersion(Node *parsetree)
{
	char *newVersion = NULL;
	List *optionsList = NIL;
	ListCell *optionsCell = NULL;

	if (IsA(parsetree, CreateExtensionStmt))
	{
		optionsList = ((CreateExtensionStmt *) parsetree)->options;
	}
	else if (IsA(parsetree, AlterExtensionStmt))
	{
		optionsList = ((AlterExtensionStmt *) parsetree)->options;
	}
	else
	{
		/* input must be one of the two above types */
		Assert(false);
	}

	foreach(optionsCell, optionsList)
	{
		DefElem *defElement = (DefElem *) lfirst(optionsCell);
		if (strncmp(defElement->defname, "new_version", NAMEDATALEN) == 0)
		{
			newVersion = strVal(defElement->arg);
			break;
		}
	}

	return newVersion;
}


/*
 * ErrorIfUnsupportedAlterTableStmt checks if the corresponding alter table
 * statement is supported for distributed tables and errors out if it is not.
 * Currently, only the following commands are supported.
 *
 * ALTER TABLE ADD|DROP COLUMN
 * ALTER TABLE ALTER COLUMN SET DATA TYPE
 * ALTER TABLE SET|DROP NOT NULL
 * ALTER TABLE SET|DROP DEFAULT
 * ALTER TABLE ADD|DROP CONSTRAINT
 * ALTER TABLE REPLICA IDENTITY
 * ALTER TABLE SET ()
 * ALTER TABLE RESET ()
 */
static void
ErrorIfUnsupportedAlterTableStmt(AlterTableStmt *alterTableStatement)
{
	List *commandList = alterTableStatement->cmds;
	ListCell *commandCell = NULL;

	/* error out if any of the subcommands are unsupported */
	foreach(commandCell, commandList)
	{
		AlterTableCmd *command = (AlterTableCmd *) lfirst(commandCell);
		AlterTableType alterTableType = command->subtype;

		switch (alterTableType)
		{
			case AT_AddColumn:
			{
				if (IsA(command->def, ColumnDef))
				{
					ColumnDef *column = (ColumnDef *) command->def;

					/*
					 * Check for SERIAL pseudo-types. The structure of this
					 * check is copied from transformColumnDefinition.
					 */
					if (column->typeName && list_length(column->typeName->names) == 1 &&
						!column->typeName->pct_type)
					{
						char *typeName = strVal(linitial(column->typeName->names));

						if (strcmp(typeName, "smallserial") == 0 ||
							strcmp(typeName, "serial2") == 0 ||
							strcmp(typeName, "serial") == 0 ||
							strcmp(typeName, "serial4") == 0 ||
							strcmp(typeName, "bigserial") == 0 ||
							strcmp(typeName, "serial8") == 0)
						{
							ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
											errmsg("cannot execute ADD COLUMN commands "
												   "involving serial pseudotypes")));
						}
					}
				}

				break;
			}

			case AT_DropColumn:
			case AT_ColumnDefault:
			case AT_AlterColumnType:
			case AT_DropNotNull:
			{
				if (AlterInvolvesPartitionColumn(alterTableStatement, command))
				{
					ereport(ERROR, (errmsg("cannot execute ALTER TABLE command "
										   "involving partition column")));
				}
				break;
			}

			case AT_AddConstraint:
			{
				Constraint *constraint = (Constraint *) command->def;

				/* we only allow constraints if they are only subcommand */
				if (commandList->length > 1)
				{
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("cannot execute ADD CONSTRAINT command with "
										   "other subcommands"),
									errhint("You can issue each subcommand separately")));
				}

				/*
				 * We will use constraint name in each placement by extending it at
				 * workers. Therefore we require it to be exist.
				 */
				if (constraint->conname == NULL)
				{
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("cannot create constraint without a name on a "
										   "distributed table")));
				}

				break;
			}

#if (PG_VERSION_NUM >= 100000)
			case AT_AttachPartition:
			{
				Oid relationId = AlterTableLookupRelation(alterTableStatement,
														  NoLock);
				PartitionCmd *partitionCommand = (PartitionCmd *) command->def;
				bool missingOK = false;
				Oid partitionRelationId = RangeVarGetRelid(partitionCommand->name,
														   NoLock, missingOK);

				/* we only allow partitioning commands if they are only subcommand */
				if (commandList->length > 1)
				{
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("cannot execute ATTACH PARTITION command "
										   "with other subcommands"),
									errhint("You can issue each subcommand "
											"separately.")));
				}

				if (IsDistributedTable(partitionRelationId) &&
					!TablesColocated(relationId, partitionRelationId))
				{
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("distributed tables cannot have "
										   "non-colocated distributed tables as a "
										   "partition ")));
				}

				break;
			}

			case AT_DetachPartition:
			{
				/* we only allow partitioning commands if they are only subcommand */
				if (commandList->length > 1)
				{
					ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
									errmsg("cannot execute DETACH PARTITION command "
										   "with other subcommands"),
									errhint("You can issue each subcommand "
											"separately.")));
				}

				break;
			}

#endif
			case AT_DropConstraint:
			{
				LOCKMODE lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
				Oid relationId = AlterTableLookupRelation(alterTableStatement, lockmode);

				if (!OidIsValid(relationId))
				{
					return;
				}

				if (ConstraintIsAForeignKey(command->name, relationId))
				{
					MarkInvalidateForeignKeyGraph();
				}

				break;
			}

			case AT_SetNotNull:
			case AT_EnableTrigAll:
			case AT_DisableTrigAll:
			case AT_ReplicaIdentity:
			{
				/*
				 * We will not perform any special check for ALTER TABLE DROP CONSTRAINT
				 * , ALTER TABLE .. ALTER COLUMN .. SET NOT NULL and ALTER TABLE ENABLE/
				 * DISABLE TRIGGER ALL, ALTER TABLE .. REPLICA IDENTITY ..
				 */
				break;
			}

			case AT_SetRelOptions:  /* SET (...) */
			case AT_ResetRelOptions:    /* RESET (...) */
			case AT_ReplaceRelOptions:  /* replace entire option list */
			{
				/* this command is supported by Citus */
				break;
			}

			default:
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("alter table command is currently unsupported"),
						 errdetail("Only ADD|DROP COLUMN, SET|DROP NOT NULL, "
								   "SET|DROP DEFAULT, ADD|DROP CONSTRAINT, "
								   "SET (), RESET (), "
								   "ATTACH|DETACH PARTITION and TYPE subcommands "
								   "are supported.")));
			}
		}
	}
}


/*
 * ErrorIfUnsupportedAlterIndexStmt checks if the corresponding alter index
 * statement is supported for distributed tables and errors out if it is not.
 * Currently, only the following commands are supported.
 *
 * ALTER INDEX SET ()
 * ALTER INDEX RESET ()
 */
static void
ErrorIfUnsupportedAlterIndexStmt(AlterTableStmt *alterTableStatement)
{
	List *commandList = alterTableStatement->cmds;
	ListCell *commandCell = NULL;

	/* error out if any of the subcommands are unsupported */
	foreach(commandCell, commandList)
	{
		AlterTableCmd *command = (AlterTableCmd *) lfirst(commandCell);
		AlterTableType alterTableType = command->subtype;

		switch (alterTableType)
		{
			case AT_SetRelOptions:  /* SET (...) */
			case AT_ResetRelOptions:    /* RESET (...) */
			case AT_ReplaceRelOptions:  /* replace entire option list */
			{
				/* this command is supported by Citus */
				break;
			}

			/* unsupported create index statements */
			case AT_SetTableSpace:
			default:
			{
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("alter index ... set tablespace ... "
								"is currently unsupported"),
						 errdetail("Only RENAME TO, SET (), and RESET () "
								   "are supported.")));
				return; /* keep compiler happy */
			}
		}
	}
}


/*
 * ErrorIfDropPartitionColumn checks if any subcommands of the given alter table
 * command is a DROP COLUMN command which drops the partition column of a distributed
 * table. If there is such a subcommand, this function errors out.
 */
static void
ErrorIfAlterDropsPartitionColumn(AlterTableStmt *alterTableStatement)
{
	LOCKMODE lockmode = 0;
	Oid leftRelationId = InvalidOid;
	bool isDistributedRelation = false;
	List *commandList = alterTableStatement->cmds;
	ListCell *commandCell = NULL;

	/* first check whether a distributed relation is affected */
	if (alterTableStatement->relation == NULL)
	{
		return;
	}

	lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
	leftRelationId = AlterTableLookupRelation(alterTableStatement, lockmode);
	if (!OidIsValid(leftRelationId))
	{
		return;
	}

	isDistributedRelation = IsDistributedTable(leftRelationId);
	if (!isDistributedRelation)
	{
		return;
	}

	/* then check if any of subcommands drop partition column.*/
	foreach(commandCell, commandList)
	{
		AlterTableCmd *command = (AlterTableCmd *) lfirst(commandCell);
		AlterTableType alterTableType = command->subtype;
		if (alterTableType == AT_DropColumn)
		{
			if (AlterInvolvesPartitionColumn(alterTableStatement, command))
			{
				ereport(ERROR, (errmsg("cannot execute ALTER TABLE command "
									   "dropping partition column")));
			}
		}
	}
}


/*
 * ErrorIfUnsopprtedAlterAddConstraintStmt runs the constraint checks on distributed
 * table using the same logic with create_distributed_table.
 */
static void
ErrorIfUnsupportedAlterAddConstraintStmt(AlterTableStmt *alterTableStatement)
{
	LOCKMODE lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
	Oid relationId = AlterTableLookupRelation(alterTableStatement, lockmode);
	char distributionMethod = PartitionMethod(relationId);
	Var *distributionColumn = DistPartitionKey(relationId);
	uint32 colocationId = TableColocationId(relationId);
	Relation relation = relation_open(relationId, ExclusiveLock);

	ErrorIfUnsupportedConstraint(relation, distributionMethod, distributionColumn,
								 colocationId);
	relation_close(relation, NoLock);
}


/*
 * ErrorIfUnsupportedConstraint run checks related to unique index / exclude
 * constraints.
 *
 * The function skips the uniqeness checks for reference tables (i.e., distribution
 * method is 'none').
 *
 * Forbid UNIQUE, PRIMARY KEY, or EXCLUDE constraints on append partitioned
 * tables, since currently there is no way of enforcing uniqueness for
 * overlapping shards.
 *
 * Similarly, do not allow such constraints if they do not include partition
 * column. This check is important for two reasons:
 * i. First, currently Citus does not enforce uniqueness constraint on multiple
 * shards.
 * ii. Second, INSERT INTO .. ON CONFLICT (i.e., UPSERT) queries can be executed
 * with no further check for constraints.
 */
void
ErrorIfUnsupportedConstraint(Relation relation, char distributionMethod,
							 Var *distributionColumn, uint32 colocationId)
{
	char *relationName = NULL;
	List *indexOidList = NULL;
	ListCell *indexOidCell = NULL;

	/*
	 * We first perform check for foreign constraints. It is important to do this check
	 * before next check, because other types of constraints are allowed on reference
	 * tables and we return early for those constraints thanks to next check. Therefore,
	 * for reference tables, we first check for foreing constraints and if they are OK,
	 * we do not error out for other types of constraints.
	 */
	ErrorIfUnsupportedForeignConstraint(relation, distributionMethod, distributionColumn,
										colocationId);

	/*
	 * Citus supports any kind of uniqueness constraints for reference tables
	 * given that they only consist of a single shard and we can simply rely on
	 * Postgres.
	 */
	if (distributionMethod == DISTRIBUTE_BY_NONE)
	{
		return;
	}

	relationName = RelationGetRelationName(relation);
	indexOidList = RelationGetIndexList(relation);

	foreach(indexOidCell, indexOidList)
	{
		Oid indexOid = lfirst_oid(indexOidCell);
		Relation indexDesc = index_open(indexOid, RowExclusiveLock);
		IndexInfo *indexInfo = NULL;
		AttrNumber *attributeNumberArray = NULL;
		bool hasDistributionColumn = false;
		int attributeCount = 0;
		int attributeIndex = 0;

		/* extract index key information from the index's pg_index info */
		indexInfo = BuildIndexInfo(indexDesc);

		/* only check unique indexes and exclusion constraints. */
		if (indexInfo->ii_Unique == false && indexInfo->ii_ExclusionOps == NULL)
		{
			index_close(indexDesc, NoLock);
			continue;
		}

		/*
		 * Citus cannot enforce uniqueness/exclusion constraints with overlapping shards.
		 * Thus, emit a warning for unique indexes and exclusion constraints on
		 * append partitioned tables.
		 */
		if (distributionMethod == DISTRIBUTE_BY_APPEND)
		{
			ereport(WARNING, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							  errmsg("table \"%s\" has a UNIQUE or EXCLUDE constraint",
									 relationName),
							  errdetail("UNIQUE constraints, EXCLUDE constraints, "
										"and PRIMARY KEYs on "
										"append-partitioned tables cannot be enforced."),
							  errhint("Consider using hash partitioning.")));
		}

		attributeCount = indexInfo->ii_NumIndexAttrs;
		attributeNumberArray = IndexInfoAttributeNumberArray(indexInfo);

		for (attributeIndex = 0; attributeIndex < attributeCount; attributeIndex++)
		{
			AttrNumber attributeNumber = attributeNumberArray[attributeIndex];
			bool uniqueConstraint = false;
			bool exclusionConstraintWithEquality = false;

			if (distributionColumn->varattno != attributeNumber)
			{
				continue;
			}

			uniqueConstraint = indexInfo->ii_Unique;
			exclusionConstraintWithEquality = (indexInfo->ii_ExclusionOps != NULL &&
											   OperatorImplementsEquality(
												   indexInfo->ii_ExclusionOps[
													   attributeIndex]));

			if (uniqueConstraint || exclusionConstraintWithEquality)
			{
				hasDistributionColumn = true;
				break;
			}
		}

		if (!hasDistributionColumn)
		{
			ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
							errmsg("cannot create constraint on \"%s\"",
								   relationName),
							errdetail("Distributed relations cannot have UNIQUE, "
									  "EXCLUDE, or PRIMARY KEY constraints that do not "
									  "include the partition column (with an equality "
									  "operator if EXCLUDE).")));
		}

		index_close(indexDesc, NoLock);
	}
}


/*
 * ErrorIfDistributedRenameStmt errors out if the corresponding rename statement
 * operates on any part of a distributed table other than a column.
 *
 * Note: This function handles RenameStmt applied to relations handed by Citus.
 * At the moment of writing this comment, it could be either tables or indexes.
 */
static void
ErrorIfUnsupportedRenameStmt(RenameStmt *renameStmt)
{
	if (IsAlterTableRenameStmt(renameStmt) &&
		renameStmt->renameType == OBJECT_TABCONSTRAINT)
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("renaming constraints belonging to distributed tables is "
							   "currently unsupported")));
	}
}


/*
 * IsAlterTableRenameStmt returns whether the passed-in RenameStmt is one of
 * the following forms:
 *
 *   - ALTER TABLE RENAME
 *   - ALTER TABLE RENAME COLUMN
 *   - ALTER TABLE RENAME CONSTRAINT
 */
static bool
IsAlterTableRenameStmt(RenameStmt *renameStmt)
{
	bool isAlterTableRenameStmt = false;

	if (renameStmt->renameType == OBJECT_TABLE)
	{
		isAlterTableRenameStmt = true;
	}
	else if (renameStmt->renameType == OBJECT_COLUMN &&
			 renameStmt->relationType == OBJECT_TABLE)
	{
		isAlterTableRenameStmt = true;
	}
	else if (renameStmt->renameType == OBJECT_TABCONSTRAINT)
	{
		isAlterTableRenameStmt = true;
	}

	return isAlterTableRenameStmt;
}


/*
 * IsIndexRenameStmt returns whether the passed-in RenameStmt is the following
 * form:
 *
 *   - ALTER INDEX RENAME
 */
static bool
IsIndexRenameStmt(RenameStmt *renameStmt)
{
	bool isIndexRenameStmt = false;

	if (renameStmt->renameType == OBJECT_INDEX)
	{
		isIndexRenameStmt = true;
	}

	return isIndexRenameStmt;
}


/*
 * AlterInvolvesPartitionColumn checks if the given alter table command
 * involves relation's partition column.
 */
static bool
AlterInvolvesPartitionColumn(AlterTableStmt *alterTableStatement,
							 AlterTableCmd *command)
{
	bool involvesPartitionColumn = false;
	Var *partitionColumn = NULL;
	HeapTuple tuple = NULL;
	char *alterColumnName = command->name;

	LOCKMODE lockmode = AlterTableGetLockLevel(alterTableStatement->cmds);
	Oid relationId = AlterTableLookupRelation(alterTableStatement, lockmode);
	if (!OidIsValid(relationId))
	{
		return false;
	}

	partitionColumn = DistPartitionKey(relationId);

	tuple = SearchSysCacheAttName(relationId, alterColumnName);
	if (HeapTupleIsValid(tuple))
	{
		Form_pg_attribute targetAttr = (Form_pg_attribute) GETSTRUCT(tuple);

		/* reference tables do not have partition column, so allow them */
		if (partitionColumn != NULL &&
			targetAttr->attnum == partitionColumn->varattno)
		{
			involvesPartitionColumn = true;
		}

		ReleaseSysCache(tuple);
	}

	return involvesPartitionColumn;
}


/*
 * ExecuteDistributedDDLJob simply executes a provided DDLJob in a distributed trans-
 * action, including metadata sync if needed. If the multi shard commit protocol is
 * in its default value of '1pc', then a notice message indicating that '2pc' might be
 * used for extra safety. In the commit protocol, a BEGIN is sent after connection to
 * each shard placement and COMMIT/ROLLBACK is handled by
 * CoordinatedTransactionCallback function.
 *
 * The function errors out if the node is not the coordinator or if the DDL is on
 * a partitioned table which has replication factor > 1.
 *
 */
static void
ExecuteDistributedDDLJob(DDLJob *ddlJob)
{
	bool shouldSyncMetadata = ShouldSyncTableMetadata(ddlJob->targetRelationId);

	EnsureCoordinator();
	EnsurePartitionTableNotReplicated(ddlJob->targetRelationId);

	if (!ddlJob->concurrentIndexCmd)
	{
		if (shouldSyncMetadata)
		{
			char *setSearchPathCommand = SetSearchPathToCurrentSearchPathCommand();

			SendCommandToWorkers(WORKERS_WITH_METADATA, DISABLE_DDL_PROPAGATION);

			/*
			 * Given that we're relaying the query to the worker nodes directly,
			 * we should set the search path exactly the same when necessary.
			 */
			if (setSearchPathCommand != NULL)
			{
				SendCommandToWorkers(WORKERS_WITH_METADATA, setSearchPathCommand);
			}

			SendCommandToWorkers(WORKERS_WITH_METADATA, (char *) ddlJob->commandString);
		}

		if (MultiShardConnectionType == SEQUENTIAL_CONNECTION ||
			ddlJob->executeSequentially)
		{
			ExecuteModifyTasksSequentiallyWithoutResults(ddlJob->taskList, CMD_UTILITY);
		}
		else
		{
			ExecuteModifyTasksWithoutResults(ddlJob->taskList);
		}
	}
	else
	{
		/* save old commit protocol to restore at xact end */
		Assert(SavedMultiShardCommitProtocol == COMMIT_PROTOCOL_BARE);
		SavedMultiShardCommitProtocol = MultiShardCommitProtocol;
		MultiShardCommitProtocol = COMMIT_PROTOCOL_BARE;

		PG_TRY();
		{
			ExecuteModifyTasksSequentiallyWithoutResults(ddlJob->taskList, CMD_UTILITY);

			if (shouldSyncMetadata)
			{
				List *commandList = list_make1(DISABLE_DDL_PROPAGATION);
				char *setSearchPathCommand = SetSearchPathToCurrentSearchPathCommand();

				/*
				 * Given that we're relaying the query to the worker nodes directly,
				 * we should set the search path exactly the same when necessary.
				 */
				if (setSearchPathCommand != NULL)
				{
					commandList = lappend(commandList, setSearchPathCommand);
				}

				commandList = lappend(commandList, (char *) ddlJob->commandString);

				SendBareCommandListToWorkers(WORKERS_WITH_METADATA, commandList);
			}
		}
		PG_CATCH();
		{
			ereport(ERROR,
					(errmsg("CONCURRENTLY-enabled index command failed"),
					 errdetail("CONCURRENTLY-enabled index commands can fail partially, "
							   "leaving behind an INVALID index."),
					 errhint("Use DROP INDEX CONCURRENTLY IF EXISTS to remove the "
							 "invalid index, then retry the original command.")));
		}
		PG_END_TRY();
	}
}


/*
 * SetSearchPathToCurrentSearchPathCommand generates a command which can
 * set the search path to the exact same search path that the issueing node
 * has.
 *
 * If the current search path is null (or doesn't have any valid schemas),
 * the function returns NULL.
 */
static char *
SetSearchPathToCurrentSearchPathCommand(void)
{
	StringInfo setCommand = NULL;
	char *currentSearchPath = CurrentSearchPath();

	if (currentSearchPath == NULL)
	{
		return NULL;
	}

	setCommand = makeStringInfo();
	appendStringInfo(setCommand, "SET search_path TO %s;", currentSearchPath);

	return setCommand->data;
}


/*
 * CurrentSearchPath is a C interface for calling current_schemas(bool) that
 * PostgreSQL exports.
 *
 * CurrentSchemas returns all the schemas in the seach_path that are seperated
 * with comma (,) sign. The returned string can be used to set the search_path.
 *
 * The function omits implicit schemas.
 *
 * The function returns NULL if there are no valid schemas in the search_path,
 * mimicing current_schemas(false) function.
 */
static char *
CurrentSearchPath(void)
{
	StringInfo currentSearchPath = makeStringInfo();
	List *searchPathList = fetch_search_path(false);
	ListCell *searchPathCell;
	bool schemaAdded = false;

	foreach(searchPathCell, searchPathList)
	{
		char *schemaName = get_namespace_name(lfirst_oid(searchPathCell));

		/* watch out for deleted namespace */
		if (schemaName)
		{
			if (schemaAdded)
			{
				appendStringInfoString(currentSearchPath, ",");
				schemaAdded = false;
			}

			appendStringInfoString(currentSearchPath, quote_identifier(schemaName));
			schemaAdded = true;
		}
	}

	/* fetch_search_path() returns a palloc'd list that we should free now */
	list_free(searchPathList);

	return (currentSearchPath->len > 0 ? currentSearchPath->data : NULL);
}


/*
 * DDLTaskList builds a list of tasks to execute a DDL command on a
 * given list of shards.
 */
List *
DDLTaskList(Oid relationId, const char *commandString)
{
	List *taskList = NIL;
	List *shardIntervalList = LoadShardIntervalList(relationId);
	ListCell *shardIntervalCell = NULL;
	Oid schemaId = get_rel_namespace(relationId);
	char *schemaName = get_namespace_name(schemaId);
	char *escapedSchemaName = quote_literal_cstr(schemaName);
	char *escapedCommandString = quote_literal_cstr(commandString);
	uint64 jobId = INVALID_JOB_ID;
	int taskId = 1;

	/* lock metadata before getting placement lists */
	LockShardListMetadata(shardIntervalList, ShareLock);

	foreach(shardIntervalCell, shardIntervalList)
	{
		ShardInterval *shardInterval = (ShardInterval *) lfirst(shardIntervalCell);
		uint64 shardId = shardInterval->shardId;
		StringInfo applyCommand = makeStringInfo();
		Task *task = NULL;

		/*
		 * If rightRelationId is not InvalidOid, instead of worker_apply_shard_ddl_command
		 * we use worker_apply_inter_shard_ddl_command.
		 */
		appendStringInfo(applyCommand, WORKER_APPLY_SHARD_DDL_COMMAND, shardId,
						 escapedSchemaName, escapedCommandString);

		task = CitusMakeNode(Task);
		task->jobId = jobId;
		task->taskId = taskId++;
		task->taskType = DDL_TASK;
		task->queryString = applyCommand->data;
		task->replicationModel = REPLICATION_MODEL_INVALID;
		task->dependedTaskList = NULL;
		task->anchorShardId = shardId;
		task->taskPlacementList = FinalizedShardPlacementList(shardId);

		taskList = lappend(taskList, task);
	}

	return taskList;
}


/*
 * InterShardDDLTaskList builds a list of tasks to execute a inter shard DDL command on a
 * shards of given list of distributed table. At the moment this function is used to run
 * foreign key and partitioning command on worker node.
 *
 * leftRelationId is the relation id of actual distributed table which given command is
 * applied. rightRelationId is the relation id of distributed table which given command
 * refers to.
 */
static List *
InterShardDDLTaskList(Oid leftRelationId, Oid rightRelationId,
					  const char *commandString)
{
	List *taskList = NIL;

	List *leftShardList = LoadShardIntervalList(leftRelationId);
	ListCell *leftShardCell = NULL;
	Oid leftSchemaId = get_rel_namespace(leftRelationId);
	char *leftSchemaName = get_namespace_name(leftSchemaId);
	char *escapedLeftSchemaName = quote_literal_cstr(leftSchemaName);

	char rightPartitionMethod = PartitionMethod(rightRelationId);
	List *rightShardList = LoadShardIntervalList(rightRelationId);
	ListCell *rightShardCell = NULL;
	Oid rightSchemaId = get_rel_namespace(rightRelationId);
	char *rightSchemaName = get_namespace_name(rightSchemaId);
	char *escapedRightSchemaName = quote_literal_cstr(rightSchemaName);

	char *escapedCommandString = quote_literal_cstr(commandString);
	uint64 jobId = INVALID_JOB_ID;
	int taskId = 1;

	/*
	 * If the rightPartitionMethod is a reference table, we need to make sure
	 * that the tasks are created in a way that the right shard stays the same
	 * since we only have one placement per worker. This hack is first implemented
	 * for foreign constraint support from distributed tables to reference tables.
	 */
	if (rightPartitionMethod == DISTRIBUTE_BY_NONE)
	{
		ShardInterval *rightShardInterval = NULL;
		int rightShardCount = list_length(rightShardList);
		int leftShardCount = list_length(leftShardList);
		int shardCounter = 0;

		Assert(rightShardCount == 1);

		rightShardInterval = (ShardInterval *) linitial(rightShardList);
		for (shardCounter = rightShardCount; shardCounter < leftShardCount;
			 shardCounter++)
		{
			rightShardList = lappend(rightShardList, rightShardInterval);
		}
	}

	/* lock metadata before getting placement lists */
	LockShardListMetadata(leftShardList, ShareLock);

	forboth(leftShardCell, leftShardList, rightShardCell, rightShardList)
	{
		ShardInterval *leftShardInterval = (ShardInterval *) lfirst(leftShardCell);
		uint64 leftShardId = leftShardInterval->shardId;
		StringInfo applyCommand = makeStringInfo();
		Task *task = NULL;
		RelationShard *leftRelationShard = CitusMakeNode(RelationShard);
		RelationShard *rightRelationShard = CitusMakeNode(RelationShard);

		ShardInterval *rightShardInterval = (ShardInterval *) lfirst(rightShardCell);
		uint64 rightShardId = rightShardInterval->shardId;

		leftRelationShard->relationId = leftRelationId;
		leftRelationShard->shardId = leftShardId;

		rightRelationShard->relationId = rightRelationId;
		rightRelationShard->shardId = rightShardId;

		appendStringInfo(applyCommand, WORKER_APPLY_INTER_SHARD_DDL_COMMAND,
						 leftShardId, escapedLeftSchemaName, rightShardId,
						 escapedRightSchemaName, escapedCommandString);

		task = CitusMakeNode(Task);
		task->jobId = jobId;
		task->taskId = taskId++;
		task->taskType = DDL_TASK;
		task->queryString = applyCommand->data;
		task->dependedTaskList = NULL;
		task->replicationModel = REPLICATION_MODEL_INVALID;
		task->anchorShardId = leftShardId;
		task->taskPlacementList = FinalizedShardPlacementList(leftShardId);
		task->relationShardList = list_make2(leftRelationShard, rightRelationShard);

		taskList = lappend(taskList, task);
	}

	return taskList;
}


/*
 * PostProcessUtility performs additional tasks after a utility's local portion
 * has been completed. Right now, the sole use is marking new indexes invalid
 * if they were created using the CONCURRENTLY flag. This (non-transactional)
 * change provides the fallback state if an error is raised, otherwise a sub-
 * sequent change to valid will be committed.
 */
static void
PostProcessUtility(Node *parsetree)
{
	IndexStmt *indexStmt = NULL;
	Relation relation = NULL;
	Oid indexRelationId = InvalidOid;
	Relation indexRelation = NULL;
	Relation pg_index = NULL;
	HeapTuple indexTuple = NULL;
	Form_pg_index indexForm = NULL;

	/* only IndexStmts are processed */
	if (!IsA(parsetree, IndexStmt))
	{
		return;
	}

	/* and even then only if they're CONCURRENT */
	indexStmt = (IndexStmt *) parsetree;
	if (!indexStmt->concurrent)
	{
		return;
	}

	/* finally, this logic only applies to the coordinator */
	if (!IsCoordinator())
	{
		return;
	}

	/* commit the current transaction and start anew */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* get the affected relation and index */
	relation = heap_openrv(indexStmt->relation, ShareUpdateExclusiveLock);
	indexRelationId = get_relname_relid(indexStmt->idxname,
										RelationGetNamespace(relation));
	indexRelation = index_open(indexRelationId, RowExclusiveLock);

	/* close relations but retain locks */
	heap_close(relation, NoLock);
	index_close(indexRelation, NoLock);

	/* mark index as invalid, in-place (cannot be rolled back) */
	index_set_state_flags(indexRelationId, INDEX_DROP_CLEAR_VALID);

	/* re-open a transaction command from here on out */
	CommitTransactionCommand();
	StartTransactionCommand();

	/* now, update index's validity in a way that can roll back */
	pg_index = heap_open(IndexRelationId, RowExclusiveLock);

	indexTuple = SearchSysCacheCopy1(INDEXRELID, ObjectIdGetDatum(indexRelationId));
	Assert(HeapTupleIsValid(indexTuple)); /* better be present, we have lock! */

	/* mark as valid, save, and update pg_index indexes */
	indexForm = (Form_pg_index) GETSTRUCT(indexTuple);
	indexForm->indisvalid = true;

	CatalogTupleUpdate(pg_index, &indexTuple->t_self, indexTuple);

	/* clean up; index now marked valid, but ROLLBACK will mark invalid */
	heap_freetuple(indexTuple);
	heap_close(pg_index, RowExclusiveLock);
}


/*
 * PlanGrantStmt determines whether a given GRANT/REVOKE statement involves
 * a distributed table. If so, it creates DDLJobs to encapsulate information
 * needed during the worker node portion of DDL execution before returning the
 * DDLJobs in a List. If no distributed table is involved, this returns NIL.
 *
 * NB: So far column level privileges are not supported.
 */
List *
PlanGrantStmt(GrantStmt *grantStmt)
{
	StringInfoData privsString;
	StringInfoData granteesString;
	StringInfoData targetString;
	StringInfoData ddlString;
	ListCell *granteeCell = NULL;
	List *tableIdList = NIL;
	ListCell *tableListCell = NULL;
	bool isFirst = true;
	List *ddlJobs = NIL;

	initStringInfo(&privsString);
	initStringInfo(&granteesString);
	initStringInfo(&targetString);
	initStringInfo(&ddlString);

	/*
	 * So far only table level grants are supported. Most other types of
	 * grants aren't interesting anyway.
	 */
	if (grantStmt->objtype != RELATION_OBJECT_TYPE)
	{
		return NIL;
	}

	tableIdList = CollectGrantTableIdList(grantStmt);

	/* nothing to do if there is no distributed table in the grant list */
	if (tableIdList == NIL)
	{
		return NIL;
	}

	/* deparse the privileges */
	if (grantStmt->privileges == NIL)
	{
		appendStringInfo(&privsString, "ALL");
	}
	else
	{
		ListCell *privilegeCell = NULL;

		isFirst = true;
		foreach(privilegeCell, grantStmt->privileges)
		{
			AccessPriv *priv = lfirst(privilegeCell);

			if (!isFirst)
			{
				appendStringInfoString(&privsString, ", ");
			}
			isFirst = false;

			if (priv->cols != NIL)
			{
				ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
								errmsg("grant/revoke on column list is currently "
									   "unsupported")));
			}

			Assert(priv->priv_name != NULL);

			appendStringInfo(&privsString, "%s", priv->priv_name);
		}
	}

	/* deparse the grantees */
	isFirst = true;
	foreach(granteeCell, grantStmt->grantees)
	{
		RoleSpec *spec = lfirst(granteeCell);

		if (!isFirst)
		{
			appendStringInfoString(&granteesString, ", ");
		}
		isFirst = false;

		appendStringInfoString(&granteesString, RoleSpecString(spec));
	}

	/*
	 * Deparse the target objects, and issue the deparsed statements to
	 * workers, if applicable. That's so we easily can replicate statements
	 * only to distributed relations.
	 */
	isFirst = true;
	foreach(tableListCell, tableIdList)
	{
		Oid relationId = lfirst_oid(tableListCell);
		const char *grantOption = "";
		DDLJob *ddlJob = NULL;

		Assert(IsDistributedTable(relationId));

		resetStringInfo(&targetString);
		appendStringInfo(&targetString, "%s", generate_relation_name(relationId, NIL));

		if (grantStmt->is_grant)
		{
			if (grantStmt->grant_option)
			{
				grantOption = " WITH GRANT OPTION";
			}

			appendStringInfo(&ddlString, "GRANT %s ON %s TO %s%s",
							 privsString.data, targetString.data, granteesString.data,
							 grantOption);
		}
		else
		{
			if (grantStmt->grant_option)
			{
				grantOption = "GRANT OPTION FOR ";
			}

			appendStringInfo(&ddlString, "REVOKE %s%s ON %s FROM %s",
							 grantOption, privsString.data, targetString.data,
							 granteesString.data);
		}

		ddlJob = palloc0(sizeof(DDLJob));
		ddlJob->targetRelationId = relationId;
		ddlJob->concurrentIndexCmd = false;
		ddlJob->commandString = pstrdup(ddlString.data);
		ddlJob->taskList = DDLTaskList(relationId, ddlString.data);

		ddlJobs = lappend(ddlJobs, ddlJob);

		resetStringInfo(&ddlString);
	}

	return ddlJobs;
}


/*
 *  CollectGrantTableIdList determines and returns a list of distributed table
 *  Oids from grant statement.
 *  Grant statement may appear in two forms
 *  1 - grant on table:
 *      each distributed table oid in grant object list is added to returned list.
 *  2 - grant all tables in schema:
 *     Collect namespace oid list from grant statement
 *     Add each distributed table oid in the target namespace list to the returned list.
 */
static List *
CollectGrantTableIdList(GrantStmt *grantStmt)
{
	List *grantTableList = NIL;
	bool grantOnTableCommand = false;
	bool grantAllTablesOnSchemaCommand = false;

	grantOnTableCommand = (grantStmt->targtype == ACL_TARGET_OBJECT &&
						   grantStmt->objtype == RELATION_OBJECT_TYPE);
	grantAllTablesOnSchemaCommand = (grantStmt->targtype == ACL_TARGET_ALL_IN_SCHEMA &&
									 grantStmt->objtype == RELATION_OBJECT_TYPE);

	/* we are only interested in table level grants */
	if (!grantOnTableCommand && !grantAllTablesOnSchemaCommand)
	{
		return NIL;
	}

	if (grantAllTablesOnSchemaCommand)
	{
		List *distTableOidList = DistTableOidList();
		ListCell *distributedTableOidCell = NULL;
		List *namespaceOidList = NIL;

		ListCell *objectCell = NULL;
		foreach(objectCell, grantStmt->objects)
		{
			char *nspname = strVal(lfirst(objectCell));
			bool missing_ok = false;
			Oid namespaceOid = get_namespace_oid(nspname, missing_ok);
			Assert(namespaceOid != InvalidOid);
			namespaceOidList = list_append_unique_oid(namespaceOidList, namespaceOid);
		}

		foreach(distributedTableOidCell, distTableOidList)
		{
			Oid relationId = lfirst_oid(distributedTableOidCell);
			Oid namespaceOid = get_rel_namespace(relationId);
			if (list_member_oid(namespaceOidList, namespaceOid))
			{
				grantTableList = lappend_oid(grantTableList, relationId);
			}
		}
	}
	else
	{
		ListCell *objectCell = NULL;
		foreach(objectCell, grantStmt->objects)
		{
			RangeVar *relvar = (RangeVar *) lfirst(objectCell);
			Oid relationId = RangeVarGetRelid(relvar, NoLock, false);
			if (IsDistributedTable(relationId))
			{
				grantTableList = lappend_oid(grantTableList, relationId);
			}
		}
	}

	return grantTableList;
}


/*
 * RoleSpecString resolves the role specification to its string form that is suitable for transport to a worker node.
 * This function resolves the following identifiers from the current context so they are safe to transfer.
 *
 * CURRENT_USER - resolved to the user name of the current role being used
 * SESSION_USER - resolved to the user name of the user that opened the session
 */
const char *
RoleSpecString(RoleSpec *spec)
{
	switch (spec->roletype)
	{
		case ROLESPEC_CSTRING:
		{
			return quote_identifier(spec->rolename);
		}

		case ROLESPEC_CURRENT_USER:
		{
			return quote_identifier(GetUserNameFromId(GetUserId(), false));
		}

		case ROLESPEC_SESSION_USER:
		{
			return quote_identifier(GetUserNameFromId(GetSessionUserId(), false));
		}

		case ROLESPEC_PUBLIC:
		{
			return "PUBLIC";
		}

		default:
		{
			elog(ERROR, "unexpected role type %d", spec->roletype);
		}
	}
}


/*
 * SetupExecutionModeForAlterTable is the function that is responsible
 * for two things for practial purpose for not doing the same checks
 * twice:
 *     (a) For any command, decide and return whether we should
 *         run the command in sequntial mode
 *     (b) For commands in a transaction block, set the transaction local
 *         multi-shard modify mode to sequential when necessary
 *
 * The commands that operate on the same reference table shard in parallel
 * is in the interest of (a), where the return value indicates the executor
 * to run the command sequentially to prevent self-deadlocks.
 *
 * The commands that both operate on the same reference table shard in parallel
 * and cascades to run any parallel operation is in the interest of (b). By
 * setting the multi-shard mode, we ensure that the cascading parallel commands
 * are executed sequentially to prevent self-deadlocks.
 *
 * One final note on the function is that if the function decides to execute
 * the command in sequential mode, and a parallel command has already been
 * executed in the same transaction, the function errors out. See the comment
 * in the function for the rationale.
 */
static bool
SetupExecutionModeForAlterTable(Oid relationId, AlterTableCmd *command)
{
	bool executeSequentially = false;
	AlterTableType alterTableType = command->subtype;
	if (alterTableType == AT_DropConstraint)
	{
		char *constraintName = command->name;
		if (ConstraintIsAForeignKeyToReferenceTable(constraintName, relationId))
		{
			executeSequentially = true;
		}
	}
	else if (alterTableType == AT_AddColumn)
	{
		/*
		 * TODO: This code path will never be executed since we do not
		 * support foreign constraint creation via
		 * ALTER TABLE %s ADD COLUMN %s [constraint]. However, the code
		 * is kept in case we fix the constraint creation without a name
		 * and allow foreign key creation with the mentioned command.
		 */
		ColumnDef *columnDefinition = (ColumnDef *) command->def;
		List *columnConstraints = columnDefinition->constraints;

		ListCell *columnConstraint = NULL;
		foreach(columnConstraint, columnConstraints)
		{
			Constraint *constraint = (Constraint *) lfirst(columnConstraint);
			if (constraint->contype == CONSTR_FOREIGN)
			{
				Oid rightRelationId = RangeVarGetRelid(constraint->pktable, NoLock,
													   false);
				if (IsDistributedTable(rightRelationId) &&
					PartitionMethod(rightRelationId) == DISTRIBUTE_BY_NONE)
				{
					executeSequentially = true;
				}
			}
		}
	}
	else if (alterTableType == AT_DropColumn || alterTableType == AT_AlterColumnType)
	{
		char *affectedColumnName = command->name;

		if (ColumnAppearsInForeignKeyToReferenceTable(affectedColumnName,
													  relationId))
		{
			if (IsTransactionBlock() && alterTableType == AT_AlterColumnType)
			{
				SetLocalMultiShardModifyModeToSequential();
			}

			executeSequentially = true;
		}
	}
	else if (alterTableType == AT_AddConstraint)
	{
		/*
		 * We need to execute the ddls working with reference tables on the
		 * right side sequentially, because parallel ddl operations
		 * relating to one and only shard of a reference table on a worker
		 * may cause self-deadlocks.
		 */
		Constraint *constraint = (Constraint *) command->def;
		if (constraint->contype == CONSTR_FOREIGN)
		{
			Oid rightRelationId = RangeVarGetRelid(constraint->pktable, NoLock,
												   false);
			if (IsDistributedTable(rightRelationId) &&
				PartitionMethod(rightRelationId) == DISTRIBUTE_BY_NONE)
			{
				executeSequentially = true;
			}
		}
	}

	/*
	 * If there has already been a parallel query executed, the sequential mode
	 * would still use the already opened parallel connections to the workers for
	 * the distributed tables, thus contradicting our purpose of using
	 * sequential mode.
	 */
	if (executeSequentially && IsDistributedTable(relationId) &&
		PartitionMethod(relationId) != DISTRIBUTE_BY_NONE &&
		ParallelQueryExecutedInTransaction())
	{
		char *relationName = get_rel_name(relationId);

		ereport(ERROR, (errmsg("cannot modify table \"%s\" because there "
							   "was a parallel operation on a distributed table "
							   "in the transaction", relationName),
						errdetail("When there is a foreign key to a reference "
								  "table, Citus needs to perform all operations "
								  "over a single connection per node to ensure "
								  "consistency."),
						errhint("Try re-running the transaction with "
								"\"SET LOCAL citus.multi_shard_modify_mode TO "
								"\'sequential\';\"")));
	}

	return executeSequentially;
}
