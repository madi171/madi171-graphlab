/*
 * Copyright 1997, Regents of the University of Minnesota
 *
 * minitpart.c
 *
 * This file contains code that performs the initial partition of the
 * coarsest graph
 *
 * Started 7/23/97
 * George
 *
 * $Id: minitpart.c,v 1.4 2003/07/31 16:14:40 karypis Exp $
 *
 */

#include <metislib.h>

/*************************************************************************
* This function computes the initial bisection of the coarsest graph
**************************************************************************/
void MocInit2WayPartition(CtrlType *ctrl, GraphType *graph, float *tpwgts, float ubfactor) 
{
  idxtype i, dbglvl;

  dbglvl = ctrl->dbglvl;
  IFSET(ctrl->dbglvl, DBG_REFINE, ctrl->dbglvl -= DBG_REFINE);
  IFSET(ctrl->dbglvl, DBG_MOVEINFO, ctrl->dbglvl -= DBG_MOVEINFO);

  IFSET(ctrl->dbglvl, DBG_TIME, gk_startcputimer(ctrl->InitPartTmr));

  switch (ctrl->IType) {
    case ITYPE_GGPKL:
      if (graph->nedges == 0)
        MocRandomBisection(ctrl, graph, tpwgts, ubfactor);
      else
        MocGrowBisection(ctrl, graph, tpwgts, ubfactor);
      break;
    case ITYPE_RANDOM:
      MocRandomBisection(ctrl, graph, tpwgts, ubfactor);
      break;
    default:
      errexit("Unknown initial partition type: %d\n", ctrl->IType);
  }

  IFSET(ctrl->dbglvl, DBG_IPART, mprintf("Initial Cut: %D [%D]\n", graph->mincut, graph->where[0]));
  IFSET(ctrl->dbglvl, DBG_TIME, gk_stopcputimer(ctrl->InitPartTmr));
  ctrl->dbglvl = dbglvl;

}





/*************************************************************************
* This function takes a graph and produces a bisection by using a region
* growing algorithm. The resulting partition is returned in
* graph->where
**************************************************************************/
void MocGrowBisection(CtrlType *ctrl, GraphType *graph, float *tpwgts, float ubfactor)
{
  idxtype i, j, k, nvtxs, ncon, from, bestcut, mincut, nbfs, inbfs;
  idxtype *bestwhere, *where;

  nvtxs = graph->nvtxs;

  MocAllocate2WayPartitionMemory(ctrl, graph);
  where = graph->where;

  bestwhere = idxmalloc(nvtxs, "BisectGraph: bestwhere");
  nbfs = 2*(nvtxs <= ctrl->CoarsenTo ? SMALLNIPARTS : LARGENIPARTS);

  for (inbfs=0; inbfs<nbfs; inbfs++) {
    idxset(nvtxs, 1, where);
    where[RandomInRange(nvtxs)] = 0;

    MocCompute2WayPartitionParams(ctrl, graph);

    MocInit2WayBalance(ctrl, graph, tpwgts);

    MocFM_2WayEdgeRefine(ctrl, graph, tpwgts, 4); 

    MocBalance2Way(ctrl, graph, tpwgts, 1.02);
    MocFM_2WayEdgeRefine(ctrl, graph, tpwgts, 4); 

    if (inbfs == 0 || bestcut >= graph->mincut) {
      bestcut = graph->mincut;
      idxcopy(nvtxs, where, bestwhere);
      if (bestcut == 0)
        break;
    }
  }

  graph->mincut = bestcut;
  idxcopy(nvtxs, bestwhere, where);

  gk_free((void **)&bestwhere, LTERM);
}



