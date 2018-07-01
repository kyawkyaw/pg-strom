/*
 * relscan.c
 *
 * Common routines related to relation scan
 * ----
 * Copyright 2011-2018 (C) KaiGai Kohei <kaigai@kaigai.gr.jp>
 * Copyright 2014-2018 (C) The PG-Strom Development Team
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include "pg_strom.h"


/* Data structure for collecting qual clauses that match an index */
typedef struct
{
	bool		nonempty;		/* True if lists are not all empty */
	/* Lists of RestrictInfos, one per index column */
	List	   *indexclauses[INDEX_MAX_KEYS];
} IndexClauseSet;

/*
 * simple_match_clause_to_indexcol
 *
 * It is a simplified version of match_clause_to_indexcol.
 * Also see optimizer/path/indxpath.c
 */
static bool
simple_match_clause_to_indexcol(IndexOptInfo *index,
								int indexcol,
								RestrictInfo *rinfo)
{
	Expr	   *clause = rinfo->clause;
	Index		index_relid = index->rel->relid;
	Oid			opfamily = index->opfamily[indexcol];
	Oid			idxcollation = index->indexcollations[indexcol];
	Node	   *leftop;
	Node	   *rightop;
	Relids		left_relids;
	Relids		right_relids;
	Oid			expr_op;
	Oid			expr_coll;

	/* Clause must be a binary opclause */
	if (!is_opclause(clause))
		return false;

	leftop = get_leftop(clause);
	rightop = get_rightop(clause);
	if (!leftop || !rightop)
		return false;
	left_relids = rinfo->left_relids;
	right_relids = rinfo->right_relids;
	expr_op = ((OpExpr *) clause)->opno;
	expr_coll = ((OpExpr *) clause)->inputcollid;

	if (OidIsValid(idxcollation) && idxcollation != expr_coll)
		return false;

	/*
	 * Check for clauses of the form:
	 *    (indexkey operator constant) OR
	 *    (constant operator indexkey)
	 */
	if (match_index_to_operand(leftop, indexcol, index) &&
		!bms_is_member(index_relid, right_relids) &&
		!contain_volatile_functions(rightop) &&
		op_in_opfamily(expr_op, opfamily))
		return true;

	if (match_index_to_operand(rightop, indexcol, index) &&
		!bms_is_member(index_relid, left_relids) &&
		!contain_volatile_functions(leftop) &&
		op_in_opfamily(get_commutator(expr_op), opfamily))
		return true;

	return false;
}

/*
 * simple_match_clause_to_index
 *
 * It is a simplified version of match_clause_to_index.
 * Also see optimizer/path/indxpath.c
 */
static void
simple_match_clause_to_index(IndexOptInfo *index,
							 RestrictInfo *rinfo,
							 IndexClauseSet *clauseset)
{
	int		indexcol;

    /*
     * Never match pseudoconstants to indexes.  (Normally a match could not
     * happen anyway, since a pseudoconstant clause couldn't contain a Var,
     * but what if someone builds an expression index on a constant? It's not
     * totally unreasonable to do so with a partial index, either.)
     */
    if (rinfo->pseudoconstant)
        return;

    /*
     * If clause can't be used as an indexqual because it must wait till after
     * some lower-security-level restriction clause, reject it.
     */
    if (!restriction_is_securely_promotable(rinfo, index->rel))
        return;

	/* OK, check each index column for a match */
	for (indexcol = 0; indexcol < index->ncolumns; indexcol++)
	{
		if (simple_match_clause_to_indexcol(index,
											indexcol,
											rinfo))
		{
			clauseset->indexclauses[indexcol] =
				list_append_unique_ptr(clauseset->indexclauses[indexcol],
									   rinfo);
			clauseset->nonempty = true;
			break;
		}
	}
}

/*
 * estimate_brinindex_scan_nblocks
 *
 * Also see brincostestimate at utils/adt/selfuncs.c
 */
