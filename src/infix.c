/*  You may distribute under the terms of either the GNU General Public License
 *  or the Artistic License (the same terms as Perl itself)
 *
 *  (C) Paul Evans, 2021 -- leonerd@leonerd.org.uk
 */

#include "EXTERN.h"
#include "perl.h"
#include "XSUB.h"

#include "XSParseInfix.h"

#include "infix.h"

#include "perl-backcompat.c.inc"

#include "force_list_keeping_pushmark.c.inc"
#include "make_argcheck_ops.c.inc"
#include "newOP_CUSTOM.c.inc"
#include "op_sibling_splice.c.inc"

#if HAVE_PERL_VERSION(5,32,0)
#  define HAVE_OP_ISA
#endif

#if HAVE_PERL_VERSION(5,22,0)
   /* assert() can be used as an expression */
#  define HAVE_ASSERT_AS_EXPRESSION
#endif

/* These only became full API macros at perl v5.22, but they're available as
 * the full Perl_... name before that
 */
#ifndef block_start
#  define block_start(a)         Perl_block_start(aTHX_ a)
#endif

#ifndef block_end
#  define block_end(a,b)         Perl_block_end(aTHX_ a,b)
#endif

#ifndef XS_INTERNAL
/* copypasta from perl-v5.16.0/XSUB.h */
#  if defined(__CYGWIN__) && defined(USE_DYNAMIC_LOADING)
#    define XS_INTERNAL(name) STATIC XSPROTO(name)
#  endif
#  if defined(__SYMBIAN32__)
#    define XS_INTERNAL(name) EXPORT_C STATIC XSPROTO(name)
#  endif
#  ifndef XS_INTERNAL
#    if defined(HASATTRIBUTE_UNUSED) && !defined(__cplusplus)
#      define XS_INTERNAL(name) STATIC void name(pTHX_ CV* cv __attribute__unused__)
#    else
#      ifdef __cplusplus
#        define XS_INTERNAL(name) static XSPROTO(name)
#      else
#        define XS_INTERNAL(name) STATIC XSPROTO(name)
#      endif
#    endif
#  endif
#endif

struct HooksAndData {
  const struct XSParseInfixHooks *hooks;
  void *data;
};

enum OperandShape {
  SHAPE_SCALARSCALAR,
  SHAPE_SCALARLIST,
  SHAPE_LISTLIST,
};

static enum OperandShape operand_shape(const struct HooksAndData *hd)
{
  U8 args_flags = (hd->hooks->lhs_flags & 0x07) << 4 | (hd->hooks->rhs_flags & 0x07);

  switch(args_flags) {
    /* scalar OP scalar */
    case XPI_OPERAND_TERM:
      return SHAPE_SCALARSCALAR;

    /* scalar OP list */
    case XPI_OPERAND_TERM_LIST:
    case XPI_OPERAND_LIST:
      return SHAPE_SCALARLIST;

    /* list OP list */
    case (XPI_OPERAND_TERM_LIST<<4) | XPI_OPERAND_TERM_LIST:
    case (XPI_OPERAND_TERM_LIST<<4) | XPI_OPERAND_LIST:
      return SHAPE_LISTLIST;

    default:
      croak("TODO: Unsure how to classify operand shape of args_flags=%02X\n",
          args_flags);
      break;
  }
}

struct Registration;
struct Registration {
#ifdef HAVE_PL_INFIX_PLUGIN
  struct Perl_custom_infix def; /* must be first */
#endif

  struct Registration *next;
  struct XSParseInfixInfo info;

  STRLEN      oplen;
  enum XSParseInfixClassification cls;

  struct HooksAndData hd;

  STRLEN permit_hintkey_len;

  int opname_is_WIDE : 1;
};

static struct Registration *registrations;

