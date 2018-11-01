/*-------------------------------------------------------------------------
 *
 * schemacmds.c
 *    Commands for altering and creating schemas for distributed tables.
 *
 * Copyright (c) 2018, Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */


#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "catalog/pg_class_d.h"
#include "distributed/commands/common.h"
#include "distributed/commands/schemacmds.h"
#include "distributed/foreign_constraint.h"
#include "distributed/metadata_cache.h"
#include "nodes/parsenodes.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/relcache.h"


/* Local functions forward declarations for helper functions */
static char * GetSchemaNameFromDropObject(ListCell *dropSchemaCell);

/*
 * ProcessDropSchemaStmt invalidates the foreign key cache if any table created
 * under dropped schema involved in any foreign key relationship.
 */
void
ProcessDropSchemaStmt(DropStmt *dropStatement)
{
	Relation pgClass = NULL;
	HeapTuple heapTuple = NULL;
	SysScanDesc scanDescriptor = NULL;
	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	Oid scanIndexId = InvalidOid;
	bool useIndex = false;
	ListCell *dropSchemaCell;

	if (dropStatement->behavior != DROP_CASCADE)
	{
		return;
	}

	foreach(dropSchemaCell, dropStatement->objects)
	{
		char *schemaString = GetSchemaNameFromDropObject(dropSchemaCell);
		Oid namespaceOid = get_namespace_oid(schemaString, true);

		if (namespaceOid == InvalidOid)
		{
			continue;
		}

		pgClass = heap_open(RelationRelationId, AccessShareLock);

		ScanKeyInit(&scanKey[0], Anum_pg_class_relnamespace, BTEqualStrategyNumber,
					F_OIDEQ, namespaceOid);
		scanDescriptor = systable_beginscan(pgClass, scanIndexId, useIndex, NULL,
											scanKeyCount, scanKey);

		heapTuple = systable_getnext(scanDescriptor);
		while (HeapTupleIsValid(heapTuple))
		{
			Form_pg_class relationForm = (Form_pg_class) GETSTRUCT(heapTuple);
			char *relationName = NameStr(relationForm->relname);
			Oid relationId = get_relname_relid(relationName, namespaceOid);

			/* we're not interested in non-valid, non-distributed relations */
			if (relationId == InvalidOid || !IsDistributedTable(relationId))
			{
				heapTuple = systable_getnext(scanDescriptor);
				continue;
			}

			/* invalidate foreign key cache if the table involved in any foreign key */
			if (TableReferenced(relationId) || TableReferencing(relationId))
			{
				MarkInvalidateForeignKeyGraph();

				systable_endscan(scanDescriptor);
				heap_close(pgClass, NoLock);
				return;
			}

			heapTuple = systable_getnext(scanDescriptor);
		}

		systable_endscan(scanDescriptor);
		heap_close(pgClass, NoLock);
	}
}


/*
 * GetSchemaNameFromDropObject gets the name of the drop schema from given
 * list cell. This function is defined due to API change between PG 9.6 and
 * PG 10.
 */
static char *
GetSchemaNameFromDropObject(ListCell *dropSchemaCell)
{
	char *schemaString = NULL;

#if (PG_VERSION_NUM >= 100000)
	Value *schemaValue = (Value *) lfirst(dropSchemaCell);
	schemaString = strVal(schemaValue);
#else
	List *schemaNameList = (List *) lfirst(dropSchemaCell);
	schemaString = NameListToString(schemaNameList);
#endif

	return schemaString;
}
