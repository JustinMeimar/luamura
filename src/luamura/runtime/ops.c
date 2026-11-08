/*
** One function per Lua instruction, for compiled implementations. Each is
** the body of its case in luaV_execute (lvm.c), with the macros it uses.
** 'pc' points past the instruction 'i', as in luaV_execute. Most return
** how far they moved 'pc'; calls and returns return the next call to run
** (see lua_AotFunction), or NULL to continue with the next instruction.
*/

/* clang-format off: kept in lvm.c's style, to compare with it */

#define LUA_CORE

#include "luamura.h"

#include "lapi.h"
#include "ldebug.h"
#include "ldo.h"
#include "lfunc.h"
#include "lgc.h"
#include "lopcodes.h"
#include "ltable.h"
#include "ltm.h"
#include "lvm.h"

#include <math.h>

/* ---------------------------------------------------------------------
** From lvm.c
** ------------------------------------------------------------------ */

#define l_addi(L,a,b)	intop(+, a, b)
#define l_subi(L,a,b)	intop(-, a, b)
#define l_muli(L,a,b)	intop(*, a, b)
#define l_band(a,b)	intop(&, a, b)
#define l_bor(a,b)	intop(|, a, b)
#define l_bxor(a,b)	intop(^, a, b)

#define l_lti(a,b)	(a < b)
#define l_lei(a,b)	(a <= b)
#define l_gti(a,b)	(a > b)
#define l_gei(a,b)	(a >= b)


/*
** Arithmetic operations with immediate operands. 'iop' is the integer
** operation, 'fop' is the float operation.
*/
#define op_arithI(L,iop,fop) {  \
  TValue *ra = vRA(i); \
  TValue *v1 = vRB(i);  \
  int imm = GETARG_sC(i);  \
  if (ttisinteger(v1)) {  \
    lua_Integer iv1 = ivalue(v1);  \
    pc++; setivalue(ra, iop(L, iv1, imm));  \
  }  \
  else if (ttisfloat(v1)) {  \
    lua_Number nb = fltvalue(v1);  \
    lua_Number fimm = cast_num(imm);  \
    pc++; setfltvalue(ra, fop(L, nb, fimm)); \
  }}


/*
** Auxiliary function for arithmetic operations over floats and others
** with two operands.
*/
#define op_arithf_aux(L,v1,v2,fop) {  \
  lua_Number n1; lua_Number n2;  \
  if (tonumberns(v1, n1) && tonumberns(v2, n2)) {  \
    StkId ra = RA(i);  \
    pc++; setfltvalue(s2v(ra), fop(L, n1, n2));  \
  }}


/*
** Arithmetic operations over floats and others with register operands.
*/
#define op_arithf(L,fop) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = vRC(i);  \
  op_arithf_aux(L, v1, v2, fop); }


/*
** Arithmetic operations with K operands for floats.
*/
#define op_arithfK(L,fop) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = KC(i); lua_assert(ttisnumber(v2));  \
  op_arithf_aux(L, v1, v2, fop); }


/*
** Arithmetic operations over integers and floats.
*/
#define op_arith_aux(L,v1,v2,iop,fop) {  \
  if (ttisinteger(v1) && ttisinteger(v2)) {  \
    StkId ra = RA(i); \
    lua_Integer i1 = ivalue(v1); lua_Integer i2 = ivalue(v2);  \
    pc++; setivalue(s2v(ra), iop(L, i1, i2));  \
  }  \
  else op_arithf_aux(L, v1, v2, fop); }


/*
** Arithmetic operations with register operands.
*/
#define op_arith(L,iop,fop) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = vRC(i);  \
  op_arith_aux(L, v1, v2, iop, fop); }


/*
** Arithmetic operations with K operands.
*/
#define op_arithK(L,iop,fop) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = KC(i); lua_assert(ttisnumber(v2));  \
  op_arith_aux(L, v1, v2, iop, fop); }


/*
** Bitwise operations with constant operand.
*/
#define op_bitwiseK(L,op) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = KC(i);  \
  lua_Integer i1;  \
  lua_Integer i2 = ivalue(v2);  \
  if (tointegerns(v1, &i1)) {  \
    StkId ra = RA(i); \
    pc++; setivalue(s2v(ra), op(i1, i2));  \
  }}


/*
** Bitwise operations with register operands.
*/
#define op_bitwise(L,op) {  \
  TValue *v1 = vRB(i);  \
  TValue *v2 = vRC(i);  \
  lua_Integer i1; lua_Integer i2;  \
  if (tointegerns(v1, &i1) && tointegerns(v2, &i2)) {  \
    StkId ra = RA(i); \
    pc++; setivalue(s2v(ra), op(i1, i2));  \
  }}


