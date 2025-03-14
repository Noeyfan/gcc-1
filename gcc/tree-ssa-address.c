/* Memory address lowering and addressing mode selection.
   Copyright (C) 2004-2015 Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

GCC is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

/* Utility functions for manipulation with TARGET_MEM_REFs -- tree expressions
   that directly map to addressing modes of the target.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "input.h"
#include "alias.h"
#include "symtab.h"
#include "tree.h"
#include "fold-const.h"
#include "stor-layout.h"
#include "tm_p.h"
#include "predict.h"
#include "hard-reg-set.h"
#include "function.h"
#include "basic-block.h"
#include "tree-pretty-print.h"
#include "tree-ssa-alias.h"
#include "internal-fn.h"
#include "gimple-expr.h"
#include "is-a.h"
#include "gimple.h"
#include "gimple-iterator.h"
#include "gimplify-me.h"
#include "stringpool.h"
#include "tree-ssanames.h"
#include "tree-ssa-loop-ivopts.h"
#include "rtl.h"
#include "flags.h"
#include "insn-config.h"
#include "expmed.h"
#include "dojump.h"
#include "explow.h"
#include "calls.h"
#include "emit-rtl.h"
#include "varasm.h"
#include "stmt.h"
#include "expr.h"
#include "tree-dfa.h"
#include "dumpfile.h"
#include "tree-inline.h"
#include "tree-affine.h"

/* FIXME: We compute address costs using RTL.  */
#include "recog.h"
#include "target.h"
#include "tree-ssa-address.h"

/* TODO -- handling of symbols (according to Richard Hendersons
   comments, http://gcc.gnu.org/ml/gcc-patches/2005-04/msg00949.html):

   There are at least 5 different kinds of symbols that we can run up against:

     (1) binds_local_p, small data area.
     (2) binds_local_p, eg local statics
     (3) !binds_local_p, eg global variables
     (4) thread local, local_exec
     (5) thread local, !local_exec

   Now, (1) won't appear often in an array context, but it certainly can.
   All you have to do is set -GN high enough, or explicitly mark any
   random object __attribute__((section (".sdata"))).

   All of these affect whether or not a symbol is in fact a valid address.
   The only one tested here is (3).  And that result may very well
   be incorrect for (4) or (5).

   An incorrect result here does not cause incorrect results out the
   back end, because the expander in expr.c validizes the address.  However
   it would be nice to improve the handling here in order to produce more
   precise results.  */

/* A "template" for memory address, used to determine whether the address is
   valid for mode.  */

typedef struct GTY (()) mem_addr_template {
  rtx ref;			/* The template.  */
  rtx * GTY ((skip)) step_p;	/* The point in template where the step should be
				   filled in.  */
  rtx * GTY ((skip)) off_p;	/* The point in template where the offset should
				   be filled in.  */
} mem_addr_template;


/* The templates.  Each of the low five bits of the index corresponds to one
   component of TARGET_MEM_REF being present, while the high bits identify
   the address space.  See TEMPL_IDX.  */

static GTY(()) vec<mem_addr_template, va_gc> *mem_addr_template_list;

#define TEMPL_IDX(AS, SYMBOL, BASE, INDEX, STEP, OFFSET) \
  (((int) (AS) << 5) \
   | ((SYMBOL != 0) << 4) \
   | ((BASE != 0) << 3) \
   | ((INDEX != 0) << 2) \
   | ((STEP != 0) << 1) \
   | (OFFSET != 0))

/* Stores address for memory reference with parameters SYMBOL, BASE, INDEX,
   STEP and OFFSET to *ADDR using address mode ADDRESS_MODE.  Stores pointers
   to where step is placed to *STEP_P and offset to *OFFSET_P.  */