static OP *new_op(pTHX_ const struct HooksAndData hd, U32 flags, OP *lhs, OP *rhs)
{
  if(hd.hooks->new_op)
    return (*hd.hooks->new_op)(aTHX_ flags, lhs, rhs, hd.data);

  OP *ret = newBINOP_CUSTOM(hd.hooks->ppaddr, flags, lhs, rhs);

  /* TODO: opchecker? */

  return ret;
}

static bool op_extract_onerefgen(OP *o, OP **kidp)
{
  OP *first;
  switch(o->op_type) {
    case OP_SREFGEN:
      first = cUNOPo->op_first;
      if(first->op_type == OP_NULL && first->op_targ == OP_LIST &&
          (*kidp = cLISTOPx(first)->op_first))
        return TRUE;
      break;

    case OP_REFGEN:
      first = cUNOPo->op_first;
      if(first->op_type == OP_NULL && first->op_targ == OP_LIST &&
#ifdef HAVE_ASSERT_AS_EXPRESSION
          (assert(cLISTOPx(first)->op_first->op_type == OP_PUSHMARK), 1) &&
#endif
          (*kidp = OpSIBLING(cLISTOPx(first)->op_first)) &&
          !OpSIBLING(*kidp))
        return TRUE;

      op_dump(first);
  }

  return FALSE;
}

#define unwrap_list(o, may_unwrap_anonlist)  S_unwrap_list(aTHX_ o, may_unwrap_anonlist)
static OP *S_unwrap_list(pTHX_ OP *o, bool may_unwrap_anonlist)
{
  OP *kid;

  /* Look out for some sort of \THING */
  if(op_extract_onerefgen(o, &kid)) {
    if(kid->op_type == OP_PADAV) {
      /* \@padav can just yield the array directly */
      cLISTOPx(cUNOPo->op_first)->op_first = NULL;
      op_free(o);

      kid->op_flags &= ~(OPf_MOD|OPf_REF);
      return force_list_keeping_pushmark(kid);
    }
    if(kid->op_type == OP_RV2AV) {
      /* we can just yield this op directly at this point. It might be \@pkgav
       * or something else, but whatever it is we might as well do it
       */
      cLISTOPx(cUNOPo->op_first)->op_first = NULL;
      op_free(o);

      kid->op_flags &= ~(OPf_MOD|OPf_REF);
      return force_list_keeping_pushmark(kid);
    }
  }

  /* We might be permitted to unwrap a [THING] */
  if(may_unwrap_anonlist &&
      o->op_type == OP_ANONLIST) {
    /* Just turn it into a list and we're already done */
    o->op_type = OP_LIST;
    return force_list_keeping_pushmark(o);
  }

  return force_list_keeping_pushmark(newUNOP(OP_RV2AV, 0, o));
}

#ifdef HAVE_PL_INFIX_PLUGIN

OP *parse(pTHX_ OP *lhs, struct Perl_custom_infix *def)
{
  struct Registration *reg = (struct Registration *)def;

  switch(reg->hd.hooks->lhs_flags & 0x07) {
    case XPI_OPERAND_TERM:
      break;

    case XPI_OPERAND_TERM_LIST:
      lhs = force_list_keeping_pushmark(lhs);
      break;
  }

  /* TODO: maybe operator has a 'parse' hook? */

  lex_read_space(0);
  OP *rhs = NULL;

  switch(reg->hd.hooks->rhs_flags & 0x07) {
    case XPI_OPERAND_TERM:
      rhs = parse_termexpr(0);
      break;

    case XPI_OPERAND_TERM_LIST:
      rhs = force_list_keeping_pushmark(parse_termexpr(0));
      break;

    case XPI_OPERAND_LIST:
      rhs = force_list_keeping_pushmark(parse_listexpr(0));
      break;

    default:
      croak("hooks->rhs_flags did not provide a valid RHS type");
  }

  return new_op(aTHX_ reg->hd, 0, lhs, rhs);
}

static STRLEN (*next_infix_plugin)(pTHX_ char *, STRLEN, struct Perl_custom_infix **);

