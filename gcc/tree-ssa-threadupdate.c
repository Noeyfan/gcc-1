/* Thread edges through blocks and update the control flow and SSA graphs.
   Copyright (C) 2004-2015 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "input.h"
#include "alias.h"
#include "symtab.h"
#include "options.h"
#include "tree.h"
#include "fold-const.h"
#include "flags.h"
#include "predict.h"
#include "tm.h"
#include "hard-reg-set.h"
#include "input.h"
#include "function.h"
#include "dominance.h"
#include "cfg.h"
#include "cfganal.h"
#include "basic-block.h"
#include "tree-ssa-alias.h"
#include "internal-fn.h"
#include "gimple-expr.h"
#include "is-a.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimple-ssa.h"
#include "tree-phinodes.h"
#include "tree-ssa.h"
#include "tree-ssa-threadupdate.h"
#include "ssa-iterators.h"
#include "dumpfile.h"
#include "cfgloop.h"
#include "dbgcnt.h"
#include "tree-cfg.h"
#include "tree-pass.h"

/* Given a block B, update the CFG and SSA graph to reflect redirecting
   one or more in-edges to B to instead reach the destination of an
   out-edge from B while preserving any side effects in B.

   i.e., given A->B and B->C, change A->B to be A->C yet still preserve the
   side effects of executing B.

     1. Make a copy of B (including its outgoing edges and statements).  Call
	the copy B'.  Note B' has no incoming edges or PHIs at this time.

     2. Remove the control statement at the end of B' and all outgoing edges
	except B'->C.

     3. Add a new argument to each PHI in C with the same value as the existing
	argument associated with edge B->C.  Associate the new PHI arguments
	with the edge B'->C.

     4. For each PHI in B, find or create a PHI in B' with an identical
	PHI_RESULT.  Add an argument to the PHI in B' which has the same
	value as the PHI in B associated with the edge A->B.  Associate
	the new argument in the PHI in B' with the edge A->B.

     5. Change the edge A->B to A->B'.

	5a. This automatically deletes any PHI arguments associated with the
	    edge A->B in B.

	5b. This automatically associates each new argument added in step 4
	    with the edge A->B'.

     6. Repeat for other incoming edges into B.

     7. Put the duplicated resources in B and all the B' blocks into SSA form.

   Note that block duplication can be minimized by first collecting the
   set of unique destination blocks that the incoming edges should
   be threaded to.

   We reduce the number of edges and statements we create by not copying all
   the outgoing edges and the control statement in step #1.  We instead create
   a template block without the outgoing edges and duplicate the template.

   Another case this code handles is threading through a "joiner" block.  In
   this case, we do not know the destination of the joiner block, but one
   of the outgoing edges from the joiner block leads to a threadable path.  This
   case largely works as outlined above, except the duplicate of the joiner
   block still contains a full set of outgoing edges and its control statement.
   We just redirect one of its outgoing edges to our jump threading path.  */


/* Steps #5 and #6 of the above algorithm are best implemented by walking
   all the incoming edges which thread to the same destination edge at
   the same time.  That avoids lots of table lookups to get information
   for the destination edge.

   To realize that implementation we create a list of incoming edges
   which thread to the same outgoing edge.  Thus to implement steps
   #5 and #6 we traverse our hash table of outgoing edge information.
   For each entry we walk the list of incoming edges which thread to
   the current outgoing edge.  */

struct el
{
  edge e;
  struct el *next;
};

/* Main data structure recording information regarding B's duplicate
   blocks.  */

/* We need to efficiently record the unique thread destinations of this
   block and specific information associated with those destinations.  We
   may have many incoming edges threaded to the same outgoing edge.  This
   can be naturally implemented with a hash table.  */

struct redirection_data : typed_free_remove<redirection_data>
{
  /* We support wiring up two block duplicates in a jump threading path.

     One is a normal block copy where we remove the control statement
     and wire up its single remaining outgoing edge to the thread path.

     The other is a joiner block where we leave the control statement
     in place, but wire one of the outgoing edges to a thread path.

     In theory we could have multiple block duplicates in a jump
     threading path, but I haven't tried that.

     The duplicate blocks appear in this array in the same order in
     which they appear in the jump thread path.  */
  basic_block dup_blocks[2];

  /* The jump threading path.  */
  vec<jump_thread_edge *> *path;

  /* A list of incoming edges which we want to thread to the
     same path.  */
  struct el *incoming_edges;

  /* hash_table support.  */
  typedef redirection_data *value_type;
  typedef redirection_data *compare_type;
  static inline hashval_t hash (const redirection_data *);
  static inline int equal (const redirection_data *, const redirection_data *);
};

/* Dump a jump threading path, including annotations about each
   edge in the path.  */

static void
dump_jump_thread_path (FILE *dump_file, vec<jump_thread_edge *> path,
		       bool registering)
{
  fprintf (dump_file,
	   "  %s%s jump thread: (%d, %d) incoming edge; ",
	   (registering ? "Registering" : "Cancelling"),
	   (path[0]->type == EDGE_FSM_THREAD ? " FSM": ""),
	   path[0]->e->src->index, path[0]->e->dest->index);

  for (unsigned int i = 1; i < path.length (); i++)
    {
      /* We can get paths with a NULL edge when the final destination
	 of a jump thread turns out to be a constant address.  We dump
	 those paths when debugging, so we have to be prepared for that
	 possibility here.  */
      if (path[i]->e == NULL)
	continue;

      if (path[i]->type == EDGE_COPY_SRC_JOINER_BLOCK)
	fprintf (dump_file, " (%d, %d) joiner; ",
		 path[i]->e->src->index, path[i]->e->dest->index);
      if (path[i]->type == EDGE_COPY_SRC_BLOCK)
       fprintf (dump_file, " (%d, %d) normal;",
		 path[i]->e->src->index, path[i]->e->dest->index);
      if (path[i]->type == EDGE_NO_COPY_SRC_BLOCK)
       fprintf (dump_file, " (%d, %d) nocopy;",
		 path[i]->e->src->index, path[i]->e->dest->index);
      if (path[0]->type == EDGE_FSM_THREAD)
	fprintf (dump_file, " (%d, %d) ",
		 path[i]->e->src->index, path[i]->e->dest->index);
    }
  fputc ('\n', dump_file);
}

/* Simple hashing function.  For any given incoming edge E, we're going
   to be most concerned with the final destination of its jump thread
   path.  So hash on the block index of the final edge in the path.  */

inline hashval_t
redirection_data::hash (const redirection_data *p)
{
  vec<jump_thread_edge *> *path = p->path;
  return path->last ()->e->dest->index;
}

/* Given two hash table entries, return true if they have the same
   jump threading path.  */
inline int
redirection_data::equal (const redirection_data *p1, const redirection_data *p2)
{
  vec<jump_thread_edge *> *path1 = p1->path;
  vec<jump_thread_edge *> *path2 = p2->path;

  if (path1->length () != path2->length ())
    return false;

  for (unsigned int i = 1; i < path1->length (); i++)
    {
      if ((*path1)[i]->type != (*path2)[i]->type
	  || (*path1)[i]->e != (*path2)[i]->e)
	return false;
    }

  return true;
}

/* Data structure of information to pass to hash table traversal routines.  */
struct ssa_local_info_t
{
  /* The current block we are working on.  */
  basic_block bb;

  /* We only create a template block for the first duplicated block in a
     jump threading path as we may need many duplicates of that block.

     The second duplicate block in a path is specific to that path.  Creating
     and sharing a template for that block is considerably more difficult.  */
  basic_block template_block;

  /* TRUE if we thread one or more jumps, FALSE otherwise.  */
  bool jumps_threaded;

  /* Blocks duplicated for the thread.  */
  bitmap duplicate_blocks;
};

/* Passes which use the jump threading code register jump threading
   opportunities as they are discovered.  We keep the registered
   jump threading opportunities in this vector as edge pairs
   (original_edge, target_edge).  */
static vec<vec<jump_thread_edge *> *> paths;

/* When we start updating the CFG for threading, data necessary for jump
   threading is attached to the AUX field for the incoming edge.  Use these
   macros to access the underlying structure attached to the AUX field.  */
#define THREAD_PATH(E) ((vec<jump_thread_edge *> *)(E)->aux)

/* Jump threading statistics.  */

struct thread_stats_d
{
  unsigned long num_threaded_edges;
};

struct thread_stats_d thread_stats;


/* Remove the last statement in block BB if it is a control statement
   Also remove all outgoing edges except the edge which reaches DEST_BB.
   If DEST_BB is NULL, then remove all outgoing edges.  */

static void
remove_ctrl_stmt_and_useless_edges (basic_block bb, basic_block dest_bb)
{
  gimple_stmt_iterator gsi;
  edge e;
  edge_iterator ei;

  gsi = gsi_last_bb (bb);

  /* If the duplicate ends with a control statement, then remove it.

     Note that if we are duplicating the template block rather than the
     original basic block, then the duplicate might not have any real
     statements in it.  */
  if (!gsi_end_p (gsi)
      && gsi_stmt (gsi)
      && (gimple_code (gsi_stmt (gsi)) == GIMPLE_COND
	  || gimple_code (gsi_stmt (gsi)) == GIMPLE_GOTO
	  || gimple_code (gsi_stmt (gsi)) == GIMPLE_SWITCH))
    gsi_remove (&gsi, true);

  for (ei = ei_start (bb->succs); (e = ei_safe_edge (ei)); )
    {
      if (e->dest != dest_bb)
	remove_edge (e);
      else
	ei_next (&ei);
    }
}

/* Create a duplicate of BB.  Record the duplicate block in an array
   indexed by COUNT stored in RD.  */

static void
create_block_for_threading (basic_block bb,
			    struct redirection_data *rd,
			    unsigned int count,
			    bitmap *duplicate_blocks)
{
  edge_iterator ei;
  edge e;

  /* We can use the generic block duplication code and simply remove
     the stuff we do not need.  */
  rd->dup_blocks[count] = duplicate_block (bb, NULL, NULL);

  FOR_EACH_EDGE (e, ei, rd->dup_blocks[count]->succs)
    e->aux = NULL;

  /* Zero out the profile, since the block is unreachable for now.  */
  rd->dup_blocks[count]->frequency = 0;
  rd->dup_blocks[count]->count = 0;
  if (duplicate_blocks)
    bitmap_set_bit (*duplicate_blocks, rd->dup_blocks[count]->index);
}

/* Main data structure to hold information for duplicates of BB.  */

static hash_table<redirection_data> *redirection_data;

/* Given an outgoing edge E lookup and return its entry in our hash table.

   If INSERT is true, then we insert the entry into the hash table if
   it is not already present.  INCOMING_EDGE is added to the list of incoming
   edges associated with E in the hash table.  */

static struct redirection_data *
lookup_redirection_data (edge e, enum insert_option insert)
{
  struct redirection_data **slot;
  struct redirection_data *elt;
  vec<jump_thread_edge *> *path = THREAD_PATH (e);

 /* Build a hash table element so we can see if E is already
     in the table.  */
  elt = XNEW (struct redirection_data);
  elt->path = path;
  elt->dup_blocks[0] = NULL;
  elt->dup_blocks[1] = NULL;
  elt->incoming_edges = NULL;

  slot = redirection_data->find_slot (elt, insert);

  /* This will only happen if INSERT is false and the entry is not
     in the hash table.  */
  if (slot == NULL)
    {
      free (elt);
      return NULL;
    }

  /* This will only happen if E was not in the hash table and
     INSERT is true.  */
  if (*slot == NULL)
    {
      *slot = elt;
      elt->incoming_edges = XNEW (struct el);
      elt->incoming_edges->e = e;
      elt->incoming_edges->next = NULL;
      return elt;
    }
  /* E was in the hash table.  */
  else
    {
      /* Free ELT as we do not need it anymore, we will extract the
	 relevant entry from the hash table itself.  */
      free (elt);

      /* Get the entry stored in the hash table.  */
      elt = *slot;

      /* If insertion was requested, then we need to add INCOMING_EDGE
	 to the list of incoming edges associated with E.  */
      if (insert)
	{
	  struct el *el = XNEW (struct el);
	  el->next = elt->incoming_edges;
	  el->e = e;
	  elt->incoming_edges = el;
	}

      return elt;
    }
}