static void
gen_addr_rtx (machine_mode address_mode,
	      rtx symbol, rtx base, rtx index, rtx step, rtx offset,
	      rtx *addr, rtx **step_p, rtx **offset_p)
{
  rtx act_elem;

  *addr = NULL_RTX;
  if (step_p)
    *step_p = NULL;
  if (offset_p)
    *offset_p = NULL;

  if (index)
    {
      act_elem = index;
      if (step)
	{
	  act_elem = gen_rtx_MULT (address_mode, act_elem, step);

	  if (step_p)
	    *step_p = &XEXP (act_elem, 1);
	}

      *addr = act_elem;
    }

  if (base && base != const0_rtx)
    {
      if (*addr)
	*addr = simplify_gen_binary (PLUS, address_mode, base, *addr);
      else
	*addr = base;
    }

  if (symbol)
    {
      act_elem = symbol;
      if (offset)
	{
	  act_elem = gen_rtx_PLUS (address_mode, act_elem, offset);

	  if (offset_p)
	    *offset_p = &XEXP (act_elem, 1);

	  if (GET_CODE (symbol) == SYMBOL_REF
	      || GET_CODE (symbol) == LABEL_REF
	      || GET_CODE (symbol) == CONST)
	    act_elem = gen_rtx_CONST (address_mode, act_elem);
	}

      if (*addr)
	*addr = gen_rtx_PLUS (address_mode, *addr, act_elem);
      else
	*addr = act_elem;
    }
  else if (offset)
    {
      if (*addr)
	{
	  *addr = gen_rtx_PLUS (address_mode, *addr, offset);
	  if (offset_p)
	    *offset_p = &XEXP (*addr, 1);
	}
      else
	{
	  *addr = offset;
	  if (offset_p)
	    *offset_p = addr;
	}
    }

  if (!*addr)
    *addr = const0_rtx;
}

/* Description of a memory address.  */

struct mem_address
{
  tree symbol, base, index, step, offset;
};

/* Returns address for TARGET_MEM_REF with parameters given by ADDR
   in address space AS.
   If REALLY_EXPAND is false, just make fake registers instead
   of really expanding the operands, and perform the expansion in-place
   by using one of the "templates".  */

rtx
addr_for_mem_ref (struct mem_address *addr, addr_space_t as,
		  bool really_expand)
{
  machine_mode address_mode = targetm.addr_space.address_mode (as);
  machine_mode pointer_mode = targetm.addr_space.pointer_mode (as);
  rtx address, sym, bse, idx, st, off;
  struct mem_addr_template *templ;

  if (addr->step && !integer_onep (addr->step))
    st = immed_wide_int_const (addr->step, pointer_mode);
  else
    st = NULL_RTX;

  if (addr->offset && !integer_zerop (addr->offset))
    {
      offset_int dc = offset_int::from (addr->offset, SIGNED);
      off = immed_wide_int_const (dc, pointer_mode);
    }
  else
    off = NULL_RTX;

  if (!really_expand)
    {
      unsigned int templ_index
	= TEMPL_IDX (as, addr->symbol, addr->base, addr->index, st, off);

      if (templ_index >= vec_safe_length (mem_addr_template_list))
	vec_safe_grow_cleared (mem_addr_template_list, templ_index + 1);

      /* Reuse the templates for addresses, so that we do not waste memory.  */
      templ = &(*mem_addr_template_list)[templ_index];
      if (!templ->ref)
	{
	  sym = (addr->symbol ?
		 gen_rtx_SYMBOL_REF (pointer_mode, ggc_strdup ("test_symbol"))
		 : NULL_RTX);
	  bse = (addr->base ?
		 gen_raw_REG (pointer_mode, LAST_VIRTUAL_REGISTER + 1)
		 : NULL_RTX);
	  idx = (addr->index ?
		 gen_raw_REG (pointer_mode, LAST_VIRTUAL_REGISTER + 2)
		 : NULL_RTX);

	  gen_addr_rtx (pointer_mode, sym, bse, idx,
			st? const0_rtx : NULL_RTX,
			off? const0_rtx : NULL_RTX,
			&templ->ref,
			&templ->step_p,
			&templ->off_p);
	}

      if (st)
	*templ->step_p = st;
      if (off)
	*templ->off_p = off;

      return templ->ref;
    }

  /* Otherwise really expand the expressions.  */
  sym = (addr->symbol
	 ? expand_expr (addr->symbol, NULL_RTX, pointer_mode, EXPAND_NORMAL)
	 : NULL_RTX);
  bse = (addr->base
	 ? expand_expr (addr->base, NULL_RTX, pointer_mode, EXPAND_NORMAL)
	 : NULL_RTX);
  idx = (addr->index
	 ? expand_expr (addr->index, NULL_RTX, pointer_mode, EXPAND_NORMAL)
	 : NULL_RTX);

  gen_addr_rtx (pointer_mode, sym, bse, idx, st, off, &address, NULL, NULL);
  if (pointer_mode != address_mode)
    address = convert_memory_address (address_mode, address);
  return address;
}