/*
** Order operations with register operands. 'opn' actually works
** for all numbers, but the fast track improves performance for
** integers.
*/
#define op_order(L,opi,opn,other) {  \
  TValue *ra = vRA(i); \
  int cond;  \
  TValue *rb = vRB(i);  \
  if (ttisinteger(ra) && ttisinteger(rb)) {  \
    lua_Integer ia = ivalue(ra);  \
    lua_Integer ib = ivalue(rb);  \
    cond = opi(ia, ib);  \
  }  \
  else if (ttisnumber(ra) && ttisnumber(rb))  \
    cond = opn(ra, rb);  \
  else  \
    Protect(cond = other(L, ra, rb));  \
  docondjump(); }


/*
** Order operations with immediate operand. (Immediate operand is
** always small enough to have an exact representation as a float.)
*/
#define op_orderI(L,opi,opf,inv,tm) {  \
  TValue *ra = vRA(i); \
  int cond;  \
  int im = GETARG_sB(i);  \
  if (ttisinteger(ra))  \
    cond = opi(ivalue(ra), im);  \
  else if (ttisfloat(ra)) {  \
    lua_Number fa = fltvalue(ra);  \
    lua_Number fim = cast_num(im);  \
    cond = opf(fa, fim);  \
  }  \
  else {  \
    int isf = GETARG_C(i);  \
    Protect(cond = luaT_callorderiTM(L, ra, im, inv, isf, tm));  \
  }  \
  docondjump(); }


#define RA(i)	(base+GETARG_A(i))
#define vRA(i)	s2v(RA(i))
#define RB(i)	(base+GETARG_B(i))
#define vRB(i)	s2v(RB(i))
#define KB(i)	(k+GETARG_B(i))
#define RC(i)	(base+GETARG_C(i))
#define vRC(i)	s2v(RC(i))
#define KC(i)	(k+GETARG_C(i))
#define RKC(i)	((TESTARG_k(i)) ? k + GETARG_C(i) : s2v(base + GETARG_C(i)))



#define updatetrap(ci)  (trap = ci->u.l.trap)

#define updatebase(ci)	(base = ci->func.p + 1)


#define updatestack(ci)  \
	{ if (l_unlikely(trap)) { updatebase(ci); ra = RA(i); } }


/*
** Execute a jump instruction. The 'updatetrap' allows signals to stop
** tight loops. (Without it, the local copy of 'trap' could never change.)
*/
#define dojump(ci,i,e)	{ pc += GETARG_sJ(i) + e; updatetrap(ci); }


/* for test instructions, execute the jump instruction that follows it */
#define donextjump(ci)	{ Instruction ni = *pc; dojump(ci, ni, 1); }

/*
** do a conditional jump: skip next instruction if 'cond' is not what
** was expected (parameter 'k'), else do next instruction, which must
** be a jump.
*/
#define docondjump()	if (cond != GETARG_k(i)) pc++; else donextjump(ci);


/*
** Correct global 'pc'.
*/
#define savepc(ci)	(ci->u.l.savedpc = pc)


/*
** Whenever code can raise errors, the global 'pc' and the global
** 'top' must be correct to report occasional errors.
*/
#define savestate(L,ci)		(savepc(ci), L->top.p = ci->top.p)


/*
** Protect code that, in general, can raise errors, reallocate the
** stack, and change the hooks.
*/
#define Protect(exp)  (savestate(L,ci), (exp), updatetrap(ci))

/* special version that does not change the top */
#define ProtectNT(exp)  (savepc(ci), (exp), updatetrap(ci))

/*
** Protect code that can only raise errors. (That is, it cannot change
** the stack or hooks.)
*/
#define halfProtect(exp)  (savestate(L,ci), (exp))

/*
** macro executed during Lua functions at points where the
** function can yield.
*/
#if !defined(luai_threadyield)
#define luai_threadyield(L)	{lua_unlock(L); lua_lock(L);}
#endif

/* 'c' is the limit of live values in the stack */
#define checkGC(L,c)  \
	{ luaC_condGC(L, (savepc(ci), L->top.p = (c)), \
                         updatetrap(ci)); \
           luai_threadyield(L); }



/* lvm.c's static comparisons, through their exported forms. */
#define LTnum(a, b) luaV_lessthan(L, a, b)
#define LEnum(a, b) luaV_lessequal(L, a, b)