static STRLEN my_infix_plugin(pTHX_ char *op, STRLEN oplen, struct Perl_custom_infix **def)
{
  if(PL_parser && PL_parser->error_count)
    return (*next_infix_plugin)(aTHX_ op, oplen, def);

  HV *hints = GvHV(PL_hintgv);

  struct Registration *reg;
  for(reg = registrations; reg; reg = reg->next) {
    /* custom registrations have hooks, builtin ones do not */
    if(!reg->hd.hooks)
      continue;

    if(reg->oplen != oplen || !strEQ(reg->info.opname, op))
      continue;

    if(reg->hd.hooks->permit_hintkey &&
      (!hints || !hv_fetch(hints, reg->hd.hooks->permit_hintkey, reg->permit_hintkey_len, 0)))
      continue;

    if(reg->hd.hooks->permit &&
      !(*reg->hd.hooks->permit)(aTHX_ reg->hd.data))
      continue;

    *def = &reg->def;
    return oplen;
  }

  return (*next_infix_plugin)(aTHX_ op, oplen, def);
}
#endif

/* What classifications are included in what selections? */
static const U32 infix_selections[] = {
  [XPI_SELECT_ANY]       = 0xFFFFFFFF,

  [XPI_SELECT_PREDICATE] = (1<<XPI_CLS_PREDICATE)|(1<<XPI_CLS_RELATION)|(1<<XPI_CLS_EQUALITY),
  [XPI_SELECT_RELATION]  =                        (1<<XPI_CLS_RELATION)|(1<<XPI_CLS_EQUALITY),
  [XPI_SELECT_EQUALITY]  =                                              (1<<XPI_CLS_EQUALITY),

  [XPI_SELECT_ORDERING]  = (1<<XPI_CLS_ORDERING),

  [XPI_SELECT_MATCH_NOSMART] = (1<<XPI_CLS_EQUALITY)|(1<<XPI_CLS_MATCHRE)|(1<<XPI_CLS_ISA)|(1<<XPI_CLS_MATCH_MISC),
  [XPI_SELECT_MATCH_SMART]   = (1<<XPI_CLS_EQUALITY)|(1<<XPI_CLS_MATCHRE)|(1<<XPI_CLS_ISA)|(1<<XPI_CLS_MATCH_MISC)|
                                  (1<<XPI_CLS_SMARTMATCH),
};

bool XSParseInfix_parse(pTHX_ enum XSParseInfixSelection select, struct XSParseInfixInfo **infop)
{
  /* PL_parser->bufptr now points exactly at where we expect to find an operator name */

  int selection = infix_selections[select];

  HV *hints = GvHV(PL_hintgv);

  const char *buf = PL_parser->bufptr;
  const STRLEN buflen = PL_parser->bufend - PL_parser->bufptr;

  struct Registration *reg;
  for(reg = registrations; reg; reg = reg->next) {
    if(reg->oplen > buflen)
      continue;
    if(!strnEQ(buf, reg->info.opname, reg->oplen))
      continue;

    if(!(selection & (1 << reg->cls)))
      continue;

    if(reg->hd.hooks && reg->hd.hooks->permit_hintkey &&
      (!hints || !hv_fetch(hints, reg->hd.hooks->permit_hintkey, reg->permit_hintkey_len, 0)))
      continue;

    if(reg->hd.hooks && reg->hd.hooks->permit &&
      !(*reg->hd.hooks->permit)(aTHX_ reg->hd.data))
      continue;

    *infop = &reg->info;

    lex_read_to(PL_parser->bufptr + reg->oplen);
    return TRUE;
  }

  return FALSE;
}

OP *XSParseInfix_new_op(pTHX_ const struct XSParseInfixInfo *info, U32 flags, OP *lhs, OP *rhs)
{
  if(info->opcode == OP_CUSTOM)
    return new_op(aTHX_ (struct HooksAndData) {
        .hooks = info->hooks,
        .data  = info->hookdata,
      }, flags, lhs, rhs);

  return newBINOP(info->opcode, flags, lhs, rhs);
}