static cl_long
estimate_brinindex_scan_nblocks(PlannerInfo *root,
                                RelOptInfo *baserel,
								IndexOptInfo *index,
								IndexClauseSet *clauseset,
								List **p_indexQuals)
{
	RangeTblEntry  *rte = root->simple_rte_array[baserel->relid];
	Relation		indexRel;
	BrinStatsData	statsData;
	List		   *indexQuals = NIL;
	ListCell	   *lc;
	int				icol = 1;
	Selectivity		qualSelectivity;
	Selectivity		indexSelectivity;
	double			indexCorrelation = 0.0;
	double			indexRanges;
	double			minimalRanges;
	double			estimatedRanges;
	VariableStatData vardata;

	/* Obtain some data from the index itself. */
	indexRel = index_open(index->indexoid, AccessShareLock);
	brinGetStats(indexRel, &statsData);
	index_close(indexRel, AccessShareLock);

	/* Get selectivity of the index qualifiers */
	foreach (lc, index->indextlist)
	{
		TargetEntry *tle = lfirst(lc);
		ListCell   *cell;

		foreach (cell, clauseset->indexclauses[icol-1])
		{
			RestrictInfo *rinfo = lfirst(cell);

			indexQuals = lappend(indexQuals, rinfo);
		}

		if (IsA(tle->expr, Var))
		{
			Var	   *var = (Var *) tle->expr;

			/* in case of BRIN index on simple column */
			if (get_relation_stats_hook &&
				(*get_relation_stats_hook)(root, rte, var->varattno,
										   &vardata))
			{
				if (HeapTupleIsValid(vardata.statsTuple) && !vardata.freefunc)
					elog(ERROR, "no callback to release stats variable");
			}
			else
			{
				vardata.statsTuple =
					SearchSysCache3(STATRELATTINH,
									ObjectIdGetDatum(rte->relid),
									Int16GetDatum(var->varattno),
									BoolGetDatum(false));
				vardata.freefunc = ReleaseSysCache;
			}
		}
		else
		{
			if (get_index_stats_hook &&
				(*get_index_stats_hook)(root, index->indexoid, icol,
										&vardata))
			{
				if (HeapTupleIsValid(vardata.statsTuple) && !vardata.freefunc)
					elog(ERROR, "no callback to release stats variable");
			}
			else
			{
				vardata.statsTuple
					= SearchSysCache3(STATRELATTINH,
									  ObjectIdGetDatum(index->indexoid),
									  Int16GetDatum(icol),
									  BoolGetDatum(false));
                vardata.freefunc = ReleaseSysCache;
			}
		}

		if (HeapTupleIsValid(vardata.statsTuple))
		{
			AttStatsSlot	sslot;

			if (get_attstatsslot(&sslot, vardata.statsTuple,
								 STATISTIC_KIND_CORRELATION,
								 InvalidOid,
								 ATTSTATSSLOT_NUMBERS))
			{
				double		varCorrelation = 0.0;

				if (sslot.nnumbers > 0)
					varCorrelation = Abs(sslot.numbers[0]);

				if (varCorrelation > indexCorrelation)
					indexCorrelation = varCorrelation;

				free_attstatsslot(&sslot);
			}
		}
		ReleaseVariableStats(vardata);

		icol++;
	}
	qualSelectivity = clauselist_selectivity(root,
											 indexQuals,
											 baserel->relid,
											 JOIN_INNER,
											 NULL);

	/* estimate number of blocks to read */
	indexRanges = ceil((double) baserel->pages / statsData.pagesPerRange);
	if (indexRanges < 1.0)
		indexRanges = 1.0;
	minimalRanges = ceil(indexRanges * qualSelectivity);

	//elog(INFO, "strom: qualSelectivity=%.6f indexRanges=%.6f minimalRanges=%.6f indexCorrelation=%.6f", qualSelectivity, indexRanges, minimalRanges, indexCorrelation);

	if (indexCorrelation < 1.0e-10)
		estimatedRanges = indexRanges;
	else
		estimatedRanges = Min(minimalRanges / indexCorrelation, indexRanges);

	indexSelectivity = estimatedRanges / indexRanges;
	if (indexSelectivity < 0.0)
		indexSelectivity = 0.0;
	if (indexSelectivity > 1.0)
		indexSelectivity = 1.0;

	/* index quals, if any */
	if (p_indexQuals)
		*p_indexQuals = indexQuals;
	/* estimated number of blocks to read */
	return (cl_long)(indexSelectivity * (double) baserel->pages);
}

