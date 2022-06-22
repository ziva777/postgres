/*-------------------------------------------------------------------------
 *
 * tuplesortops.c
 *	  Implementation of tuple sorting.
 *
 *
 * Copyright (c) 2022, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/utils/sort/tuplesortops.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/hash.h"
#include "access/htup_details.h"
#include "access/nbtree.h"
#include "catalog/index.h"
#include "executor/executor.h"
#include "pg_trace.h"
#include "utils/datum.h"
#include "utils/lsyscache.h"
#include "utils/guc.h"
#include "utils/tuplesort.h"


/* sort-type codes for sort__start probes */
#define HEAP_SORT		0
#define INDEX_SORT		1
#define DATUM_SORT		2
#define CLUSTER_SORT	3

static void getdatum1_heap(Tuplesortstate *state,  SortTuple *stup);
static void getdatum1_cluster(Tuplesortstate *state,  SortTuple *stup);
static void getdatum1_index(Tuplesortstate *state,  SortTuple *stup);
static void getdatum1_datum(Tuplesortstate *state,  SortTuple *stup);
static int	comparetup_heap(const SortTuple *a, const SortTuple *b,
							Tuplesortstate *state);
static void writetup_heap(Tuplesortstate *state, LogicalTape *tape,
						  SortTuple *stup);
static void readtup_heap(Tuplesortstate *state, SortTuple *stup,
						 LogicalTape *tape, unsigned int len);
static int	comparetup_cluster(const SortTuple *a, const SortTuple *b,
							   Tuplesortstate *state);
static void writetup_cluster(Tuplesortstate *state, LogicalTape *tape,
							 SortTuple *stup);
static void readtup_cluster(Tuplesortstate *state, SortTuple *stup,
							LogicalTape *tape, unsigned int len);
static int	comparetup_index_btree(const SortTuple *a, const SortTuple *b,
								   Tuplesortstate *state);
static int	comparetup_index_hash(const SortTuple *a, const SortTuple *b,
								  Tuplesortstate *state);
static void writetup_index(Tuplesortstate *state, LogicalTape *tape,
						   SortTuple *stup);
static void readtup_index(Tuplesortstate *state, SortTuple *stup,
						  LogicalTape *tape, unsigned int len);
static int	comparetup_datum(const SortTuple *a, const SortTuple *b,
							 Tuplesortstate *state);
static void writetup_datum(Tuplesortstate *state, LogicalTape *tape,
						   SortTuple *stup);
static void readtup_datum(Tuplesortstate *state, SortTuple *stup,
						  LogicalTape *tape, unsigned int len);
static void freestate_cluster(Tuplesortstate *state);

typedef struct
{
	TupleDesc	tupDesc;

	/*
	 * These variables are specific to the CLUSTER case; they are set by
	 * tuplesort_begin_cluster.
	 */
	IndexInfo  *indexInfo;		/* info about index being used for reference */
	EState	   *estate;			/* for evaluating index expressions */
} TupleSortClusterArg;

typedef struct
{
	/*
	 * These variables are specific to the IndexTuple case; they are set by
	 * tuplesort_begin_index_xxx and used only by the IndexTuple routines.
	 */
	Relation	heapRel;		/* table the index is being built on */
	Relation	indexRel;		/* index being built */
} TupleSortIndexArg;

typedef struct
{
	TupleSortIndexArg	index;

	/* These are specific to the index_btree subcase: */
	bool		enforceUnique;	/* complain if we find duplicate tuples */
	bool		uniqueNullsNotDistinct; /* unique constraint null treatment */
} TupleSortIndexBTreeArg;

typedef struct
{
	TupleSortIndexArg	index;

	/* These are specific to the index_hash subcase: */
	uint32		high_mask;		/* masks for sortable part of hash code */
	uint32		low_mask;
	uint32		max_buckets;
} TupleSortIndexHashArg;

typedef struct
{
	TupleSortIndexArg	index;

	/*
	 * These variables are specific to the Datum case; they are set by
	 * tuplesort_begin_datum and used only by the DatumTuple routines.
	 */
	Oid			datumType;
	/* we need typelen in order to know how to copy the Datums. */
	int			datumTypeLen;
} TupleSortDatumArg;