/* Similar to copy_phi_args, except that the PHI arg exists, it just
   does not have a value associated with it.  */

static void
copy_phi_arg_into_existing_phi (edge src_e, edge tgt_e)
{
  int src_idx = src_e->dest_idx;
  int tgt_idx = tgt_e->dest_idx;

  /* Iterate over each PHI in e->dest.  */
  for (gphi_iterator gsi = gsi_start_phis (src_e->dest),
			   gsi2 = gsi_start_phis (tgt_e->dest);
       !gsi_end_p (gsi);
       gsi_next (&gsi), gsi_next (&gsi2))
    {
      gphi *src_phi = gsi.phi ();
      gphi *dest_phi = gsi2.phi ();
      tree val = gimple_phi_arg_def (src_phi, src_idx);
      source_location locus = gimple_phi_arg_location (src_phi, src_idx);

      SET_PHI_ARG_DEF (dest_phi, tgt_idx, val);
      gimple_phi_arg_set_location (dest_phi, tgt_idx, locus);
    }
}

/* Given ssa_name DEF, backtrack jump threading PATH from node IDX
   to see if it has constant value in a flow sensitive manner.  Set
   LOCUS to location of the constant phi arg and return the value.
   Return DEF directly if either PATH or idx is ZERO.  */

static tree
get_value_locus_in_path (tree def, vec<jump_thread_edge *> *path,
			 basic_block bb, int idx, source_location *locus)
{
  tree arg;
  gphi *def_phi;
  basic_block def_bb;

  if (path == NULL || idx == 0)
    return def;

  def_phi = dyn_cast <gphi *> (SSA_NAME_DEF_STMT (def));
  if (!def_phi)
    return def;

  def_bb = gimple_bb (def_phi);
  /* Don't propagate loop invariants into deeper loops.  */
  if (!def_bb || bb_loop_depth (def_bb) < bb_loop_depth (bb))
    return def;

  /* Backtrack jump threading path from IDX to see if def has constant
     value.  */
  for (int j = idx - 1; j >= 0; j--)
    {
      edge e = (*path)[j]->e;
      if (e->dest == def_bb)
	{
	  arg = gimple_phi_arg_def (def_phi, e->dest_idx);
	  if (is_gimple_min_invariant (arg))
	    {
	      *locus = gimple_phi_arg_location (def_phi, e->dest_idx);
	      return arg;
	    }
	  break;
	}
    }

  return def;
}

/* For each PHI in BB, copy the argument associated with SRC_E to TGT_E.
   Try to backtrack jump threading PATH from node IDX to see if the arg
   has constant value, copy constant value instead of argument itself
   if yes.  */

static void
copy_phi_args (basic_block bb, edge src_e, edge tgt_e,
	       vec<jump_thread_edge *> *path, int idx)
{
  gphi_iterator gsi;
  int src_indx = src_e->dest_idx;

  for (gsi = gsi_start_phis (bb); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      gphi *phi = gsi.phi ();
      tree def = gimple_phi_arg_def (phi, src_indx);
      source_location locus = gimple_phi_arg_location (phi, src_indx);

      if (TREE_CODE (def) == SSA_NAME
	  && !virtual_operand_p (gimple_phi_result (phi)))
	def = get_value_locus_in_path (def, path, bb, idx, &locus);

      add_phi_arg (phi, def, tgt_e, locus);
    }
}

/* We have recently made a copy of ORIG_BB, including its outgoing
   edges.  The copy is NEW_BB.  Every PHI node in every direct successor of
   ORIG_BB has a new argument associated with edge from NEW_BB to the
   successor.  Initialize the PHI argument so that it is equal to the PHI
   argument associated with the edge from ORIG_BB to the successor.
   PATH and IDX are used to check if the new PHI argument has constant
   value in a flow sensitive manner.  */

static void
update_destination_phis (basic_block orig_bb, basic_block new_bb,
			 vec<jump_thread_edge *> *path, int idx)
{
  edge_iterator ei;
  edge e;

  FOR_EACH_EDGE (e, ei, orig_bb->succs)
    {
      edge e2 = find_edge (new_bb, e->dest);
      copy_phi_args (e->dest, e, e2, path, idx);
    }
}

/* Given a duplicate block and its single destination (both stored
   in RD).  Create an edge between the duplicate and its single
   destination.

   Add an additional argument to any PHI nodes at the single
   destination.  IDX is the start node in jump threading path
   we start to check to see if the new PHI argument has constant
   value along the jump threading path.  */

static void
create_edge_and_update_destination_phis (struct redirection_data *rd,
					 basic_block bb, int idx)
{
  edge e = make_edge (bb, rd->path->last ()->e->dest, EDGE_FALLTHRU);

  rescan_loop_exit (e, true, false);
  e->probability = REG_BR_PROB_BASE;
  e->count = bb->count;

  /* We used to copy the thread path here.  That was added in 2007
     and dutifully updated through the representation changes in 2013.

     In 2013 we added code to thread from an interior node through
     the backedge to another interior node.  That runs after the code
     to thread through loop headers from outside the loop.

     The latter may delete edges in the CFG, including those
     which appeared in the jump threading path we copied here.  Thus
     we'd end up using a dangling pointer.

     After reviewing the 2007/2011 code, I can't see how anything
     depended on copying the AUX field and clearly copying the jump
     threading path is problematical due to embedded edge pointers.
     It has been removed.  */
  e->aux = NULL;

  /* If there are any PHI nodes at the destination of the outgoing edge
     from the duplicate block, then we will need to add a new argument
     to them.  The argument should have the same value as the argument
     associated with the outgoing edge stored in RD.  */
  copy_phi_args (e->dest, rd->path->last ()->e, e, rd->path, idx);
}

/* Look through PATH beginning at START and return TRUE if there are
   any additional blocks that need to be duplicated.  Otherwise,
   return FALSE.  */
static bool
any_remaining_duplicated_blocks (vec<jump_thread_edge *> *path,
				 unsigned int start)
{
  for (unsigned int i = start + 1; i < path->length (); i++)
    {
      if ((*path)[i]->type == EDGE_COPY_SRC_JOINER_BLOCK
	  || (*path)[i]->type == EDGE_COPY_SRC_BLOCK)
	return true;
    }
  return false;
}


/* Compute the amount of profile count/frequency coming into the jump threading
   path stored in RD that we are duplicating, returned in PATH_IN_COUNT_PTR and
   PATH_IN_FREQ_PTR, as well as the amount of counts flowing out of the
   duplicated path, returned in PATH_OUT_COUNT_PTR.  LOCAL_INFO is used to
   identify blocks duplicated for jump threading, which have duplicated
   edges that need to be ignored in the analysis.  Return true if path contains
   a joiner, false otherwise.

   In the non-joiner case, this is straightforward - all the counts/frequency
   flowing into the jump threading path should flow through the duplicated
   block and out of the duplicated path.

   In the joiner case, it is very tricky.  Some of the counts flowing into
   the original path go offpath at the joiner.  The problem is that while
   we know how much total count goes off-path in the original control flow,
   we don't know how many of the counts corresponding to just the jump
   threading path go offpath at the joiner.

   For example, assume we have the following control flow and identified
   jump threading paths:

		A     B     C
		 \    |    /
	       Ea \   |Eb / Ec
		   \  |  /
		    v v v
		      J       <-- Joiner
		     / \
		Eoff/   \Eon
		   /     \
		  v       v
		Soff     Son  <--- Normal
			 /\
		      Ed/  \ Ee
		       /    \
		      v     v
		      D      E

	    Jump threading paths: A -> J -> Son -> D (path 1)
				  C -> J -> Son -> E (path 2)

   Note that the control flow could be more complicated:
   - Each jump threading path may have more than one incoming edge.  I.e. A and
   Ea could represent multiple incoming blocks/edges that are included in
   path 1.
   - There could be EDGE_NO_COPY_SRC_BLOCK edges after the joiner (either
   before or after the "normal" copy block).  These are not duplicated onto
   the jump threading path, as they are single-successor.
   - Any of the blocks along the path may have other incoming edges that
   are not part of any jump threading path, but add profile counts along
   the path.

   In the aboe example, after all jump threading is complete, we will
   end up with the following control flow:

		A	  B	    C
		|	  |	    |
	      Ea|	  |Eb	  |Ec
		|	  |	    |
		v	  v	    v
	       Ja	  J	   Jc
	       / \	/ \Eon'     / \
	  Eona/   \   ---/---\--------   \Eonc
	     /     \ /  /     \	   \
	    v       v  v       v	  v
	   Sona     Soff      Son	Sonc
	     \		 /\	 /
	      \___________    /  \  _____/
			  \  /    \/
			   vv      v
			    D      E

   The main issue to notice here is that when we are processing path 1
   (A->J->Son->D) we need to figure out the outgoing edge weights to
   the duplicated edges Ja->Sona and Ja->Soff, while ensuring that the
   sum of the incoming weights to D remain Ed.  The problem with simply
   assuming that Ja (and Jc when processing path 2) has the same outgoing
   probabilities to its successors as the original block J, is that after
   all paths are processed and other edges/counts removed (e.g. none
   of Ec will reach D after processing path 2), we may end up with not
   enough count flowing along duplicated edge Sona->D.

   Therefore, in the case of a joiner, we keep track of all counts
   coming in along the current path, as well as from predecessors not
   on any jump threading path (Eb in the above example).  While we
   first assume that the duplicated Eona for Ja->Sona has the same
   probability as the original, we later compensate for other jump
   threading paths that may eliminate edges.  We do that by keep track
   of all counts coming into the original path that are not in a jump
   thread (Eb in the above example, but as noted earlier, there could
   be other predecessors incoming to the path at various points, such
   as at Son).  Call this cumulative non-path count coming into the path
   before D as Enonpath.  We then ensure that the count from Sona->D is as at
   least as big as (Ed - Enonpath), but no bigger than the minimum
   weight along the jump threading path.  The probabilities of both the
   original and duplicated joiner block J and Ja will be adjusted
   accordingly after the updates.  */