/*
 * extract_index_conditions
 */
static Node *
__fixup_indexqual_operand(Node *node, IndexOptInfo *indexOpt)
{
	ListCell   *lc;

	if (!node)
		return NULL;

	if (IsA(node, RelabelType))
	{
		RelabelType *relabel = (RelabelType *) node;

		return __fixup_indexqual_operand((Node *)relabel->arg, indexOpt);
	}

	foreach (lc, indexOpt->indextlist)
	{
		TargetEntry *tle = lfirst(lc);

		if (equal(node, tle->expr))
		{
			return (Node *)makeVar(INDEX_VAR,
								   tle->resno,
								   exprType((Node *)tle->expr),
								   exprTypmod((Node *) tle->expr),
								   exprCollation((Node *) tle->expr),
								   0);
		}
	}
	if (IsA(node, Var))
		elog(ERROR, "Bug? variable is not found at index tlist");
	return expression_tree_mutator(node, __fixup_indexqual_operand, indexOpt);
}

static List *
extract_index_conditions(List *index_quals, IndexOptInfo *indexOpt)
{
	List	   *result = NIL;
	ListCell   *lc;

	foreach (lc, index_quals)
	{
		RestrictInfo *rinfo = lfirst(lc);
		OpExpr	   *op = (OpExpr *) rinfo->clause;

		if (!IsA(rinfo->clause, OpExpr))
			elog(ERROR, "Bug? unexpected index clause: %s",
				 nodeToString(rinfo->clause));
		if (list_length(((OpExpr *)rinfo->clause)->args) != 2)
			elog(ERROR, "indexqual clause must be binary opclause");
		op = (OpExpr *)copyObject(rinfo->clause);
		if (!bms_equal(rinfo->left_relids, indexOpt->rel->relids))
			CommuteOpExpr(op);
		/* replace the indexkey expression with an index Var */
		linitial(op->args) = __fixup_indexqual_operand(linitial(op->args),
													   indexOpt);
		result = lappend(result, op);
	}
	return result;
}

/*
 * pgstrom_tryfind_brinindex
 */
IndexOptInfo *
pgstrom_tryfind_brinindex(PlannerInfo *root,
						  RelOptInfo *baserel,
						  List **p_indexConds,
						  List **p_indexQuals,
						  cl_long *p_indexNBlocks)
{
	cl_long			indexNBlocks = LONG_MAX;
	IndexOptInfo   *indexOpt = NULL;
	List		   *indexQuals = NIL;
	ListCell	   *cell;

	/* skip if no indexes */
	if (baserel->indexlist == NIL)
		return false;

	foreach (cell, baserel->indexlist)
	{
		IndexOptInfo   *index = (IndexOptInfo *) lfirst(cell);
		List		   *temp = NIL;
		ListCell	   *lc;
		cl_long			nblocks;
		IndexClauseSet	clauseset;

		/* Protect limited-size array in IndexClauseSets */
		Assert(index->ncolumns <= INDEX_MAX_KEYS);

		/* Ignore partial indexes that do not match the query. */
		if (index->indpred != NIL && !index->predOK)
			continue;

		/* Only BRIN-indexes are now supported */
		if (index->relam != BRIN_AM_OID)
			continue;

		/* see match_clauses_to_index */
		memset(&clauseset, 0, sizeof(IndexClauseSet));
		foreach (lc, index->indrestrictinfo)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			simple_match_clause_to_index(index, rinfo, &clauseset);
		}
		if (!clauseset.nonempty)
			continue;

		/*
		 * In case when multiple BRIN-indexes are configured,
		 * the one with minimal selectivity is the best choice.
		 */
 		nblocks = estimate_brinindex_scan_nblocks(root, baserel,
												  index,
												  &clauseset,
												  &temp);
		if (indexNBlocks > nblocks)
		{
			indexOpt = index;
			indexQuals = temp;
			indexNBlocks = nblocks;
		}
	}

	if (indexOpt)
	{
		if (p_indexConds)
			*p_indexConds = extract_index_conditions(indexQuals, indexOpt);
		if (p_indexQuals)
			*p_indexQuals = indexQuals;
		if (p_indexNBlocks)
			*p_indexNBlocks = indexNBlocks;
	}
	return indexOpt;
}