/*************************************************************************
* This function takes a graph and produces a bisection by using a region
* growing algorithm. The resulting partition is returned in
* graph->where
**************************************************************************/
void MocRandomBisection(CtrlType *ctrl, GraphType *graph, float *tpwgts, float ubfactor)
{
  idxtype i, ii, j, k, nvtxs, ncon, from, bestcut, mincut, nbfs, inbfs, qnum;
  idxtype *bestwhere, *where, *perm;
  idxtype counts[MAXNCON];
  float *nvwgt;

  nvtxs = graph->nvtxs;
  ncon = graph->ncon;
  nvwgt = graph->nvwgt;

  MocAllocate2WayPartitionMemory(ctrl, graph);
  where = graph->where;

  bestwhere = idxmalloc(nvtxs, "BisectGraph: bestwhere");
  nbfs = 2*(nvtxs <= ctrl->CoarsenTo ? SMALLNIPARTS : LARGENIPARTS);
  perm = idxmalloc(nvtxs, "BisectGraph: perm");

  for (inbfs=0; inbfs<nbfs; inbfs++) {
    for (i=0; i<ncon; i++)
      counts[i] = 0;

    RandomPermute(nvtxs, perm, 1);

    /* Partition by spliting the queues randomly */
    for (ii=0; ii<nvtxs; ii++) {
      i = perm[ii];
      qnum = gk_fargmax(ncon, nvwgt+i*ncon);
      where[i] = counts[qnum];
      counts[qnum] = (counts[qnum]+1)%2;
    }

    MocCompute2WayPartitionParams(ctrl, graph);

    MocFM_2WayEdgeRefine(ctrl, graph, tpwgts, 6); 
    MocBalance2Way(ctrl, graph, tpwgts, 1.02);
    MocFM_2WayEdgeRefine(ctrl, graph, tpwgts, 6); 
    MocBalance2Way(ctrl, graph, tpwgts, 1.02);
    MocFM_2WayEdgeRefine(ctrl, graph, tpwgts, 6); 

    /*
    mprintf("Edgecut: %6D, NPwgts: [", graph->mincut);
    for (i=0; i<graph->ncon; i++)
      mprintf("(%.3f %.3f) ", graph->npwgts[i], graph->npwgts[graph->ncon+i]);
    mprintf("]\n");
    */

    if (inbfs == 0 || bestcut >= graph->mincut) {
      bestcut = graph->mincut;
      idxcopy(nvtxs, where, bestwhere);
      if (bestcut == 0)
        break;
    }
  }

  graph->mincut = bestcut;
  idxcopy(nvtxs, bestwhere, where);

  gk_free((void **)&bestwhere, &perm, LTERM);
}