static bool
compute_path_counts (struct redirection_data *rd,
		     ssa_local_info_t *local_info,
		     gcov_type *path_in_count_ptr,
		     gcov_type *path_out_count_ptr,
		     int *path_in_freq_ptr)
{
  edge e = rd->incoming_edges->e;
  vec<jump_thread_edge *> *path = THREAD_PATH (e);
  edge elast = path->last ()->e;
  gcov_type nonpath_count = 0;
  bool has_joiner = false;
  gcov_type path_in_count = 0;
  int path_in_freq = 0;

  /* Start by accumulating incoming edge counts to the path's first bb
     into a couple buckets:
	path_in_count: total count of incoming edges that flow into the
		  current path.
	nonpath_count: total count of incoming edges that are not
		  flowing along *any* path.  These are the counts
		  that will still flow along the original path after
		  all path duplication is done by potentially multiple
		  calls to this routine.
     (any other incoming edge counts are for a different jump threading
     path that will be handled by a later call to this routine.)
     To make this easier, start by recording all incoming edges that flow into
     the current path in a bitmap.  We could add up the path's incoming edge
     counts here, but we still need to walk all the first bb's incoming edges
     below to add up the counts of the other edges not included in this jump
     threading path.  */
  struct el *next, *el;
  bitmap in_edge_srcs = BITMAP_ALLOC (NULL);
  for (el = rd->incoming_edges; el; el = next)
    {
      next = el->next;
      bitmap_set_bit (in_edge_srcs, el->e->src->index);
    }
  edge ein;
  edge_iterator ei;
  FOR_EACH_EDGE (ein, ei, e->dest->preds)
    {
      vec<jump_thread_edge *> *ein_path = THREAD_PATH (ein);
      /* Simply check the incoming edge src against the set captured above.  */
      if (ein_path
	  && bitmap_bit_p (in_edge_srcs, (*ein_path)[0]->e->src->index))
	{
	  /* It is necessary but not sufficient that the last path edges
	     are identical.  There may be different paths that share the
	     same last path edge in the case where the last edge has a nocopy
	     source block.  */
	  gcc_assert (ein_path->last ()->e == elast);
	  path_in_count += ein->count;
	  path_in_freq += EDGE_FREQUENCY (ein);
	}
      else if (!ein_path)
	{
	  /* Keep track of the incoming edges that are not on any jump-threading
	     path.  These counts will still flow out of original path after all
	     jump threading is complete.  */
	    nonpath_count += ein->count;
	}
    }

  /* This is needed due to insane incoming frequencies.  */
  if (path_in_freq > BB_FREQ_MAX)
    path_in_freq = BB_FREQ_MAX;

  BITMAP_FREE (in_edge_srcs);

  /* Now compute the fraction of the total count coming into the first
     path bb that is from the current threading path.  */
  gcov_type total_count = e->dest->count;
  /* Handle incoming profile insanities.  */
  if (total_count < path_in_count)
    path_in_count = total_count;
  int onpath_scale = GCOV_COMPUTE_SCALE (path_in_count, total_count);

  /* Walk the entire path to do some more computation in order to estimate
     how much of the path_in_count will flow out of the duplicated threading
     path.  In the non-joiner case this is straightforward (it should be
     the same as path_in_count, although we will handle incoming profile
     insanities by setting it equal to the minimum count along the path).

     In the joiner case, we need to estimate how much of the path_in_count
     will stay on the threading path after the joiner's conditional branch.
     We don't really know for sure how much of the counts
     associated with this path go to each successor of the joiner, but we'll
     estimate based on the fraction of the total count coming into the path
     bb was from the threading paths (computed above in onpath_scale).
     Afterwards, we will need to do some fixup to account for other threading
     paths and possible profile insanities.

     In order to estimate the joiner case's counts we also need to update
     nonpath_count with any additional counts coming into the path.  Other
     blocks along the path may have additional predecessors from outside
     the path.  */
  gcov_type path_out_count = path_in_count;
  gcov_type min_path_count = path_in_count;
  for (unsigned int i = 1; i < path->length (); i++)
    {
      edge epath = (*path)[i]->e;
      gcov_type cur_count = epath->count;
      if ((*path)[i]->type == EDGE_COPY_SRC_JOINER_BLOCK)
	{
	  has_joiner = true;
	  cur_count = apply_probability (cur_count, onpath_scale);
	}
      /* In the joiner case we need to update nonpath_count for any edges
	 coming into the path that will contribute to the count flowing
	 into the path successor.  */
      if (has_joiner && epath != elast)
      {
	/* Look for other incoming edges after joiner.  */
	FOR_EACH_EDGE (ein, ei, epath->dest->preds)
	  {
	    if (ein != epath
		/* Ignore in edges from blocks we have duplicated for a
		   threading path, which have duplicated edge counts until
		   they are redirected by an invocation of this routine.  */
		&& !bitmap_bit_p (local_info->duplicate_blocks,
				  ein->src->index))
	      nonpath_count += ein->count;
	  }
      }
      if (cur_count < path_out_count)
	path_out_count = cur_count;
      if (epath->count < min_path_count)
	min_path_count = epath->count;
    }

  /* We computed path_out_count above assuming that this path targeted
     the joiner's on-path successor with the same likelihood as it
     reached the joiner.  However, other thread paths through the joiner
     may take a different path through the normal copy source block
     (i.e. they have a different elast), meaning that they do not
     contribute any counts to this path's elast.  As a result, it may
     turn out that this path must have more count flowing to the on-path
     successor of the joiner.  Essentially, all of this path's elast
     count must be contributed by this path and any nonpath counts
     (since any path through the joiner with a different elast will not
     include a copy of this elast in its duplicated path).
     So ensure that this path's path_out_count is at least the
     difference between elast->count and nonpath_count.  Otherwise the edge
     counts after threading will not be sane.  */
  if (has_joiner && path_out_count < elast->count - nonpath_count)
  {
    path_out_count = elast->count - nonpath_count;
    /* But neither can we go above the minimum count along the path
       we are duplicating.  This can be an issue due to profile
       insanities coming in to this pass.  */
    if (path_out_count > min_path_count)
      path_out_count = min_path_count;
  }

  *path_in_count_ptr = path_in_count;
  *path_out_count_ptr = path_out_count;
  *path_in_freq_ptr = path_in_freq;
  return has_joiner;
}


/* Update the counts and frequencies for both an original path
   edge EPATH and its duplicate EDUP.  The duplicate source block
   will get a count/frequency of PATH_IN_COUNT and PATH_IN_FREQ,
   and the duplicate edge EDUP will have a count of PATH_OUT_COUNT.  */
static void
update_profile (edge epath, edge edup, gcov_type path_in_count,
		gcov_type path_out_count, int path_in_freq)
{

  /* First update the duplicated block's count / frequency.  */
  if (edup)
    {
      basic_block dup_block = edup->src;
      gcc_assert (dup_block->count == 0);
      gcc_assert (dup_block->frequency == 0);
      dup_block->count = path_in_count;
      dup_block->frequency = path_in_freq;
    }

  /* Now update the original block's count and frequency in the
     opposite manner - remove the counts/freq that will flow
     into the duplicated block.  Handle underflow due to precision/
     rounding issues.  */
  epath->src->count -= path_in_count;
  if (epath->src->count < 0)
    epath->src->count = 0;
  epath->src->frequency -= path_in_freq;
  if (epath->src->frequency < 0)
    epath->src->frequency = 0;

  /* Next update this path edge's original and duplicated counts.  We know
     that the duplicated path will have path_out_count flowing
     out of it (in the joiner case this is the count along the duplicated path
     out of the duplicated joiner).  This count can then be removed from the
     original path edge.  */
  if (edup)
    edup->count = path_out_count;
  epath->count -= path_out_count;
  gcc_assert (epath->count >= 0);
}


/* The duplicate and original joiner blocks may end up with different
   probabilities (different from both the original and from each other).
   Recompute the probabilities here once we have updated the edge
   counts and frequencies.  */

static void
recompute_probabilities (basic_block bb)
{
  edge esucc;
  edge_iterator ei;
  FOR_EACH_EDGE (esucc, ei, bb->succs)
    {
      if (!bb->count)
	continue;

      /* Prevent overflow computation due to insane profiles.  */
      if (esucc->count < bb->count)
	esucc->probability = GCOV_COMPUTE_SCALE (esucc->count,
						 bb->count);
      else
	/* Can happen with missing/guessed probabilities, since we
	   may determine that more is flowing along duplicated
	   path than joiner succ probabilities allowed.
	   Counts and freqs will be insane after jump threading,
	   at least make sure probability is sane or we will
	   get a flow verification error.
	   Not much we can do to make counts/freqs sane without
	   redoing the profile estimation.  */
	esucc->probability = REG_BR_PROB_BASE;
    }
}


/* Update the counts of the original and duplicated edges from a joiner
   that go off path, given that we have already determined that the
   duplicate joiner DUP_BB has incoming count PATH_IN_COUNT and
   outgoing count along the path PATH_OUT_COUNT.  The original (on-)path
   edge from joiner is EPATH.  */

static void
update_joiner_offpath_counts (edge epath, basic_block dup_bb,
			      gcov_type path_in_count,
			      gcov_type path_out_count)
{
  /* Compute the count that currently flows off path from the joiner.
     In other words, the total count of joiner's out edges other than
     epath.  Compute this by walking the successors instead of
     subtracting epath's count from the joiner bb count, since there
     are sometimes slight insanities where the total out edge count is
     larger than the bb count (possibly due to rounding/truncation
     errors).  */
  gcov_type total_orig_off_path_count = 0;
  edge enonpath;
  edge_iterator ei;
  FOR_EACH_EDGE (enonpath, ei, epath->src->succs)
    {
      if (enonpath == epath)
	continue;
      total_orig_off_path_count += enonpath->count;
    }

  /* For the path that we are duplicating, the amount that will flow
     off path from the duplicated joiner is the delta between the
     path's cumulative in count and the portion of that count we
     estimated above as flowing from the joiner along the duplicated
     path.  */
  gcov_type total_dup_off_path_count = path_in_count - path_out_count;

  /* Now do the actual updates of the off-path edges.  */
  FOR_EACH_EDGE (enonpath, ei, epath->src->succs)
    {
      /* Look for edges going off of the threading path.  */
      if (enonpath == epath)
	continue;

      /* Find the corresponding edge out of the duplicated joiner.  */
      edge enonpathdup = find_edge (dup_bb, enonpath->dest);
      gcc_assert (enonpathdup);

      /* We can't use the original probability of the joiner's out
	 edges, since the probabilities of the original branch
	 and the duplicated branches may vary after all threading is
	 complete.  But apportion the duplicated joiner's off-path
	 total edge count computed earlier (total_dup_off_path_count)
	 among the duplicated off-path edges based on their original
	 ratio to the full off-path count (total_orig_off_path_count).
	 */
      int scale = GCOV_COMPUTE_SCALE (enonpath->count,
				      total_orig_off_path_count);
      /* Give the duplicated offpath edge a portion of the duplicated
	 total.  */
      enonpathdup->count = apply_scale (scale,
					total_dup_off_path_count);
      /* Now update the original offpath edge count, handling underflow
	 due to rounding errors.  */
      enonpath->count -= enonpathdup->count;
      if (enonpath->count < 0)
	enonpath->count = 0;
    }
}


/* Check if the paths through RD all have estimated frequencies but zero
   profile counts.  This is more accurate than checking the entry block
   for a zero profile count, since profile insanities sometimes creep in.  */

static bool
estimated_freqs_path (struct redirection_data *rd)
{
  edge e = rd->incoming_edges->e;
  vec<jump_thread_edge *> *path = THREAD_PATH (e);
  edge ein;
  edge_iterator ei;
  bool non_zero_freq = false;
  FOR_EACH_EDGE (ein, ei, e->dest->preds)
    {
      if (ein->count)
	return false;
      non_zero_freq |= ein->src->frequency != 0;
    }

  for (unsigned int i = 1; i < path->length (); i++)
    {
      edge epath = (*path)[i]->e;
      if (epath->src->count)
	return false;
      non_zero_freq |= epath->src->frequency != 0;
      edge esucc;
      FOR_EACH_EDGE (esucc, ei, epath->src->succs)
	{
	  if (esucc->count)
	    return false;
	  non_zero_freq |= esucc->src->frequency != 0;
	}
    }
  return non_zero_freq;
}


/* Invoked for routines that have guessed frequencies and no profile
   counts to record the block and edge frequencies for paths through RD
   in the profile count fields of those blocks and edges.  This is because
   ssa_fix_duplicate_block_edges incrementally updates the block and
   edge counts as edges are redirected, and it is difficult to do that
   for edge frequencies which are computed on the fly from the source
   block frequency and probability.  When a block frequency is updated
   its outgoing edge frequencies are affected and become difficult to
   adjust.  */