/* implement addr_for_mem_ref() directly from a tree, which avoids exporting
   the mem_address structure.  */

rtx
addr_for_mem_ref (tree exp, addr_space_t as, bool really_expand)
{
  struct mem_address addr;
  get_address_description (exp, &addr);
  return addr_for_mem_ref (&addr, as, really_expand);
}

/* Returns address of MEM_REF in TYPE.  */

tree
tree_mem_ref_addr (tree type, tree mem_ref)
{
  tree addr;
  tree act_elem;
  tree step = TMR_STEP (mem_ref), offset = TMR_OFFSET (mem_ref);
  tree addr_base = NULL_TREE, addr_off = NULL_TREE;

  addr_base = fold_convert (type, TMR_BASE (mem_ref));

  act_elem = TMR_INDEX (mem_ref);
  if (act_elem)
    {
      if (step)
	act_elem = fold_build2 (MULT_EXPR, TREE_TYPE (act_elem),
				act_elem, step);
      addr_off = act_elem;
    }

  act_elem = TMR_INDEX2 (mem_ref);
  if (act_elem)
    {
      if (addr_off)
	addr_off = fold_build2 (PLUS_EXPR, TREE_TYPE (addr_off),
				addr_off, act_elem);
      else
	addr_off = act_elem;
    }

  if (offset && !integer_zerop (offset))
    {
      if (addr_off)
	addr_off = fold_build2 (PLUS_EXPR, TREE_TYPE (addr_off), addr_off,
				fold_convert (TREE_TYPE (addr_off), offset));
      else
	addr_off = offset;
    }

  if (addr_off)
    addr = fold_build_pointer_plus (addr_base, addr_off);
  else
    addr = addr_base;

  return addr;
}

/* Returns true if a memory reference in MODE and with parameters given by
   ADDR is valid on the current target.  */

static bool
valid_mem_ref_p (machine_mode mode, addr_space_t as,
		 struct mem_address *addr)
{
  rtx address;

  address = addr_for_mem_ref (addr, as, false);
  if (!address)
    return false;

  return memory_address_addr_space_p (mode, address, as);
}

/* Checks whether a TARGET_MEM_REF with type TYPE and parameters given by ADDR
   is valid on the current target and if so, creates and returns the
   TARGET_MEM_REF.  If VERIFY is false omit the verification step.  */

static tree
create_mem_ref_raw (tree type, tree alias_ptr_type, struct mem_address *addr,
		    bool verify)
{
  tree base, index2;

  if (verify
      && !valid_mem_ref_p (TYPE_MODE (type), TYPE_ADDR_SPACE (type), addr))
    return NULL_TREE;

  if (addr->step && integer_onep (addr->step))
    addr->step = NULL_TREE;

  if (addr->offset)
    addr->offset = fold_convert (alias_ptr_type, addr->offset);
  else
    addr->offset = build_int_cst (alias_ptr_type, 0);

  if (addr->symbol)
    {
      base = addr->symbol;
      index2 = addr->base;
    }
  else if (addr->base
	   && POINTER_TYPE_P (TREE_TYPE (addr->base)))
    {
      base = addr->base;
      index2 = NULL_TREE;
    }
  else
    {
      base = build_int_cst (ptr_type_node, 0);
      index2 = addr->base;
    }

  /* If possible use a plain MEM_REF instead of a TARGET_MEM_REF.
     ???  As IVOPTs does not follow restrictions to where the base
     pointer may point to create a MEM_REF only if we know that
     base is valid.  */
  if ((TREE_CODE (base) == ADDR_EXPR || TREE_CODE (base) == INTEGER_CST)
      && (!index2 || integer_zerop (index2))
      && (!addr->index || integer_zerop (addr->index)))
    return fold_build2 (MEM_REF, type, base, addr->offset);

  return build5 (TARGET_MEM_REF, type,
		 base, addr->offset, addr->index, addr->step, index2);
}