/*************************************************************************
* This function balances two partitions by moving the highest gain 
* (including negative gain) vertices to the other domain.
* It is used only when tha unbalance is due to non contigous
* subdomains. That is, the are no boundary vertices.
* It moves vertices from the domain that is overweight to the one that 
* is underweight.
**************************************************************************/
void MocInit2WayBalance(CtrlType *ctrl, GraphType *graph, float *tpwgts)
{
  idxtype i, ii, j, k, l, kwgt, nvtxs, nbnd, ncon, nswaps, from, to, pass, me, cnum, tmp;
  idxtype *xadj, *adjncy, *adjwgt, *where, *id, *ed, *bndptr, *bndind;
  idxtype *perm, *qnum;
  float *nvwgt, *npwgts;
  PQueueType parts[MAXNCON][2];
  idxtype higain, oldgain, mincut;

  nvtxs = graph->nvtxs;
  ncon = graph->ncon;
  xadj = graph->xadj;
  adjncy = graph->adjncy;
  nvwgt = graph->nvwgt;
  adjwgt = graph->adjwgt;
  where = graph->where;
  id = graph->id;
  ed = graph->ed;
  npwgts = graph->npwgts;
  bndptr = graph->bndptr;
  bndind = graph->bndind;

  perm = idxwspacemalloc(ctrl, nvtxs);
  qnum = idxwspacemalloc(ctrl, nvtxs);

  /* This is called for initial partitioning so we know from where to pick nodes */
  from = 1;
  to = (from+1)%2;

  if (ctrl->dbglvl&DBG_REFINE) {
    mprintf("Parts: [");
    for (l=0; l<ncon; l++)
      mprintf("(%.3f, %.3f) ", npwgts[l], npwgts[ncon+l]);
    mprintf("] T[%.3f %.3f], Nv-Nb[%5D, %5D]. ICut: %6D, LB: %.3f [B]\n", tpwgts[0], tpwgts[1], 
           graph->nvtxs, graph->nbnd, graph->mincut, 
           Compute2WayHLoadImbalance(ncon, npwgts, tpwgts));
  }

  for (i=0; i<ncon; i++) {
    PQueueInit(ctrl, &parts[i][0], nvtxs, PLUS_GAINSPAN+1);
    PQueueInit(ctrl, &parts[i][1], nvtxs, PLUS_GAINSPAN+1);
  }

  ASSERT(ComputeCut(graph, where) == graph->mincut);
  ASSERT(CheckBnd(graph));
  ASSERT(CheckGraph(graph));

  /* Compute the queues in which each vertex will be assigned to */
  for (i=0; i<nvtxs; i++)
    qnum[i] = gk_fargmax(ncon, nvwgt+i*ncon);

  /* Insert the nodes of the proper partition in the appropriate priority queue */
  RandomPermute(nvtxs, perm, 1);
  for (ii=0; ii<nvtxs; ii++) {
    i = perm[ii];
    if (where[i] == from) {
      if (ed[i] > 0)
        PQueueInsert(&parts[qnum[i]][0], i, ed[i]-id[i]);
      else
        PQueueInsert(&parts[qnum[i]][1], i, ed[i]-id[i]);
    }
  }


  mincut = graph->mincut;
  nbnd = graph->nbnd;
  for (nswaps=0; nswaps<nvtxs; nswaps++) {
    if (AreAnyVwgtsBelow(ncon, 1.0, npwgts+from*ncon, 0.0, nvwgt, tpwgts[from]))
      break;

    if ((cnum = SelectQueueOneWay(ncon, npwgts, tpwgts, from, parts)) == -1)
      break;

    if ((higain = PQueueGetMax(&parts[cnum][0])) == -1)
      higain = PQueueGetMax(&parts[cnum][1]);

    mincut -= (ed[higain]-id[higain]);
    gk_faxpy(ncon, 1.0, nvwgt+higain*ncon, 1, npwgts+to*ncon, 1);
    gk_faxpy(ncon, -1.0, nvwgt+higain*ncon, 1, npwgts+from*ncon, 1);

    where[higain] = to;

    if (ctrl->dbglvl&DBG_MOVEINFO) {
      mprintf("Moved %6D from %D(%D). [%5D] %5D, NPwgts: ", higain, from, cnum, ed[higain]-id[higain], mincut);
      for (l=0; l<ncon; l++) 
        mprintf("(%.3f, %.3f) ", npwgts[l], npwgts[ncon+l]);
      mprintf(", LB: %.3f\n", Compute2WayHLoadImbalance(ncon, npwgts, tpwgts));
      if (ed[higain] == 0 && id[higain] > 0)
        mprintf("\t Pulled from the interior!\n");
    }


    /**************************************************************
    * Update the id[i]/ed[i] values of the affected nodes
    ***************************************************************/
    SWAP(id[higain], ed[higain], tmp);
    if (ed[higain] == 0 && bndptr[higain] != -1 && xadj[higain] < xadj[higain+1]) 
      BNDDelete(nbnd, bndind,  bndptr, higain);
    if (ed[higain] > 0 && bndptr[higain] == -1)
      BNDInsert(nbnd, bndind,  bndptr, higain);

    for (j=xadj[higain]; j<xadj[higain+1]; j++) {
      k = adjncy[j];
      oldgain = ed[k]-id[k];

      kwgt = (to == where[k] ? adjwgt[j] : -adjwgt[j]);
      INC_DEC(id[k], ed[k], kwgt);

      /* Update the queue position */
      if (where[k] == from) {
        if (ed[k] > 0 && bndptr[k] == -1) {  /* It moves in boundary */
          PQueueDelete(&parts[qnum[k]][1], k, oldgain);
          PQueueInsert(&parts[qnum[k]][0], k, ed[k]-id[k]);
        }
        else { /* It must be in the boundary already */
          if (bndptr[k] == -1)
            mprintf("What you thought was wrong!\n");
          PQueueUpdate(&parts[qnum[k]][0], k, oldgain, ed[k]-id[k]);
        }
      }

      /* Update its boundary information */
      if (ed[k] == 0 && bndptr[k] != -1) 
        BNDDelete(nbnd, bndind, bndptr, k);
      else if (ed[k] > 0 && bndptr[k] == -1)  
        BNDInsert(nbnd, bndind, bndptr, k);
    }

    ASSERTP(ComputeCut(graph, where) == mincut, ("%d != %d\n", ComputeCut(graph, where), mincut));

  }

  if (ctrl->dbglvl&DBG_REFINE) {
    mprintf("\tMincut: %6D, NBND: %6D, NPwgts: ", mincut, nbnd);
    for (l=0; l<ncon; l++)
      mprintf("(%.3f, %.3f) ", npwgts[l], npwgts[ncon+l]);
    mprintf(", LB: %.3f\n", Compute2WayHLoadImbalance(ncon, npwgts, tpwgts));
  }

  graph->mincut = mincut;
  graph->nbnd = nbnd;

  for (i=0; i<ncon; i++) {
    PQueueFree(ctrl, &parts[i][0]);
    PQueueFree(ctrl, &parts[i][1]);
  }

  ASSERT(ComputeCut(graph, where) == graph->mincut);
  ASSERT(CheckBnd(graph));

  idxwspacefree(ctrl, nvtxs);
  idxwspacefree(ctrl, nvtxs);
}




/*************************************************************************
* This function selects the partition number and the queue from which
* we will move vertices out
**************************************************************************/ 
idxtype SelectQueueOneWay(idxtype ncon, float *npwgts, float *tpwgts, idxtype from, PQueueType queues[MAXNCON][2])
{
  idxtype i, cnum=-1;
  float max=0.0;

  for (i=0; i<ncon; i++) {
    if (npwgts[from*ncon+i]-tpwgts[from] >= max && 
        PQueueGetSize(&queues[i][0]) + PQueueGetSize(&queues[i][1]) > 0) {
      max = npwgts[from*ncon+i]-tpwgts[0];
      cnum = i;
    }
  }

  return cnum;
}