static void
freqs_to_counts_path (struct redirection_data *rd)
{
  edge e = rd->incoming_edges->e;
  vec<jump_thread_edge *> *path = THREAD_PATH (e);
  edge ein;
  edge_iterator ei;
  FOR_EACH_EDGE (ein, ei, e->dest->preds)
    {
      /* Scale up the frequency by REG_BR_PROB_BASE, to avoid rounding
	 errors applying the probability when the frequencies are very
	 small.  */
      ein->count = apply_probability (ein->src->frequency * REG_BR_PROB_BASE,
				      ein->probability);
    }

  for (unsigned int i = 1; i < path->length (); i++)
    {
      edge epath = (*path)[i]->e;
      edge esucc;
      /* Scale up the frequency by REG_BR_PROB_BASE, to avoid rounding
	 errors applying the edge probability when the frequencies are very
	 small.  */
      epath->src->count = epath->src->frequency * REG_BR_PROB_BASE;
      FOR_EACH_EDGE (esucc, ei, epath->src->succs)
	esucc->count = apply_probability (esucc->src->count,
					  esucc->probability);
    }
}


/* For routines that have guessed frequencies and no profile counts, where we
   used freqs_to_counts_path to record block and edge frequencies for paths
   through RD, we clear the counts after completing all updates for RD.
   The updates in ssa_fix_duplicate_block_edges are based off the count fields,
   but the block frequencies and edge probabilities were updated as well,
   so we can simply clear the count fields.  */

static void
clear_counts_path (struct redirection_data *rd)
{
  edge e = rd->incoming_edges->e;
  vec<jump_thread_edge *> *path = THREAD_PATH (e);
  edge ein, esucc;
  edge_iterator ei;
  FOR_EACH_EDGE (ein, ei, e->dest->preds)
    ein->count = 0;

  /* First clear counts along original path.  */
  for (unsigned int i = 1; i < path->length (); i++)
    {
      edge epath = (*path)[i]->e;
      FOR_EACH_EDGE (esucc, ei, epath->src->succs)
	esucc->count = 0;
      epath->src->count = 0;
    }
  /* Also need to clear the counts along duplicated path.  */
  for (unsigned int i = 0; i < 2; i++)
    {
      basic_block dup = rd->dup_blocks[i];
      if (!dup)
	continue;
      FOR_EACH_EDGE (esucc, ei, dup->succs)
	esucc->count = 0;
      dup->count = 0;
    }
}

/* Wire up the outgoing edges from the duplicate blocks and
   update any PHIs as needed.  Also update the profile counts
   on the original and duplicate blocks and edges.  */
void
ssa_fix_duplicate_block_edges (struct redirection_data *rd,
			       ssa_local_info_t *local_info)
{
  bool multi_incomings = (rd->incoming_edges->next != NULL);
  edge e = rd->incoming_edges->e;
  vec<jump_thread_edge *> *path = THREAD_PATH (e);
  edge elast = path->last ()->e;
  gcov_type path_in_count = 0;
  gcov_type path_out_count = 0;
  int path_in_freq = 0;

  /* This routine updates profile counts, frequencies, and probabilities
     incrementally. Since it is difficult to do the incremental updates
     using frequencies/probabilities alone, for routines without profile
     data we first take a snapshot of the existing block and edge frequencies
     by copying them into the empty profile count fields.  These counts are
     then used to do the incremental updates, and cleared at the end of this
     routine.  If the function is marked as having a profile, we still check
     to see if the paths through RD are using estimated frequencies because
     the routine had zero profile counts.  */
  bool do_freqs_to_counts = (profile_status_for_fn (cfun) != PROFILE_READ
			     || estimated_freqs_path (rd));
  if (do_freqs_to_counts)
    freqs_to_counts_path (rd);

  /* First determine how much profile count to move from original
     path to the duplicate path.  This is tricky in the presence of
     a joiner (see comments for compute_path_counts), where some portion
     of the path's counts will flow off-path from the joiner.  In the
     non-joiner case the path_in_count and path_out_count should be the
     same.  */
  bool has_joiner = compute_path_counts (rd, local_info,
					 &path_in_count, &path_out_count,
					 &path_in_freq);

  int cur_path_freq = path_in_freq;
  for (unsigned int count = 0, i = 1; i < path->length (); i++)
    {
      edge epath = (*path)[i]->e;

      /* If we were threading through an joiner block, then we want
	 to keep its control statement and redirect an outgoing edge.
	 Else we want to remove the control statement & edges, then create
	 a new outgoing edge.  In both cases we may need to update PHIs.  */
      if ((*path)[i]->type == EDGE_COPY_SRC_JOINER_BLOCK)
	{
	  edge victim;
	  edge e2;

	  gcc_assert (has_joiner);

	  /* This updates the PHIs at the destination of the duplicate
	     block.  Pass 0 instead of i if we are threading a path which
	     has multiple incoming edges.  */
	  update_destination_phis (local_info->bb, rd->dup_blocks[count],
				   path, multi_incomings ? 0 : i);

	  /* Find the edge from the duplicate block to the block we're
	     threading through.  That's the edge we want to redirect.  */
	  victim = find_edge (rd->dup_blocks[count], (*path)[i]->e->dest);

	  /* If there are no remaining blocks on the path to duplicate,
	     then redirect VICTIM to the final destination of the jump
	     threading path.  */
	  if (!any_remaining_duplicated_blocks (path, i))
	    {
	      e2 = redirect_edge_and_branch (victim, elast->dest);
	      /* If we redirected the edge, then we need to copy PHI arguments
		 at the target.  If the edge already existed (e2 != victim
		 case), then the PHIs in the target already have the correct
		 arguments.  */
	      if (e2 == victim)
		copy_phi_args (e2->dest, elast, e2,
			       path, multi_incomings ? 0 : i);
	    }
	  else
	    {
	      /* Redirect VICTIM to the next duplicated block in the path.  */
	      e2 = redirect_edge_and_branch (victim, rd->dup_blocks[count + 1]);

	      /* We need to update the PHIs in the next duplicated block.  We
		 want the new PHI args to have the same value as they had
		 in the source of the next duplicate block.

		 Thus, we need to know which edge we traversed into the
		 source of the duplicate.  Furthermore, we may have
		 traversed many edges to reach the source of the duplicate.

		 Walk through the path starting at element I until we
		 hit an edge marked with EDGE_COPY_SRC_BLOCK.  We want
		 the edge from the prior element.  */
	      for (unsigned int j = i + 1; j < path->length (); j++)
		{
		  if ((*path)[j]->type == EDGE_COPY_SRC_BLOCK)
		    {
		      copy_phi_arg_into_existing_phi ((*path)[j - 1]->e, e2);
		      break;
		    }
		}
	    }

	  /* Update the counts and frequency of both the original block
	     and path edge, and the duplicates.  The path duplicate's
	     incoming count and frequency are the totals for all edges
	     incoming to this jump threading path computed earlier.
	     And we know that the duplicated path will have path_out_count
	     flowing out of it (i.e. along the duplicated path out of the
	     duplicated joiner).  */
	  update_profile (epath, e2, path_in_count, path_out_count,
			  path_in_freq);

	  /* Next we need to update the counts of the original and duplicated
	     edges from the joiner that go off path.  */
	  update_joiner_offpath_counts (epath, e2->src, path_in_count,
					path_out_count);

	  /* Finally, we need to set the probabilities on the duplicated
	     edges out of the duplicated joiner (e2->src).  The probabilities
	     along the original path will all be updated below after we finish
	     processing the whole path.  */
	  recompute_probabilities (e2->src);

	  /* Record the frequency flowing to the downstream duplicated
	     path blocks.  */
	  cur_path_freq = EDGE_FREQUENCY (e2);
	}
      else if ((*path)[i]->type == EDGE_COPY_SRC_BLOCK)
	{
	  remove_ctrl_stmt_and_useless_edges (rd->dup_blocks[count], NULL);
	  create_edge_and_update_destination_phis (rd, rd->dup_blocks[count],
						   multi_incomings ? 0 : i);
	  if (count == 1)
	    single_succ_edge (rd->dup_blocks[1])->aux = NULL;

	  /* Update the counts and frequency of both the original block
	     and path edge, and the duplicates.  Since we are now after
	     any joiner that may have existed on the path, the count
	     flowing along the duplicated threaded path is path_out_count.
	     If we didn't have a joiner, then cur_path_freq was the sum
	     of the total frequencies along all incoming edges to the
	     thread path (path_in_freq).  If we had a joiner, it would have
	     been updated at the end of that handling to the edge frequency
	     along the duplicated joiner path edge.  */
	  update_profile (epath, EDGE_SUCC (rd->dup_blocks[count], 0),
			  path_out_count, path_out_count,
			  cur_path_freq);
	}
      else
	{
	  /* No copy case.  In this case we don't have an equivalent block
	     on the duplicated thread path to update, but we do need
	     to remove the portion of the counts/freqs that were moved
	     to the duplicated path from the counts/freqs flowing through
	     this block on the original path.  Since all the no-copy edges
	     are after any joiner, the removed count is the same as
	     path_out_count.

	     If we didn't have a joiner, then cur_path_freq was the sum
	     of the total frequencies along all incoming edges to the
	     thread path (path_in_freq).  If we had a joiner, it would have
	     been updated at the end of that handling to the edge frequency
	     along the duplicated joiner path edge.  */
	     update_profile (epath, NULL, path_out_count, path_out_count,
			     cur_path_freq);
	}

      /* Increment the index into the duplicated path when we processed
	 a duplicated block.  */
      if ((*path)[i]->type == EDGE_COPY_SRC_JOINER_BLOCK
	  || (*path)[i]->type == EDGE_COPY_SRC_BLOCK)
      {
	  count++;
      }
    }

  /* Now walk orig blocks and update their probabilities, since the
     counts and freqs should be updated properly by above loop.  */
  for (unsigned int i = 1; i < path->length (); i++)
    {
      edge epath = (*path)[i]->e;
      recompute_probabilities (epath->src);
    }

  /* Done with all profile and frequency updates, clear counts if they
     were copied.  */
  if (do_freqs_to_counts)
    clear_counts_path (rd);
}

/* Hash table traversal callback routine to create duplicate blocks.  */

int
ssa_create_duplicates (struct redirection_data **slot,
		       ssa_local_info_t *local_info)
{
  struct redirection_data *rd = *slot;

  /* The second duplicated block in a jump threading path is specific
     to the path.  So it gets stored in RD rather than in LOCAL_DATA.

     Each time we're called, we have to look through the path and see
     if a second block needs to be duplicated.

     Note the search starts with the third edge on the path.  The first
     edge is the incoming edge, the second edge always has its source
     duplicated.  Thus we start our search with the third edge.  */
  vec<jump_thread_edge *> *path = rd->path;
  for (unsigned int i = 2; i < path->length (); i++)
    {
      if ((*path)[i]->type == EDGE_COPY_SRC_BLOCK
	  || (*path)[i]->type == EDGE_COPY_SRC_JOINER_BLOCK)
	{
	  create_block_for_threading ((*path)[i]->e->src, rd, 1,
				      &local_info->duplicate_blocks);
	  break;
	}
    }

  /* Create a template block if we have not done so already.  Otherwise
     use the template to create a new block.  */
  if (local_info->template_block == NULL)
    {
      create_block_for_threading ((*path)[1]->e->src, rd, 0,
				  &local_info->duplicate_blocks);
      local_info->template_block = rd->dup_blocks[0];

      /* We do not create any outgoing edges for the template.  We will
	 take care of that in a later traversal.  That way we do not
	 create edges that are going to just be deleted.  */
    }
  else
    {
      create_block_for_threading (local_info->template_block, rd, 0,
				  &local_info->duplicate_blocks);

      /* Go ahead and wire up outgoing edges and update PHIs for the duplicate
	 block.   */
      ssa_fix_duplicate_block_edges (rd, local_info);
    }

  /* Keep walking the hash table.  */
  return 1;
}

