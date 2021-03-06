/**************************************************************************
 *
 * Copyright 2009 VMware, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL VMWARE AND/OR ITS SUPPLIERS BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/


/**
 * @file
 * Blend LLVM IR generation -- AoS layout.
 *
 * AoS blending is in general much slower than SoA, but there are some cases
 * where it might be faster. In particular, if a pixel is rendered only once
 * then the overhead of tiling and untiling will dominate over the speedup that
 * SoA gives. So we might want to detect such cases and fallback to AoS in the
 * future, but for now this function is here for historical/benchmarking
 * purposes.
 *
 * Run lp_blend_test after any change to this file.
 *
 * @author Jose Fonseca <jfonseca@vmware.com>
 */


#include "pipe/p_state.h"
#include "util/u_debug.h"
#include "util/u_format.h"

#include "gallivm/lp_bld_type.h"
#include "gallivm/lp_bld_const.h"
#include "gallivm/lp_bld_arit.h"
#include "gallivm/lp_bld_logic.h"
#include "gallivm/lp_bld_swizzle.h"
#include "gallivm/lp_bld_bitarit.h"
#include "gallivm/lp_bld_debug.h"

#include "lp_bld_blend.h"


/**
 * We may the same values several times, so we keep them here to avoid
 * recomputing them. Also reusing the values allows us to do simplifications
 * that LLVM optimization passes wouldn't normally be able to do.
 */
struct lp_build_blend_aos_context
{
   struct lp_build_context base;
   
   LLVMValueRef src;
   LLVMValueRef dst;
   LLVMValueRef const_;

   LLVMValueRef inv_src;
   LLVMValueRef inv_dst;
   LLVMValueRef inv_const;
   LLVMValueRef saturate;

   LLVMValueRef rgb_src_factor;
   LLVMValueRef alpha_src_factor;
   LLVMValueRef rgb_dst_factor;
   LLVMValueRef alpha_dst_factor;
};


static LLVMValueRef
lp_build_blend_factor_unswizzled(struct lp_build_blend_aos_context *bld,
                                 unsigned factor,
                                 boolean alpha)
{
   switch (factor) {
   case PIPE_BLENDFACTOR_ZERO:
      return bld->base.zero;
   case PIPE_BLENDFACTOR_ONE:
      return bld->base.one;
   case PIPE_BLENDFACTOR_SRC_COLOR:
   case PIPE_BLENDFACTOR_SRC_ALPHA:
      return bld->src;
   case PIPE_BLENDFACTOR_DST_COLOR:
   case PIPE_BLENDFACTOR_DST_ALPHA:
      return bld->dst;
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
      if(alpha)
         return bld->base.one;
      else {
         if(!bld->inv_dst)
            bld->inv_dst = lp_build_comp(&bld->base, bld->dst);
         if(!bld->saturate)
            bld->saturate = lp_build_min(&bld->base, bld->src, bld->inv_dst);
         return bld->saturate;
      }
   case PIPE_BLENDFACTOR_CONST_COLOR:
   case PIPE_BLENDFACTOR_CONST_ALPHA:
      return bld->const_;
   case PIPE_BLENDFACTOR_SRC1_COLOR:
   case PIPE_BLENDFACTOR_SRC1_ALPHA:
      /* TODO */
      assert(0);
      return bld->base.zero;
   case PIPE_BLENDFACTOR_INV_SRC_COLOR:
   case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
      if(!bld->inv_src)
         bld->inv_src = lp_build_comp(&bld->base, bld->src);
      return bld->inv_src;
   case PIPE_BLENDFACTOR_INV_DST_COLOR:
   case PIPE_BLENDFACTOR_INV_DST_ALPHA:
      if(!bld->inv_dst)
         bld->inv_dst = lp_build_comp(&bld->base, bld->dst);
      return bld->inv_dst;
   case PIPE_BLENDFACTOR_INV_CONST_COLOR:
   case PIPE_BLENDFACTOR_INV_CONST_ALPHA:
      if(!bld->inv_const)
         bld->inv_const = lp_build_comp(&bld->base, bld->const_);
      return bld->inv_const;
   case PIPE_BLENDFACTOR_INV_SRC1_COLOR:
   case PIPE_BLENDFACTOR_INV_SRC1_ALPHA:
      /* TODO */
      assert(0);
      return bld->base.zero;
   default:
      assert(0);
      return bld->base.zero;
   }
}


enum lp_build_blend_swizzle {
   LP_BUILD_BLEND_SWIZZLE_RGBA = 0,
   LP_BUILD_BLEND_SWIZZLE_AAAA = 1
};


/**
 * How should we shuffle the base factor.
 */