static bool op_yields_oneval(OP *o)
{
  if(OP_GIMME(o, 0) == G_SCALAR)
    return TRUE;

  if(PL_opargs[o->op_type] & OA_RETSCALAR)
    return TRUE;

  /* It might still yield a single value, we'll just have to check harder */
  switch(o->op_type) {
    case OP_REFGEN:
    {
      OP *list = cUNOPo->op_first;
      OP *kid;
      assert(cLISTOPx(list)->op_first->op_type == OP_PUSHMARK);
      if((kid = OpSIBLING(cLISTOPx(list)->op_first)) &&
         !OpSIBLING(kid) &&
         (kid->op_flags & OPf_REF))
        return TRUE;
    }
  }

  return FALSE;
}

static bool extract_wrapper2_args(pTHX_ OP *op, OP **leftp, OP **rightp)
{
  assert(op->op_type == OP_ENTERSUB);

  /* Attempt to extract the LHS and RHS operands, if we can find them */

  OP *kid = cUNOPx(op)->op_first;
  /* The first kid is usually an ex-list whose ->op_first begins the actual args list */
  if(kid->op_type == OP_NULL && kid->op_targ == OP_LIST)
    kid = cUNOPx(kid)->op_first;

  assert(kid->op_type == OP_PUSHMARK);
  OP *pushmark = kid;

  OP *left = OpSIBLING(kid);
  if(!left)
    return FALSE;
  if(!op_yields_oneval(left))
    return FALSE;

  OP *right = OpSIBLING(left);
  if(!right)
    return FALSE;
  if(!op_yields_oneval(right))
    return FALSE;

  kid = OpSIBLING(right);
  if(!kid)
    return FALSE;
  if(OpSIBLING(kid))
    return FALSE;

  /* Check that kid is now OP_NULL[ OP_GV ] */
  if(kid->op_type != OP_NULL || kid->op_targ != OP_RV2CV)
    return FALSE;
  if(cUNOPx(kid)->op_first->op_type != OP_GV)
    return FALSE;

  /* Splice out these two args and throw away the old optree */
  OpMORESIB_set(left, NULL);
  OpMORESIB_set(right, NULL);
  OpMORESIB_set(pushmark, kid);
  op_free(op);

  OpLASTSIB_set(left, NULL);
  OpLASTSIB_set(right, NULL);

  *leftp  = left;
  *rightp = right;
  return TRUE;
}

static OP *ckcall_wrapper_func_scalarscalar(pTHX_ OP *op, GV *namegv, SV *ckobj)
{
  struct HooksAndData *hd = NUM2PTR(struct HooksAndData *, SvUV(ckobj));

  OP *left, *right;
  if(!extract_wrapper2_args(aTHX_ op, &left, &right))
    return op;

  return new_op(aTHX_ *hd, 0, left, right);
}

static OP *ckcall_wrapper_func_listlist(pTHX_ OP *op, GV *namegv, SV *ckobj)
{
  struct HooksAndData *hd = NUM2PTR(struct HooksAndData *, SvUV(ckobj));

  OP *left, *right;
  if(!extract_wrapper2_args(aTHX_ op, &left, &right))
    return op;

  return new_op(aTHX_ *hd, 0,
      unwrap_list(left,  hd->hooks->lhs_flags & XPI_OPERAND_ONLY_LOOK),
      unwrap_list(right, hd->hooks->rhs_flags & XPI_OPERAND_ONLY_LOOK));
}

#define newSLUGOP(idx)  S_newSLUGOP(aTHX_ idx)
static OP *S_newSLUGOP(pTHX_ int idx)
{
  OP *op = newGVOP(OP_AELEMFAST, 0, PL_defgv);
  op->op_private = idx;
  return op;
}