/* We did not create any outgoing edges for the template block during
   block creation.  This hash table traversal callback creates the
   outgoing edge for the template block.  */

inline int
ssa_fixup_template_block (struct redirection_data **slot,
			  ssa_local_info_t *local_info)
{
  struct redirection_data *rd = *slot;

  /* If this is the template block halt the traversal after updating
     it appropriately.

     If we were threading through an joiner block, then we want
     to keep its control statement and redirect an outgoing edge.
     Else we want to remove the control statement & edges, then create
     a new outgoing edge.  In both cases we may need to update PHIs.  */
  if (rd->dup_blocks[0] && rd->dup_blocks[0] == local_info->template_block)
    {
      ssa_fix_duplicate_block_edges (rd, local_info);
      return 0;
    }

  return 1;
}

/* Hash table traversal callback to redirect each incoming edge
   associated with this hash table element to its new destination.  */

int
ssa_redirect_edges (struct redirection_data **slot,
		    ssa_local_info_t *local_info)
{
  struct redirection_data *rd = *slot;
  struct el *next, *el;

  /* Walk over all the incoming edges associated associated with this
     hash table entry.  */
  for (el = rd->incoming_edges; el; el = next)
    {
      edge e = el->e;
      vec<jump_thread_edge *> *path = THREAD_PATH (e);

      /* Go ahead and free this element from the list.  Doing this now
	 avoids the need for another list walk when we destroy the hash
	 table.  */
      next = el->next;
      free (el);

      thread_stats.num_threaded_edges++;

      if (rd->dup_blocks[0])
	{
	  edge e2;

	  if (dump_file && (dump_flags & TDF_DETAILS))
	    fprintf (dump_file, "  Threaded jump %d --> %d to %d\n",
		     e->src->index, e->dest->index, rd->dup_blocks[0]->index);

	  /* If we redirect a loop latch edge cancel its loop.  */
	  if (e->src == e->src->loop_father->latch)
	    mark_loop_for_removal (e->src->loop_father);

	  /* Redirect the incoming edge (possibly to the joiner block) to the
	     appropriate duplicate block.  */
	  e2 = redirect_edge_and_branch (e, rd->dup_blocks[0]);
	  gcc_assert (e == e2);
	  flush_pending_stmts (e2);
	}

      /* Go ahead and clear E->aux.  It's not needed anymore and failure
	 to clear it will cause all kinds of unpleasant problems later.  */
      delete_jump_thread_path (path);
      e->aux = NULL;

    }

  /* Indicate that we actually threaded one or more jumps.  */
  if (rd->incoming_edges)
    local_info->jumps_threaded = true;

  return 1;
}

/* Return true if this block has no executable statements other than
   a simple ctrl flow instruction.  When the number of outgoing edges
   is one, this is equivalent to a "forwarder" block.  */

static bool
redirection_block_p (basic_block bb)
{
  gimple_stmt_iterator gsi;

  /* Advance to the first executable statement.  */
  gsi = gsi_start_bb (bb);
  while (!gsi_end_p (gsi)
	 && (gimple_code (gsi_stmt (gsi)) == GIMPLE_LABEL
	     || is_gimple_debug (gsi_stmt (gsi))
	     || gimple_nop_p (gsi_stmt (gsi))
	     || gimple_clobber_p (gsi_stmt (gsi))))
    gsi_next (&gsi);

  /* Check if this is an empty block.  */
  if (gsi_end_p (gsi))
    return true;

  /* Test that we've reached the terminating control statement.  */
  return gsi_stmt (gsi)
	 && (gimple_code (gsi_stmt (gsi)) == GIMPLE_COND
	     || gimple_code (gsi_stmt (gsi)) == GIMPLE_GOTO
	     || gimple_code (gsi_stmt (gsi)) == GIMPLE_SWITCH);
}

/* BB is a block which ends with a COND_EXPR or SWITCH_EXPR and when BB
   is reached via one or more specific incoming edges, we know which
   outgoing edge from BB will be traversed.

   We want to redirect those incoming edges to the target of the
   appropriate outgoing edge.  Doing so avoids a conditional branch
   and may expose new optimization opportunities.  Note that we have
   to update dominator tree and SSA graph after such changes.

   The key to keeping the SSA graph update manageable is to duplicate
   the side effects occurring in BB so that those side effects still
   occur on the paths which bypass BB after redirecting edges.

   We accomplish this by creating duplicates of BB and arranging for
   the duplicates to unconditionally pass control to one specific
   successor of BB.  We then revector the incoming edges into BB to
   the appropriate duplicate of BB.

   If NOLOOP_ONLY is true, we only perform the threading as long as it
   does not affect the structure of the loops in a nontrivial way.

   If JOINERS is true, then thread through joiner blocks as well.  */

static bool
thread_block_1 (basic_block bb, bool noloop_only, bool joiners)
{
  /* E is an incoming edge into BB that we may or may not want to
     redirect to a duplicate of BB.  */
  edge e, e2;
  edge_iterator ei;
  ssa_local_info_t local_info;

  local_info.duplicate_blocks = BITMAP_ALLOC (NULL);

  /* To avoid scanning a linear array for the element we need we instead
     use a hash table.  For normal code there should be no noticeable
     difference.  However, if we have a block with a large number of
     incoming and outgoing edges such linear searches can get expensive.  */
  redirection_data
    = new hash_table<struct redirection_data> (EDGE_COUNT (bb->succs));

  /* Record each unique threaded destination into a hash table for
     efficient lookups.  */
  FOR_EACH_EDGE (e, ei, bb->preds)
    {
      if (e->aux == NULL)
	continue;

      vec<jump_thread_edge *> *path = THREAD_PATH (e);

      if (((*path)[1]->type == EDGE_COPY_SRC_JOINER_BLOCK && !joiners)
	  || ((*path)[1]->type == EDGE_COPY_SRC_BLOCK && joiners))
	continue;

      e2 = path->last ()->e;
      if (!e2 || noloop_only)
	{
	  /* If NOLOOP_ONLY is true, we only allow threading through the
	     header of a loop to exit edges.  */

	  /* One case occurs when there was loop header buried in a jump
	     threading path that crosses loop boundaries.  We do not try
	     and thread this elsewhere, so just cancel the jump threading
	     request by clearing the AUX field now.  */
	  if ((bb->loop_father != e2->src->loop_father
	       && !loop_exit_edge_p (e2->src->loop_father, e2))
	      || (e2->src->loop_father != e2->dest->loop_father
		  && !loop_exit_edge_p (e2->src->loop_father, e2)))
	    {
	      /* Since this case is not handled by our special code
		 to thread through a loop header, we must explicitly
		 cancel the threading request here.  */
	      delete_jump_thread_path (path);
	      e->aux = NULL;
	      continue;
	    }

	  /* Another case occurs when trying to thread through our
	     own loop header, possibly from inside the loop.  We will
	     thread these later.  */
	  unsigned int i;
	  for (i = 1; i < path->length (); i++)
	    {
	      if ((*path)[i]->e->src == bb->loop_father->header
		  && (!loop_exit_edge_p (bb->loop_father, e2)
		      || (*path)[1]->type == EDGE_COPY_SRC_JOINER_BLOCK))
		break;
	    }

	  if (i != path->length ())
	    continue;
	}

      /* Insert the outgoing edge into the hash table if it is not
	 already in the hash table.  */
      lookup_redirection_data (e, INSERT);
    }

  /* We do not update dominance info.  */
  free_dominance_info (CDI_DOMINATORS);

  /* We know we only thread through the loop header to loop exits.
     Let the basic block duplication hook know we are not creating
     a multiple entry loop.  */
  if (noloop_only
      && bb == bb->loop_father->header)
    set_loop_copy (bb->loop_father, loop_outer (bb->loop_father));

  /* Now create duplicates of BB.

     Note that for a block with a high outgoing degree we can waste
     a lot of time and memory creating and destroying useless edges.

     So we first duplicate BB and remove the control structure at the
     tail of the duplicate as well as all outgoing edges from the
     duplicate.  We then use that duplicate block as a template for
     the rest of the duplicates.  */
  local_info.template_block = NULL;
  local_info.bb = bb;
  local_info.jumps_threaded = false;
  redirection_data->traverse <ssa_local_info_t *, ssa_create_duplicates>
			    (&local_info);

  /* The template does not have an outgoing edge.  Create that outgoing
     edge and update PHI nodes as the edge's target as necessary.

     We do this after creating all the duplicates to avoid creating
     unnecessary edges.  */
  redirection_data->traverse <ssa_local_info_t *, ssa_fixup_template_block>
			    (&local_info);

  /* The hash table traversals above created the duplicate blocks (and the
     statements within the duplicate blocks).  This loop creates PHI nodes for
     the duplicated blocks and redirects the incoming edges into BB to reach
     the duplicates of BB.  */
  redirection_data->traverse <ssa_local_info_t *, ssa_redirect_edges>
			    (&local_info);

  /* Done with this block.  Clear REDIRECTION_DATA.  */
  delete redirection_data;
  redirection_data = NULL;

  if (noloop_only
      && bb == bb->loop_father->header)
    set_loop_copy (bb->loop_father, NULL);

  BITMAP_FREE (local_info.duplicate_blocks);
  local_info.duplicate_blocks = NULL;

  /* Indicate to our caller whether or not any jumps were threaded.  */
  return local_info.jumps_threaded;
}

/* Wrapper for thread_block_1 so that we can first handle jump
   thread paths which do not involve copying joiner blocks, then
   handle jump thread paths which have joiner blocks.

   By doing things this way we can be as aggressive as possible and
   not worry that copying a joiner block will create a jump threading
   opportunity.  */

static bool
thread_block (basic_block bb, bool noloop_only)
{
  bool retval;
  retval = thread_block_1 (bb, noloop_only, false);
  retval |= thread_block_1 (bb, noloop_only, true);
  return retval;
}


/* Threads edge E through E->dest to the edge THREAD_TARGET (E).  Returns the
   copy of E->dest created during threading, or E->dest if it was not necessary
   to copy it (E is its single predecessor).  */

static basic_block
thread_single_edge (edge e)
{
  basic_block bb = e->dest;
  struct redirection_data rd;
  vec<jump_thread_edge *> *path = THREAD_PATH (e);
  edge eto = (*path)[1]->e;

  delete_jump_thread_path (path);
  e->aux = NULL;

  thread_stats.num_threaded_edges++;

  if (single_pred_p (bb))
    {
      /* If BB has just a single predecessor, we should only remove the
	 control statements at its end, and successors except for ETO.  */
      remove_ctrl_stmt_and_useless_edges (bb, eto->dest);

      /* And fixup the flags on the single remaining edge.  */
      eto->flags &= ~(EDGE_TRUE_VALUE | EDGE_FALSE_VALUE | EDGE_ABNORMAL);
      eto->flags |= EDGE_FALLTHRU;

      return bb;
    }

  /* Otherwise, we need to create a copy.  */
  if (e->dest == eto->src)
    update_bb_profile_for_threading (bb, EDGE_FREQUENCY (e), e->count, eto);

  vec<jump_thread_edge *> *npath = new vec<jump_thread_edge *> ();
  jump_thread_edge *x = new jump_thread_edge (e, EDGE_START_JUMP_THREAD);
  npath->safe_push (x);

  x = new jump_thread_edge (eto, EDGE_COPY_SRC_BLOCK);
  npath->safe_push (x);
  rd.path = npath;

  create_block_for_threading (bb, &rd, 0, NULL);
  remove_ctrl_stmt_and_useless_edges (rd.dup_blocks[0], NULL);
  create_edge_and_update_destination_phis (&rd, rd.dup_blocks[0], 0);

  if (dump_file && (dump_flags & TDF_DETAILS))
    fprintf (dump_file, "  Threaded jump %d --> %d to %d\n",
	     e->src->index, e->dest->index, rd.dup_blocks[0]->index);

  rd.dup_blocks[0]->count = e->count;
  rd.dup_blocks[0]->frequency = EDGE_FREQUENCY (e);
  single_succ_edge (rd.dup_blocks[0])->count = e->count;
  redirect_edge_and_branch (e, rd.dup_blocks[0]);
  flush_pending_stmts (e);

  delete_jump_thread_path (npath);
  return rd.dup_blocks[0];
}