/* ---------------------------------------------------------------------
** Instructions
** ------------------------------------------------------------------ */

#define OP(name)                                                       \
    int lm_##name(lua_State* L, CallInfo* ci, const Instruction* pc,   \
                  Instruction i)

#define CALLOP(name)                                                   \
    CallInfo* lm_##name(lua_State* L, CallInfo* ci,                    \
                        const Instruction* pc, Instruction i)

#define FRAME                                                          \
    LClosure* cl = ci_func(ci);                                        \
    TValue* k = cl->p->k;                                              \
    StkId base = ci->func.p + 1;                                       \
    const Instruction* const start = pc;                               \
    int trap = 0;                                                      \
    (void)cl, (void)k, (void)base, (void)start, (void)trap

#define vmbreak return cast_int(pc - start)

const Instruction* lm_code(CallInfo* ci) { return ci_func(ci)->p->code; }

const TValue* lm_constants(CallInfo* ci) { return ci_func(ci)->p->k; }

/* The implementation to run ci with, or NULL to interpret it. */
lua_AotFunction lm_compiled(lua_State* L, CallInfo* ci) {
    return L->hookmask ? NULL : ci_func(ci)->p->aot;
}

int lm_resume(CallInfo* ci) {
    return cast_int(ci->u.l.savedpc - ci_func(ci)->p->code);
}

/* After a return: continue the caller, or end a fresh call. */
static CallInfo* returned(CallInfo* ci) {
    return (ci->callstatus & CIST_FRESH) ? NULL : ci->previous;
}

OP(MOVE) {
  FRAME;
  StkId ra = RA(i);
  setobjs2s(L, ra, RB(i));
  vmbreak;
}

OP(LOADI) {
  FRAME;
  StkId ra = RA(i);
  lua_Integer b = GETARG_sBx(i);
  setivalue(s2v(ra), b);
  vmbreak;
}

OP(LOADF) {
  FRAME;
  StkId ra = RA(i);
  int b = GETARG_sBx(i);
  setfltvalue(s2v(ra), cast_num(b));
  vmbreak;
}

OP(LOADK) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = k + GETARG_Bx(i);
  setobj2s(L, ra, rb);
  vmbreak;
}

OP(LOADKX) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb;
  rb = k + GETARG_Ax(*pc); pc++;
  setobj2s(L, ra, rb);
  vmbreak;
}

OP(LOADFALSE) {
  FRAME;
  StkId ra = RA(i);
  setbfvalue(s2v(ra));
  vmbreak;
}

OP(LFALSESKIP) {
  FRAME;
  StkId ra = RA(i);
  setbfvalue(s2v(ra));
  pc++;  /* skip next instruction */
  vmbreak;
}

OP(LOADTRUE) {
  FRAME;
  StkId ra = RA(i);
  setbtvalue(s2v(ra));
  vmbreak;
}

OP(LOADNIL) {
  FRAME;
  StkId ra = RA(i);
  int b = GETARG_B(i);
  do {
    setnilvalue(s2v(ra++));
  } while (b--);
  vmbreak;
}

OP(GETUPVAL) {
  FRAME;
  StkId ra = RA(i);
  int b = GETARG_B(i);
  setobj2s(L, ra, cl->upvals[b]->v.p);
  vmbreak;
}

OP(SETUPVAL) {
  FRAME;
  StkId ra = RA(i);
  UpVal *uv = cl->upvals[GETARG_B(i)];
  setobj(L, uv->v.p, s2v(ra));
  luaC_barrier(L, uv, s2v(ra));
  vmbreak;
}

OP(GETTABUP) {
  FRAME;
  StkId ra = RA(i);
  TValue *upval = cl->upvals[GETARG_B(i)]->v.p;
  TValue *rc = KC(i);
  TString *key = tsvalue(rc);  /* key must be a short string */
  lu_byte tag;
  luaV_fastget(upval, key, s2v(ra), luaH_getshortstr, tag);
  if (tagisempty(tag))
    Protect(luaV_finishget(L, upval, rc, ra, tag));
  vmbreak;
}

OP(GETTABLE) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  TValue *rc = vRC(i);
  lu_byte tag;
  if (ttisinteger(rc)) {  /* fast track for integers? */
    luaV_fastgeti(rb, ivalue(rc), s2v(ra), tag);
  }
  else
    luaV_fastget(rb, rc, s2v(ra), luaH_get, tag);
  if (tagisempty(tag))
    Protect(luaV_finishget(L, rb, rc, ra, tag));
  vmbreak;
}