static enum lp_build_blend_swizzle
lp_build_blend_factor_swizzle(unsigned factor)
{
   switch (factor) {
   case PIPE_BLENDFACTOR_ONE:
   case PIPE_BLENDFACTOR_ZERO:
   case PIPE_BLENDFACTOR_SRC_COLOR:
   case PIPE_BLENDFACTOR_DST_COLOR:
   case PIPE_BLENDFACTOR_CONST_COLOR:
   case PIPE_BLENDFACTOR_SRC1_COLOR:
   case PIPE_BLENDFACTOR_INV_SRC_COLOR:
   case PIPE_BLENDFACTOR_INV_DST_COLOR:
   case PIPE_BLENDFACTOR_INV_CONST_COLOR:
   case PIPE_BLENDFACTOR_INV_SRC1_COLOR:
      return LP_BUILD_BLEND_SWIZZLE_RGBA;
   case PIPE_BLENDFACTOR_SRC_ALPHA:
   case PIPE_BLENDFACTOR_DST_ALPHA:
   case PIPE_BLENDFACTOR_SRC_ALPHA_SATURATE:
   case PIPE_BLENDFACTOR_SRC1_ALPHA:
   case PIPE_BLENDFACTOR_CONST_ALPHA:
   case PIPE_BLENDFACTOR_INV_SRC_ALPHA:
   case PIPE_BLENDFACTOR_INV_DST_ALPHA:
   case PIPE_BLENDFACTOR_INV_CONST_ALPHA:
   case PIPE_BLENDFACTOR_INV_SRC1_ALPHA:
      return LP_BUILD_BLEND_SWIZZLE_AAAA;
   default:
      assert(0);
      return LP_BUILD_BLEND_SWIZZLE_RGBA;
   }
}


static LLVMValueRef
lp_build_blend_swizzle(struct lp_build_blend_aos_context *bld,
                       LLVMValueRef rgb, 
                       LLVMValueRef alpha, 
                       enum lp_build_blend_swizzle rgb_swizzle,
                       unsigned alpha_swizzle)
{
   LLVMValueRef swizzled_rgb;

   switch (rgb_swizzle) {
   case LP_BUILD_BLEND_SWIZZLE_RGBA:
      swizzled_rgb = rgb;
      break;
   case LP_BUILD_BLEND_SWIZZLE_AAAA:
      swizzled_rgb = lp_build_swizzle_scalar_aos(&bld->base, rgb, alpha_swizzle);
      break;
   default:
      assert(0);
      swizzled_rgb = bld->base.undef;
   }

   if (rgb != alpha) {
      swizzled_rgb = lp_build_select_aos(&bld->base, 1 << alpha_swizzle,
                                         alpha, swizzled_rgb);
   }

   return swizzled_rgb;
}


/**
 * @sa http://www.opengl.org/sdk/docs/man/xhtml/glBlendFuncSeparate.xml
 */
static LLVMValueRef
lp_build_blend_factor(struct lp_build_blend_aos_context *bld,
                      LLVMValueRef factor1,
                      unsigned rgb_factor,
                      unsigned alpha_factor,
                      unsigned alpha_swizzle)
{
   LLVMValueRef rgb_factor_;
   LLVMValueRef alpha_factor_;
   LLVMValueRef factor2;
   enum lp_build_blend_swizzle rgb_swizzle;

   rgb_factor_   = lp_build_blend_factor_unswizzled(bld, rgb_factor,   FALSE);
   alpha_factor_ = lp_build_blend_factor_unswizzled(bld, alpha_factor, TRUE);

   rgb_swizzle = lp_build_blend_factor_swizzle(rgb_factor);

   factor2 = lp_build_blend_swizzle(bld, rgb_factor_, alpha_factor_, rgb_swizzle, alpha_swizzle);

   return lp_build_mul(&bld->base, factor1, factor2);
}


/**
 * Is (a OP b) == (b OP a)?
 */
boolean
lp_build_blend_func_commutative(unsigned func)
{
   switch (func) {
   case PIPE_BLEND_ADD:
   case PIPE_BLEND_MIN:
   case PIPE_BLEND_MAX:
      return TRUE;
   case PIPE_BLEND_SUBTRACT:
   case PIPE_BLEND_REVERSE_SUBTRACT:
      return FALSE;
   default:
      assert(0);
      return TRUE;
   }
}


boolean
lp_build_blend_func_reverse(unsigned rgb_func, unsigned alpha_func)
{
   if(rgb_func == alpha_func)
      return FALSE;
   if(rgb_func == PIPE_BLEND_SUBTRACT && alpha_func == PIPE_BLEND_REVERSE_SUBTRACT)
      return TRUE;
   if(rgb_func == PIPE_BLEND_REVERSE_SUBTRACT && alpha_func == PIPE_BLEND_SUBTRACT)
      return TRUE;
   return FALSE;
}