/* Callback for dfs_enumerate_from.  Returns true if BB is different
   from STOP and DBDS_CE_STOP.  */

static basic_block dbds_ce_stop;
static bool
dbds_continue_enumeration_p (const_basic_block bb, const void *stop)
{
  return (bb != (const_basic_block) stop
	  && bb != dbds_ce_stop);
}

/* Evaluates the dominance relationship of latch of the LOOP and BB, and
   returns the state.  */

enum bb_dom_status
{
  /* BB does not dominate latch of the LOOP.  */
  DOMST_NONDOMINATING,
  /* The LOOP is broken (there is no path from the header to its latch.  */
  DOMST_LOOP_BROKEN,
  /* BB dominates the latch of the LOOP.  */
  DOMST_DOMINATING
};

static enum bb_dom_status
determine_bb_domination_status (struct loop *loop, basic_block bb)
{
  basic_block *bblocks;
  unsigned nblocks, i;
  bool bb_reachable = false;
  edge_iterator ei;
  edge e;

  /* This function assumes BB is a successor of LOOP->header.
     If that is not the case return DOMST_NONDOMINATING which
     is always safe.  */
    {
      bool ok = false;

      FOR_EACH_EDGE (e, ei, bb->preds)
	{
     	  if (e->src == loop->header)
	    {
	      ok = true;
	      break;
	    }
	}

      if (!ok)
	return DOMST_NONDOMINATING;
    }

  if (bb == loop->latch)
    return DOMST_DOMINATING;

  /* Check that BB dominates LOOP->latch, and that it is back-reachable
     from it.  */

  bblocks = XCNEWVEC (basic_block, loop->num_nodes);
  dbds_ce_stop = loop->header;
  nblocks = dfs_enumerate_from (loop->latch, 1, dbds_continue_enumeration_p,
				bblocks, loop->num_nodes, bb);
  for (i = 0; i < nblocks; i++)
    FOR_EACH_EDGE (e, ei, bblocks[i]->preds)
      {
	if (e->src == loop->header)
	  {
	    free (bblocks);
	    return DOMST_NONDOMINATING;
	  }
	if (e->src == bb)
	  bb_reachable = true;
      }

  free (bblocks);
  return (bb_reachable ? DOMST_DOMINATING : DOMST_LOOP_BROKEN);
}

/* Return true if BB is part of the new pre-header that is created
   when threading the latch to DATA.  */

static bool
def_split_header_continue_p (const_basic_block bb, const void *data)
{
  const_basic_block new_header = (const_basic_block) data;
  const struct loop *l;

  if (bb == new_header
      || loop_depth (bb->loop_father) < loop_depth (new_header->loop_father))
    return false;
  for (l = bb->loop_father; l; l = loop_outer (l))
    if (l == new_header->loop_father)
      return true;
  return false;
}

/* Thread jumps through the header of LOOP.  Returns true if cfg changes.
   If MAY_PEEL_LOOP_HEADERS is false, we avoid threading from entry edges
   to the inside of the loop.  */

static bool
thread_through_loop_header (struct loop *loop, bool may_peel_loop_headers)
{
  basic_block header = loop->header;
  edge e, tgt_edge, latch = loop_latch_edge (loop);
  edge_iterator ei;
  basic_block tgt_bb, atgt_bb;
  enum bb_dom_status domst;

  /* We have already threaded through headers to exits, so all the threading
     requests now are to the inside of the loop.  We need to avoid creating
     irreducible regions (i.e., loops with more than one entry block), and
     also loop with several latch edges, or new subloops of the loop (although
     there are cases where it might be appropriate, it is difficult to decide,
     and doing it wrongly may confuse other optimizers).

     We could handle more general cases here.  However, the intention is to
     preserve some information about the loop, which is impossible if its
     structure changes significantly, in a way that is not well understood.
     Thus we only handle few important special cases, in which also updating
     of the loop-carried information should be feasible:

     1) Propagation of latch edge to a block that dominates the latch block
	of a loop.  This aims to handle the following idiom:

	first = 1;
	while (1)
	  {
	    if (first)
	      initialize;
	    first = 0;
	    body;
	  }

	After threading the latch edge, this becomes

	first = 1;
	if (first)
	  initialize;
	while (1)
	  {
	    first = 0;
	    body;
	  }

	The original header of the loop is moved out of it, and we may thread
	the remaining edges through it without further constraints.

     2) All entry edges are propagated to a single basic block that dominates
	the latch block of the loop.  This aims to handle the following idiom
	(normally created for "for" loops):

	i = 0;
	while (1)
	  {
	    if (i >= 100)
	      break;
	    body;
	    i++;
	  }

	This becomes

	i = 0;
	while (1)
	  {
	    body;
	    i++;
	    if (i >= 100)
	      break;
	  }
     */

  /* Threading through the header won't improve the code if the header has just
     one successor.  */
  if (single_succ_p (header))
    goto fail;

  /* If we threaded the latch using a joiner block, we cancel the
     threading opportunity out of an abundance of caution.  However,
     still allow threading from outside to inside the loop.  */
  if (latch->aux)
    {
      vec<jump_thread_edge *> *path = THREAD_PATH (latch);
      if ((*path)[1]->type == EDGE_COPY_SRC_JOINER_BLOCK)
	{
	  delete_jump_thread_path (path);
	  latch->aux = NULL;
	}
    }

  if (latch->aux)
    {
      vec<jump_thread_edge *> *path = THREAD_PATH (latch);
      tgt_edge = (*path)[1]->e;
      tgt_bb = tgt_edge->dest;
    }
  else if (!may_peel_loop_headers
	   && !redirection_block_p (loop->header))
    goto fail;
  else
    {
      tgt_bb = NULL;
      tgt_edge = NULL;
      FOR_EACH_EDGE (e, ei, header->preds)
	{
	  if (!e->aux)
	    {
	      if (e == latch)
		continue;

	      /* If latch is not threaded, and there is a header
		 edge that is not threaded, we would create loop
		 with multiple entries.  */
	      goto fail;
	    }

	  vec<jump_thread_edge *> *path = THREAD_PATH (e);

	  if ((*path)[1]->type == EDGE_COPY_SRC_JOINER_BLOCK)
	    goto fail;
	  tgt_edge = (*path)[1]->e;
	  atgt_bb = tgt_edge->dest;
	  if (!tgt_bb)
	    tgt_bb = atgt_bb;
	  /* Two targets of threading would make us create loop
	     with multiple entries.  */
	  else if (tgt_bb != atgt_bb)
	    goto fail;
	}

      if (!tgt_bb)
	{
	  /* There are no threading requests.  */
	  return false;
	}

      /* Redirecting to empty loop latch is useless.  */
      if (tgt_bb == loop->latch
	  && empty_block_p (loop->latch))
	goto fail;
    }

  /* The target block must dominate the loop latch, otherwise we would be
     creating a subloop.  */
  domst = determine_bb_domination_status (loop, tgt_bb);
  if (domst == DOMST_NONDOMINATING)
    goto fail;
  if (domst == DOMST_LOOP_BROKEN)
    {
      /* If the loop ceased to exist, mark it as such, and thread through its
	 original header.  */
      mark_loop_for_removal (loop);
      return thread_block (header, false);
    }

  if (tgt_bb->loop_father->header == tgt_bb)
    {
      /* If the target of the threading is a header of a subloop, we need
	 to create a preheader for it, so that the headers of the two loops
	 do not merge.  */
      if (EDGE_COUNT (tgt_bb->preds) > 2)
	{
	  tgt_bb = create_preheader (tgt_bb->loop_father, 0);
	  gcc_assert (tgt_bb != NULL);
	}
      else
	tgt_bb = split_edge (tgt_edge);
    }

  if (latch->aux)
    {
      basic_block *bblocks;
      unsigned nblocks, i;

      /* First handle the case latch edge is redirected.  We are copying
	 the loop header but not creating a multiple entry loop.  Make the
	 cfg manipulation code aware of that fact.  */
      set_loop_copy (loop, loop);
      loop->latch = thread_single_edge (latch);
      set_loop_copy (loop, NULL);
      gcc_assert (single_succ (loop->latch) == tgt_bb);
      loop->header = tgt_bb;

      /* Remove the new pre-header blocks from our loop.  */
      bblocks = XCNEWVEC (basic_block, loop->num_nodes);
      nblocks = dfs_enumerate_from (header, 0, def_split_header_continue_p,
				    bblocks, loop->num_nodes, tgt_bb);
      for (i = 0; i < nblocks; i++)
	if (bblocks[i]->loop_father == loop)
	  {
	    remove_bb_from_loops (bblocks[i]);
	    add_bb_to_loop (bblocks[i], loop_outer (loop));
	  }
      free (bblocks);

      /* If the new header has multiple latches mark it so.  */
      FOR_EACH_EDGE (e, ei, loop->header->preds)
	if (e->src->loop_father == loop
	    && e->src != loop->latch)
	  {
	    loop->latch = NULL;
	    loops_state_set (LOOPS_MAY_HAVE_MULTIPLE_LATCHES);
	  }

      /* Cancel remaining threading requests that would make the
	 loop a multiple entry loop.  */
      FOR_EACH_EDGE (e, ei, header->preds)
	{
	  edge e2;

	  if (e->aux == NULL)
	    continue;

	  vec<jump_thread_edge *> *path = THREAD_PATH (e);
	  e2 = path->last ()->e;

	  if (e->src->loop_father != e2->dest->loop_father
	      && e2->dest != loop->header)
	    {
	      delete_jump_thread_path (path);
	      e->aux = NULL;
	    }
	}

      /* Thread the remaining edges through the former header.  */
      thread_block (header, false);
    }
  else
    {
      basic_block new_preheader;

      /* Now consider the case entry edges are redirected to the new entry
	 block.  Remember one entry edge, so that we can find the new
	 preheader (its destination after threading).  */
      FOR_EACH_EDGE (e, ei, header->preds)
	{
	  if (e->aux)
	    break;
	}

      /* The duplicate of the header is the new preheader of the loop.  Ensure
	 that it is placed correctly in the loop hierarchy.  */
      set_loop_copy (loop, loop_outer (loop));

      thread_block (header, false);
      set_loop_copy (loop, NULL);
      new_preheader = e->dest;

      /* Create the new latch block.  This is always necessary, as the latch
	 must have only a single successor, but the original header had at
	 least two successors.  */
      loop->latch = NULL;
      mfb_kj_edge = single_succ_edge (new_preheader);
      loop->header = mfb_kj_edge->dest;
      latch = make_forwarder_block (tgt_bb, mfb_keep_just, NULL);
      loop->header = latch->dest;
      loop->latch = latch->src;
    }

  return true;

fail:
  /* We failed to thread anything.  Cancel the requests.  */
  FOR_EACH_EDGE (e, ei, header->preds)
    {
      vec<jump_thread_edge *> *path = THREAD_PATH (e);

      if (path)
	{
	  delete_jump_thread_path (path);
	  e->aux = NULL;
	}
    }
  return false;
}