OP(GETI) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  int c = GETARG_C(i);
  lu_byte tag;
  luaV_fastgeti(rb, c, s2v(ra), tag);
  if (tagisempty(tag)) {
    TValue key;
    setivalue(&key, c);
    Protect(luaV_finishget(L, rb, &key, ra, tag));
  }
  vmbreak;
}

OP(GETFIELD) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  TValue *rc = KC(i);
  TString *key = tsvalue(rc);  /* key must be a short string */
  lu_byte tag;
  luaV_fastget(rb, key, s2v(ra), luaH_getshortstr, tag);
  if (tagisempty(tag))
    Protect(luaV_finishget(L, rb, rc, ra, tag));
  vmbreak;
}

OP(SETTABUP) {
  FRAME;
  int hres;
  TValue *upval = cl->upvals[GETARG_A(i)]->v.p;
  TValue *rb = KB(i);
  TValue *rc = RKC(i);
  TString *key = tsvalue(rb);  /* key must be a short string */
  luaV_fastset(upval, key, rc, hres, luaH_psetshortstr);
  if (hres == HOK)
    luaV_finishfastset(L, upval, rc);
  else
    Protect(luaV_finishset(L, upval, rb, rc, hres));
  vmbreak;
}

OP(SETTABLE) {
  FRAME;
  StkId ra = RA(i);
  int hres;
  TValue *rb = vRB(i);  /* key (table is in 'ra') */
  TValue *rc = RKC(i);  /* value */
  if (ttisinteger(rb)) {  /* fast track for integers? */
    luaV_fastseti(s2v(ra), ivalue(rb), rc, hres);
  }
  else {
    luaV_fastset(s2v(ra), rb, rc, hres, luaH_pset);
  }
  if (hres == HOK)
    luaV_finishfastset(L, s2v(ra), rc);
  else
    Protect(luaV_finishset(L, s2v(ra), rb, rc, hres));
  vmbreak;
}

OP(SETI) {
  FRAME;
  StkId ra = RA(i);
  int hres;
  int b = GETARG_B(i);
  TValue *rc = RKC(i);
  luaV_fastseti(s2v(ra), b, rc, hres);
  if (hres == HOK)
    luaV_finishfastset(L, s2v(ra), rc);
  else {
    TValue key;
    setivalue(&key, b);
    Protect(luaV_finishset(L, s2v(ra), &key, rc, hres));
  }
  vmbreak;
}

OP(SETFIELD) {
  FRAME;
  StkId ra = RA(i);
  int hres;
  TValue *rb = KB(i);
  TValue *rc = RKC(i);
  TString *key = tsvalue(rb);  /* key must be a short string */
  luaV_fastset(s2v(ra), key, rc, hres, luaH_psetshortstr);
  if (hres == HOK)
    luaV_finishfastset(L, s2v(ra), rc);
  else
    Protect(luaV_finishset(L, s2v(ra), rb, rc, hres));
  vmbreak;
}

OP(NEWTABLE) {
  FRAME;
  StkId ra = RA(i);
  unsigned b = cast_uint(GETARG_vB(i));  /* log2(hash size) + 1 */
  unsigned c = cast_uint(GETARG_vC(i));  /* array size */
  Table *t;
  if (b > 0)
    b = 1u << (b - 1);  /* hash size is 2^(b - 1) */
  if (TESTARG_k(i)) {  /* non-zero extra argument? */
    lua_assert(GETARG_Ax(*pc) != 0);
    /* add it to array size */
    c += cast_uint(GETARG_Ax(*pc)) * (MAXARG_vC + 1);
  }
  pc++;  /* skip extra argument */
  L->top.p = ra + 1;  /* correct top in case of emergency GC */
  t = luaH_new(L);  /* memory allocation */
  sethvalue2s(L, ra, t);
  if (b != 0 || c != 0)
    luaH_resize(L, t, c, b);  /* idem */
  checkGC(L, ra + 1);
  vmbreak;
}

OP(SELF) {
  FRAME;
  StkId ra = RA(i);
  lu_byte tag;
  TValue *rb = vRB(i);
  TValue *rc = KC(i);
  TString *key = tsvalue(rc);  /* key must be a short string */
  setobj2s(L, ra + 1, rb);
  luaV_fastget(rb, key, s2v(ra), luaH_getshortstr, tag);
  if (tagisempty(tag))
    Protect(luaV_finishget(L, rb, rc, ra, tag));
  vmbreak;
}

OP(ADDI) {
  FRAME;
  op_arithI(L, l_addi, luai_numadd);
  vmbreak;
}