/*
 * pgstrom_common_relscan_cost
 */
int
pgstrom_common_relscan_cost(PlannerInfo *root,
							RelOptInfo *scan_rel,
							List *scan_quals,
							int parallel_workers,
							IndexOptInfo *indexOpt,
							List *indexQuals,
							cl_long indexNBlocks,
							double *p_parallel_divisor,
							double *p_scan_ntuples,
							double *p_scan_nchunks,
							cl_uint *p_nrows_per_block,
							Cost *p_startup_cost,
							Cost *p_run_cost)
{
	int			scan_mode = PGSTROM_RELSCAN_NORMAL;
	Cost		startup_cost = 0.0;
	Cost		run_cost = 0.0;
	Cost		index_scan_cost = 0.0;
	Cost		disk_scan_cost;
	double		gpu_ratio = pgstrom_gpu_operator_cost / cpu_operator_cost;
	double		parallel_divisor = (double) parallel_workers;
	double		ntuples = scan_rel->tuples;
	double		nblocks = scan_rel->pages;
	double		nchunks;
	double		selectivity;
	double		spc_seq_page_cost;
	double		spc_rand_page_cost;
	cl_uint		nrows_per_block = 0;
	Size		heap_size;
	Size		htup_size;
	QualCost	qcost;
	ListCell   *lc;

	Assert((scan_rel->reloptkind == RELOPT_BASEREL ||
			scan_rel->reloptkind == RELOPT_OTHER_MEMBER_REL) &&
		   scan_rel->relid > 0 &&
		   scan_rel->relid < root->simple_rel_array_size);

	/* selectivity of device executable qualifiers */
	selectivity = clauselist_selectivity(root,
										 scan_quals,
										 scan_rel->relid,
										 JOIN_INNER,
										 NULL);
	/* cost of full-table scan, if no index */
	get_tablespace_page_costs(scan_rel->reltablespace,
							  &spc_rand_page_cost,
							  &spc_seq_page_cost);
	disk_scan_cost = spc_seq_page_cost * nblocks;

	/* consideration for BRIN-index, if any */
	if (indexOpt)
	{
		BrinStatsData	statsData;
		Relation		index_rel;
		Cost			x;

		index_rel = index_open(indexOpt->indexoid, AccessShareLock);
		brinGetStats(index_rel, &statsData);
		index_close(index_rel, AccessShareLock);

		get_tablespace_page_costs(indexOpt->reltablespace,
								  &spc_rand_page_cost,
								  &spc_seq_page_cost);
		index_scan_cost = spc_seq_page_cost * statsData.revmapNumPages;
		foreach (lc, indexQuals)
		{
			cost_qual_eval_node(&qcost, (Node *)lfirst(lc), root);
			index_scan_cost += qcost.startup + qcost.per_tuple;
		}

		x = index_scan_cost + spc_rand_page_cost * (double)indexNBlocks;
		if (disk_scan_cost > x)
		{
			disk_scan_cost = x;
			ntuples = scan_rel->tuples * ((double) indexNBlocks / nblocks);
			nblocks = indexNBlocks;
			scan_mode |= PGSTROM_RELSCAN_BRIN_INDEX;
		}
	}

	/* check whether NVMe-Strom is capable */
	if (ScanPathWillUseNvmeStrom(root, scan_rel))
		scan_mode |= PGSTROM_RELSCAN_SSD2GPU;

	/*
	 * Cost adjustment by CPU parallelism, if used.
	 * (overall logic is equivalent to cost_seqscan())
	 */
	if (parallel_workers > 0)
	{
		double		leader_contribution;

		/* How much leader process can contribute query execution? */
		leader_contribution = 1.0 - (0.3 * (double)parallel_workers);
		if (leader_contribution > 0)
			parallel_divisor += leader_contribution;

		/* number of tuples to be actually processed */
		ntuples  = clamp_row_est(ntuples / parallel_divisor);

		/*
		 * After the v2.0, pg_strom.gpu_setup_cost represents the cost for
		 * run-time code build by NVRTC. Once binary is constructed, it can
		 * be shared with all the worker process, so we can discount the
		 * cost by parallel_divisor.
		 */
		startup_cost += pgstrom_gpu_setup_cost / parallel_divisor;

		/*
		 * Cost discount for more efficient I/O with multiplexing.
		 * PG background workers can issue read request to filesystem
		 * concurrently. It enables to work I/O subsystem during blocking-
		 * time for other workers, then, it pulls up usage ratio of the
		 * storage system.
		 */
		disk_scan_cost /= Min(2.0, sqrt(parallel_divisor));

		/* more disk i/o discount if NVMe-Strom is available */
		if ((scan_mode & PGSTROM_RELSCAN_SSD2GPU) != 0)
			disk_scan_cost /= 1.5;
	}
	else
	{
		parallel_divisor = 1.0;
		startup_cost += pgstrom_gpu_setup_cost;
	}
	run_cost += disk_scan_cost;

	/* estimation for number of chunks (assume KDS_FORMAT_ROW) */
	heap_size = (double)(BLCKSZ - SizeOfPageHeaderData) * nblocks;
	htup_size = (MAXALIGN(offsetof(HeapTupleHeaderData,
								   t_bits[BITMAPLEN(scan_rel->max_attr)])) +
				 MAXALIGN(heap_size / Max(scan_rel->tuples, 1.0) -
						  sizeof(ItemIdData) - SizeofHeapTupleHeader));
	nchunks =  (((double)(offsetof(kern_tupitem, htup) + htup_size +
						  sizeof(cl_uint)) * Max(ntuples, 1.0)) /
				((double)(pgstrom_chunk_size() -
						  KDS_CALCULATE_HEAD_LENGTH(scan_rel->max_attr))));
	nchunks = Max(nchunks, 1);

	/*
	 * estimation of the tuple density per block - this logic follows
	 * the manner in estimate_rel_size()
	 */
	if (scan_rel->pages > 0)
		nrows_per_block = ceil(scan_rel->tuples / (double)scan_rel->pages);
	else
	{
		RangeTblEntry *rte = root->simple_rte_array[scan_rel->relid];
		size_t		tuple_width = get_relation_data_width(rte->relid, NULL);

		tuple_width += MAXALIGN(SizeofHeapTupleHeader);
		tuple_width += sizeof(ItemIdData);
		/* note: integer division is intentional here */
		nrows_per_block = (BLCKSZ - SizeOfPageHeaderData) / tuple_width;
	}

	/* Cost for GPU qualifiers */
	cost_qual_eval_node(&qcost, (Node *)scan_quals, root);
	startup_cost += qcost.startup;
	run_cost += qcost.per_tuple * gpu_ratio * ntuples;
	ntuples *= selectivity;

	/* Cost for DMA transfer (host/storage --> GPU) */
	run_cost += pgstrom_gpu_dma_cost * nchunks;

	*p_parallel_divisor = parallel_divisor;
	*p_scan_ntuples = ntuples / parallel_divisor;
	*p_scan_nchunks = nchunks / parallel_divisor;
	*p_nrows_per_block =
		((scan_mode & PGSTROM_RELSCAN_SSD2GPU) != 0 ? nrows_per_block : 0);
	*p_startup_cost = startup_cost;
	*p_run_cost = run_cost;

	return scan_mode;
}