/* E1 and E2 are edges into the same basic block.  Return TRUE if the
   PHI arguments associated with those edges are equal or there are no
   PHI arguments, otherwise return FALSE.  */

static bool
phi_args_equal_on_edges (edge e1, edge e2)
{
  gphi_iterator gsi;
  int indx1 = e1->dest_idx;
  int indx2 = e2->dest_idx;

  for (gsi = gsi_start_phis (e1->dest); !gsi_end_p (gsi); gsi_next (&gsi))
    {
      gphi *phi = gsi.phi ();

      if (!operand_equal_p (gimple_phi_arg_def (phi, indx1),
			    gimple_phi_arg_def (phi, indx2), 0))
	return false;
    }
  return true;
}

/* Walk through the registered jump threads and convert them into a
   form convenient for this pass.

   Any block which has incoming edges threaded to outgoing edges
   will have its entry in THREADED_BLOCK set.

   Any threaded edge will have its new outgoing edge stored in the
   original edge's AUX field.

   This form avoids the need to walk all the edges in the CFG to
   discover blocks which need processing and avoids unnecessary
   hash table lookups to map from threaded edge to new target.  */

static void
mark_threaded_blocks (bitmap threaded_blocks)
{
  unsigned int i;
  bitmap_iterator bi;
  bitmap tmp = BITMAP_ALLOC (NULL);
  basic_block bb;
  edge e;
  edge_iterator ei;

  /* It is possible to have jump threads in which one is a subpath
     of the other.  ie, (A, B), (B, C), (C, D) where B is a joiner
     block and (B, C), (C, D) where no joiner block exists.

     When this occurs ignore the jump thread request with the joiner
     block.  It's totally subsumed by the simpler jump thread request.

     This results in less block copying, simpler CFGs.  More importantly,
     when we duplicate the joiner block, B, in this case we will create
     a new threading opportunity that we wouldn't be able to optimize
     until the next jump threading iteration.

     So first convert the jump thread requests which do not require a
     joiner block.  */
  for (i = 0; i < paths.length (); i++)
    {
      vec<jump_thread_edge *> *path = paths[i];

      if ((*path)[1]->type != EDGE_COPY_SRC_JOINER_BLOCK)
	{
	  edge e = (*path)[0]->e;
	  e->aux = (void *)path;
	  bitmap_set_bit (tmp, e->dest->index);
	}
    }

  /* Now iterate again, converting cases where we want to thread
     through a joiner block, but only if no other edge on the path
     already has a jump thread attached to it.  We do this in two passes,
     to avoid situations where the order in the paths vec can hide overlapping
     threads (the path is recorded on the incoming edge, so we would miss
     cases where the second path starts at a downstream edge on the same
     path).  First record all joiner paths, deleting any in the unexpected
     case where there is already a path for that incoming edge.  */
  for (i = 0; i < paths.length (); i++)
    {
      vec<jump_thread_edge *> *path = paths[i];

      if ((*path)[1]->type == EDGE_COPY_SRC_JOINER_BLOCK)
	{
	  /* Attach the path to the starting edge if none is yet recorded.  */
	  if ((*path)[0]->e->aux == NULL)
	    {
	      (*path)[0]->e->aux = path;
	    }
	  else
	    {
	      paths.unordered_remove (i);
	      if (dump_file && (dump_flags & TDF_DETAILS))
		dump_jump_thread_path (dump_file, *path, false);
	      delete_jump_thread_path (path);
	    }
	}
    }
  /* Second, look for paths that have any other jump thread attached to
     them, and either finish converting them or cancel them.  */
  for (i = 0; i < paths.length (); i++)
    {
      vec<jump_thread_edge *> *path = paths[i];
      edge e = (*path)[0]->e;

      if ((*path)[1]->type == EDGE_COPY_SRC_JOINER_BLOCK && e->aux == path)
	{
	  unsigned int j;
	  for (j = 1; j < path->length (); j++)
	    if ((*path)[j]->e->aux != NULL)
	      break;

	  /* If we iterated through the entire path without exiting the loop,
	     then we are good to go, record it.  */
	  if (j == path->length ())
	    bitmap_set_bit (tmp, e->dest->index);
	  else
	    {
	      e->aux = NULL;
	      paths.unordered_remove (i);
	      if (dump_file && (dump_flags & TDF_DETAILS))
		dump_jump_thread_path (dump_file, *path, false);
	      delete_jump_thread_path (path);
	    }
	}
    }

  /* If optimizing for size, only thread through block if we don't have
     to duplicate it or it's an otherwise empty redirection block.  */
  if (optimize_function_for_size_p (cfun))
    {
      EXECUTE_IF_SET_IN_BITMAP (tmp, 0, i, bi)
	{
	  bb = BASIC_BLOCK_FOR_FN (cfun, i);
	  if (EDGE_COUNT (bb->preds) > 1
	      && !redirection_block_p (bb))
	    {
	      FOR_EACH_EDGE (e, ei, bb->preds)
		{
		  if (e->aux)
		    {
		      vec<jump_thread_edge *> *path = THREAD_PATH (e);
		      delete_jump_thread_path (path);
		      e->aux = NULL;
		    }
		}
	    }
	  else
	    bitmap_set_bit (threaded_blocks, i);
	}
    }
  else
    bitmap_copy (threaded_blocks, tmp);

  /* Look for jump threading paths which cross multiple loop headers.

     The code to thread through loop headers will change the CFG in ways
     that break assumptions made by the loop optimization code.

     We don't want to blindly cancel the requests.  We can instead do better
     by trimming off the end of the jump thread path.  */
  EXECUTE_IF_SET_IN_BITMAP (tmp, 0, i, bi)
    {
      basic_block bb = BASIC_BLOCK_FOR_FN (cfun, i);
      FOR_EACH_EDGE (e, ei, bb->preds)
	{
	  if (e->aux)
	    {
	      vec<jump_thread_edge *> *path = THREAD_PATH (e);

	      for (unsigned int i = 0, crossed_headers = 0;
		   i < path->length ();
		   i++)
		{
		  basic_block dest = (*path)[i]->e->dest;
		  crossed_headers += (dest == dest->loop_father->header);
		  if (crossed_headers > 1)
		    {
		      /* Trim from entry I onwards.  */
		      for (unsigned int j = i; j < path->length (); j++)
			delete (*path)[j];
		      path->truncate (i);

		      /* Now that we've truncated the path, make sure
			 what's left is still valid.   We need at least
			 two edges on the path and the last edge can not
			 be a joiner.  This should never happen, but let's
			 be safe.  */
		      if (path->length () < 2
			  || (path->last ()->type
			      == EDGE_COPY_SRC_JOINER_BLOCK))
			{
			  delete_jump_thread_path (path);
			  e->aux = NULL;
			}
		      break;
		    }
		}
	    }
	}
    }

  /* If we have a joiner block (J) which has two successors S1 and S2 and
     we are threading though S1 and the final destination of the thread
     is S2, then we must verify that any PHI nodes in S2 have the same
     PHI arguments for the edge J->S2 and J->S1->...->S2.

     We used to detect this prior to registering the jump thread, but
     that prohibits propagation of edge equivalences into non-dominated
     PHI nodes as the equivalency test might occur before propagation.

     This must also occur after we truncate any jump threading paths
     as this scenario may only show up after truncation.

     This works for now, but will need improvement as part of the FSA
     optimization.

     Note since we've moved the thread request data to the edges,
     we have to iterate on those rather than the threaded_edges vector.  */
  EXECUTE_IF_SET_IN_BITMAP (tmp, 0, i, bi)
    {
      bb = BASIC_BLOCK_FOR_FN (cfun, i);
      FOR_EACH_EDGE (e, ei, bb->preds)
	{
	  if (e->aux)
	    {
	      vec<jump_thread_edge *> *path = THREAD_PATH (e);
	      bool have_joiner = ((*path)[1]->type == EDGE_COPY_SRC_JOINER_BLOCK);

	      if (have_joiner)
		{
		  basic_block joiner = e->dest;
		  edge final_edge = path->last ()->e;
		  basic_block final_dest = final_edge->dest;
		  edge e2 = find_edge (joiner, final_dest);

		  if (e2 && !phi_args_equal_on_edges (e2, final_edge))
		    {
		      delete_jump_thread_path (path);
		      e->aux = NULL;
		    }
		}
	    }
	}
    }

  BITMAP_FREE (tmp);
}


/* Return TRUE if BB ends with a switch statement or a computed goto.
   Otherwise return false.  */
static bool
bb_ends_with_multiway_branch (basic_block bb ATTRIBUTE_UNUSED)
{
  gimple stmt = last_stmt (bb);
  if (stmt && gimple_code (stmt) == GIMPLE_SWITCH)
    return true;
  if (stmt && gimple_code (stmt) == GIMPLE_GOTO
      && TREE_CODE (gimple_goto_dest (stmt)) == SSA_NAME)
    return true;
  return false;
}

/* Verify that the REGION is a valid jump thread.  A jump thread is a special
   case of SEME Single Entry Multiple Exits region in which all nodes in the
   REGION have exactly one incoming edge.  The only exception is the first block
   that may not have been connected to the rest of the cfg yet.  */

DEBUG_FUNCTION void
verify_jump_thread (basic_block *region, unsigned n_region)
{
  for (unsigned i = 0; i < n_region; i++)
    gcc_assert (EDGE_COUNT (region[i]->preds) <= 1);
}

/* Return true when BB is one of the first N items in BBS.  */

static inline bool
bb_in_bbs (basic_block bb, basic_block *bbs, int n)
{
  for (int i = 0; i < n; i++)
    if (bb == bbs[i])
      return true;

  return false;
}

/* Duplicates a jump-thread path of N_REGION basic blocks.
   The ENTRY edge is redirected to the duplicate of the region.

   Remove the last conditional statement in the last basic block in the REGION,
   and create a single fallthru edge pointing to the same destination as the
   EXIT edge.

   The new basic blocks are stored to REGION_COPY in the same order as they had
   in REGION, provided that REGION_COPY is not NULL.

   Returns false if it is unable to copy the region, true otherwise.  */