OP(ADDK) {
  FRAME;
  op_arithK(L, l_addi, luai_numadd);
  vmbreak;
}

OP(SUBK) {
  FRAME;
  op_arithK(L, l_subi, luai_numsub);
  vmbreak;
}

OP(MULK) {
  FRAME;
  op_arithK(L, l_muli, luai_nummul);
  vmbreak;
}

OP(MODK) {
  FRAME;
  savestate(L, ci);  /* in case of division by 0 */
  op_arithK(L, luaV_mod, luaV_modf);
  vmbreak;
}

OP(POWK) {
  FRAME;
  op_arithfK(L, luai_numpow);
  vmbreak;
}

OP(DIVK) {
  FRAME;
  op_arithfK(L, luai_numdiv);
  vmbreak;
}

OP(IDIVK) {
  FRAME;
  savestate(L, ci);  /* in case of division by 0 */
  op_arithK(L, luaV_idiv, luai_numidiv);
  vmbreak;
}

OP(BANDK) {
  FRAME;
  op_bitwiseK(L, l_band);
  vmbreak;
}

OP(BORK) {
  FRAME;
  op_bitwiseK(L, l_bor);
  vmbreak;
}

OP(BXORK) {
  FRAME;
  op_bitwiseK(L, l_bxor);
  vmbreak;
}

OP(SHLI) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  int ic = GETARG_sC(i);
  lua_Integer ib;
  if (tointegerns(rb, &ib)) {
    pc++; setivalue(s2v(ra), luaV_shiftl(ic, ib));
  }
  vmbreak;
}

OP(SHRI) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  int ic = GETARG_sC(i);
  lua_Integer ib;
  if (tointegerns(rb, &ib)) {
    pc++; setivalue(s2v(ra), luaV_shiftl(ib, -ic));
  }
  vmbreak;
}

OP(ADD) {
  FRAME;
  op_arith(L, l_addi, luai_numadd);
  vmbreak;
}

OP(SUB) {
  FRAME;
  op_arith(L, l_subi, luai_numsub);
  vmbreak;
}

OP(MUL) {
  FRAME;
  op_arith(L, l_muli, luai_nummul);
  vmbreak;
}

OP(MOD) {
  FRAME;
  savestate(L, ci);  /* in case of division by 0 */
  op_arith(L, luaV_mod, luaV_modf);
  vmbreak;
}

OP(POW) {
  FRAME;
  op_arithf(L, luai_numpow);
  vmbreak;
}

OP(DIV) {
  FRAME;  /* float division (always with floats) */
  op_arithf(L, luai_numdiv);
  vmbreak;
}

OP(IDIV) {
  FRAME;  /* floor division */
  savestate(L, ci);  /* in case of division by 0 */
  op_arith(L, luaV_idiv, luai_numidiv);
  vmbreak;
}

OP(BAND) {
  FRAME;
  op_bitwise(L, l_band);
  vmbreak;
}

OP(BOR) {
  FRAME;
  op_bitwise(L, l_bor);
  vmbreak;
}

OP(BXOR) {
  FRAME;
  op_bitwise(L, l_bxor);
  vmbreak;
}

OP(SHL) {
  FRAME;
  op_bitwise(L, luaV_shiftl);
  vmbreak;
}

OP(SHR) {
  FRAME;
  op_bitwise(L, luaV_shiftr);
  vmbreak;
}

OP(MMBIN) {
  FRAME;
  StkId ra = RA(i);
  Instruction pi = *(pc - 2);  /* original arith. expression */
  TValue *rb = vRB(i);
  TMS tm = (TMS)GETARG_C(i);
  StkId result = RA(pi);
  lua_assert(OP_ADD <= GET_OPCODE(pi) && GET_OPCODE(pi) <= OP_SHR);
  Protect(luaT_trybinTM(L, s2v(ra), rb, result, tm));
  vmbreak;
}

OP(MMBINI) {
  FRAME;
  StkId ra = RA(i);
  Instruction pi = *(pc - 2);  /* original arith. expression */
  int imm = GETARG_sB(i);
  TMS tm = (TMS)GETARG_C(i);
  int flip = GETARG_k(i);
  StkId result = RA(pi);
  Protect(luaT_trybiniTM(L, s2v(ra), imm, flip, result, tm));
  vmbreak;
}