static void make_wrapper_func(pTHX_ const struct HooksAndData *hd)
{
  /* Prepare to make a new optree-based CV */
  I32 floor_ix = start_subparse(FALSE, 0);
  SAVEFREESV(PL_compcv);

  SV *funcname = newSVpvn(hd->hooks->wrapper_func_name, strlen(hd->hooks->wrapper_func_name));

  I32 save_ix = block_start(TRUE);

  OP *body = NULL;
  OP *(*ckcall)(pTHX_ OP *, GV *, SV *) = NULL;

  switch(operand_shape(hd)) {
    case SHAPE_SCALARSCALAR:
      body = op_append_list(OP_LINESEQ, body,
          make_argcheck_ops(2, 0, 0, funcname));

      body = op_append_list(OP_LINESEQ, body,
          newSTATEOP(0, NULL, NULL));

      /* Body of the function is just  $_[0] OP $_[1] */
      body = op_append_list(OP_LINESEQ, body,
          new_op(aTHX_ *hd, 0, newSLUGOP(0), newSLUGOP(1)));

      ckcall = &ckcall_wrapper_func_scalarscalar;
      break;

    case SHAPE_SCALARLIST:
      body = op_append_list(OP_LINESEQ, body,
          make_argcheck_ops(1, 0, '@', funcname));

      body = op_append_list(OP_LINESEQ, body,
          newSTATEOP(0, NULL, NULL));

      /* Body of the function is just  shift OP @_ */
      body = op_append_list(OP_LINESEQ, body,
          new_op(aTHX_ *hd, 0,
            newOP(OP_SHIFT, 0),
            force_list_keeping_pushmark(newUNOP(OP_RV2AV, OPf_WANT_LIST, newGVOP(OP_GV, 0, PL_defgv)))));

      /* no ckcall */
      break;

    case SHAPE_LISTLIST:
      body = op_append_list(OP_LINESEQ, body,
          make_argcheck_ops(2, 0, 0, funcname));

      body = op_append_list(OP_LINESEQ, body,
          newSTATEOP(0, NULL, NULL));

      /* Body of the function is  @{ $_[0] } OP @{ $_[1] } */
      body = op_append_list(OP_LINESEQ, body,
          new_op(aTHX_ *hd, 0,
            force_list_keeping_pushmark(newUNOP(OP_RV2AV, 0, newSLUGOP(0))),
            force_list_keeping_pushmark(newUNOP(OP_RV2AV, 0, newSLUGOP(1)))));

      ckcall = &ckcall_wrapper_func_listlist;
      break;
  }

  SvREFCNT_inc(PL_compcv);
  body = block_end(save_ix, body);

  CV *cv = newATTRSUB(floor_ix, newSVOP(OP_CONST, 0, funcname), NULL, NULL, body);

  if(ckcall)
    cv_set_call_checker(cv, ckcall, newSVuv(PTR2UV(hd)));
}