/* Returns true if OBJ is an object whose address is a link time constant.  */

static bool
fixed_address_object_p (tree obj)
{
  return (TREE_CODE (obj) == VAR_DECL
	  && (TREE_STATIC (obj)
	      || DECL_EXTERNAL (obj))
	  && ! DECL_DLLIMPORT_P (obj));
}

/* If ADDR contains an address of object that is a link time constant,
   move it to PARTS->symbol.  */

static void
move_fixed_address_to_symbol (struct mem_address *parts, aff_tree *addr)
{
  unsigned i;
  tree val = NULL_TREE;

  for (i = 0; i < addr->n; i++)
    {
      if (addr->elts[i].coef != 1)
	continue;

      val = addr->elts[i].val;
      if (TREE_CODE (val) == ADDR_EXPR
	  && fixed_address_object_p (TREE_OPERAND (val, 0)))
	break;
    }

  if (i == addr->n)
    return;

  parts->symbol = val;
  aff_combination_remove_elt (addr, i);
}

/* If ADDR contains an instance of BASE_HINT, move it to PARTS->base.  */

static void
move_hint_to_base (tree type, struct mem_address *parts, tree base_hint,
		   aff_tree *addr)
{
  unsigned i;
  tree val = NULL_TREE;
  int qual;

  for (i = 0; i < addr->n; i++)
    {
      if (addr->elts[i].coef != 1)
	continue;

      val = addr->elts[i].val;
      if (operand_equal_p (val, base_hint, 0))
	break;
    }

  if (i == addr->n)
    return;

  /* Cast value to appropriate pointer type.  We cannot use a pointer
     to TYPE directly, as the back-end will assume registers of pointer
     type are aligned, and just the base itself may not actually be.
     We use void pointer to the type's address space instead.  */
  qual = ENCODE_QUAL_ADDR_SPACE (TYPE_ADDR_SPACE (type));
  type = build_qualified_type (void_type_node, qual);
  parts->base = fold_convert (build_pointer_type (type), val);
  aff_combination_remove_elt (addr, i);
}

/* If ADDR contains an address of a dereferenced pointer, move it to
   PARTS->base.  */

static void
move_pointer_to_base (struct mem_address *parts, aff_tree *addr)
{
  unsigned i;
  tree val = NULL_TREE;

  for (i = 0; i < addr->n; i++)
    {
      if (addr->elts[i].coef != 1)
	continue;

      val = addr->elts[i].val;
      if (POINTER_TYPE_P (TREE_TYPE (val)))
	break;
    }

  if (i == addr->n)
    return;

  parts->base = val;
  aff_combination_remove_elt (addr, i);
}

/* Moves the loop variant part V in linear address ADDR to be the index
   of PARTS.  */

static void
move_variant_to_index (struct mem_address *parts, aff_tree *addr, tree v)
{
  unsigned i;
  tree val = NULL_TREE;

  gcc_assert (!parts->index);
  for (i = 0; i < addr->n; i++)
    {
      val = addr->elts[i].val;
      if (operand_equal_p (val, v, 0))
	break;
    }

  if (i == addr->n)
    return;

  parts->index = fold_convert (sizetype, val);
  parts->step = wide_int_to_tree (sizetype, addr->elts[i].coef);
  aff_combination_remove_elt (addr, i);
}

/* Adds ELT to PARTS.  */

static void
add_to_parts (struct mem_address *parts, tree elt)
{
  tree type;

  if (!parts->index)
    {
      parts->index = fold_convert (sizetype, elt);
      return;
    }

  if (!parts->base)
    {
      parts->base = elt;
      return;
    }

  /* Add ELT to base.  */
  type = TREE_TYPE (parts->base);
  if (POINTER_TYPE_P (type))
    parts->base = fold_build_pointer_plus (parts->base, elt);
  else
    parts->base = fold_build2 (PLUS_EXPR, type,
			       parts->base, elt);
}