OP(MMBINK) {
  FRAME;
  StkId ra = RA(i);
  Instruction pi = *(pc - 2);  /* original arith. expression */
  TValue *imm = KB(i);
  TMS tm = (TMS)GETARG_C(i);
  int flip = GETARG_k(i);
  StkId result = RA(pi);
  Protect(luaT_trybinassocTM(L, s2v(ra), imm, flip, result, tm));
  vmbreak;
}

OP(UNM) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  lua_Number nb;
  if (ttisinteger(rb)) {
    lua_Integer ib = ivalue(rb);
    setivalue(s2v(ra), intop(-, 0, ib));
  }
  else if (tonumberns(rb, nb)) {
    setfltvalue(s2v(ra), luai_numunm(L, nb));
  }
  else
    Protect(luaT_trybinTM(L, rb, rb, ra, TM_UNM));
  vmbreak;
}

OP(BNOT) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  lua_Integer ib;
  if (tointegerns(rb, &ib)) {
    setivalue(s2v(ra), intop(^, ~l_castS2U(0), ib));
  }
  else
    Protect(luaT_trybinTM(L, rb, rb, ra, TM_BNOT));
  vmbreak;
}

OP(NOT) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  if (l_isfalse(rb))
    setbtvalue(s2v(ra));
  else
    setbfvalue(s2v(ra));
  vmbreak;
}

OP(LEN) {
  FRAME;
  StkId ra = RA(i);
  Protect(luaV_objlen(L, ra, vRB(i)));
  vmbreak;
}

OP(CONCAT) {
  FRAME;
  StkId ra = RA(i);
  int n = GETARG_B(i);  /* number of elements to concatenate */
  L->top.p = ra + n;  /* mark the end of concat operands */
  ProtectNT(luaV_concat(L, n));
  checkGC(L, L->top.p); /* 'luaV_concat' ensures correct top */
  vmbreak;
}

OP(CLOSE) {
  FRAME;
  StkId ra = RA(i);
  lua_assert(!GETARG_B(i));  /* 'close must be alive */
  Protect(luaF_close(L, ra, LUA_OK, 1));
  vmbreak;
}

OP(TBC) {
  FRAME;
  StkId ra = RA(i);
  /* create new to-be-closed upvalue */
  halfProtect(luaF_newtbcupval(L, ra));
  vmbreak;
}

OP(EQ) {
  FRAME;
  StkId ra = RA(i);
  int cond;
  TValue *rb = vRB(i);
  Protect(cond = luaV_equalobj(L, s2v(ra), rb));
  docondjump();
  vmbreak;
}

OP(LT) {
  FRAME;
  op_order(L, l_lti, LTnum, luaV_lessthan);
  vmbreak;
}

OP(LE) {
  FRAME;
  op_order(L, l_lei, LEnum, luaV_lessequal);
  vmbreak;
}

OP(EQK) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = KB(i);
  /* basic types do not use '__eq'; we can use raw equality */
  int cond = luaV_rawequalobj(s2v(ra), rb);
  docondjump();
  vmbreak;
}

OP(EQI) {
  FRAME;
  StkId ra = RA(i);
  int cond;
  int im = GETARG_sB(i);
  if (ttisinteger(s2v(ra)))
    cond = (ivalue(s2v(ra)) == im);
  else if (ttisfloat(s2v(ra)))
    cond = luai_numeq(fltvalue(s2v(ra)), cast_num(im));
  else
    cond = 0;  /* other types cannot be equal to a number */
  docondjump();
  vmbreak;
}

OP(LTI) {
  FRAME;
  op_orderI(L, l_lti, luai_numlt, 0, TM_LT);
  vmbreak;
}

OP(LEI) {
  FRAME;
  op_orderI(L, l_lei, luai_numle, 0, TM_LE);
  vmbreak;
}

OP(GTI) {
  FRAME;
  op_orderI(L, l_gti, luai_numgt, 1, TM_LT);
  vmbreak;
}

OP(GEI) {
  FRAME;
  op_orderI(L, l_gei, luai_numge, 1, TM_LE);
  vmbreak;
}

OP(TEST) {
  FRAME;
  StkId ra = RA(i);
  int cond = !l_isfalse(s2v(ra));
  docondjump();
  vmbreak;
}

OP(TESTSET) {
  FRAME;
  StkId ra = RA(i);
  TValue *rb = vRB(i);
  if (l_isfalse(rb) == GETARG_k(i))
    pc++;
  else {
    setobj2s(L, ra, rb);
    donextjump(ci);
  }
  vmbreak;
}