XS_INTERNAL(deparse_infix);
XS_INTERNAL(deparse_infix)
{
  dXSARGS;
  struct Registration *reg = XSANY.any_ptr;

  SV *deparseobj = ST(0);
  SV *ret;

#ifdef HAVE_PL_INFIX_PLUGIN
  SV **hinthashsvp = hv_fetchs(MUTABLE_HV(SvRV(deparseobj)), "hinthash", 0);
  HV *hinthash = hinthashsvp ? MUTABLE_HV(SvRV(*hinthashsvp)) : NULL;

  if(hinthash && hv_fetch(hinthash, reg->hd.hooks->permit_hintkey, reg->permit_hintkey_len, 0)) {
    ENTER;
    SAVETMPS;

    EXTEND(SP, 4);
    PUSHMARK(SP);
    PUSHs(deparseobj);
    mPUSHs(newSVpvn_flags(reg->info.opname, reg->oplen, reg->opname_is_WIDE ? SVf_UTF8 : 0));
    PUSHs(ST(1));
    PUSHs(ST(2));
    PUTBACK;

    call_method("_deparse_infix_named", G_SCALAR);

    SPAGAIN;
    ret = SvREFCNT_inc(POPs);

    FREETMPS;
    LEAVE;
  }
  else
#endif
  {
    ENTER;
    SAVETMPS;

    EXTEND(SP, 4);
    PUSHMARK(SP);
    PUSHs(deparseobj);
    mPUSHp(reg->hd.hooks->wrapper_func_name, strlen(reg->hd.hooks->wrapper_func_name));
    PUSHs(ST(1));
    PUSHs(ST(2));
    PUTBACK;

    switch(operand_shape(&reg->hd)) {
      case SHAPE_SCALARSCALAR:
      case SHAPE_SCALARLIST: /* not really */
        call_method("_deparse_infix_wrapperfunc_scalarscalar", G_SCALAR);
        break;

      case SHAPE_LISTLIST:
        call_method("_deparse_infix_wrapperfunc_listlist", G_SCALAR);
        break;
    }

    SPAGAIN;
    ret = SvREFCNT_inc(POPs);

    FREETMPS;
    LEAVE;
  }

  ST(0) = sv_2mortal(ret);
  XSRETURN(1);
}

static void reg_builtin(pTHX_ const char *opname, enum XSParseInfixClassification cls, OPCODE opcode)
{
  struct Registration *reg;
  Newx(reg, 1, struct Registration);

  reg->info.opname = savepv(opname);
  reg->info.opcode = opcode;
  reg->info.hooks  = NULL;

  reg->oplen  = strlen(opname);
  reg->cls    = cls;

  reg->hd.hooks = NULL;
  reg->hd.data  = NULL;

  reg->permit_hintkey_len = 0;

  {
    reg->next = registrations;
    registrations = reg;
  }
}

void XSParseInfix_register(pTHX_ const char *opname, const struct XSParseInfixHooks *hooks, void *hookdata)
{
  switch(hooks->flags) {
    case 0:
      break;
    default:
      croak("Unrecognised XSParseInfixHooks.flags value 0x%X", hooks->flags);
  }

  switch(hooks->lhs_flags & ~(XPI_OPERAND_ONLY_LOOK)) {
    case XPI_OPERAND_TERM:
    case XPI_OPERAND_TERM_LIST:
      break;
    default:
      croak("Unrecognised XSParseInfixHooks.lhs_flags value 0x%X", hooks->lhs_flags);
  }

  switch(hooks->rhs_flags & ~(XPI_OPERAND_ONLY_LOOK)) {
    case XPI_OPERAND_TERM:
    case XPI_OPERAND_TERM_LIST:
    case XPI_OPERAND_LIST:
      break;
    default:
      croak("Unrecognised XSParseInfixHooks.rhs_flags value 0x%X", hooks->rhs_flags);
  }

  struct Registration *reg;
  Newx(reg, 1, struct Registration);

#ifdef HAVE_PL_INFIX_PLUGIN
  reg->def.parse = &parse;
#endif

  reg->info.opname = savepv(opname);
  reg->info.opcode = OP_CUSTOM;
  reg->info.hooks    = hooks;
  reg->info.hookdata = hookdata;

  reg->oplen  = strlen(opname);
  reg->cls    = hooks->cls;

  reg->hd.hooks = hooks;
  reg->hd.data  = hookdata;

  reg->opname_is_WIDE = FALSE;
  int i;
  for(i = 0; i < reg->oplen; i++) {
    if(opname[i] & 0x80) {
      reg->opname_is_WIDE = TRUE;
      break;
    }
  }

  if(hooks->permit_hintkey)
    reg->permit_hintkey_len = strlen(hooks->permit_hintkey);
  else
    reg->permit_hintkey_len = 0;

  {
    reg->next = registrations;
    registrations = reg;
  }

  if(hooks->wrapper_func_name) {
    make_wrapper_func(aTHX_ &reg->hd);
  }

  if(hooks->ppaddr) {
    XOP *xop;
    Newx(xop, 1, XOP);

    /* Use both the opname for human-readability, and the address of its
     * ppfunc for disambiguating in case of name clashes
     */
    SV *namesv = newSVpvf("B::Deparse::pp_infix_%s_0x%p", opname, hooks->ppaddr);
    if(reg->opname_is_WIDE)
      SvUTF8_on(namesv);
    SAVEFREESV(namesv);

    XopENTRY_set(xop, xop_name, savepv(SvPVX(namesv) + sizeof("B::Deparse::pp")));
    XopENTRY_set(xop, xop_desc, "custom infix operator");
    XopENTRY_set(xop, xop_class, OA_BINOP);
    XopENTRY_set(xop, xop_peep, NULL);

    Perl_custom_op_register(aTHX_ hooks->ppaddr, xop);

    CV *cv = newXS(SvPVX(namesv), deparse_infix, __FILE__);
    CvXSUBANY(cv).any_ptr = reg;

    load_module(PERL_LOADMOD_NOIMPORT, newSVpvs("XS::Parse::Infix"), NULL);
  }
}