/* Finds the most expensive multiplication in ADDR that can be
   expressed in an addressing mode and move the corresponding
   element(s) to PARTS.  */

static void
most_expensive_mult_to_index (tree type, struct mem_address *parts,
			      aff_tree *addr, bool speed)
{
  addr_space_t as = TYPE_ADDR_SPACE (type);
  machine_mode address_mode = targetm.addr_space.address_mode (as);
  HOST_WIDE_INT coef;
  unsigned best_mult_cost = 0, acost;
  tree mult_elt = NULL_TREE, elt;
  unsigned i, j;
  enum tree_code op_code;

  offset_int best_mult = 0;
  for (i = 0; i < addr->n; i++)
    {
      if (!wi::fits_shwi_p (addr->elts[i].coef))
	continue;

      coef = addr->elts[i].coef.to_shwi ();
      if (coef == 1
	  || !multiplier_allowed_in_address_p (coef, TYPE_MODE (type), as))
	continue;

      acost = mult_by_coeff_cost (coef, address_mode, speed);

      if (acost > best_mult_cost)
	{
	  best_mult_cost = acost;
	  best_mult = offset_int::from (addr->elts[i].coef, SIGNED);
	}
    }

  if (!best_mult_cost)
    return;

  /* Collect elements multiplied by best_mult.  */
  for (i = j = 0; i < addr->n; i++)
    {
      offset_int amult = offset_int::from (addr->elts[i].coef, SIGNED);
      offset_int amult_neg = -wi::sext (amult, TYPE_PRECISION (addr->type));

      if (amult == best_mult)
	op_code = PLUS_EXPR;
      else if (amult_neg == best_mult)
	op_code = MINUS_EXPR;
      else
	{
	  addr->elts[j] = addr->elts[i];
	  j++;
	  continue;
	}

      elt = fold_convert (sizetype, addr->elts[i].val);
      if (mult_elt)
	mult_elt = fold_build2 (op_code, sizetype, mult_elt, elt);
      else if (op_code == PLUS_EXPR)
	mult_elt = elt;
      else
	mult_elt = fold_build1 (NEGATE_EXPR, sizetype, elt);
    }
  addr->n = j;

  parts->index = mult_elt;
  parts->step = wide_int_to_tree (sizetype, best_mult);
}

/* Splits address ADDR for a memory access of type TYPE into PARTS.
   If BASE_HINT is non-NULL, it specifies an SSA name to be used
   preferentially as base of the reference, and IV_CAND is the selected
   iv candidate used in ADDR.

   TODO -- be more clever about the distribution of the elements of ADDR
   to PARTS.  Some architectures do not support anything but single
   register in address, possibly with a small integer offset; while
   create_mem_ref will simplify the address to an acceptable shape
   later, it would be more efficient to know that asking for complicated
   addressing modes is useless.  */

static void
addr_to_parts (tree type, aff_tree *addr, tree iv_cand,
	       tree base_hint, struct mem_address *parts,
               bool speed)
{
  tree part;
  unsigned i;

  parts->symbol = NULL_TREE;
  parts->base = NULL_TREE;
  parts->index = NULL_TREE;
  parts->step = NULL_TREE;

  if (addr->offset != 0)
    parts->offset = wide_int_to_tree (sizetype, addr->offset);
  else
    parts->offset = NULL_TREE;

  /* Try to find a symbol.  */
  move_fixed_address_to_symbol (parts, addr);

  /* No need to do address parts reassociation if the number of parts
     is <= 2 -- in that case, no loop invariant code motion can be
     exposed.  */

  if (!base_hint && (addr->n > 2))
    move_variant_to_index (parts, addr, iv_cand);

  /* First move the most expensive feasible multiplication
     to index.  */
  if (!parts->index)
    most_expensive_mult_to_index (type, parts, addr, speed);

  /* Try to find a base of the reference.  Since at the moment
     there is no reliable way how to distinguish between pointer and its
     offset, this is just a guess.  */
  if (!parts->symbol && base_hint)
    move_hint_to_base (type, parts, base_hint, addr);
  if (!parts->symbol && !parts->base)
    move_pointer_to_base (parts, addr);

  /* Then try to process the remaining elements.  */
  for (i = 0; i < addr->n; i++)
    {
      part = fold_convert (sizetype, addr->elts[i].val);
      if (addr->elts[i].coef != 1)
	part = fold_build2 (MULT_EXPR, sizetype, part,
			    wide_int_to_tree (sizetype, addr->elts[i].coef));
      add_to_parts (parts, part);
    }
  if (addr->rest)
    add_to_parts (parts, fold_convert (sizetype, addr->rest));
}