/*
 * pgstromExecInitBrinIndexMap
 */
void
pgstromExecInitBrinIndexMap(GpuTaskState *gts,
							Oid index_oid,
							List *index_conds)
{
	pgstromIndexState *pi_state = NULL;
	Relation	relation = gts->css.ss.ss_currentRelation;
	EState	   *estate = gts->css.ss.ps.state;
	Index		scanrelid;
	LOCKMODE	lockmode = NoLock;

	if (!OidIsValid(index_oid))
	{
		Assert(index_conds == NIL);
		gts->outer_index_state = NULL;
		return;
	}
	Assert(relation != NULL);
	scanrelid = ((Scan *) gts->css.ss.ps.plan)->scanrelid;
	if (!ExecRelationIsTargetRelation(estate, scanrelid))
		lockmode = AccessShareLock;

	pi_state = palloc0(sizeof(pgstromIndexState));
	pi_state->index_oid = index_oid;
	pi_state->index_rel = index_open(index_oid, lockmode);
	ExecIndexBuildScanKeys(&gts->css.ss.ps,
						   pi_state->index_rel,
						   index_conds,
						   false,
						   &pi_state->scan_keys,
						   &pi_state->num_scan_keys,
						   &pi_state->runtime_keys_info,
						   &pi_state->num_runtime_keys,
						   NULL,
						   NULL);

	/* ExprContext to evaluate runtime keys, if any */
	if (pi_state->num_runtime_keys != 0)
		pi_state->runtime_econtext = CreateExprContext(estate);
	else
		pi_state->runtime_econtext = NULL;

	/* BRIN index specific initialization */
	pi_state->nblocks = RelationGetNumberOfBlocks(relation);
	pi_state->brin_revmap = brinRevmapInitialize(pi_state->index_rel,
												 &pi_state->range_sz,
												 estate->es_snapshot);
	pi_state->brin_desc = brin_build_desc(pi_state->index_rel);

	/* save the state */
	gts->outer_index_state = pi_state;
}