Tuplesortstate *
tuplesort_begin_heap(TupleDesc tupDesc,
					 int nkeys, AttrNumber *attNums,
					 Oid *sortOperators, Oid *sortCollations,
					 bool *nullsFirstFlags,
					 int workMem, SortCoordinate coordinate, int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortOps   *ops = TuplesortstateGetOps(state);
	MemoryContext oldcontext;
	int			i;

	oldcontext = MemoryContextSwitchTo(ops->maincontext);

	AssertArg(nkeys > 0);

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin tuple sort: nkeys = %d, workMem = %d, randomAccess = %c",
			 nkeys, workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	ops->nKeys = nkeys;

	TRACE_POSTGRESQL_SORT_START(HEAP_SORT,
								false,	/* no unique check */
								nkeys,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	ops->getdatum1 = getdatum1_heap;
	ops->comparetup = comparetup_heap;
	ops->writetup = writetup_heap;
	ops->readtup = readtup_heap;
	ops->haveDatum1 = true;
	ops->arg = tupDesc;	/* assume we need not copy tupDesc */

	/* Prepare SortSupport data for each column */
	ops->sortKeys = (SortSupport) palloc0(nkeys * sizeof(SortSupportData));

	for (i = 0; i < nkeys; i++)
	{
		SortSupport sortKey = ops->sortKeys + i;

		AssertArg(attNums[i] != 0);
		AssertArg(sortOperators[i] != 0);

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = sortCollations[i];
		sortKey->ssup_nulls_first = nullsFirstFlags[i];
		sortKey->ssup_attno = attNums[i];
		/* Convey if abbreviation optimization is applicable in principle */
		sortKey->abbreviate = (i == 0 && ops->haveDatum1);

		PrepareSortSupportFromOrderingOp(sortOperators[i], sortKey);
	}

	/*
	 * The "onlyKey" optimization cannot be used with abbreviated keys, since
	 * tie-breaker comparisons may be required.  Typically, the optimization
	 * is only of value to pass-by-value types anyway, whereas abbreviated
	 * keys are typically only of value to pass-by-reference types.
	 */
	if (nkeys == 1 && !ops->sortKeys->abbrev_converter)
		ops->onlyKey = ops->sortKeys;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_cluster(TupleDesc tupDesc,
						Relation indexRel,
						int workMem,
						SortCoordinate coordinate, int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortOps   *ops = TuplesortstateGetOps(state);
	BTScanInsert indexScanKey;
	MemoryContext oldcontext;
	TupleSortClusterArg *arg;
	int			i;

	Assert(indexRel->rd_rel->relam == BTREE_AM_OID);

	oldcontext = MemoryContextSwitchTo(ops->maincontext);
	arg = (TupleSortClusterArg *) palloc0(sizeof(TupleSortClusterArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin tuple sort: nkeys = %d, workMem = %d, randomAccess = %c",
			 RelationGetNumberOfAttributes(indexRel),
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	ops->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(CLUSTER_SORT,
								false,	/* no unique check */
								ops->nKeys,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	ops->getdatum1 = getdatum1_cluster;
	ops->comparetup = comparetup_cluster;
	ops->writetup = writetup_cluster;
	ops->readtup = readtup_cluster;
	ops->freestate = freestate_cluster;
	ops->arg = arg;

	arg->indexInfo = BuildIndexInfo(indexRel);

	/*
	 * If we don't have a simple leading attribute, we don't currently
	 * initialize datum1, so disable optimizations that require it.
	 */
	if (arg->indexInfo->ii_IndexAttrNumbers[0] == 0)
		ops->haveDatum1 = false;
	else
		ops->haveDatum1 = true;

	arg->tupDesc = tupDesc;	/* assume we need not copy tupDesc */

	indexScanKey = _bt_mkscankey(indexRel, NULL);

	if (arg->indexInfo->ii_Expressions != NULL)
	{
		TupleTableSlot *slot;
		ExprContext *econtext;

		/*
		 * We will need to use FormIndexDatum to evaluate the index
		 * expressions.  To do that, we need an EState, as well as a
		 * TupleTableSlot to put the table tuples into.  The econtext's
		 * scantuple has to point to that slot, too.
		 */
		arg->estate = CreateExecutorState();
		slot = MakeSingleTupleTableSlot(tupDesc, &TTSOpsHeapTuple);
		econtext = GetPerTupleExprContext(arg->estate);
		econtext->ecxt_scantuple = slot;
	}

	/* Prepare SortSupport data for each column */
	ops->sortKeys = (SortSupport) palloc0(ops->nKeys *
											sizeof(SortSupportData));

	for (i = 0; i < ops->nKeys; i++)
	{
		SortSupport sortKey = ops->sortKeys + i;
		ScanKey		scanKey = indexScanKey->scankeys + i;
		int16		strategy;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = scanKey->sk_collation;
		sortKey->ssup_nulls_first =
			(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
		sortKey->ssup_attno = scanKey->sk_attno;
		/* Convey if abbreviation optimization is applicable in principle */
		sortKey->abbreviate = (i == 0 && ops->haveDatum1);

		AssertState(sortKey->ssup_attno != 0);

		strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
			BTGreaterStrategyNumber : BTLessStrategyNumber;

		PrepareSortSupportFromIndexRel(indexRel, strategy, sortKey);
	}

	pfree(indexScanKey);

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_btree(Relation heapRel,
							Relation indexRel,
							bool enforceUnique,
							bool uniqueNullsNotDistinct,
							int workMem,
							SortCoordinate coordinate,
							int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortOps   *ops = TuplesortstateGetOps(state);
	BTScanInsert indexScanKey;
	TupleSortIndexBTreeArg *arg;
	MemoryContext oldcontext;
	int			i;

	oldcontext = MemoryContextSwitchTo(ops->maincontext);
	arg = (TupleSortIndexBTreeArg *) palloc(sizeof(TupleSortIndexBTreeArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: unique = %c, workMem = %d, randomAccess = %c",
			 enforceUnique ? 't' : 'f',
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	ops->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	TRACE_POSTGRESQL_SORT_START(INDEX_SORT,
								enforceUnique,
								ops->nKeys,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	ops->getdatum1 = getdatum1_index;
	ops->comparetup = comparetup_index_btree;
	ops->writetup = writetup_index;
	ops->readtup = readtup_index;
	ops->haveDatum1 = true;
	ops->arg = arg;

	arg->index.heapRel = heapRel;
	arg->index.indexRel = indexRel;
	arg->enforceUnique = enforceUnique;
	arg->uniqueNullsNotDistinct = uniqueNullsNotDistinct;

	indexScanKey = _bt_mkscankey(indexRel, NULL);

	/* Prepare SortSupport data for each column */
	ops->sortKeys = (SortSupport) palloc0(ops->nKeys *
												sizeof(SortSupportData));

	for (i = 0; i < ops->nKeys; i++)
	{
		SortSupport sortKey = ops->sortKeys + i;
		ScanKey		scanKey = indexScanKey->scankeys + i;
		int16		strategy;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = scanKey->sk_collation;
		sortKey->ssup_nulls_first =
			(scanKey->sk_flags & SK_BT_NULLS_FIRST) != 0;
		sortKey->ssup_attno = scanKey->sk_attno;
		/* Convey if abbreviation optimization is applicable in principle */
		sortKey->abbreviate = (i == 0 && ops->haveDatum1);

		AssertState(sortKey->ssup_attno != 0);

		strategy = (scanKey->sk_flags & SK_BT_DESC) != 0 ?
			BTGreaterStrategyNumber : BTLessStrategyNumber;

		PrepareSortSupportFromIndexRel(indexRel, strategy, sortKey);
	}

	pfree(indexScanKey);

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_hash(Relation heapRel,
						   Relation indexRel,
						   uint32 high_mask,
						   uint32 low_mask,
						   uint32 max_buckets,
						   int workMem,
						   SortCoordinate coordinate,
						   int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortOps   *ops = TuplesortstateGetOps(state);
	MemoryContext oldcontext;
	TupleSortIndexHashArg *arg;

	oldcontext = MemoryContextSwitchTo(ops->maincontext);
	arg = (TupleSortIndexHashArg *) palloc(sizeof(TupleSortIndexHashArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: high_mask = 0x%x, low_mask = 0x%x, "
			 "max_buckets = 0x%x, workMem = %d, randomAccess = %c",
			 high_mask,
			 low_mask,
			 max_buckets,
			 workMem,
			 sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	ops->nKeys = 1;			/* Only one sort column, the hash code */

	ops->getdatum1 = getdatum1_index;
	ops->comparetup = comparetup_index_hash;
	ops->writetup = writetup_index;
	ops->readtup = readtup_index;
	ops->haveDatum1 = true;
	ops->arg = arg;

	arg->index.heapRel = heapRel;
	arg->index.indexRel = indexRel;

	arg->high_mask = high_mask;
	arg->low_mask = low_mask;
	arg->max_buckets = max_buckets;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_index_gist(Relation heapRel,
						   Relation indexRel,
						   int workMem,
						   SortCoordinate coordinate,
						   int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortOps   *ops = TuplesortstateGetOps(state);
	MemoryContext oldcontext;
	TupleSortIndexBTreeArg *arg;
	int			i;

	oldcontext = MemoryContextSwitchTo(ops->maincontext);
	arg = (TupleSortIndexBTreeArg *) palloc(sizeof(TupleSortIndexBTreeArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin index sort: workMem = %d, randomAccess = %c",
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	ops->nKeys = IndexRelationGetNumberOfKeyAttributes(indexRel);

	ops->getdatum1 = getdatum1_index;
	ops->comparetup = comparetup_index_btree;
	ops->writetup = writetup_index;
	ops->readtup = readtup_index;
	ops->haveDatum1 = true;
	ops->arg = arg;

	arg->index.heapRel = heapRel;
	arg->index.indexRel = indexRel;
	arg->enforceUnique = false;
	arg->uniqueNullsNotDistinct = false;

	/* Prepare SortSupport data for each column */
	ops->sortKeys = (SortSupport) palloc0(ops->nKeys *
										  sizeof(SortSupportData));

	for (i = 0; i < ops->nKeys; i++)
	{
		SortSupport sortKey = ops->sortKeys + i;

		sortKey->ssup_cxt = CurrentMemoryContext;
		sortKey->ssup_collation = indexRel->rd_indcollation[i];
		sortKey->ssup_nulls_first = false;
		sortKey->ssup_attno = i + 1;
		/* Convey if abbreviation optimization is applicable in principle */
		sortKey->abbreviate = (i == 0 && ops->haveDatum1);

		AssertState(sortKey->ssup_attno != 0);

		/* Look for a sort support function */
		PrepareSortSupportFromGistIndexRel(indexRel, sortKey);
	}

	MemoryContextSwitchTo(oldcontext);

	return state;
}

Tuplesortstate *
tuplesort_begin_datum(Oid datumType, Oid sortOperator, Oid sortCollation,
					  bool nullsFirstFlag, int workMem,
					  SortCoordinate coordinate, int sortopt)
{
	Tuplesortstate *state = tuplesort_begin_common(workMem, coordinate,
												   sortopt);
	TuplesortOps   *ops = TuplesortstateGetOps(state);
	TupleSortDatumArg *arg;
	MemoryContext oldcontext;
	int16		typlen;
	bool		typbyval;

	oldcontext = MemoryContextSwitchTo(ops->maincontext);
	arg = (TupleSortDatumArg *) palloc(sizeof(TupleSortDatumArg));

#ifdef TRACE_SORT
	if (trace_sort)
		elog(LOG,
			 "begin datum sort: workMem = %d, randomAccess = %c",
			 workMem, sortopt & TUPLESORT_RANDOMACCESS ? 't' : 'f');
#endif

	ops->nKeys = 1;			/* always a one-column sort */

	TRACE_POSTGRESQL_SORT_START(DATUM_SORT,
								false,	/* no unique check */
								1,
								workMem,
								sortopt & TUPLESORT_RANDOMACCESS,
								PARALLEL_SORT(coordinate));

	ops->getdatum1 = getdatum1_datum;
	ops->comparetup = comparetup_datum;
	ops->writetup = writetup_datum;
	ops->readtup = readtup_datum;
	ops->haveDatum1 = true;
	ops->arg = arg;

	arg->datumType = datumType;

	/* lookup necessary attributes of the datum type */
	get_typlenbyval(datumType, &typlen, &typbyval);
	arg->datumTypeLen = typlen;
	ops->tuples = !typbyval;

	/* Prepare SortSupport data */
	ops->sortKeys = (SortSupport) palloc0(sizeof(SortSupportData));

	ops->sortKeys->ssup_cxt = CurrentMemoryContext;
	ops->sortKeys->ssup_collation = sortCollation;
	ops->sortKeys->ssup_nulls_first = nullsFirstFlag;

	/*
	 * Abbreviation is possible here only for by-reference types.  In theory,
	 * a pass-by-value datatype could have an abbreviated form that is cheaper
	 * to compare.  In a tuple sort, we could support that, because we can
	 * always extract the original datum from the tuple as needed.  Here, we
	 * can't, because a datum sort only stores a single copy of the datum; the
	 * "tuple" field of each SortTuple is NULL.
	 */
	ops->sortKeys->abbreviate = !typbyval;

	PrepareSortSupportFromOrderingOp(sortOperator, ops->sortKeys);

	/*
	 * The "onlyKey" optimization cannot be used with abbreviated keys, since
	 * tie-breaker comparisons may be required.  Typically, the optimization
	 * is only of value to pass-by-value types anyway, whereas abbreviated
	 * keys are typically only of value to pass-by-reference types.
	 */
	if (!ops->sortKeys->abbrev_converter)
		ops->onlyKey = ops->sortKeys;

	MemoryContextSwitchTo(oldcontext);

	return state;
}

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input data is always copied; the caller need not save it.
 */
void
tuplesort_puttupleslot(Tuplesortstate *state, TupleTableSlot *slot)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(ops->tuplecontext);
	TupleDesc	tupDesc = (TupleDesc) ops->arg;
	SortTuple	stup;
	MinimalTuple tuple;
	HeapTupleData htup;

	/* copy the tuple into sort storage */
	tuple = ExecCopySlotMinimalTuple(slot);
	stup.tuple = (void *) tuple;
	/* set up first-column key value */
	htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
	stup.datum1 = heap_getattr(&htup,
							   ops->sortKeys[0].ssup_attno,
							   tupDesc,
							   &stup.isnull1);

	tuplesort_puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one tuple while collecting input data for sort.
 *
 * Note that the input data is always copied; the caller need not save it.
 */
void
tuplesort_putheaptuple(Tuplesortstate *state, HeapTuple tup)
{
	SortTuple	stup;
	TuplesortOps *ops = TuplesortstateGetOps(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(ops->tuplecontext);
	TupleSortClusterArg *arg = (TupleSortClusterArg *) ops->arg;

	/* copy the tuple into sort storage */
	tup = heap_copytuple(tup);
	stup.tuple = (void *) tup;

	/*
	 * set up first-column key value, and potentially abbreviate, if it's a
	 * simple column
	 */
	if (ops->haveDatum1)
	{
		stup.datum1 = heap_getattr(tup,
								   arg->indexInfo->ii_IndexAttrNumbers[0],
								   arg->tupDesc,
								   &stup.isnull1);
	}

	tuplesort_puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Collect one index tuple while collecting input data for sort, building
 * it from caller-supplied values.
 */
void
tuplesort_putindextuplevalues(Tuplesortstate *state, Relation rel,
							  ItemPointer self, Datum *values,
							  bool *isnull)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortIndexArg *arg = (TupleSortIndexArg *) ops->arg;
	MemoryContext oldcontext = MemoryContextSwitchTo(ops->tuplecontext);
	SortTuple	stup;
	IndexTuple	tuple;

	stup.tuple = index_form_tuple(RelationGetDescr(rel), values, isnull);
	tuple = ((IndexTuple) stup.tuple);
	tuple->t_tid = *self;
	/* set up first-column key value */
	stup.datum1 = index_getattr(tuple,
								1,
								RelationGetDescr(arg->indexRel),
								&stup.isnull1);

	tuplesort_puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Accept one Datum while collecting input data for sort.
 *
 * If the Datum is pass-by-ref type, the value will be copied.
 */
void
tuplesort_putdatum(Tuplesortstate *state, Datum val, bool isNull)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortDatumArg *arg = (TupleSortDatumArg *) ops->arg;
	MemoryContext oldcontext = MemoryContextSwitchTo(ops->tuplecontext);
	SortTuple	stup;

	/*
	 * Pass-by-value types or null values are just stored directly in
	 * stup.datum1 (and stup.tuple is not used and set to NULL).
	 *
	 * Non-null pass-by-reference values need to be copied into memory we
	 * control, and possibly abbreviated. The copied value is pointed to by
	 * stup.tuple and is treated as the canonical copy (e.g. to return via
	 * tuplesort_getdatum or when writing to tape); stup.datum1 gets the
	 * abbreviated value if abbreviation is happening, otherwise it's
	 * identical to stup.tuple.
	 */

	if (isNull || !ops->tuples)
	{
		/*
		 * Set datum1 to zeroed representation for NULLs (to be consistent,
		 * and to support cheap inequality tests for NULL abbreviated keys).
		 */
		stup.datum1 = !isNull ? val : (Datum) 0;
		stup.isnull1 = isNull;
		stup.tuple = NULL;		/* no separate storage */
	}
	else
	{
		stup.isnull1 = false;
		stup.datum1 = datumCopy(val, false, arg->datumTypeLen);
		stup.tuple = DatumGetPointer(stup.datum1);
	}

	tuplesort_puttuple_common(state, &stup);

	MemoryContextSwitchTo(oldcontext);
}

/*
 * Fetch the next tuple in either forward or back direction.
 * If successful, put tuple in slot and return true; else, clear the slot
 * and return false.
 *
 * Caller may optionally be passed back abbreviated value (on true return
 * value) when abbreviation was used, which can be used to cheaply avoid
 * equality checks that might otherwise be required.  Caller can safely make a
 * determination of "non-equal tuple" based on simple binary inequality.  A
 * NULL value in leading attribute will set abbreviated value to zeroed
 * representation, which caller may rely on in abbreviated inequality check.
 *
 * If copy is true, the slot receives a tuple that's been copied into the
 * caller's memory context, so that it will stay valid regardless of future
 * manipulations of the tuplesort's state (up to and including deleting the
 * tuplesort).  If copy is false, the slot will just receive a pointer to a
 * tuple held within the tuplesort, which is more efficient, but only safe for
 * callers that are prepared to have any subsequent manipulation of the
 * tuplesort's state invalidate slot contents.
 */
bool
tuplesort_gettupleslot(Tuplesortstate *state, bool forward, bool copy,
					   TupleTableSlot *slot, Datum *abbrev)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(ops->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	if (stup.tuple)
	{
		/* Record abbreviated key for caller */
		if (ops->sortKeys->abbrev_converter && abbrev)
			*abbrev = stup.datum1;

		if (copy)
			stup.tuple = heap_copy_minimal_tuple((MinimalTuple) stup.tuple);

		ExecStoreMinimalTuple((MinimalTuple) stup.tuple, slot, copy);
		return true;
	}
	else
	{
		ExecClearTuple(slot);
		return false;
	}
}

/*
 * Fetch the next tuple in either forward or back direction.
 * Returns NULL if no more tuples.  Returned tuple belongs to tuplesort memory
 * context, and must not be freed by caller.  Caller may not rely on tuple
 * remaining valid after any further manipulation of tuplesort.
 */
HeapTuple
tuplesort_getheaptuple(Tuplesortstate *state, bool forward)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(ops->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	return stup.tuple;
}

/*
 * Fetch the next index tuple in either forward or back direction.
 * Returns NULL if no more tuples.  Returned tuple belongs to tuplesort memory
 * context, and must not be freed by caller.  Caller may not rely on tuple
 * remaining valid after any further manipulation of tuplesort.
 */
IndexTuple
tuplesort_getindextuple(Tuplesortstate *state, bool forward)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	MemoryContext oldcontext = MemoryContextSwitchTo(ops->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
		stup.tuple = NULL;

	MemoryContextSwitchTo(oldcontext);

	return (IndexTuple) stup.tuple;
}

/*
 * Fetch the next Datum in either forward or back direction.
 * Returns false if no more datums.
 *
 * If the Datum is pass-by-ref type, the returned value is freshly palloc'd
 * in caller's context, and is now owned by the caller (this differs from
 * similar routines for other types of tuplesorts).
 *
 * Caller may optionally be passed back abbreviated value (on true return
 * value) when abbreviation was used, which can be used to cheaply avoid
 * equality checks that might otherwise be required.  Caller can safely make a
 * determination of "non-equal tuple" based on simple binary inequality.  A
 * NULL value will have a zeroed abbreviated value representation, which caller
 * may rely on in abbreviated inequality check.
 */
bool
tuplesort_getdatum(Tuplesortstate *state, bool forward,
				   Datum *val, bool *isNull, Datum *abbrev)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortDatumArg *arg = (TupleSortDatumArg *) ops->arg;
	MemoryContext oldcontext = MemoryContextSwitchTo(ops->sortcontext);
	SortTuple	stup;

	if (!tuplesort_gettuple_common(state, forward, &stup))
	{
		MemoryContextSwitchTo(oldcontext);
		return false;
	}

	/* Ensure we copy into caller's memory context */
	MemoryContextSwitchTo(oldcontext);

	/* Record abbreviated key for caller */
	if (ops->sortKeys->abbrev_converter && abbrev)
		*abbrev = stup.datum1;

	if (stup.isnull1 || !ops->tuples)
	{
		*val = stup.datum1;
		*isNull = stup.isnull1;
	}
	else
	{
		/* use stup.tuple because stup.datum1 may be an abbreviation */
		*val = datumCopy(PointerGetDatum(stup.tuple), false, arg->datumTypeLen);
		*isNull = false;
	}

	return true;
}


/*
 * Routines specialized for HeapTuple (actually MinimalTuple) case
 */

static void
getdatum1_heap(Tuplesortstate *state, SortTuple *stup)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	HeapTupleData htup;

	htup.t_len = ((MinimalTuple) stup->tuple)->t_len +
		MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) stup->tuple -
										MINIMAL_TUPLE_OFFSET);

	stup->datum1 = heap_getattr(&htup,
								ops->sortKeys[0].ssup_attno,
								(TupleDesc) ops->arg,
								&stup->isnull1);

}

static int
comparetup_heap(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	SortSupport sortKey = ops->sortKeys;
	HeapTupleData ltup;
	HeapTupleData rtup;
	TupleDesc	tupDesc;
	int			nkey;
	int32		compare;
	AttrNumber	attno;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;


	/* Compare the leading sort key */
	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  sortKey);
	if (compare != 0)
		return compare;

	/* Compare additional sort keys */
	ltup.t_len = ((MinimalTuple) a->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
	ltup.t_data = (HeapTupleHeader) ((char *) a->tuple - MINIMAL_TUPLE_OFFSET);
	rtup.t_len = ((MinimalTuple) b->tuple)->t_len + MINIMAL_TUPLE_OFFSET;
	rtup.t_data = (HeapTupleHeader) ((char *) b->tuple - MINIMAL_TUPLE_OFFSET);
	tupDesc = (TupleDesc) ops->arg;

	if (sortKey->abbrev_converter)
	{
		attno = sortKey->ssup_attno;

		datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

		compare = ApplySortAbbrevFullComparator(datum1, isnull1,
												datum2, isnull2,
												sortKey);
		if (compare != 0)
			return compare;
	}

	sortKey++;
	for (nkey = 1; nkey < ops->nKeys; nkey++, sortKey++)
	{
		attno = sortKey->ssup_attno;

		datum1 = heap_getattr(&ltup, attno, tupDesc, &isnull1);
		datum2 = heap_getattr(&rtup, attno, tupDesc, &isnull2);

		compare = ApplySortComparator(datum1, isnull1,
									  datum2, isnull2,
									  sortKey);
		if (compare != 0)
			return compare;
	}

	return 0;
}

static void
writetup_heap(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	MinimalTuple tuple = (MinimalTuple) stup->tuple;

	/* the part of the MinimalTuple we'll write: */
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	unsigned int tupbodylen = tuple->t_len - MINIMAL_TUPLE_DATA_OFFSET;

	/* total on-disk footprint: */
	unsigned int tuplen = tupbodylen + sizeof(int);

	LogicalTapeWrite(tape, (void *) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(tape, (void *) tupbody, tupbodylen);
	if (ops->sortopt & TUPLESORT_RANDOMACCESS)	/* need trailing length
													 * word? */
		LogicalTapeWrite(tape, (void *) &tuplen, sizeof(tuplen));
}

static void
readtup_heap(Tuplesortstate *state, SortTuple *stup,
			 LogicalTape *tape, unsigned int len)
{
	unsigned int tupbodylen = len - sizeof(int);
	unsigned int tuplen = tupbodylen + MINIMAL_TUPLE_DATA_OFFSET;
	MinimalTuple tuple = (MinimalTuple) tuplesort_readtup_alloc(state, tuplen);
	char	   *tupbody = (char *) tuple + MINIMAL_TUPLE_DATA_OFFSET;
	TuplesortOps *ops = TuplesortstateGetOps(state);
	HeapTupleData htup;

	/* read in the tuple proper */
	tuple->t_len = tuplen;
	LogicalTapeReadExact(tape, tupbody, tupbodylen);
	if (ops->sortopt & TUPLESORT_RANDOMACCESS)	/* need trailing length
													 * word? */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value */
	htup.t_len = tuple->t_len + MINIMAL_TUPLE_OFFSET;
	htup.t_data = (HeapTupleHeader) ((char *) tuple - MINIMAL_TUPLE_OFFSET);
	stup->datum1 = heap_getattr(&htup,
								ops->sortKeys[0].ssup_attno,
								(TupleDesc) ops->arg,
								&stup->isnull1);
}

/*
 * Routines specialized for the CLUSTER case (HeapTuple data, with
 * comparisons per a btree index definition)
 */

static void
getdatum1_cluster(Tuplesortstate *state, SortTuple *stup)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortClusterArg *arg = (TupleSortClusterArg *) ops->arg;
	HeapTuple tup;

	tup = (HeapTuple) stup->tuple;
	stup->datum1 = heap_getattr(tup,
								arg->indexInfo->ii_IndexAttrNumbers[0],
								arg->tupDesc,
								&stup->isnull1);
}

static int
comparetup_cluster(const SortTuple *a, const SortTuple *b,
				   Tuplesortstate *state)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortClusterArg *arg = (TupleSortClusterArg *) ops->arg;
	SortSupport sortKey = ops->sortKeys;
	HeapTuple	ltup;
	HeapTuple	rtup;
	TupleDesc	tupDesc;
	int			nkey;
	int32		compare;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;

	/* Be prepared to compare additional sort keys */
	ltup = (HeapTuple) a->tuple;
	rtup = (HeapTuple) b->tuple;
	tupDesc = arg->tupDesc;

	/* Compare the leading sort key, if it's simple */
	if (ops->haveDatum1)
	{
		compare = ApplySortComparator(a->datum1, a->isnull1,
									  b->datum1, b->isnull1,
									  sortKey);
		if (compare != 0)
			return compare;

		if (sortKey->abbrev_converter)
		{
			AttrNumber	leading = arg->indexInfo->ii_IndexAttrNumbers[0];

			datum1 = heap_getattr(ltup, leading, tupDesc, &isnull1);
			datum2 = heap_getattr(rtup, leading, tupDesc, &isnull2);

			compare = ApplySortAbbrevFullComparator(datum1, isnull1,
													datum2, isnull2,
													sortKey);
		}
		if (compare != 0 || ops->nKeys == 1)
			return compare;
		/* Compare additional columns the hard way */
		sortKey++;
		nkey = 1;
	}
	else
	{
		/* Must compare all keys the hard way */
		nkey = 0;
	}

	if (arg->indexInfo->ii_Expressions == NULL)
	{
		/* If not expression index, just compare the proper heap attrs */

		for (; nkey < ops->nKeys; nkey++, sortKey++)
		{
			AttrNumber	attno = arg->indexInfo->ii_IndexAttrNumbers[nkey];

			datum1 = heap_getattr(ltup, attno, tupDesc, &isnull1);
			datum2 = heap_getattr(rtup, attno, tupDesc, &isnull2);

			compare = ApplySortComparator(datum1, isnull1,
										  datum2, isnull2,
										  sortKey);
			if (compare != 0)
				return compare;
		}
	}
	else
	{
		/*
		 * In the expression index case, compute the whole index tuple and
		 * then compare values.  It would perhaps be faster to compute only as
		 * many columns as we need to compare, but that would require
		 * duplicating all the logic in FormIndexDatum.
		 */
		Datum		l_index_values[INDEX_MAX_KEYS];
		bool		l_index_isnull[INDEX_MAX_KEYS];
		Datum		r_index_values[INDEX_MAX_KEYS];
		bool		r_index_isnull[INDEX_MAX_KEYS];
		TupleTableSlot *ecxt_scantuple;

		/* Reset context each time to prevent memory leakage */
		ResetPerTupleExprContext(arg->estate);

		ecxt_scantuple = GetPerTupleExprContext(arg->estate)->ecxt_scantuple;

		ExecStoreHeapTuple(ltup, ecxt_scantuple, false);
		FormIndexDatum(arg->indexInfo, ecxt_scantuple, arg->estate,
					   l_index_values, l_index_isnull);

		ExecStoreHeapTuple(rtup, ecxt_scantuple, false);
		FormIndexDatum(arg->indexInfo, ecxt_scantuple, arg->estate,
					   r_index_values, r_index_isnull);

		for (; nkey < ops->nKeys; nkey++, sortKey++)
		{
			compare = ApplySortComparator(l_index_values[nkey],
										  l_index_isnull[nkey],
										  r_index_values[nkey],
										  r_index_isnull[nkey],
										  sortKey);
			if (compare != 0)
				return compare;
		}
	}

	return 0;
}

static void
writetup_cluster(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	HeapTuple	tuple = (HeapTuple) stup->tuple;
	unsigned int tuplen = tuple->t_len + sizeof(ItemPointerData) + sizeof(int);

	/* We need to store t_self, but not other fields of HeapTupleData */
	LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
	LogicalTapeWrite(tape, &tuple->t_self, sizeof(ItemPointerData));
	LogicalTapeWrite(tape, tuple->t_data, tuple->t_len);
	if (ops->sortopt & TUPLESORT_RANDOMACCESS)	/* need trailing length
													 * word? */
		LogicalTapeWrite(tape, &tuplen, sizeof(tuplen));
}

static void
readtup_cluster(Tuplesortstate *state, SortTuple *stup,
				LogicalTape *tape, unsigned int tuplen)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortClusterArg *arg = (TupleSortClusterArg *) ops->arg;
	unsigned int t_len = tuplen - sizeof(ItemPointerData) - sizeof(int);
	HeapTuple	tuple = (HeapTuple) tuplesort_readtup_alloc(state,
												  t_len + HEAPTUPLESIZE);

	/* Reconstruct the HeapTupleData header */
	tuple->t_data = (HeapTupleHeader) ((char *) tuple + HEAPTUPLESIZE);
	tuple->t_len = t_len;
	LogicalTapeReadExact(tape, &tuple->t_self, sizeof(ItemPointerData));
	/* We don't currently bother to reconstruct t_tableOid */
	tuple->t_tableOid = InvalidOid;
	/* Read in the tuple body */
	LogicalTapeReadExact(tape, tuple->t_data, tuple->t_len);
	if (ops->sortopt & TUPLESORT_RANDOMACCESS)	/* need trailing length
													 * word? */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value, if it's a simple column */
	if (ops->haveDatum1)
		stup->datum1 = heap_getattr(tuple,
									arg->indexInfo->ii_IndexAttrNumbers[0],
									arg->tupDesc,
									&stup->isnull1);
}

static void
freestate_cluster(Tuplesortstate *state)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortClusterArg *arg = (TupleSortClusterArg *) ops->arg;

	/* Free any execution state created for CLUSTER case */
	if (arg->estate != NULL)
	{
		ExprContext *econtext = GetPerTupleExprContext(arg->estate);

		ExecDropSingleTupleTableSlot(econtext->ecxt_scantuple);
		FreeExecutorState(arg->estate);
	}
}

/*
 * Routines specialized for IndexTuple case
 *
 * The btree and hash cases require separate comparison functions, but the
 * IndexTuple representation is the same so the copy/write/read support
 * functions can be shared.
 */

static void
getdatum1_index(Tuplesortstate *state, SortTuple *stup)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortIndexArg *arg = (TupleSortIndexArg *) ops->arg;
	IndexTuple	tuple;

	tuple = stup->tuple;
	stup->datum1 = index_getattr(tuple,
								 1,
								 RelationGetDescr(arg->indexRel),
								 &stup->isnull1);
}

static int
comparetup_index_btree(const SortTuple *a, const SortTuple *b,
					   Tuplesortstate *state)
{
	/*
	 * This is similar to comparetup_heap(), but expects index tuples.  There
	 * is also special handling for enforcing uniqueness, and special
	 * treatment for equal keys at the end.
	 */
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortIndexBTreeArg *arg = (TupleSortIndexBTreeArg *) ops->arg;
	SortSupport sortKey = ops->sortKeys;
	IndexTuple	tuple1;
	IndexTuple	tuple2;
	int			keysz;
	TupleDesc	tupDes;
	bool		equal_hasnull = false;
	int			nkey;
	int32		compare;
	Datum		datum1,
				datum2;
	bool		isnull1,
				isnull2;


	/* Compare the leading sort key */
	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  sortKey);
	if (compare != 0)
		return compare;

	/* Compare additional sort keys */
	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;
	keysz = ops->nKeys;
	tupDes = RelationGetDescr(arg->index.indexRel);

	if (sortKey->abbrev_converter)
	{
		datum1 = index_getattr(tuple1, 1, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, 1, tupDes, &isnull2);

		compare = ApplySortAbbrevFullComparator(datum1, isnull1,
												datum2, isnull2,
												sortKey);
		if (compare != 0)
			return compare;
	}

	/* they are equal, so we only need to examine one null flag */
	if (a->isnull1)
		equal_hasnull = true;

	sortKey++;
	for (nkey = 2; nkey <= keysz; nkey++, sortKey++)
	{
		datum1 = index_getattr(tuple1, nkey, tupDes, &isnull1);
		datum2 = index_getattr(tuple2, nkey, tupDes, &isnull2);

		compare = ApplySortComparator(datum1, isnull1,
									  datum2, isnull2,
									  sortKey);
		if (compare != 0)
			return compare;		/* done when we find unequal attributes */

		/* they are equal, so we only need to examine one null flag */
		if (isnull1)
			equal_hasnull = true;
	}

	/*
	 * If btree has asked us to enforce uniqueness, complain if two equal
	 * tuples are detected (unless there was at least one NULL field and NULLS
	 * NOT DISTINCT was not set).
	 *
	 * It is sufficient to make the test here, because if two tuples are equal
	 * they *must* get compared at some stage of the sort --- otherwise the
	 * sort algorithm wouldn't have checked whether one must appear before the
	 * other.
	 */
	if (arg->enforceUnique && !(!arg->uniqueNullsNotDistinct && equal_hasnull))
	{
		Datum		values[INDEX_MAX_KEYS];
		bool		isnull[INDEX_MAX_KEYS];
		char	   *key_desc;

		/*
		 * Some rather brain-dead implementations of qsort (such as the one in
		 * QNX 4) will sometimes call the comparison routine to compare a
		 * value to itself, but we always use our own implementation, which
		 * does not.
		 */
		Assert(tuple1 != tuple2);

		index_deform_tuple(tuple1, tupDes, values, isnull);

		key_desc = BuildIndexValueDescription(arg->index.indexRel, values, isnull);

		ereport(ERROR,
				(errcode(ERRCODE_UNIQUE_VIOLATION),
				 errmsg("could not create unique index \"%s\"",
						RelationGetRelationName(arg->index.indexRel)),
				 key_desc ? errdetail("Key %s is duplicated.", key_desc) :
				 errdetail("Duplicate keys exist."),
				 errtableconstraint(arg->index.heapRel,
									RelationGetRelationName(arg->index.indexRel))));
	}

	/*
	 * If key values are equal, we sort on ItemPointer.  This is required for
	 * btree indexes, since heap TID is treated as an implicit last key
	 * attribute in order to ensure that all keys in the index are physically
	 * unique.
	 */
	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	/* ItemPointer values should never be equal */
	Assert(false);

	return 0;
}

static int
comparetup_index_hash(const SortTuple *a, const SortTuple *b,
					  Tuplesortstate *state)
{
	Bucket		bucket1;
	Bucket		bucket2;
	IndexTuple	tuple1;
	IndexTuple	tuple2;
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortIndexHashArg *arg = (TupleSortIndexHashArg *) ops->arg;

	/*
	 * Fetch hash keys and mask off bits we don't want to sort by. We know
	 * that the first column of the index tuple is the hash key.
	 */
	Assert(!a->isnull1);
	bucket1 = _hash_hashkey2bucket(DatumGetUInt32(a->datum1),
								   arg->max_buckets, arg->high_mask,
								   arg->low_mask);
	Assert(!b->isnull1);
	bucket2 = _hash_hashkey2bucket(DatumGetUInt32(b->datum1),
								   arg->max_buckets, arg->high_mask,
								   arg->low_mask);
	if (bucket1 > bucket2)
		return 1;
	else if (bucket1 < bucket2)
		return -1;

	/*
	 * If hash values are equal, we sort on ItemPointer.  This does not affect
	 * validity of the finished index, but it may be useful to have index
	 * scans in physical order.
	 */
	tuple1 = (IndexTuple) a->tuple;
	tuple2 = (IndexTuple) b->tuple;

	{
		BlockNumber blk1 = ItemPointerGetBlockNumber(&tuple1->t_tid);
		BlockNumber blk2 = ItemPointerGetBlockNumber(&tuple2->t_tid);

		if (blk1 != blk2)
			return (blk1 < blk2) ? -1 : 1;
	}
	{
		OffsetNumber pos1 = ItemPointerGetOffsetNumber(&tuple1->t_tid);
		OffsetNumber pos2 = ItemPointerGetOffsetNumber(&tuple2->t_tid);

		if (pos1 != pos2)
			return (pos1 < pos2) ? -1 : 1;
	}

	/* ItemPointer values should never be equal */
	Assert(false);

	return 0;
}

static void
writetup_index(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	IndexTuple	tuple = (IndexTuple) stup->tuple;
	unsigned int tuplen;

	tuplen = IndexTupleSize(tuple) + sizeof(tuplen);
	LogicalTapeWrite(tape, (void *) &tuplen, sizeof(tuplen));
	LogicalTapeWrite(tape, (void *) tuple, IndexTupleSize(tuple));
	if (ops->sortopt & TUPLESORT_RANDOMACCESS)	/* need trailing length
													 * word? */
		LogicalTapeWrite(tape, (void *) &tuplen, sizeof(tuplen));
}

static void
readtup_index(Tuplesortstate *state, SortTuple *stup,
			  LogicalTape *tape, unsigned int len)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortIndexArg *arg = (TupleSortIndexArg *) ops->arg;
	unsigned int tuplen = len - sizeof(unsigned int);
	IndexTuple	tuple = (IndexTuple) tuplesort_readtup_alloc(state, tuplen);

	LogicalTapeReadExact(tape, tuple, tuplen);
	if (ops->sortopt & TUPLESORT_RANDOMACCESS)	/* need trailing length
													 * word? */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
	stup->tuple = (void *) tuple;
	/* set up first-column key value */
	stup->datum1 = index_getattr(tuple,
								 1,
								 RelationGetDescr(arg->indexRel),
								 &stup->isnull1);
}

/*
 * Routines specialized for DatumTuple case
 */

static void
getdatum1_datum(Tuplesortstate *state, SortTuple *stup)
{
	stup->datum1 = PointerGetDatum(stup->tuple);
}

static int
comparetup_datum(const SortTuple *a, const SortTuple *b, Tuplesortstate *state)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	int			compare;

	compare = ApplySortComparator(a->datum1, a->isnull1,
								  b->datum1, b->isnull1,
								  ops->sortKeys);
	if (compare != 0)
		return compare;

	/* if we have abbreviations, then "tuple" has the original value */

	if (ops->sortKeys->abbrev_converter)
		compare = ApplySortAbbrevFullComparator(PointerGetDatum(a->tuple), a->isnull1,
												PointerGetDatum(b->tuple), b->isnull1,
												ops->sortKeys);

	return compare;
}

static void
writetup_datum(Tuplesortstate *state, LogicalTape *tape, SortTuple *stup)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	TupleSortDatumArg *arg = (TupleSortDatumArg *) ops->arg;
	void	   *waddr;
	unsigned int tuplen;
	unsigned int writtenlen;

	if (stup->isnull1)
	{
		waddr = NULL;
		tuplen = 0;
	}
	else if (!ops->tuples)
	{
		waddr = &stup->datum1;
		tuplen = sizeof(Datum);
	}
	else
	{
		waddr = stup->tuple;
		tuplen = datumGetSize(PointerGetDatum(stup->tuple), false, arg->datumTypeLen);
		Assert(tuplen != 0);
	}

	writtenlen = tuplen + sizeof(unsigned int);

	LogicalTapeWrite(tape, (void *) &writtenlen, sizeof(writtenlen));
	LogicalTapeWrite(tape, waddr, tuplen);
	if (ops->sortopt & TUPLESORT_RANDOMACCESS)	/* need trailing length
													 * word? */
		LogicalTapeWrite(tape, (void *) &writtenlen, sizeof(writtenlen));
}

static void
readtup_datum(Tuplesortstate *state, SortTuple *stup,
			  LogicalTape *tape, unsigned int len)
{
	TuplesortOps *ops = TuplesortstateGetOps(state);
	unsigned int tuplen = len - sizeof(unsigned int);

	if (tuplen == 0)
	{
		/* it's NULL */
		stup->datum1 = (Datum) 0;
		stup->isnull1 = true;
		stup->tuple = NULL;
	}
	else if (!ops->tuples)
	{
		Assert(tuplen == sizeof(Datum));
		LogicalTapeReadExact(tape, &stup->datum1, tuplen);
		stup->isnull1 = false;
		stup->tuple = NULL;
	}
	else
	{
		void	   *raddr = tuplesort_readtup_alloc(state, tuplen);

		LogicalTapeReadExact(tape, raddr, tuplen);
		stup->datum1 = PointerGetDatum(raddr);
		stup->isnull1 = false;
		stup->tuple = raddr;
	}

	if (ops->sortopt & TUPLESORT_RANDOMACCESS)	/* need trailing length
													 * word? */
		LogicalTapeReadExact(tape, &tuplen, sizeof(tuplen));
}