/* Force the PARTS to register.  */

static void
gimplify_mem_ref_parts (gimple_stmt_iterator *gsi, struct mem_address *parts)
{
  if (parts->base)
    parts->base = force_gimple_operand_gsi_1 (gsi, parts->base,
					    is_gimple_mem_ref_addr, NULL_TREE,
					    true, GSI_SAME_STMT);
  if (parts->index)
    parts->index = force_gimple_operand_gsi (gsi, parts->index,
					     true, NULL_TREE,
					     true, GSI_SAME_STMT);
}

/* Creates and returns a TARGET_MEM_REF for address ADDR.  If necessary
   computations are emitted in front of GSI.  TYPE is the mode
   of created memory reference. IV_CAND is the selected iv candidate in ADDR,
   and BASE_HINT is non NULL if IV_CAND comes from a base address
   object.  */

tree
create_mem_ref (gimple_stmt_iterator *gsi, tree type, aff_tree *addr,
		tree alias_ptr_type, tree iv_cand, tree base_hint, bool speed)
{
  tree mem_ref, tmp;
  struct mem_address parts;

  addr_to_parts (type, addr, iv_cand, base_hint, &parts, speed);
  gimplify_mem_ref_parts (gsi, &parts);
  mem_ref = create_mem_ref_raw (type, alias_ptr_type, &parts, true);
  if (mem_ref)
    return mem_ref;

  /* The expression is too complicated.  Try making it simpler.  */

  if (parts.step && !integer_onep (parts.step))
    {
      /* Move the multiplication to index.  */
      gcc_assert (parts.index);
      parts.index = force_gimple_operand_gsi (gsi,
				fold_build2 (MULT_EXPR, sizetype,
					     parts.index, parts.step),
				true, NULL_TREE, true, GSI_SAME_STMT);
      parts.step = NULL_TREE;

      mem_ref = create_mem_ref_raw (type, alias_ptr_type, &parts, true);
      if (mem_ref)
	return mem_ref;
    }

  if (parts.symbol)
    {
      tmp = parts.symbol;
      gcc_assert (is_gimple_val (tmp));

      /* Add the symbol to base, eventually forcing it to register.  */
      if (parts.base)
	{
	  gcc_assert (useless_type_conversion_p
				(sizetype, TREE_TYPE (parts.base)));

	  if (parts.index)
	    {
	      parts.base = force_gimple_operand_gsi_1 (gsi,
			fold_build_pointer_plus (tmp, parts.base),
			is_gimple_mem_ref_addr, NULL_TREE, true, GSI_SAME_STMT);
	    }
	  else
	    {
	      parts.index = parts.base;
	      parts.base = tmp;
	    }
	}
      else
	parts.base = tmp;
      parts.symbol = NULL_TREE;

      mem_ref = create_mem_ref_raw (type, alias_ptr_type, &parts, true);
      if (mem_ref)
	return mem_ref;
    }

  if (parts.index)
    {
      /* Add index to base.  */
      if (parts.base)
	{
	  parts.base = force_gimple_operand_gsi_1 (gsi,
			fold_build_pointer_plus (parts.base, parts.index),
			is_gimple_mem_ref_addr, NULL_TREE, true, GSI_SAME_STMT);
	}
      else
	parts.base = parts.index;
      parts.index = NULL_TREE;

      mem_ref = create_mem_ref_raw (type, alias_ptr_type, &parts, true);
      if (mem_ref)
	return mem_ref;
    }

  if (parts.offset && !integer_zerop (parts.offset))
    {
      /* Try adding offset to base.  */
      if (parts.base)
	{
	  parts.base = force_gimple_operand_gsi_1 (gsi,
			fold_build_pointer_plus (parts.base, parts.offset),
			is_gimple_mem_ref_addr, NULL_TREE, true, GSI_SAME_STMT);
	}
      else
	parts.base = parts.offset;

      parts.offset = NULL_TREE;

      mem_ref = create_mem_ref_raw (type, alias_ptr_type, &parts, true);
      if (mem_ref)
	return mem_ref;
    }

  /* Verify that the address is in the simplest possible shape
     (only a register).  If we cannot create such a memory reference,
     something is really wrong.  */
  gcc_assert (parts.symbol == NULL_TREE);
  gcc_assert (parts.index == NULL_TREE);
  gcc_assert (!parts.step || integer_onep (parts.step));
  gcc_assert (!parts.offset || integer_zerop (parts.offset));
  gcc_unreachable ();
}