/*
 * pgstromSizeOfBrinIndexMap
 */
Size
pgstromSizeOfBrinIndexMap(GpuTaskState *gts)
{
	pgstromIndexState *pi_state = gts->outer_index_state;
	int		nwords;

	if (!pi_state)
		return 0;

	nwords = (pi_state->nblocks +
			  pi_state->range_sz - 1) / pi_state->range_sz;
	return STROMALIGN(offsetof(Bitmapset, words) +
					  sizeof(bitmapword) * nwords);

}

/*
 * pgstromExecGetBrinIndexMap
 *
 * Also see bringetbitmap
 */
static void
__pgstromExecGetBrinIndexMap(pgstromIndexState *pi_state,
							 Bitmapset *brin_map,
							 Snapshot snapshot)
{
	BrinDesc	   *bdesc = pi_state->brin_desc;
	TupleDesc		bd_tupdesc = bdesc->bd_tupdesc;
	BlockNumber		nblocks = pi_state->nblocks;
	BlockNumber		range_sz = pi_state->range_sz;
	BlockNumber		heapBlk;
	BlockNumber		index;
	Buffer			buf = InvalidBuffer;
	FmgrInfo	   *consistentFn;
	BrinMemTuple   *dtup;
	BrinTuple	   *btup = NULL;
	Size			btupsz = 0;
	int				nranges;
	int				nwords;
	MemoryContext	oldcxt;
	MemoryContext	perRangeCxt;

	/* rooms for the consistent support procedures of indexed columns */
	consistentFn = palloc0(sizeof(FmgrInfo) * bd_tupdesc->natts);
	/* allocate an initial in-memory tuple */
	dtup = brin_new_memtuple(bdesc);

	/* moves to the working memory context per range */
	perRangeCxt = AllocSetContextCreate(CurrentMemoryContext,
										"PG-Strom BRIN-index temporary",
										ALLOCSET_DEFAULT_SIZES);
	oldcxt = MemoryContextSwitchTo(perRangeCxt);

	nranges = (pi_state->nblocks +
			   pi_state->range_sz - 1) / pi_state->range_sz;
	nwords = (nranges + BITS_PER_BITMAPWORD - 1) / BITS_PER_BITMAPWORD;
	Assert(brin_map->nwords < 0);
	memset(brin_map->words, 0, sizeof(bitmapword) * nwords);
	/*
	 * Now scan the revmap.  We start by querying for heap page 0,
	 * incrementing by the number of pages per range; this gives us a full
	 * view of the table.
	 */
	for (heapBlk = 0, index = 0;
		 heapBlk < nblocks;
		 heapBlk += range_sz, index++)
	{
		bool		addrange = true;
		BrinTuple  *tup;
		OffsetNumber off;
		Size		size;
		int			keyno;

		CHECK_FOR_INTERRUPTS();

		MemoryContextResetAndDeleteChildren(perRangeCxt);

		tup = brinGetTupleForHeapBlock(pi_state->brin_revmap, heapBlk,
									   &buf, &off, &size,
									   BUFFER_LOCK_SHARE,
									   snapshot);
		if (tup)
		{
			btup = brin_copy_tuple(tup, size, btup, &btupsz);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			dtup = brin_deform_tuple(bdesc, btup, dtup);
			if (!dtup->bt_placeholder)
			{
				for (keyno = 0; keyno < pi_state->num_scan_keys; keyno++)
				{
					ScanKey		key = &pi_state->scan_keys[keyno];
					AttrNumber	keyattno = key->sk_attno;
					BrinValues *bval = &dtup->bt_columns[keyattno - 1];
					Datum		rv;

					Assert((key->sk_flags & SK_ISNULL) ||
						   (key->sk_collation ==
							bd_tupdesc->attrs[keyattno - 1]->attcollation));
					/* First time this column? look up consistent function */
					if (consistentFn[keyattno - 1].fn_oid == InvalidOid)
					{
						FmgrInfo   *tmp;

						tmp = index_getprocinfo(pi_state->index_rel, keyattno,
												BRIN_PROCNUM_CONSISTENT);
						fmgr_info_copy(&consistentFn[keyattno - 1], tmp,
									   CurrentMemoryContext);
					}

					/*
					 * Check whether the scan key is consistent with the page
					 * range values; if so, have the pages in the range added
					 * to the output bitmap.
					 */
					rv = FunctionCall3Coll(&consistentFn[keyattno - 1],
										   key->sk_collation,
										   PointerGetDatum(bdesc),
										   PointerGetDatum(bval),
										   PointerGetDatum(key));
					addrange = DatumGetBool(rv);
					if (!addrange)
						break;
				}
			}
		}

		if (addrange)
		{
			if (index / BITS_PER_BITMAPWORD < nwords)
			{
				brin_map->words[index / BITS_PER_BITMAPWORD]
					|= (1U << (index % BITS_PER_BITMAPWORD));
			}
		}
	}
	MemoryContextSwitchTo(oldcxt);
	MemoryContextDelete(perRangeCxt);

	if (buf != InvalidBuffer)
		ReleaseBuffer(buf);
	/* mark this bitmapset is ready */
	pg_memory_barrier();
	brin_map->nwords = nwords;
}