OP(FORLOOP) {
  FRAME;
  StkId ra = RA(i);
  if (ttisinteger(s2v(ra + 1))) {  /* integer loop? */
    lua_Unsigned count = l_castS2U(ivalue(s2v(ra)));
    if (count > 0) {  /* still more iterations? */
      lua_Integer step = ivalue(s2v(ra + 1));
      lua_Integer idx = ivalue(s2v(ra + 2));  /* control variable */
      chgivalue(s2v(ra), l_castU2S(count - 1));  /* update counter */
      idx = intop(+, idx, step);  /* add step to index */
      chgivalue(s2v(ra + 2), idx);  /* update control variable */
      pc -= GETARG_Bx(i);  /* jump back */
    }
  }
  else if (luaV_floatforloop(L, ra))  /* float loop */
    pc -= GETARG_Bx(i);  /* jump back */
  updatetrap(ci);  /* allows a signal to break the loop */
  vmbreak;
}

OP(FORPREP) {
  FRAME;
  StkId ra = RA(i);
  savestate(L, ci);  /* in case of errors */
  if (luaV_forprep(L, ra))
    pc += GETARG_Bx(i) + 1;  /* skip the loop */
  vmbreak;
}

OP(SETLIST) {
  FRAME;
  StkId ra = RA(i);
  unsigned n = cast_uint(GETARG_vB(i));
  unsigned last = cast_uint(GETARG_vC(i));
  Table *h = hvalue(s2v(ra));
  if (n == 0)
    n = cast_uint(L->top.p - ra) - 1;  /* get up to the top */
  else
    L->top.p = ci->top.p;  /* correct top in case of emergency GC */
  last += n;
  if (TESTARG_k(i)) {
    last += cast_uint(GETARG_Ax(*pc)) * (MAXARG_vC + 1);
    pc++;
  }
  /* when 'n' is known, table should have proper size */
  if (last > h->asize) {  /* needs more space? */
    /* fixed-size sets should have space preallocated */
    lua_assert(GETARG_vB(i) == 0);
    luaH_resizearray(L, h, last);  /* preallocate it at once */
  }
  for (; n > 0; n--) {
    TValue *val = s2v(ra + n);
    obj2arr(h, last - 1, val);
    last--;
    luaC_barrierback(L, obj2gco(h), val);
  }
  vmbreak;
}

OP(CLOSURE) {
  FRAME;
  StkId ra = RA(i);
  Proto *p = cl->p->p[GETARG_Bx(i)];
  halfProtect(luaV_pushclosure(L, p, cl->upvals, base, ra));
  checkGC(L, ra + 1);
  vmbreak;
}

OP(VARARG) {
  FRAME;
  StkId ra = RA(i);
  int n = GETARG_C(i) - 1;  /* required results (-1 means all) */
  int vatab = GETARG_k(i) ? GETARG_B(i) : -1;
  Protect(luaT_getvarargs(L, ci, ra, n, vatab));
  vmbreak;
}

OP(GETVARG) {
  FRAME;
  StkId ra = RA(i);
  TValue *rc = vRC(i);
  luaT_getvararg(ci, ra, rc);
  vmbreak;
}

OP(ERRNNIL) {
  FRAME;
  TValue *ra = vRA(i);
  if (!ttisnil(ra))
    halfProtect(luaG_errnnil(L, cl, GETARG_Bx(i)));
  vmbreak;
}

OP(VARARGPREP) {
  FRAME;
  ProtectNT(luaT_adjustvarargs(L, ci, cl->p));
  if (l_unlikely(trap)) {  /* previous "Protect" updated trap */
    luaD_hookcall(L, ci);
    L->oldpc = 1;  /* next opcode will be seen as a "new" line */
  }
  updatebase(ci);  /* function has new base after adjustment */
  vmbreak;
}

/* Runs the callee next if it is a Lua function, and continues in the
** interpreter if a C function set hooks. */
CALLOP(CALL) {
  FRAME;
  StkId ra = RA(i);
  CallInfo *newci;
  int b = GETARG_B(i);
  int nresults = GETARG_C(i) - 1;
  if (b != 0)  /* fixed number of arguments? */
    L->top.p = ra + b;  /* top signals number of arguments */
  /* else previous instruction set top */
  savepc(ci);  /* in case of errors */
  if ((newci = luaD_precall(L, ra, nresults)) != NULL)
    return newci;
  return l_unlikely(L->hookmask) ? ci : NULL;
}