/**
 * @sa http://www.opengl.org/sdk/docs/man/xhtml/glBlendEquationSeparate.xml
 */
LLVMValueRef
lp_build_blend_func(struct lp_build_context *bld,
                    unsigned func,
                    LLVMValueRef term1, 
                    LLVMValueRef term2)
{
   switch (func) {
   case PIPE_BLEND_ADD:
      return lp_build_add(bld, term1, term2);
   case PIPE_BLEND_SUBTRACT:
      return lp_build_sub(bld, term1, term2);
   case PIPE_BLEND_REVERSE_SUBTRACT:
      return lp_build_sub(bld, term2, term1);
   case PIPE_BLEND_MIN:
      return lp_build_min(bld, term1, term2);
   case PIPE_BLEND_MAX:
      return lp_build_max(bld, term1, term2);
   default:
      assert(0);
      return bld->zero;
   }
}


/**
 * Performs blending of src and dst pixels
 *
 * @param blend         the blend state of the shader variant
 * @param cbuf_format   format of the colour buffer
 * @param type          data type of the pixel vector
 * @param rt            rt number
 * @param src           blend src
 * @param dst           blend dst
 * @param mask          optional mask to apply to the blending result
 * @param const_        const blend color
 * @param swizzle       swizzle values for RGBA
 *
 * @return the result of blending src and dst
 */
LLVMValueRef
lp_build_blend_aos(struct gallivm_state *gallivm,
                   const struct pipe_blend_state *blend,
                   const enum pipe_format *cbuf_format,
                   struct lp_type type,
                   unsigned rt,
                   LLVMValueRef src,
                   LLVMValueRef dst,
                   LLVMValueRef mask,
                   LLVMValueRef const_,
                   const unsigned char swizzle[4])
{
   struct lp_build_blend_aos_context bld;
   LLVMValueRef src_term;
   LLVMValueRef dst_term;
   LLVMValueRef result;
   unsigned alpha_swizzle = swizzle[3];
   boolean fullcolormask;

   /* Setup build context */
   memset(&bld, 0, sizeof bld);
   lp_build_context_init(&bld.base, gallivm, type);
   bld.src = src;
   bld.dst = dst;
   bld.const_ = const_;

   if (!blend->rt[rt].blend_enable) {
      result = src;
   } else {

      /* TODO: There are still a few optimization opportunities here. For certain
       * combinations it is possible to reorder the operations and therefore saving
       * some instructions. */

      src_term = lp_build_blend_factor(&bld, src, blend->rt[rt].rgb_src_factor,
                                       blend->rt[rt].alpha_src_factor, alpha_swizzle);
      dst_term = lp_build_blend_factor(&bld, dst, blend->rt[rt].rgb_dst_factor,
                                       blend->rt[rt].alpha_dst_factor, alpha_swizzle);

      lp_build_name(src_term, "src_term");
      lp_build_name(dst_term, "dst_term");

      if(blend->rt[rt].rgb_func == blend->rt[rt].alpha_func) {
         result = lp_build_blend_func(&bld.base, blend->rt[rt].rgb_func, src_term, dst_term);
      }
      else {
         /* Seperate RGB / A functions */

         LLVMValueRef rgb;
         LLVMValueRef alpha;

         rgb   = lp_build_blend_func(&bld.base, blend->rt[rt].rgb_func,   src_term, dst_term);
         alpha = lp_build_blend_func(&bld.base, blend->rt[rt].alpha_func, src_term, dst_term);

         result = lp_build_blend_swizzle(&bld, rgb, alpha, LP_BUILD_BLEND_SWIZZLE_RGBA, alpha_swizzle);
      }
   }

   /* Check if color mask is necessary */
   fullcolormask = util_format_colormask_full(util_format_description(cbuf_format[rt]), blend->rt[rt].colormask);

   if (!fullcolormask) {
      LLVMValueRef color_mask;

      color_mask = lp_build_const_mask_aos_swizzled(gallivm, bld.base.type, blend->rt[rt].colormask, swizzle);
      lp_build_name(color_mask, "color_mask");

      /* Combine with input mask if necessary */
      if (mask) {
         mask = lp_build_and(&bld.base, color_mask, mask);
      } else {
         mask = color_mask;
      }
   }

   /* Apply mask, if one exists */
   if (mask) {
      result = lp_build_select(&bld.base, mask, result, dst);
   }

   return result;
}