/* Copies components of the address from OP to ADDR.  */

void
get_address_description (tree op, struct mem_address *addr)
{
  if (TREE_CODE (TMR_BASE (op)) == ADDR_EXPR)
    {
      addr->symbol = TMR_BASE (op);
      addr->base = TMR_INDEX2 (op);
    }
  else
    {
      addr->symbol = NULL_TREE;
      if (TMR_INDEX2 (op))
	{
	  gcc_assert (integer_zerop (TMR_BASE (op)));
	  addr->base = TMR_INDEX2 (op);
	}
      else
	addr->base = TMR_BASE (op);
    }
  addr->index = TMR_INDEX (op);
  addr->step = TMR_STEP (op);
  addr->offset = TMR_OFFSET (op);
}

/* Copies the reference information from OLD_REF to NEW_REF, where
   NEW_REF should be either a MEM_REF or a TARGET_MEM_REF.  */

void
copy_ref_info (tree new_ref, tree old_ref)
{
  tree new_ptr_base = NULL_TREE;

  gcc_assert (TREE_CODE (new_ref) == MEM_REF
	      || TREE_CODE (new_ref) == TARGET_MEM_REF);

  TREE_SIDE_EFFECTS (new_ref) = TREE_SIDE_EFFECTS (old_ref);
  TREE_THIS_VOLATILE (new_ref) = TREE_THIS_VOLATILE (old_ref);

  new_ptr_base = TREE_OPERAND (new_ref, 0);

  /* We can transfer points-to information from an old pointer
     or decl base to the new one.  */
  if (new_ptr_base
      && TREE_CODE (new_ptr_base) == SSA_NAME
      && !SSA_NAME_PTR_INFO (new_ptr_base))
    {
      tree base = get_base_address (old_ref);
      if (!base)
	;
      else if ((TREE_CODE (base) == MEM_REF
		|| TREE_CODE (base) == TARGET_MEM_REF)
	       && TREE_CODE (TREE_OPERAND (base, 0)) == SSA_NAME
	       && SSA_NAME_PTR_INFO (TREE_OPERAND (base, 0)))
	{
	  struct ptr_info_def *new_pi;
	  unsigned int align, misalign;

	  duplicate_ssa_name_ptr_info
	    (new_ptr_base, SSA_NAME_PTR_INFO (TREE_OPERAND (base, 0)));
	  new_pi = SSA_NAME_PTR_INFO (new_ptr_base);
	  /* We have to be careful about transferring alignment information.  */
	  if (get_ptr_info_alignment (new_pi, &align, &misalign)
	      && TREE_CODE (old_ref) == MEM_REF
	      && !(TREE_CODE (new_ref) == TARGET_MEM_REF
		   && (TMR_INDEX2 (new_ref)
		       || (TMR_STEP (new_ref)
			   && (TREE_INT_CST_LOW (TMR_STEP (new_ref))
			       < align)))))
	    {
	      unsigned int inc = (mem_ref_offset (old_ref).to_short_addr ()
				  - mem_ref_offset (new_ref).to_short_addr ());
	      adjust_ptr_info_misalignment (new_pi, inc);
	    }
	  else
	    mark_ptr_info_alignment_unknown (new_pi);
	}
      else if (TREE_CODE (base) == VAR_DECL
	       || TREE_CODE (base) == PARM_DECL
	       || TREE_CODE (base) == RESULT_DECL)
	{
	  struct ptr_info_def *pi = get_ptr_info (new_ptr_base);
	  pt_solution_set_var (&pi->pt, base);
	}
    }
}