void
pgstromExecGetBrinIndexMap(GpuTaskState *gts)
{
	pgstromIndexState *pi_state = gts->outer_index_state;

	if (!gts->outer_index_map || gts->outer_index_map->nwords < 0)
	{
		EState	   *estate = gts->css.ss.ps.state;

		if (!gts->outer_index_map)
		{
			Assert(!IsParallelWorker());
			gts->outer_index_map
				= MemoryContextAlloc(estate->es_query_cxt,
									 pgstromSizeOfBrinIndexMap(gts));
			gts->outer_index_map->nwords = -1;
		}

		ResetLatch(MyLatch);
		while (gts->outer_index_map->nwords < 0)
		{
			if (!IsParallelWorker())
			{
				__pgstromExecGetBrinIndexMap(pi_state,
											 gts->outer_index_map,
											 estate->es_snapshot);
				/* wake up parallel workers if any */
				if (gts->pcxt)
				{
					ParallelContext *pcxt = gts->pcxt;
					pid_t		pid;
					int			i;

					for (i=0; i < pcxt->nworkers_launched; i++)
					{
						if (GetBackgroundWorkerPid(pcxt->worker[i].bgwhandle,
												   &pid) == BGWH_STARTED)
							ProcSendSignal(pid);
					}
				}
			}
			else
			{
				/* wait for completion of BRIN-index preload */
				CHECK_FOR_INTERRUPTS();

				WaitLatch(MyLatch,
						  WL_LATCH_SET,
						  -1
#if PG_VERSION_NUM >= 100000
						  ,PG_WAIT_EXTENSION
#endif
					);
				ResetLatch(MyLatch);
			}
		}
	}
}

void
pgstromExecEndBrinIndexMap(GpuTaskState *gts)
{
	pgstromIndexState *pi_state = gts->outer_index_state;

	if (!pi_state)
		return;
	brinRevmapTerminate(pi_state->brin_revmap);
	index_close(pi_state->index_rel, NoLock);
}

void
pgstromExecRewindBrinIndexMap(GpuTaskState *gts)
{}