void XSParseInfix_boot(pTHX)
{
  /* stringy relations */
  reg_builtin(aTHX_ "eq", XPI_CLS_EQUALITY, OP_SEQ);
  reg_builtin(aTHX_ "ne", XPI_CLS_RELATION, OP_SNE);
  reg_builtin(aTHX_ "lt", XPI_CLS_RELATION, OP_SLT);
  reg_builtin(aTHX_ "le", XPI_CLS_RELATION, OP_SLE);
  reg_builtin(aTHX_ "ge", XPI_CLS_RELATION, OP_SGE);
  reg_builtin(aTHX_ "gt", XPI_CLS_RELATION, OP_SGT);
  reg_builtin(aTHX_ "cmp", XPI_CLS_ORDERING, OP_SCMP);

  /* numerical relations */
  reg_builtin(aTHX_ "==", XPI_CLS_EQUALITY, OP_EQ);
  reg_builtin(aTHX_ "!=", XPI_CLS_RELATION, OP_NE);
  reg_builtin(aTHX_ "<",  XPI_CLS_RELATION, OP_LT);
  reg_builtin(aTHX_ "<=", XPI_CLS_RELATION, OP_LE);
  reg_builtin(aTHX_ ">=", XPI_CLS_RELATION, OP_GE);
  reg_builtin(aTHX_ ">",  XPI_CLS_RELATION, OP_GT);
  reg_builtin(aTHX_ "<=>", XPI_CLS_ORDERING, OP_NCMP);

  /* other predicates */
  reg_builtin(aTHX_ "~~", XPI_CLS_SMARTMATCH, OP_SMARTMATCH);
  reg_builtin(aTHX_ "=~", XPI_CLS_MATCHRE, OP_MATCH);
  /* TODO: !~ */
#ifdef HAVE_OP_ISA
  reg_builtin(aTHX_ "isa", XPI_CLS_ISA, OP_ISA);
#endif

  /* TODO:
   * Other numerics
   *   + - * / % **
   *   << >>
   *
   * Bitwise
   *   & | ^
   * Stringwise
   *   &. |. ^.
   *
   * Boolean
   *   && || //
   */

  HV *stash = gv_stashpvs("XS::Parse::Infix", TRUE);
  newCONSTSUB(stash, "HAVE_PL_INFIX_PLUGIN", boolSV(
#ifdef HAVE_PL_INFIX_PLUGIN
      TRUE
#else
      FALSE
#endif
  ));

#ifdef HAVE_PL_INFIX_PLUGIN
  OP_CHECK_MUTEX_LOCK;
  if(!next_infix_plugin) {
    next_infix_plugin = PL_infix_plugin;
    PL_infix_plugin = &my_infix_plugin;
  }
  OP_CHECK_MUTEX_UNLOCK;
#endif
}