/* Move constants in target_mem_ref REF to offset.  Returns the new target
   mem ref if anything changes, NULL_TREE otherwise.  */

tree
maybe_fold_tmr (tree ref)
{
  struct mem_address addr;
  bool changed = false;
  tree new_ref, off;

  get_address_description (ref, &addr);

  if (addr.base
      && TREE_CODE (addr.base) == INTEGER_CST
      && !integer_zerop (addr.base))
    {
      addr.offset = fold_binary_to_constant (PLUS_EXPR,
					     TREE_TYPE (addr.offset),
					     addr.offset, addr.base);
      addr.base = NULL_TREE;
      changed = true;
    }

  if (addr.symbol
      && TREE_CODE (TREE_OPERAND (addr.symbol, 0)) == MEM_REF)
    {
      addr.offset = fold_binary_to_constant
			(PLUS_EXPR, TREE_TYPE (addr.offset),
			 addr.offset,
			 TREE_OPERAND (TREE_OPERAND (addr.symbol, 0), 1));
      addr.symbol = TREE_OPERAND (TREE_OPERAND (addr.symbol, 0), 0);
      changed = true;
    }
  else if (addr.symbol
	   && handled_component_p (TREE_OPERAND (addr.symbol, 0)))
    {
      HOST_WIDE_INT offset;
      addr.symbol = build_fold_addr_expr
		      (get_addr_base_and_unit_offset
		         (TREE_OPERAND (addr.symbol, 0), &offset));
      addr.offset = int_const_binop (PLUS_EXPR,
				     addr.offset, size_int (offset));
      changed = true;
    }

  if (addr.index && TREE_CODE (addr.index) == INTEGER_CST)
    {
      off = addr.index;
      if (addr.step)
	{
	  off = fold_binary_to_constant (MULT_EXPR, sizetype,
					 off, addr.step);
	  addr.step = NULL_TREE;
	}

      addr.offset = fold_binary_to_constant (PLUS_EXPR,
					     TREE_TYPE (addr.offset),
					     addr.offset, off);
      addr.index = NULL_TREE;
      changed = true;
    }

  if (!changed)
    return NULL_TREE;

  /* If we have propagated something into this TARGET_MEM_REF and thus
     ended up folding it, always create a new TARGET_MEM_REF regardless
     if it is valid in this for on the target - the propagation result
     wouldn't be anyway.  */
  new_ref = create_mem_ref_raw (TREE_TYPE (ref),
			        TREE_TYPE (addr.offset), &addr, false);
  TREE_SIDE_EFFECTS (new_ref) = TREE_SIDE_EFFECTS (ref);
  TREE_THIS_VOLATILE (new_ref) = TREE_THIS_VOLATILE (ref);
  return new_ref;
}

/* Dump PARTS to FILE.  */

extern void dump_mem_address (FILE *, struct mem_address *);
void
dump_mem_address (FILE *file, struct mem_address *parts)
{
  if (parts->symbol)
    {
      fprintf (file, "symbol: ");
      print_generic_expr (file, TREE_OPERAND (parts->symbol, 0), TDF_SLIM);
      fprintf (file, "\n");
    }
  if (parts->base)
    {
      fprintf (file, "base: ");
      print_generic_expr (file, parts->base, TDF_SLIM);
      fprintf (file, "\n");
    }
  if (parts->index)
    {
      fprintf (file, "index: ");
      print_generic_expr (file, parts->index, TDF_SLIM);
      fprintf (file, "\n");
    }
  if (parts->step)
    {
      fprintf (file, "step: ");
      print_generic_expr (file, parts->step, TDF_SLIM);
      fprintf (file, "\n");
    }
  if (parts->offset)
    {
      fprintf (file, "offset: ");
      print_generic_expr (file, parts->offset, TDF_SLIM);
      fprintf (file, "\n");
    }
}

#include "gt-tree-ssa-address.h"