static bool
duplicate_thread_path (edge entry, edge exit,
		       basic_block *region, unsigned n_region,
		       basic_block *region_copy)
{
  unsigned i;
  bool free_region_copy = false;
  struct loop *loop = entry->dest->loop_father;
  edge exit_copy;
  edge redirected;
  int total_freq = 0, entry_freq = 0;
  gcov_type total_count = 0, entry_count = 0;

  if (!can_copy_bbs_p (region, n_region))
    return false;

  /* Some sanity checking.  Note that we do not check for all possible
     missuses of the functions.  I.e. if you ask to copy something weird,
     it will work, but the state of structures probably will not be
     correct.  */
  for (i = 0; i < n_region; i++)
    {
      /* We do not handle subloops, i.e. all the blocks must belong to the
	 same loop.  */
      if (region[i]->loop_father != loop)
	return false;
    }

  initialize_original_copy_tables ();

  set_loop_copy (loop, loop);

  if (!region_copy)
    {
      region_copy = XNEWVEC (basic_block, n_region);
      free_region_copy = true;
    }

  if (entry->dest->count)
    {
      total_count = entry->dest->count;
      entry_count = entry->count;
      /* Fix up corner cases, to avoid division by zero or creation of negative
	 frequencies.  */
      if (entry_count > total_count)
	entry_count = total_count;
    }
  else
    {
      total_freq = entry->dest->frequency;
      entry_freq = EDGE_FREQUENCY (entry);
      /* Fix up corner cases, to avoid division by zero or creation of negative
	 frequencies.  */
      if (total_freq == 0)
	total_freq = 1;
      else if (entry_freq > total_freq)
	entry_freq = total_freq;
    }

  copy_bbs (region, n_region, region_copy, &exit, 1, &exit_copy, loop,
	    split_edge_bb_loc (entry), false);

  /* Fix up: copy_bbs redirects all edges pointing to copied blocks.  The
     following code ensures that all the edges exiting the jump-thread path are
     redirected back to the original code: these edges are exceptions
     invalidating the property that is propagated by executing all the blocks of
     the jump-thread path in order.  */

  for (i = 0; i < n_region; i++)
    {
      edge e;
      edge_iterator ei;
      basic_block bb = region_copy[i];

      if (single_succ_p (bb))
	{
	  /* Make sure the successor is the next node in the path.  */
	  gcc_assert (i + 1 == n_region
		      || region_copy[i + 1] == single_succ_edge (bb)->dest);
	  continue;
	}

      /* Special case the last block on the path: make sure that it does not
	 jump back on the copied path.  */
      if (i + 1 == n_region)
	{
	  FOR_EACH_EDGE (e, ei, bb->succs)
	    if (bb_in_bbs (e->dest, region_copy, n_region - 1))
	      {
		basic_block orig = get_bb_original (e->dest);
		if (orig)
		  redirect_edge_and_branch_force (e, orig);
	      }
	  continue;
	}

      /* Redirect all other edges jumping to non-adjacent blocks back to the
	 original code.  */
      FOR_EACH_EDGE (e, ei, bb->succs)
	if (region_copy[i + 1] != e->dest)
	  {
	    basic_block orig = get_bb_original (e->dest);
	    if (orig)
	      redirect_edge_and_branch_force (e, orig);
	  }
    }

  if (total_count)
    {
      scale_bbs_frequencies_gcov_type (region, n_region,
				       total_count - entry_count,
				       total_count);
      scale_bbs_frequencies_gcov_type (region_copy, n_region, entry_count,
				       total_count);
    }
  else
    {
      scale_bbs_frequencies_int (region, n_region, total_freq - entry_freq,
				 total_freq);
      scale_bbs_frequencies_int (region_copy, n_region, entry_freq, total_freq);
    }

#ifdef ENABLE_CHECKING
  verify_jump_thread (region_copy, n_region);
#endif

  /* Remove the last branch in the jump thread path.  */
  remove_ctrl_stmt_and_useless_edges (region_copy[n_region - 1], exit->dest);
  edge e = make_edge (region_copy[n_region - 1], exit->dest, EDGE_FALLTHRU);

  if (e) {
    rescan_loop_exit (e, true, false);
    e->probability = REG_BR_PROB_BASE;
    e->count = region_copy[n_region - 1]->count;
  }

  /* Redirect the entry and add the phi node arguments.  */
  if (entry->dest == loop->header)
    mark_loop_for_removal (loop);
  redirected = redirect_edge_and_branch (entry, get_bb_copy (entry->dest));
  gcc_assert (redirected != NULL);
  flush_pending_stmts (entry);

  /* Add the other PHI node arguments.  */
  add_phi_args_after_copy (region_copy, n_region, NULL);

  if (free_region_copy)
    free (region_copy);

  free_original_copy_tables ();
  return true;
}

/* Return true when PATH is a valid jump-thread path.  */

static bool
valid_jump_thread_path (vec<jump_thread_edge *> *path)
{
  unsigned len = path->length ();

  /* Check that the path is connected.  */
  for (unsigned int j = 0; j < len - 1; j++)
    if ((*path)[j]->e->dest != (*path)[j+1]->e->src)
      return false;

  return true;
}

/* Walk through all blocks and thread incoming edges to the appropriate
   outgoing edge for each edge pair recorded in THREADED_EDGES.

   It is the caller's responsibility to fix the dominance information
   and rewrite duplicated SSA_NAMEs back into SSA form.

   If MAY_PEEL_LOOP_HEADERS is false, we avoid threading edges through
   loop headers if it does not simplify the loop.

   Returns true if one or more edges were threaded, false otherwise.  */

bool
thread_through_all_blocks (bool may_peel_loop_headers)
{
  bool retval = false;
  unsigned int i;
  bitmap_iterator bi;
  bitmap threaded_blocks;
  struct loop *loop;

  if (!paths.exists ())
    return false;

  threaded_blocks = BITMAP_ALLOC (NULL);
  memset (&thread_stats, 0, sizeof (thread_stats));

  /* Jump-thread all FSM threads before other jump-threads.  */
  for (i = 0; i < paths.length ();)
    {
      vec<jump_thread_edge *> *path = paths[i];
      edge entry = (*path)[0]->e;

      /* Only code-generate FSM jump-threads in this loop.  */
      if ((*path)[0]->type != EDGE_FSM_THREAD)
	{
	  i++;
	  continue;
	}

      /* Do not jump-thread twice from the same block.  */
      if (bitmap_bit_p (threaded_blocks, entry->src->index)
	  /* Verify that the jump thread path is still valid: a
	     previous jump-thread may have changed the CFG, and
	     invalidated the current path.  */
	  || !valid_jump_thread_path (path))
	{
	  /* Remove invalid FSM jump-thread paths.  */
	  delete_jump_thread_path (path);
	  paths.unordered_remove (i);
	  continue;
	}

      unsigned len = path->length ();
      edge exit = (*path)[len - 1]->e;
      basic_block *region = XNEWVEC (basic_block, len - 1);

      for (unsigned int j = 0; j < len - 1; j++)
	region[j] = (*path)[j]->e->dest;

      if (duplicate_thread_path (entry, exit, region, len - 1, NULL))
	{
	  /* We do not update dominance info.  */
	  free_dominance_info (CDI_DOMINATORS);
	  bitmap_set_bit (threaded_blocks, entry->src->index);
	  retval = true;
	}

      delete_jump_thread_path (path);
      paths.unordered_remove (i);
    }

  /* Remove from PATHS all the jump-threads starting with an edge already
     jump-threaded.  */
  for (i = 0; i < paths.length ();)
    {
      vec<jump_thread_edge *> *path = paths[i];
      edge entry = (*path)[0]->e;

      /* Do not jump-thread twice from the same block.  */
      if (bitmap_bit_p (threaded_blocks, entry->src->index))
	{
	  delete_jump_thread_path (path);
	  paths.unordered_remove (i);
	}
      else
	i++;
    }

  bitmap_clear (threaded_blocks);

  mark_threaded_blocks (threaded_blocks);

  initialize_original_copy_tables ();

  /* First perform the threading requests that do not affect
     loop structure.  */
  EXECUTE_IF_SET_IN_BITMAP (threaded_blocks, 0, i, bi)
    {
      basic_block bb = BASIC_BLOCK_FOR_FN (cfun, i);

      if (EDGE_COUNT (bb->preds) > 0)
	retval |= thread_block (bb, true);
    }

  /* Then perform the threading through loop headers.  We start with the
     innermost loop, so that the changes in cfg we perform won't affect
     further threading.  */
  FOR_EACH_LOOP (loop, LI_FROM_INNERMOST)
    {
      if (!loop->header
	  || !bitmap_bit_p (threaded_blocks, loop->header->index))
	continue;

      retval |= thread_through_loop_header (loop, may_peel_loop_headers);
    }

  /* Any jump threading paths that are still attached to edges at this
     point must be one of two cases.

     First, we could have a jump threading path which went from outside
     a loop to inside a loop that was ignored because a prior jump thread
     across a backedge was realized (which indirectly causes the loop
     above to ignore the latter thread).  We can detect these because the
     loop structures will be different and we do not currently try to
     optimize this case.

     Second, we could be threading across a backedge to a point within the
     same loop.  This occurrs for the FSA/FSM optimization and we would
     like to optimize it.  However, we have to be very careful as this
     may completely scramble the loop structures, with the result being
     irreducible loops causing us to throw away our loop structure.

     As a compromise for the latter case, if the thread path ends in
     a block where the last statement is a multiway branch, then go
     ahead and thread it, else ignore it.  */
  basic_block bb;
  edge e;
  FOR_EACH_BB_FN (bb, cfun)
    {
      /* If we do end up threading here, we can remove elements from
	 BB->preds.  Thus we can not use the FOR_EACH_EDGE iterator.  */
      for (edge_iterator ei = ei_start (bb->preds);
	   (e = ei_safe_edge (ei));)
	if (e->aux)
	  {
	    vec<jump_thread_edge *> *path = THREAD_PATH (e);

	    /* Case 1, threading from outside to inside the loop
	       after we'd already threaded through the header.  */
	    if ((*path)[0]->e->dest->loop_father
		!= path->last ()->e->src->loop_father)
	      {
		delete_jump_thread_path (path);
		e->aux = NULL;
		ei_next (&ei);
	      }
	   else if (bb_ends_with_multiway_branch (path->last ()->e->src))
	      {
		/* The code to thread through loop headers may have
		   split a block with jump threads attached to it.

		   We can identify this with a disjoint jump threading
		   path.  If found, just remove it.  */
		for (unsigned int i = 0; i < path->length () - 1; i++)
		  if ((*path)[i]->e->dest != (*path)[i + 1]->e->src)
		    {
		      delete_jump_thread_path (path);
		      e->aux = NULL;
		      ei_next (&ei);
		      break;
		    }

		/* Our path is still valid, thread it.  */
		if (e->aux)
		  {
		    if (thread_block ((*path)[0]->e->dest, false))
		      e->aux = NULL;
		    else
		      {
			delete_jump_thread_path (path);
			e->aux = NULL;
			ei_next (&ei);
		      }
		  }
	      }
	   else
	      {
		delete_jump_thread_path (path);
		e->aux = NULL;
		ei_next (&ei);
	      }
 	  }
	else
	  ei_next (&ei);
    }

  statistics_counter_event (cfun, "Jumps threaded",
			    thread_stats.num_threaded_edges);

  free_original_copy_tables ();

  BITMAP_FREE (threaded_blocks);
  threaded_blocks = NULL;
  paths.release ();

  if (retval)
    loops_state_set (LOOPS_NEED_FIXUP);

  return retval;
}

/* Delete the jump threading path PATH.  We have to explcitly delete
   each entry in the vector, then the container.  */

void
delete_jump_thread_path (vec<jump_thread_edge *> *path)
{
  for (unsigned int i = 0; i < path->length (); i++)
    delete (*path)[i];
  path->release();
  delete path;
}

/* Register a jump threading opportunity.  We queue up all the jump
   threading opportunities discovered by a pass and update the CFG
   and SSA form all at once.

   E is the edge we can thread, E2 is the new target edge, i.e., we
   are effectively recording that E->dest can be changed to E2->dest
   after fixing the SSA graph.  */

void
register_jump_thread (vec<jump_thread_edge *> *path)
{
  if (!dbg_cnt (registered_jump_thread))
    {
      delete_jump_thread_path (path);
      return;
    }

  /* First make sure there are no NULL outgoing edges on the jump threading
     path.  That can happen for jumping to a constant address.  */
  for (unsigned int i = 0; i < path->length (); i++)
    if ((*path)[i]->e == NULL)
      {
	if (dump_file && (dump_flags & TDF_DETAILS))
	  {
	    fprintf (dump_file,
		     "Found NULL edge in jump threading path.  Cancelling jump thread:\n");
	    dump_jump_thread_path (dump_file, *path, false);
	  }

	delete_jump_thread_path (path);
	return;
      }

  if (dump_file && (dump_flags & TDF_DETAILS))
    dump_jump_thread_path (dump_file, *path, true);

  if (!paths.exists ())
    paths.create (5);

  paths.safe_push (path);
}