CALLOP(TAILCALL) {
  FRAME;
  StkId ra = RA(i);
  int b = GETARG_B(i);  /* number of arguments + 1 (function) */
  int n;  /* number of results when calling a C function */
  int nparams1 = GETARG_C(i);
  /* delta is virtual 'func' - real 'func' (vararg functions) */
  int delta = (nparams1) ? ci->u.l.nextraargs + nparams1 : 0;
  if (b != 0)
    L->top.p = ra + b;
  else  /* previous instruction set top */
    b = cast_int(L->top.p - ra);
  savepc(ci);  /* several calls here can raise errors */
  if (TESTARG_k(i)) {
    luaF_closeupval(L, base);  /* close upvalues from current call */
    lua_assert(L->tbclist.p < base);  /* no pending tbc variables */
    lua_assert(base == ci->func.p + 1);
  }
  if ((n = luaD_pretailcall(L, ci, ra, b, delta)) < 0)  /* Lua function? */
    return ci;  /* the callee runs in this call */
  ci->func.p -= delta;  /* restore 'func' (if vararg) */
  luaD_poscall(L, ci, n);  /* finish caller */
  return returned(ci);
}

CALLOP(RETURN) {
  FRAME;
  StkId ra = RA(i);
  int n = GETARG_B(i) - 1;  /* number of results */
  int nparams1 = GETARG_C(i);
  if (n < 0)  /* not fixed? */
    n = cast_int(L->top.p - ra);  /* get what is available */
  savepc(ci);
  if (TESTARG_k(i)) {  /* may there be open upvalues? */
    ci->u2.nres = n;  /* save number of returns */
    if (L->top.p < ci->top.p)
      L->top.p = ci->top.p;
    luaF_close(L, base, CLOSEKTOP, 1);
    updatebase(ci);
    ra = RA(i);
  }
  if (nparams1)  /* vararg function? */
    ci->func.p -= ci->u.l.nextraargs + nparams1;
  L->top.p = ra + n;  /* set call for 'luaD_poscall' */
  luaD_poscall(L, ci, n);
  return returned(ci);
}

CALLOP(RETURN0) {
  FRAME;
  if (l_unlikely(L->hookmask)) {
    StkId ra = RA(i);
    L->top.p = ra;
    savepc(ci);
    luaD_poscall(L, ci, 0);  /* no hurry... */
  }
  else {  /* do the 'poscall' here */
    int nres = get_nresults(ci->callstatus);
    L->ci = ci->previous;  /* back to caller */
    L->top.p = base - 1;
    for (; l_unlikely(nres > 0); nres--)
      setnilvalue(s2v(L->top.p++));  /* all results are nil */
  }
  return returned(ci);
}

CALLOP(RETURN1) {
  FRAME;
  if (l_unlikely(L->hookmask)) {
    StkId ra = RA(i);
    L->top.p = ra + 1;
    savepc(ci);
    luaD_poscall(L, ci, 1);  /* no hurry... */
  }
  else {  /* do the 'poscall' here */
    int nres = get_nresults(ci->callstatus);
    L->ci = ci->previous;  /* back to caller */
    if (nres == 0)
      L->top.p = base - 1;  /* asked for no results */
    else {
      StkId ra = RA(i);
      setobjs2s(L, base - 1, ra);  /* at least this result */
      L->top.p = base;
      for (; l_unlikely(nres > 1); nres--)
        setnilvalue(s2v(L->top.p++));  /* complete missing results */
    }
  }
  return returned(ci);
}

/* Moves 'pc' to the loop's TFORCALL. */
OP(TFORPREP) {
  FRAME;
  StkId ra = RA(i);
  TValue temp;  /* to swap control and closing variables */
  setobj(L, &temp, s2v(ra + 3));
  setobjs2s(L, ra + 3, ra + 2);
  setobj2s(L, ra + 2, &temp);
  /* create to-be-closed upvalue (if closing var. is not nil) */
  halfProtect(luaF_newtbcupval(L, ra + 2));
  pc += GETARG_Bx(i);  /* go to end of the loop */
  vmbreak;
}

OP(TFORCALL) {
  FRAME;
  StkId ra = RA(i);
  setobjs2s(L, ra + 5, ra + 3);  /* copy the control variable */
  setobjs2s(L, ra + 4, ra + 1);  /* copy state */
  setobjs2s(L, ra + 3, ra);  /* copy function */
  L->top.p = ra + 3 + 3;
  ProtectNT(luaD_call(L, ra + 3, GETARG_C(i)));  /* do the call */
  vmbreak;
}

OP(TFORLOOP) {
  FRAME;
  StkId ra = RA(i);
  if (!ttisnil(s2v(ra + 3)))  /* continue loop? */
    pc -= GETARG_Bx(i);  /* jump back */
  vmbreak;
}
