/*
 * Copyright (c) 2003, 2015, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2014, Red Hat Inc. All rights reserved.
 * Copyright (c) 2015, Linaro Ltd. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "asm/macroAssembler.hpp"
#include "interp_masm_aarch32.hpp"
#include "interpreter/interpreter.hpp"
#include "interpreter/interpreterRuntime.hpp"
#include "interpreter/templateTable.hpp"
#include "memory/universe.inline.hpp"
#include "oops/method.hpp"
#include "oops/methodData.hpp"
#include "oops/objArrayKlass.hpp"
#include "oops/oop.inline.hpp"
#include "prims/methodHandles.hpp"
#include "runtime/sharedRuntime.hpp"
#include "runtime/stubRoutines.hpp"
#include "runtime/synchronizer.hpp"

#ifndef CC_INTERP

#define __ _masm->

// FIXME Remove both of these functions
void print_constantpoolcache(ConstantPoolCache *cpc) {
  cpc->print_on(tty);
}
void print_constantpool(ConstantPool *cp) {
  cp->print_on(tty);
}
// FIXME They're just for debugging

// Platform-dependent initialization

extern void aarch32TestHook();

void TemplateTable::pd_initialize() {
  aarch32TestHook();
}

// Address computation: local variables

static inline Address iaddress(int n) {
  return Address(rlocals, Interpreter::local_offset_in_bytes(n));
}

static inline Address laddress(int n) {
  return iaddress(n + 1);
}

static inline Address faddress(int n) {
  return iaddress(n);
}

static inline Address daddress(int n) {
  return laddress(n);
}

static inline Address aaddress(int n) {
  return iaddress(n);
}

static inline Address iaddress(Register r) {
  // FIXME
  return Address(rlocals, r, lsl(2));
}

// Note these two are different as VLDR/VSTR don't
// support base + (offset{ << x })
static inline Address faddress(Register r, Register scratch,
                               InterpreterMacroAssembler* _masm) {
  __ lea(scratch, Address(rlocals, r, lsl(2)));
  return Address(scratch);
}

static inline Address daddress(Register r, Register scratch,
                               InterpreterMacroAssembler* _masm) {
  __ lea(scratch, Address(rlocals, r, lsl(2)));
  return Address(scratch, Interpreter::local_offset_in_bytes(1));
}

static inline Address laddress(Register r, Register scratch,
                               InterpreterMacroAssembler * _masm) {
  return daddress(r, scratch, _masm);
}

static inline Address aaddress(Register r) {
  return iaddress(r);
}

static inline Address at_rsp() {
  return Address(sp, 0);
}

// At top of Java expression stack which may be different than sp().  It
// isn't for category 1 objects.
static inline Address at_tos   () {
  return Address(sp,  Interpreter::expr_offset_in_bytes(0));
}

static inline Address at_tos_p1() {
  return Address(sp,  Interpreter::expr_offset_in_bytes(1));
}

static inline Address at_tos_p2() {
  return Address(sp,  Interpreter::expr_offset_in_bytes(2));
}

static inline Address at_tos_p3() {
  return Address(sp,  Interpreter::expr_offset_in_bytes(3));
}

static inline Address at_tos_p4() {
  return Address(sp,  Interpreter::expr_offset_in_bytes(4));
}

static inline Address at_tos_p5() {
  return Address(sp,  Interpreter::expr_offset_in_bytes(5));
}

// Condition conversion
static Assembler::Condition j_not(TemplateTable::Condition cc) {
  switch (cc) {
  case TemplateTable::equal        : return Assembler::NE;
  case TemplateTable::not_equal    : return Assembler::EQ;
  case TemplateTable::less         : return Assembler::GE;
  case TemplateTable::less_equal   : return Assembler::GT;
  case TemplateTable::greater      : return Assembler::LE;
  case TemplateTable::greater_equal: return Assembler::LT;
  }
  ShouldNotReachHere();
  return Assembler::EQ;
}


// Miscelaneous helper routines
// Store an oop (or NULL) at the Address described by obj.
// If val == noreg this means store a NULL
static void do_oop_store(InterpreterMacroAssembler* _masm,
                         Address obj,
                         Register val,
                         BarrierSet::Name barrier,
                         bool precise) {
  assert(val == noreg || val == r0, "parameter is just for looks");
  switch (barrier) {
#if INCLUDE_ALL_GCS
    case BarrierSet::G1SATBCTLogging:
      {
        // flatten object address if needed
        if (obj.index() == noreg && obj.offset() == 0) {
          if (obj.base() != r3) {
            __ mov(r3, obj.base());
          }
        } else {
          __ lea(r3, obj);
        }
        __ g1_write_barrier_pre(r3 /* obj */,
                                r1 /* pre_val */,
                                rthread /* thread */,
                                r14  /* tmp */,
                                val != noreg /* tosca_live */,
                                false /* expand_call */);
        if (val == noreg) {
          __ store_heap_oop_null(Address(r3, 0));
        } else {
          // G1 barrier needs uncompressed oop for region cross check.
          Register new_val = val;
          __ store_heap_oop(Address(r3, 0), val);
          __ g1_write_barrier_post(r3 /* store_adr */,
                                   new_val /* new_val */,
                                   rthread /* thread */,
                                   r14 /* tmp */,
                                   r1 /* tmp2 */);
        }

      }
      break;
#endif // INCLUDE_ALL_GCS
    case BarrierSet::CardTableModRef:
    case BarrierSet::CardTableExtension:
      {
        if (val == noreg) {
          __ store_heap_oop_null(obj);
        } else {
          __ store_heap_oop(obj, val);
          // flatten object address if needed
          if (!precise || (obj.index() == noreg && obj.offset() == 0)) {
            __ store_check(obj.base());
          } else {
            __ lea(r3, obj);
            __ store_check(r3);
          }
        }
      }
      break;
    case BarrierSet::ModRef:
    case BarrierSet::Other:
      if (val == noreg) {
        __ store_heap_oop_null(obj);
      } else {
        __ store_heap_oop(obj, val);
      }
      break;
    default      :
      ShouldNotReachHere();

  }
}

Address TemplateTable::at_bcp(int offset) {
  assert(_desc->uses_bcp(), "inconsistent uses_bcp information");
  return Address(rbcp, offset);
}

void TemplateTable::patch_bytecode(Bytecodes::Code bc, Register bc_reg,
                                   Register temp_reg, bool load_bc_into_bc_reg/*=true*/,
                                   int byte_no)
{
  if (!RewriteBytecodes)  return;
  Label L_patch_done;

  switch (bc) {
  case Bytecodes::_fast_aputfield:
  case Bytecodes::_fast_bputfield:
  case Bytecodes::_fast_zputfield:
  case Bytecodes::_fast_cputfield:
  case Bytecodes::_fast_dputfield:
  case Bytecodes::_fast_fputfield:
  case Bytecodes::_fast_iputfield:
  case Bytecodes::_fast_lputfield:
  case Bytecodes::_fast_sputfield:
    {
      // We skip bytecode quickening for putfield instructions when
      // the put_code written to the constant pool cache is zero.
      // This is required so that every execution of this instruction
      // calls out to InterpreterRuntime::resolve_get_put to do
      // additional, required work.
      assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");
      assert(load_bc_into_bc_reg, "we use bc_reg as temp");
      __ get_cache_and_index_and_bytecode_at_bcp(temp_reg, bc_reg, temp_reg, byte_no, 1);
      __ mov(bc_reg, bc);
      __ cmp(temp_reg, (unsigned) 0);
      __ b(L_patch_done, Assembler::EQ);  // don't patch
    }
    break;
  default:
    assert(byte_no == -1, "sanity");
    // the pair bytecodes have already done the load.
    if (load_bc_into_bc_reg) {
      __ mov(bc_reg, bc);
    }
  }

  if (JvmtiExport::can_post_breakpoint()) {
    Label L_fast_patch;
    // if a breakpoint is present we can't rewrite the stream directly
    __ load_unsigned_byte(temp_reg, at_bcp(0));
    __ cmp(temp_reg, Bytecodes::_breakpoint);
    __ b(L_fast_patch, Assembler::NE);
    // Let breakpoint table handling rewrite to quicker bytecode
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::set_original_bytecode_at), rmethod, rbcp, bc_reg);
    __ b(L_patch_done);
    __ bind(L_fast_patch);
  }

#ifdef ASSERT
  Label L_okay;
  __ load_unsigned_byte(temp_reg, at_bcp(0));
  __ cmp(temp_reg, (int) Bytecodes::java_code(bc));
  __ b(L_okay, Assembler::EQ);
  __ cmp(temp_reg, bc_reg);
  __ b(L_okay, Assembler::EQ);
  __ stop("patching the wrong bytecode");
  __ bind(L_okay);
#endif

  // patch bytecode
  __ strb(bc_reg, at_bcp(0));
  __ bind(L_patch_done);
}


// Individual instructions

void TemplateTable::nop() {
  transition(vtos, vtos);
  // nothing to do
}

void TemplateTable::shouldnotreachhere() {
  transition(vtos, vtos);
  __ stop("shouldnotreachhere bytecode");
}

void TemplateTable::aconst_null()
{
  transition(vtos, atos);
  __ mov(r0, 0);
}

void TemplateTable::iconst(int value)
{
  transition(vtos, itos);
  __ mov(r0, value);
}

void TemplateTable::lconst(int value)
{
  // int is 32 bit and only ever used for loading small values
  __ mov(r0, value & 0xffffffff);
  __ mov(r1, 0);
}

void TemplateTable::fconst(int value)
{
  transition(vtos, ftos);
  float fval = value;
  assert(value == 0 || value == 1 || value == 2, "invalid float const");
  if(__ operand_valid_for_float_immediate(fval)) {
    __ vmov_f32(d0, fval);
  } else {
    __ mov(r0, *((uint32_t*)&fval));
    __ vmov_f32(d0, r0);
  }
}

void TemplateTable::dconst(int value)
{
  transition(vtos, dtos);
  double dval = value;
  assert(value == 0 || value == 1 || value == 2, "invalid double const");
  if(__ operand_valid_for_double_immediate(dval)) {
    __ vmov_f64(d0, dval);
  } else {
    uint32_t* ptr = (uint32_t*)&dval;
    __ mov(r0, *ptr);
    __ mov(r1, *(ptr + 1));
    __ vmov_f64(d0, r0, r1);
  }
}

void TemplateTable::bipush()
{
  transition(vtos, itos);
  __ load_signed_byte(r0, at_bcp(1));
}

void TemplateTable::sipush()
{
  transition(vtos, itos);
  __ load_unsigned_short(r0, at_bcp(1));
  __ rev(r0, r0);
  __ asr(r0, r0, 16);
}

void TemplateTable::ldc(bool wide)
{
  transition(vtos, vtos);
  Label call_ldc, notFloat, notClass, Done;

  if (wide) {
    __ get_unsigned_2_byte_index_at_bcp(r1, 1);
  } else {
    __ load_unsigned_byte(r1, at_bcp(1));
  }
  __ get_cpool_and_tags(r2, r0);

  const int base_offset = ConstantPool::header_size() * wordSize;
  const int tags_offset = Array<u1>::base_offset_in_bytes();

  // get type
  __ add(r3, r1, tags_offset);
  __ ldrb(r3, Address(r0, r3));

  // unresolved class - get the resolved class
  __ cmp(r3, JVM_CONSTANT_UnresolvedClass);
  __ b(call_ldc, Assembler::EQ);

  // unresolved class in error state - call into runtime to throw the error
  // from the first resolution attempt
  __ cmp(r3, JVM_CONSTANT_UnresolvedClassInError);
  __ b(call_ldc, Assembler::EQ);

  // resolved class - need to call vm to get java mirror of the class
  __ cmp(r3, JVM_CONSTANT_Class);
  __ b(notClass, Assembler::NE);

  __ bind(call_ldc);
  __ mov(c_rarg1, wide);
  call_VM(r0, CAST_FROM_FN_PTR(address, InterpreterRuntime::ldc), c_rarg1);
  __ push_ptr(r0);
  __ verify_oop(r0);
  __ b(Done);

  __ bind(notClass);
  __ cmp(r3, JVM_CONSTANT_Float);
  __ b(notFloat, Assembler::NE);
  // ftos
  __ adds(r1, r2, r1, lsl(2));
  __ vldr_f32(d0, Address(r1, base_offset));

  __ push_f();

  __ b(Done);

  __ bind(notFloat);
#ifdef ASSERT
  {
    Label L;
    __ cmp(r3, JVM_CONSTANT_Integer);
    __ b(L, Assembler::EQ);
    // String and Object are rewritten to fast_aldc
    __ stop("unexpected tag type in ldc");
    __ bind(L);
  }
#endif
  // itos JVM_CONSTANT_Integer only
  __ adds(r1, r2, r1, lsl(2));
  __ ldr(r0, Address(r1, base_offset));
  __ push_i(r0);
  __ bind(Done);
}

// Fast path for caching oop constants.
void TemplateTable::fast_aldc(bool wide)
{
  transition(vtos, atos);

  Register result = r0;
  Register tmp = r1;
  int index_size = wide ? sizeof(u2) : sizeof(u1);

  Label resolved;

  // We are resolved if the resolved reference cache entry contains a
  // non-null object (String, MethodType, etc.)
  assert_different_registers(result, tmp);
  __ get_cache_index_at_bcp(tmp, 1, index_size);
  __ load_resolved_reference_at_index(result, tmp);
  __ cbnz(result, resolved);

  address entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_ldc);

  // first time invocation - must resolve first
  __ mov(tmp, (int)bytecode());
  __ call_VM(result, entry, tmp);

  __ bind(resolved);

  if (VerifyOops) {
    __ verify_oop(result);
  }
}

void TemplateTable::ldc2_w()
{
  transition(vtos, vtos);
  Label Long, Done;
  __ get_unsigned_2_byte_index_at_bcp(r0, 1);

  __ get_cpool_and_tags(r1, r2);
  const int base_offset = ConstantPool::header_size() * wordSize;
  const int tags_offset = Array<u1>::base_offset_in_bytes();

  // get type
  __ lea(r2, Address(r2, r0, lsl(0)));
  __ load_unsigned_byte(r2, Address(r2, tags_offset));
  __ cmp(r2, (int)JVM_CONSTANT_Double);
  __ b(Long, Assembler::NE);
  // dtos
  __ lea (r2, Address(r1, r0, lsl(2)));
  __ vldr_f64(d0, Address(r2, base_offset));
  __ push_d();
  __ b(Done);

  __ bind(Long);
  // ltos
  __ lea(r1, Address(r1, r0, lsl(2)));
  __ ldr(r0, Address(r1, base_offset));
  __ ldr(r1, Address(r1, base_offset + wordSize));
  __ push_l();

  __ bind(Done);
}

void TemplateTable::locals_index(Register reg, int offset)
{
  __ ldrb(reg, at_bcp(offset));
  __ neg(reg, reg);
}

void TemplateTable::iload() {
  transition(vtos, itos);
  if (RewriteFrequentPairs) {
    Label rewrite, done;
    Register bc = r2;

    // get next bytecode
    __ load_unsigned_byte(r1, at_bcp(Bytecodes::length_for(Bytecodes::_iload)));

    // if _iload, wait to rewrite to iload2.  We only want to rewrite the
    // last two iloads in a pair.  Comparing against fast_iload means that
    // the next bytecode is neither an iload or a caload, and therefore
    // an iload pair.
    __ cmp(r1, Bytecodes::_iload);
    __ b(done, Assembler::EQ);

    // if _fast_iload rewrite to _fast_iload2
    __ cmp(r1, Bytecodes::_fast_iload);
    __ mov(bc, Bytecodes::_fast_iload2);
    __ b(rewrite, Assembler::EQ);

    // if _caload rewrite to _fast_icaload
    __ cmp(r1, Bytecodes::_caload);
    __ mov(bc, Bytecodes::_fast_icaload);
    __ b(rewrite, Assembler::EQ);

    // else rewrite to _fast_iload
    __ mov(bc, Bytecodes::_fast_iload);

    // rewrite
    // bc: new bytecode
    __ bind(rewrite);
    patch_bytecode(Bytecodes::_iload, bc, r1, false);
    __ bind(done);

  }

  // do iload, get the local value into tos
  locals_index(r1);
  __ ldr(r0, iaddress(r1));
  __ reg_printf("iloaded value %d\n", r0);
}

void TemplateTable::fast_iload2()
{
  transition(vtos, itos);
  locals_index(r1);
  __ ldr(r0, iaddress(r1));
  __ push(itos);
  locals_index(r1, 3);
  __ ldr(r0, iaddress(r1));
}

void TemplateTable::fast_iload()
{
  transition(vtos, itos);
  locals_index(r1);
  __ ldr(r0, iaddress(r1));
}

void TemplateTable::lload()
{
  transition(vtos, ltos);
  locals_index(r2);
  __ ldrd(r0, r1, laddress(r2, r3, _masm));
}

void TemplateTable::fload()
{
  transition(vtos, ftos);
  locals_index(r1);
  __ vldr_f32(d0, faddress(r1, r2, _masm));
}

void TemplateTable::dload()
{
  transition(vtos, dtos);
  __ ldrb(r1, at_bcp(1));
  __ sub(r1, rlocals, r1, lsl(LogBytesPerWord));
  __ vldr_f64(d0, Address(r1, Interpreter::local_offset_in_bytes(1)));
}

void TemplateTable::aload()
{
  transition(vtos, atos);
  locals_index(r1);
  __ ldr(r0, iaddress(r1));
}

void TemplateTable::locals_index_wide(Register reg) {
  __ ldrh(reg, at_bcp(2));
  __ rev16(reg, reg);
  __ neg(reg, reg);
}

void TemplateTable::wide_iload() {
  transition(vtos, itos);
  locals_index_wide(r1);
  __ ldr(r0, iaddress(r1));
}

void TemplateTable::wide_lload()
{
  transition(vtos, ltos);
  locals_index_wide(r2);
  __ ldrd(r0, r1, laddress(r2, r3, _masm));
}

void TemplateTable::wide_fload()
{
  transition(vtos, ftos);
  locals_index_wide(r1);
  __ vldr_f32(d0, faddress(r1, rscratch1, _masm));
}

void TemplateTable::wide_dload()
{
  transition(vtos, dtos);
  __ ldrh(r1, at_bcp(2));
  __ rev16(r1, r1);
  __ sub(r1, rlocals, r1, lsl(LogBytesPerWord));
  __ vldr_f64(d0, Address(r1, Interpreter::local_offset_in_bytes(1)));
}

void TemplateTable::wide_aload()
{
  transition(vtos, atos);
  locals_index_wide(r1);
  __ ldr(r0, aaddress(r1));
}

void TemplateTable::index_check(Register array, Register index)
{
  // destroys rscratch1
  // check array
  __ null_check(array, arrayOopDesc::length_offset_in_bytes());
  // sign extend index for use by indexed load
  // __ movl2ptr(index, index);
  // check index
  Register length = rscratch1;
  __ ldr(length, Address(array, arrayOopDesc::length_offset_in_bytes()));
  __ reg_printf("Checking index in array, array = %p, alen = %d, index = %d\n", array, length, index);
  __ cmp(index, length);
  if (index != r2) {
    // ??? convention: move aberrant index into r2 for exception message
    assert(r2 != array, "different registers");
    __ mov(r2, index);
  }
  Label ok;
  __ b(ok, Assembler::LO);
  __ mov(rscratch1, Interpreter::_throw_ArrayIndexOutOfBoundsException_entry);
  __ b(rscratch1);
  __ bind(ok);
}

void TemplateTable::iaload()
{
  transition(itos, itos);
  __ mov(r2, r0);
  __ pop_ptr(r0);
  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r2, kills rscratch1
  __ lea(r2, Address(r0, r2, lsl(2)));
  __ ldr(r0, Address(r2, arrayOopDesc::base_offset_in_bytes(T_INT)));
}

void TemplateTable::laload()
{
  transition(itos, ltos);
  __ mov(r2, r0);
  __ pop_ptr(r0);
  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r2, kills rscratch1
  __ lea(r2, Address(r0, r2, lsl(3)));
  __ lea(r2, Address(r2,  arrayOopDesc::base_offset_in_bytes(T_LONG)));
  __ atomic_ldrd(r0, r1, r2);
}

void TemplateTable::faload()
{
  transition(itos, ftos);
  __ mov(r2, r0);
  __ pop_ptr(r0);
  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r2, kills rscratch1
  __ lea(r2,  Address(r0, r2, lsl(2)));
  __ vldr_f32(d0, Address(r2,  arrayOopDesc::base_offset_in_bytes(T_FLOAT)));
}

void TemplateTable::daload()
{
  transition(itos, dtos);
  __ mov(r2, r0);
  __ pop_ptr(r0);
  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r2, kills rscratch1
  __ lea(r2,  Address(r0, r2, lsl(3)));
  __ add(r2, r2, arrayOopDesc::base_offset_in_bytes(T_DOUBLE));
  __ atomic_ldrd(r0, r1, r2);
  __ vmov_f64(d0, r0, r1);
}

void TemplateTable::aaload()
{
  transition(itos, atos);
  __ mov(r2, r0);
  __ pop_ptr(r0);
  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r2, kills rscratch1
  __ lea(r2, Address(r0, r2, lsl(2)));
  __ load_heap_oop(r0, Address(r2, arrayOopDesc::base_offset_in_bytes(T_OBJECT)));
}

void TemplateTable::baload()
{
  transition(itos, itos);
  __ mov(r2, r0);
  __ pop_ptr(r0);
  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r2, kills rscratch1
  __ lea(r2,  Address(r0, r2, lsl(0)));
  __ load_signed_byte(r0, Address(r2,  arrayOopDesc::base_offset_in_bytes(T_BYTE)));
}

void TemplateTable::caload()
{
  transition(itos, itos);
  __ mov(r2, r0);
  __ pop_ptr(r0);
  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r2, kills rscratch1
  __ lea(r2,  Address(r0, r2, lsl(1)));
  __ load_unsigned_short(r0, Address(r2,  arrayOopDesc::base_offset_in_bytes(T_CHAR)));
}

// iload followed by caload frequent pair
void TemplateTable::fast_icaload()
{
  transition(vtos, itos);
  // load index out of locals
  locals_index(r2);
  __ ldr(r2, iaddress(r2));

  __ pop_ptr(r0);

  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r1, kills rscratch1
  __ lea(r2,  Address(r0, r2, lsl(1)));
  __ load_unsigned_short(r0, Address(r2,  arrayOopDesc::base_offset_in_bytes(T_CHAR)));
}

void TemplateTable::saload()
{
  transition(itos, itos);
  __ mov(r2, r0);
  __ pop_ptr(r0);
  // r0: array
  // r2: index
  index_check(r0, r2); // leaves index in r2, kills rscratch1
  __ lea(r2,  Address(r0, r2, lsl(1)));
  __ load_signed_short(r0, Address(r2,  arrayOopDesc::base_offset_in_bytes(T_SHORT)));
}

void TemplateTable::iload(int n)
{
  transition(vtos, itos);
  __ ldr(r0, iaddress(n));
}

void TemplateTable::lload(int n)
{
  transition(vtos, ltos);
  __ ldrd(r0, r1, laddress(n));
}

void TemplateTable::fload(int n)
{
  transition(vtos, ftos);
  __ vldr_f32(d0, faddress(n));
  __ vmov_f32(rscratch1, d0);
  __ reg_printf("Just loaded float 0x%08x\n", rscratch1);
  __ vmov_f32(rscratch1, d0);
  __ reg_printf("Just loaded float, confirm 0x%08x\n", rscratch1);
}

void TemplateTable::dload(int n)
{
  transition(vtos, dtos);
  __ vldr_f64(d0, daddress(n));
}

void TemplateTable::aload(int n)
{
  transition(vtos, atos);
  __ ldr(r0, iaddress(n));
  __ reg_printf("aload, loaded %p\n", r0);
}

void TemplateTable::aload_0() {
  // According to bytecode histograms, the pairs:
  //
  // _aload_0, _fast_igetfield
  // _aload_0, _fast_agetfield
  // _aload_0, _fast_fgetfield
  //
  // occur frequently. If RewriteFrequentPairs is set, the (slow)
  // _aload_0 bytecode checks if the next bytecode is either
  // _fast_igetfield, _fast_agetfield or _fast_fgetfield and then
  // rewrites the current bytecode into a pair bytecode; otherwise it
  // rewrites the current bytecode into _fast_aload_0 that doesn't do
  // the pair check anymore.
  //
  // Note: If the next bytecode is _getfield, the rewrite must be
  //       delayed, otherwise we may miss an opportunity for a pair.
  //
  // Also rewrite frequent pairs
  //   aload_0, aload_1
  //   aload_0, iload_1
  // These bytecodes with a small amount of code are most profitable
  // to rewrite
  if (RewriteFrequentPairs) {
    Label rewrite, done;
    const Register bc = r14;

    // get next bytecode
    __ load_unsigned_byte(r1, at_bcp(Bytecodes::length_for(Bytecodes::_aload_0)));

    // do actual aload_0
    aload(0);

    // if _getfield then wait with rewrite
    __ cmp(r1, Bytecodes::Bytecodes::_getfield);
    __ b(done, Assembler::EQ);

    // if _igetfield then reqrite to _fast_iaccess_0
    assert(Bytecodes::java_code(Bytecodes::_fast_iaccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
    __ cmp(r1, Bytecodes::_fast_igetfield);
    __ mov(bc, Bytecodes::_fast_iaccess_0);
    __ b(rewrite, Assembler::EQ);

    // if _agetfield then reqrite to _fast_aaccess_0
    assert(Bytecodes::java_code(Bytecodes::_fast_aaccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
    __ cmp(r1, Bytecodes::_fast_agetfield);
    __ mov(bc, Bytecodes::_fast_aaccess_0);
    __ b(rewrite, Assembler::EQ);

    // if _fgetfield then reqrite to _fast_faccess_0
    assert(Bytecodes::java_code(Bytecodes::_fast_faccess_0) == Bytecodes::_aload_0, "fix bytecode definition");
    __ cmp(r1, Bytecodes::_fast_fgetfield);
    __ mov(bc, Bytecodes::_fast_faccess_0);
    __ b(rewrite, Assembler::EQ);

    // else rewrite to _fast_aload0
    assert(Bytecodes::java_code(Bytecodes::_fast_aload_0) == Bytecodes::_aload_0, "fix bytecode definition");
    __ mov(bc, Bytecodes::Bytecodes::_fast_aload_0);

    // rewrite
    // bc: new bytecode
    __ bind(rewrite);
    patch_bytecode(Bytecodes::_aload_0, bc, r1, false);

    __ bind(done);
  } else {
    aload(0);
  }
}

void TemplateTable::istore()
{
  transition(itos, vtos);
  locals_index(r1);
  __ lea(rscratch1, iaddress(r1));
  __ str(r0, Address(rscratch1));
}

void TemplateTable::lstore()
{
  transition(ltos, vtos);
  locals_index(r2);
  __ strd(r0, r1, laddress(r2, r3, _masm));
}

void TemplateTable::fstore() {
  transition(ftos, vtos);
  locals_index(r1);
  __ lea(rscratch1, iaddress(r1));
  __ vstr_f32(d0, Address(rscratch1));
}

void TemplateTable::dstore() {
  transition(dtos, vtos);
  locals_index(r1);
  __ vstr_f64(d0, daddress(r1, rscratch1, _masm));
}

void TemplateTable::astore()
{
  transition(vtos, vtos);
  __ pop_ptr(r0);
  __ reg_printf("Astore, storing value %p\n", r0);
  locals_index(r1);
  __ str(r0, aaddress(r1));
}

void TemplateTable::wide_istore() {
  transition(vtos, vtos);
  __ pop_i();
  locals_index_wide(r1);
  __ lea(rscratch1, iaddress(r1));
  __ str(r0, Address(rscratch1));
}

void TemplateTable::wide_lstore() {
  transition(vtos, vtos);
  __ pop_l();
  locals_index_wide(r2);
  __ strd(r0, r1, laddress(r2, r3, _masm));
}

void TemplateTable::wide_fstore() {
  transition(vtos, vtos);
  __ pop_f();
  locals_index_wide(r1);
  __ lea(rscratch1, faddress(r1, rscratch1, _masm));
  __ vstr_f32(d0, rscratch1);
}

void TemplateTable::wide_dstore() {
  transition(vtos, vtos);
  __ pop_d();
  locals_index_wide(r1);
  __ vstr_f64(d0, daddress(r1, rscratch1, _masm));
}

void TemplateTable::wide_astore() {
  transition(vtos, vtos);
  __ pop_ptr(r0);
  locals_index_wide(r1);
  __ str(r0, aaddress(r1));
}

void TemplateTable::iastore() {
  transition(itos, vtos);
  __ pop_i(r2);
  __ pop_ptr(r3);
  // r0: value
  // r2: index
  // r3: array
  index_check(r3, r2); // prefer index in r2
  __ lea(rscratch1, Address(r3, r2, lsl(2)));
  __ str(r0, Address(rscratch1,
                     arrayOopDesc::base_offset_in_bytes(T_INT)));
}

void TemplateTable::lastore() {
  transition(ltos, vtos);
  __ pop_i(r2);
  __ pop_ptr(r3);
  // <r0:r1>: value
  // r2: index
  // r3: array
  index_check(r3, r2); // prefer index in r2
  __ lea(rscratch1, Address(r3, r2, lsl(3)));
  __ lea(rscratch1, Address(rscratch1,
                            arrayOopDesc::base_offset_in_bytes(T_LONG)));
  __ atomic_strd(r0, r1, rscratch1, r2, r3);
}

void TemplateTable::fastore() {
  transition(ftos, vtos);
  __ pop_i(r2);
  __ pop_ptr(r3);
  // d0: value
  // r2:  index
  // r3:  array
  index_check(r3, r2); // prefer index in r2
  __ lea(rscratch1, Address(r3, r2, lsl(2)));
  __ vstr_f32(d0, Address(rscratch1,
                      arrayOopDesc::base_offset_in_bytes(T_FLOAT)));
}

void TemplateTable::dastore() {
  transition(dtos, vtos);
  __ pop_i(r2);
  __ pop_ptr(r3);
  // d0: value
  // r2:  index
  // r3:  array
  index_check(r3, r2); // prefer index in r2
  __ lea(rscratch1, Address(r3, r2, lsl(3)));
  __ lea(rscratch1, Address(rscratch1,
                            arrayOopDesc::base_offset_in_bytes(T_DOUBLE)));
  __ vmov_f64(r0, r1, d0);
  __ atomic_strd(r0, r1, rscratch1, r2, r3);
}

void TemplateTable::aastore() {
  Label is_null, ok_is_subtype, done;
  transition(vtos, vtos);
  // stack: ..., array, index, value
  __ ldr(r0, at_tos());    // value
  __ ldr(r2, at_tos_p1()); // index
  __ ldr(r3, at_tos_p2()); // array

  Address element_address(r1, arrayOopDesc::base_offset_in_bytes(T_OBJECT));

  index_check(r3, r2);

  // do array store check - check for NULL value first
  __ cmp(r0, 0);
  __ b(is_null, Assembler::EQ);

  // Move subklass into r1
  __ load_klass(r1, r0);
  // Move superklass into r0
  __ load_klass(r0, r3);
  __ ldr(r0, Address(r0,
                     ObjArrayKlass::element_klass_offset()));
  // Compress array + index*oopSize + 12 into a single register.  Frees r2.

  // Generate subtype check.  Blows r2, r14?
  // Superklass in r0.  Subklass in r1.
  __ gen_subtype_check(r1, ok_is_subtype);

  // Come here on failure
  // object is at TOS
  __ b(Interpreter::_throw_ArrayStoreException_entry);

  // Come here on success
  __ bind(ok_is_subtype);

  // Get the value we will store
  __ ldr(r0, at_tos());
  // And the clobbered index
  __ ldr(r2, at_tos_p1()); // index
  __ lea(r1, Address(r3, r2, lsl(2)));
  // Now store using the appropriate barrier

  do_oop_store(_masm, element_address, r0, _bs->kind(), true);
  __ b(done);

  // Have a NULL in r0, r3=array, r2=index.  Store NULL at ary[idx]
  __ bind(is_null);
  __ profile_null_seen(r2);

  __ lea(r1, Address(r3, r2, lsl(2)));
  // Store a NULL
  do_oop_store(_masm, element_address, noreg, _bs->kind(), true);

  // Pop stack arguments
  __ bind(done);
  __ add(sp, sp, 3 * Interpreter::stackElementSize);
}

void TemplateTable::bastore()
{
  transition(itos, vtos);
  __ pop_i(r2);
  __ pop_ptr(r3);
  // r0: value
  // r2: index
  // r3: array
  index_check(r3, r2); // prefer index in r2

  // Need to check whether array is boolean or byte
  // since both types share the bastore bytecode.
  __ load_klass(r1, r3);
  __ ldr(r1, Address(r1, Klass::layout_helper_offset()));
  int diffbit = Klass::layout_helper_boolean_diffbit();
  __ tst(r1, diffbit);
  __ andr(r0, r0, 1, Assembler::NE);  // if it is a T_BOOLEAN array, mask the stored value to 0/1

  __ lea(rscratch1, Address(r3, r2));
  __ strb(r0, Address(rscratch1,
                      arrayOopDesc::base_offset_in_bytes(T_BYTE)));
}

void TemplateTable::castore()
{
  transition(itos, vtos);
  __ pop_i(r2);
  __ pop_ptr(r3);
  // r0: value
  // r2: index
  // r3: array
  index_check(r3, r2); // prefer index in r2
  __ lea(rscratch1, Address(r3, r2, lsl(1)));
  __ strh(r0, Address(rscratch1,
                      arrayOopDesc::base_offset_in_bytes(T_CHAR)));
}

void TemplateTable::sastore()
{
  castore();
}

void TemplateTable::istore(int n)
{
  transition(itos, vtos);
  __ str(r0, iaddress(n));
}

void TemplateTable::lstore(int n)
{
  transition(ltos, vtos);
  __ strd(r0, r1, laddress(n));
}

void TemplateTable::fstore(int n)
{
  transition(ftos, vtos);
  __ vstr_f32(d0, faddress(n));
}

void TemplateTable::dstore(int n)
{
  transition(dtos, vtos);
  __ vstr_f64(d0, daddress(n));
}

void TemplateTable::astore(int n)
{
  transition(vtos, vtos);
  __ pop_ptr(r0);
  __ str(r0, iaddress(n));
}

void TemplateTable::pop()
{
  transition(vtos, vtos);
  __ add(sp, sp, Interpreter::stackElementSize);
}

void TemplateTable::pop2()
{
  transition(vtos, vtos);
  __ add(sp, sp, 2 * Interpreter::stackElementSize);
}

void TemplateTable::dup()
{
  transition(vtos, vtos);
  __ ldr(r0, Address(sp, 0));
  __ reg_printf("Value duplicated is %p\n", r0);
  __ push(r0);
  // stack: ..., a, a
}

void TemplateTable::dup_x1()
{
  transition(vtos, vtos);
  // stack: ..., a, b
  __ ldr(r0, at_tos());  // load b
  __ ldr(r2, at_tos_p1());  // load a
  __ str(r0, at_tos_p1());  // store b
  __ str(r2, at_tos());  // store a
  __ push(r0);                  // push b
  // stack: ..., b, a, b
}

void TemplateTable::dup_x2()
{
  transition(vtos, vtos);
  // stack: ..., a, b, c
  __ ldr(r0, at_tos());  // load c
  __ ldr(r2, at_tos_p2());  // load a
  __ str(r0, at_tos_p2());  // store c in a
  __ push(r0);      // push c
  // stack: ..., c, b, c, c
  __ ldr(r0, at_tos_p2());  // load b
  __ str(r2, at_tos_p2());  // store a in b
  // stack: ..., c, a, c, c
  __ str(r0, at_tos_p1());  // store b in c
  // stack: ..., c, a, b, c
}

void TemplateTable::dup2()
{
  transition(vtos, vtos);
  // stack: ..., a, b
  __ ldr(r0, at_tos_p1());  // load a
  __ push(r0);                  // push a
  __ ldr(r0, at_tos_p1());  // load b
  __ push(r0);                  // push b
  // stack: ..., a, b, a, b
}

void TemplateTable::dup2_x1()
{
  transition(vtos, vtos);
  // stack: ..., a, b, c
  __ ldr(r2, at_tos());  // load c
  __ ldr(r0, at_tos_p1());  // load b
  __ push(r0);                  // push b
  __ push(r2);                  // push c
  // stack: ..., a, b, c, b, c
  __ str(r2, at_tos_p3());  // store c in b
  // stack: ..., a, c, c, b, c
  __ ldr(r2, at_tos_p4());  // load a
  __ str(r2, at_tos_p2());  // store a in 2nd c
  // stack: ..., a, c, a, b, c
  __ str(r0, at_tos_p4());  // store b in a
  // stack: ..., b, c, a, b, c
}

void TemplateTable::dup2_x2()
{
  transition(vtos, vtos);
  // stack: ..., a, b, c, d
  __ ldr(r2, at_tos());  // load d
  __ ldr(r0, at_tos_p1());  // load c
  __ push(r0)            ;      // push c
  __ push(r2);                  // push d
  // stack: ..., a, b, c, d, c, d
  __ ldr(r0, at_tos_p4());  // load b
  __ str(r0, at_tos_p2());  // store b in d
  __ str(r2, at_tos_p4());  // store d in b
  // stack: ..., a, d, c, b, c, d
  __ ldr(r2, at_tos_p5());  // load a
  __ ldr(r0, at_tos_p3());  // load c
  __ str(r2, at_tos_p3());  // store a in c
  __ str(r0, at_tos_p5());  // store c in a
  // stack: ..., c, d, a, b, c, d
}

void TemplateTable::swap()
{
  transition(vtos, vtos);
  // stack: ..., a, b
  __ ldr(r2, at_tos_p1());  // load a
  __ ldr(r0, at_tos());  // load b
  __ str(r2, at_tos());  // store a in b
  __ str(r0, at_tos_p1());  // store b in a
  // stack: ..., b, a
}

void TemplateTable::iop2(Operation op)
{
  transition(itos, itos);
  // r0 <== r1 op r0
  __ pop_i(r1);
  switch (op) {
  case add  : __ add(r0, r1, r0);  break;
  case sub  : __ sub(r0, r1, r0);  break;
  case mul  : __ mul(r0, r1, r0);  break;
  case _and : __ andr(r0, r1, r0); break;
  case _or  : __ orr(r0, r1, r0);  break;
  case _xor : __ eor(r0, r1, r0);  break;
  case shl  :
      __ andr(r0, r0, 0x1f);
      __ lsl(r0, r1, r0);
      break;
  case shr  :
      __ andr(r0, r0, 0x1f);
      __ asr(r0, r1, r0);
      break;
  case ushr :
      __ andr(r0, r0, 0x1f);
      __ lsr(r0, r1, r0);
      break;
  default   : ShouldNotReachHere();
  }
}

void TemplateTable::lop2(Operation op)
{
  transition(ltos, ltos);
  // <r1:r0> <== <r3:r2> op <r1:r0>
  __ pop_l(r2, r3);
  switch (op) {
  case add  : __ adds(r0, r2, r0); __ adc(r1, r3, r1);  break;
  case sub  : __ subs(r0, r2, r0); __ sbc(r1, r3, r1);  break;
  case mul  : __ mult_long(r0, r2, r0);                 break;
  case _and : __ andr(r0, r2, r0); __ andr(r1, r3, r1); break;
  case _or  : __ orr(r0, r2, r0);  __ orr(r1, r3, r1);  break;
  case _xor : __ eor(r0, r2, r0);  __ eor(r1, r3, r1);  break;
  default   : ShouldNotReachHere();
  }
}

void TemplateTable::idiv()
{
  transition(itos, itos);
  // explicitly check for div0
  Label no_div0;
  __ cmp(r0, 0);
  __ b(no_div0, Assembler::NE);
  __ mov(rscratch1, Interpreter::_throw_ArithmeticException_entry);
  __ b(rscratch1);
  __ bind(no_div0);
  __ pop_i(r1);
  // r0 <== r1 idiv r0
  __ divide(r0, r1, r0, 32, false);
}

void TemplateTable::irem()
{
  transition(itos, itos);
  // explicitly check for div0
  Label no_div0;
  __ cmp(r0, 0);
  __ b(no_div0, Assembler::NE);
  __ mov(rscratch1, Interpreter::_throw_ArithmeticException_entry);
  __ b(rscratch1);
  __ bind(no_div0);
  __ pop_i(r1);
  // r0 <== r1 irem r0
  __ divide(r0, r1, r0, 32, true);
}

void TemplateTable::lmul()
{
  transition(ltos, ltos);
  __ pop_l(r2, r3);
  __ mult_long(r0, r0, r2);
}

void TemplateTable::ldiv()
{
  transition(ltos, ltos);
  // explicitly check for div0
  __ cmp(r0, 0);
  __ cmp(r1, 0, Assembler::EQ);
  __ mov(rscratch1, Interpreter::_throw_ArithmeticException_entry, Assembler::EQ);
  __ b(rscratch1, Assembler::EQ);

  __ pop_l(r2, r3);
  // r0 <== r1 ldiv r0
  __ divide(r0, r2, r0, 64, false);
}

void TemplateTable::lrem()
{
  transition(ltos, ltos);
  // explicitly check for div0
  __ cmp(r0, 0);
  __ cmp(r1, 0, Assembler::EQ);
  __ mov(rscratch1, Interpreter::_throw_ArithmeticException_entry, Assembler::EQ);
  __ b(rscratch1, Assembler::EQ);

  __ pop_l(r2, r3);
  // r0 <== r1 lrem r0
  __ divide(r0, r2, r0, 64, true);
}

void TemplateTable::lshl() {
    transition(itos, ltos);
    // shift count is in r0 - take shift from bottom six bits only
    __ andr(r0, r0, 0x3f);
    __ pop_l(r2, r3);
    const int word_bits = 8 * wordSize;

    __ sub(r1, r0, word_bits);
    __ lsl(r3, r3, r0);
    __ orr(r3, r3, r2, lsl(r1));
    __ rsb(r1, r0, word_bits);
    __ orr(r1, r3, r2, lsr(r1));
    __ lsl(r0, r2, r0);
}

void TemplateTable::lshr() {
    transition(itos, ltos);
    // shift count is in r0 - take shift from bottom six bits only
    __ andr(rscratch1, r0, 0x3f);
    __ pop_l(r2, r3);
    const int word_bits = 8 * wordSize;

    __ lsr(r2, r2, rscratch1);
    __ rsb(r1, rscratch1, word_bits);
    __ orr(r0, r2, r3, lsl(r1));
    __ asr(r1, r3, rscratch1);
    __ subs(rscratch1, rscratch1, word_bits);
    __ orr(r0, r2, r3, asr(rscratch1), Assembler::GT);
}

void TemplateTable::lushr() {
    transition(itos, ltos);
    // shift count is in r0 - take shift from bottom six bits only
    __ andr(r0, r0, 0x3f);
    __ pop_l(r2, r3);
    const int word_bits = 8 * wordSize;

    __ lsr(r2, r2, r0);
    __ rsb(r1, r0, word_bits);
    __ orr(r2, r2, r3, lsl(r1));
    __ lsr(r1, r3, r0);
    __ sub(r0, r0, word_bits);
    __ orr(r0, r2, r3, lsr(r0));
}

void TemplateTable::fop2(Operation op)
{
  transition(ftos, ftos);
  switch (op) {
  case add:
    __ pop_f(d1);
    __ vadd_f32(d0, d1, d0);
    break;
  case sub:
    __ pop_f(d1);
    __ vsub_f32(d0, d1, d0);
    break;
  case mul:
    __ pop_f(d1);
    __ vmul_f32(d0, d1, d0);
    break;
  case div:
    __ pop_f(d1);
    __ vdiv_f32(d0, d1, d0);
    break;
  case rem:
    __ vcvt_f64_f32(d1, d0);
    __ pop_f(d0);
    __ vcvt_f64_f32(d0, d0);
#ifndef HARD_FLOAT_CC
    __ vmov_f64(r0, r1, d0);
    __ vmov_f64(r2, r3, d1);
#endif
    __ mov(rscratch1, (address)(double (*)(double, double))fmod);
    __ bl(rscratch1);
    __ vcvt_f32_f64(d0, d0);
    break;
  default:
    ShouldNotReachHere();
    break;
  }
}

void TemplateTable::dop2(Operation op)
{
  transition(dtos, dtos);
  switch (op) {
  case add:
    __ pop_d(d1);
    __ vadd_f64(d0, d1, d0);
    break;
  case sub:
    __ pop_d(d1);
    __ vsub_f64(d0, d1, d0);
    break;
  case mul:
    __ pop_d(d1);
    __ vmul_f64(d0, d1, d0);
    break;
  case div:
    __ pop_d(d1);
    __ vdiv_f64(d0, d1, d0);
    break;
  case rem:
    __ vmov_f64(d1, d0);
    __ pop_d(d0);
#ifndef HARD_FLOAT_CC
    __ vmov_f64(r0, r1, d0);
    __ vmov_f64(r2, r3, d1);
#endif
    __ mov(rscratch1, (address)(double (*)(double, double))fmod);
    __ bl(rscratch1);
    break;
  default:
    ShouldNotReachHere();
    break;
  }
}

void TemplateTable::ineg()
{
  transition(itos, itos);
  __ neg(r0, r0);

}

void TemplateTable::lneg()
{
  transition(ltos, ltos);
  __ rsbs(r0, r0, 0);
  __  rsc(r1, r1, 0);
}

void TemplateTable::fneg()
{
  transition(ftos, ftos);
  __ vneg_f32(d0, d0);
}

void TemplateTable::dneg()
{
  transition(dtos, dtos);
  __ vneg_f64(d0, d0);
}

void TemplateTable::iinc()
{
  transition(vtos, vtos);
  __ load_signed_byte(r1, at_bcp(2)); // get constant
  locals_index(r2);
  __ ldr(r0, iaddress(r2));
  __ add(r0, r0, r1);
  __ str(r0, iaddress(r2));
}

void TemplateTable::wide_iinc()
{
  transition(vtos, vtos);
  __ ldr(r1, at_bcp(2)); // get constant and index
  __ rev16(r1, r1);
  __ uxth(r2, r1);
  __ neg(r2, r2);
  __ sxth(r1, r1, ror(16));
  __ ldr(r0, iaddress(r2));
  __ add(r0, r0, r1);
  __ str(r0, iaddress(r2));
}

void TemplateTable::convert()
{
  // Checking
#ifdef ASSERT
  {
    TosState tos_in  = ilgl;
    TosState tos_out = ilgl;
    switch (bytecode()) {
    case Bytecodes::_i2l: // fall through
    case Bytecodes::_i2f: // fall through
    case Bytecodes::_i2d: // fall through
    case Bytecodes::_i2b: // fall through
    case Bytecodes::_i2c: // fall through
    case Bytecodes::_i2s: tos_in = itos; break;
    case Bytecodes::_l2i: // fall through
    case Bytecodes::_l2f: // fall through
    case Bytecodes::_l2d: tos_in = ltos; break;
    case Bytecodes::_f2i: // fall through
    case Bytecodes::_f2l: // fall through
    case Bytecodes::_f2d: tos_in = ftos; break;
    case Bytecodes::_d2i: // fall through
    case Bytecodes::_d2l: // fall through
    case Bytecodes::_d2f: tos_in = dtos; break;
    default             : ShouldNotReachHere();
    }
    switch (bytecode()) {
    case Bytecodes::_l2i: // fall through
    case Bytecodes::_f2i: // fall through
    case Bytecodes::_d2i: // fall through
    case Bytecodes::_i2b: // fall through
    case Bytecodes::_i2c: // fall through
    case Bytecodes::_i2s: tos_out = itos; break;
    case Bytecodes::_i2l: // fall through
    case Bytecodes::_f2l: // fall through
    case Bytecodes::_d2l: tos_out = ltos; break;
    case Bytecodes::_i2f: // fall through
    case Bytecodes::_l2f: // fall through
    case Bytecodes::_d2f: tos_out = ftos; break;
    case Bytecodes::_i2d: // fall through
    case Bytecodes::_l2d: // fall through
    case Bytecodes::_f2d: tos_out = dtos; break;
    default             : ShouldNotReachHere();
    }
    transition(tos_in, tos_out);
  }
#endif // ASSERT
  // static const int64_t is_nan = 0x8000000000000000L;
  //TODO fix this and remove _ sxtw and _ uxtw as don't exist in arm32
  // need to figure out about handling doubles and longs as they won't
  // fit into a single register in arm32
  // Conversion
  switch (bytecode()) {
  case Bytecodes::_i2l:
    // __ sxtw(r0, r0);
    __ reg_printf("Convert i2l (before) 0x00000000%08x\n", r0);
    __ asr(r1, r0, 31);
    __ reg_printf("Convert i2l (after) 0x%08x%08x\n", r1, r0);
    break;
  case Bytecodes::_i2f:
    //__ bkpt(735);
    //__ scvtfws(d0, r0);
    //__ reg_printf("VCVT Convert i2f, (before) 0x%08x\n", r0);
    __ vmov_f32(d0, r0);
    //__ vmov_f32(r0, d0);
    //__ reg_printf("VCVT Convert i2f, (before) 0x%08x\n", r0);
    __ vcvt_f32_s32(d0, d0);
    //__ vmov_f32(rscratch1, d0);
    //__ reg_printf("VCVT Convert i2f, (after ) 0x%08x\n", rscratch1);
    break;
  case Bytecodes::_i2d:
    //__ scvtfwd(d0, r0);
    __ vmov_f32(d0, r0);
    __ vcvt_f64_s32(d0, d0);
    break;
  case Bytecodes::_i2b:
    __ sxtb(r0, r0);
    break;
  case Bytecodes::_i2c:
    __ uxth(r0, r0);
    break;
  case Bytecodes::_i2s:
    __ sxth(r0, r0);
    break;
  case Bytecodes::_l2i:
    //__ uxtw(r0, r0);
    break;
  case Bytecodes::_l2f:
    // <r1:r0> -> d0
    __ call_VM_leaf_base(CAST_FROM_FN_PTR(address, SharedRuntime::l2f), 0);
    break;
  case Bytecodes::_l2d:
    // <r1:r0> -> d0
    __ call_VM_leaf_base(CAST_FROM_FN_PTR(address, SharedRuntime::l2d), 0);
    break;
   //FIXME these instructions have a fallback in aarch64 but not sure why these especially
  case Bytecodes::_f2i:
  {
    /*Label L_Okay;
    __ clear_fpsr();
    __ fcvtzsw(r0, d0);
    __ get_fpsr(r1);
    __ cmp(r1, 0);
    __ b(L_Okay, Assmembler::EQ);
    //__ call_VM_leaf_base1(CAST_FROM_FN_PTR(address, SharedRuntime::f2i),
    //                      0, 1, MacroAssembler::ret_type_integral);
    __ call_VM_leaf_base(CAST_FROM_FN_PTR(address, SharedRuntime::f2i), 0);
    //TODO why float not counted
    __ bind(L_Okay);*/
    __ vcvt_s32_f32(d0, d0);
    __ vmov_f32(r0, d0);
  }
    break;
  case Bytecodes::_f2l:
  {
    //float already in d0 long goes to <r1:r0>
#ifndef HARD_FLOAT_CC
    //Need to move float in d0 to r0
    __ vmov_f32(r0, d0);
#endif
    __ call_VM_leaf_base(CAST_FROM_FN_PTR(address, SharedRuntime::f2l), 0);
  }
    break;
  case Bytecodes::_f2d:
    __ vcvt_f64_f32(d0, d0);
    break;
  case Bytecodes::_d2i:
  {
    /*Label L_Okay;
    __ clear_fpsr();
    __ fcvtzdw(r0, d0);
    __ get_fpsr(r1);
    __ cmp(r1, 0);
    __ b(L_Okay, Assmembler::EQ);
    // __ call_VM_leaf_base1(CAST_FROM_FN_PTR(address, SharedRuntime::d2i),
    //                      0, 1, MacroAssembler::ret_type_integral);
    __ call_VM_leaf_base(CAST_FROM_FN_PTR(address, SharedRuntime::d2i), 0);
    // TODO why float not counted?
    __ bind(L_Okay);*/
    __ vcvt_s32_f64(d0, d0);
    __ vmov_f32(r0, d0);
  }
    break;
  case Bytecodes::_d2l:
  {
    // d0 -> <r1:r0>
#ifndef HARD_FLOAT_CC
    //Need to move float in d0 to r0
    __ vmov_f64(r0, r1, d0);
#endif
    __ call_VM_leaf_base(CAST_FROM_FN_PTR(address, SharedRuntime::d2l), 0);
  }
    break;
  case Bytecodes::_d2f:
    __ vcvt_f32_f64(d0, d0);
    break;
  default:
    ShouldNotReachHere();
  }
}

void TemplateTable::lcmp()
{
  transition(ltos, itos);
  __ pop_l(r2, r3);
  // <r1:r0> == <r3:r2> : 0
  // <r1:r0> < <r3:r2> : 1
  // <r1:r0> > <r3:r2> : -1
  __ reg_printf("Long comparing 0x%08x%08x\n", r1, r0);
  __ reg_printf("           and 0x%08x%08x\n", r3, r2);
  //cmp high
  Label lower, end;
  __ cmp(r3, r1);
  __ b(lower, Assembler::EQ);
  __ mov(r0, 1);
  __ sub(r0, r0, 2, Assembler::LT);
  __ b(end);

  __ bind(lower);
  __ subs(r0, r2, r0);
  __ mov(r0, 1, Assembler::NE);
  __ sub(r0, r0, 2, Assembler::LO); // Place -1
  __ bind(end);

  __ reg_printf("Result of comparison is %d\n", r0);
}

void TemplateTable::float_cmp(bool is_float, int unordered_result)
{
  //__ bkpt(400);
  if (is_float) {
    __ pop_f(d1);
    __ vcmp_f32(d1, d0);
  } else {
    __ pop_d(d1);
    /*__ vmov_f64(r0, r1, d0);
    __ vmov_f64(r2, r3, d1);
    __ reg_printf("Doing comparison cmp( 0x%08x%08x,\n", r3, r2);
    __ reg_printf("                      0x%08x%08x)\n", r1, r0);*/
    __ vcmp_f64(d1, d0);
  }
  __ vmrs(rscratch1);
  __ andr(rscratch1, rscratch1, Assembler::FP_MASK);
  __ reg_printf("Masked comparison result is %08x\n", rscratch1);

  if (unordered_result < 0) {
    // we want -1 for unordered or less than, 0 for equal and 1 for
    // greater than.
    __ mov(r0, -1);
    __ cmp(rscratch1, Assembler::FP_EQ);
    __ mov(r0, 0, Assembler::EQ);
    __ cmp(rscratch1, Assembler::FP_GT);
    __ mov(r0, 1, Assembler::EQ);
    __ reg_printf("un_res < 0, comparison result is %d\n", r0);
  } else {
    // we want -1 for less than, 0 for equal and 1 for unordered or
    // greater than.
    __ mov(r0, 1);
    __ cmp(rscratch1, Assembler::FP_LT);
    __ sub(r0, r0, 2, Assembler::EQ); //Load -1 - but one less instruction
    __ cmp(rscratch1, Assembler::FP_EQ);
    __ mov(r0, 0, Assembler::EQ);
    __ reg_printf("un_res >= 0, comparison result is %d\n", r0);
  }
}

void TemplateTable::branch(bool is_jsr, bool is_wide)
{
  // We might be moving to a safepoint.  The thread which calls
  // Interpreter::notice_safepoints() will effectively flush its cache
  // when it makes a system call, but we need to do something to
  // ensure that we see the changed dispatch table.
  __ membar(MacroAssembler::LoadLoad);

  __ profile_taken_branch(r0, r1);
  const ByteSize be_offset = MethodCounters::backedge_counter_offset() +
                             InvocationCounter::counter_offset();
  const ByteSize inv_offset = MethodCounters::invocation_counter_offset() +
                              InvocationCounter::counter_offset();

  // load branch displacement
  if (!is_wide) {
    __ ldrh(r2, at_bcp(1));
    __ rev16(r2, r2);
    // sign extend the 16 bit value in r2
    __ sxth(r2, r2);
  } else {
    __ ldr(r2, at_bcp(1));
    __ rev(r2, r2);
  }

  // Handle all the JSR stuff here, then exit.
  // It's much shorter and cleaner than intermingling with the non-JSR
  // normal-branch stuff occurring below.

  if (is_jsr) {
    // Pre-load the next target bytecode into rscratch1
    __ load_unsigned_byte(rscratch1, Address(rbcp, r2));
    // compute return address as bci
    __ ldr(rscratch2, Address(rmethod, Method::const_offset()));
    __ add(rscratch2, rscratch2,
           in_bytes(ConstMethod::codes_offset()) - (is_wide ? 5 : 3));
    __ sub(r1, rbcp, rscratch2);
    __ push_i(r1);
    // Adjust the bcp by the 16-bit displacement in r2
    __ add(rbcp, rbcp, r2);
    __ dispatch_only(vtos);
    return;
  }

  // Normal (non-jsr) branch handling

  // Adjust the bcp by the displacement in r2
  __ add(rbcp, rbcp, r2);

  assert(UseLoopCounter || !UseOnStackReplacement,
         "on-stack-replacement requires loop counters");
  Label backedge_counter_overflow;
  Label profile_method;
  Label dispatch;
  if (UseLoopCounter) {
    // increment backedge counter for backward branches
    // r0: MDO
    // w1: MDO bumped taken-count
    // r2: target offset
    __ cmp(r2, 0);
    __ b(dispatch, Assembler::GT); // count only if backward branch

    // ECN: FIXME: This code smells
    // check if MethodCounters exists
    Label has_counters;
    __ ldr(rscratch1, Address(rmethod, Method::method_counters_offset()));
    __ cbnz(rscratch1, has_counters);
    __ push(r0);
    __ push(r1);
    __ push(r2);
    __ call_VM(noreg, CAST_FROM_FN_PTR(address,
            InterpreterRuntime::build_method_counters), rmethod);
    __ pop(r2);
    __ pop(r1);
    __ pop(r0);
    __ ldr(rscratch1, Address(rmethod, Method::method_counters_offset()));
    __ cbz(rscratch1, dispatch); // No MethodCounters allocated, OutOfMemory
    __ bind(has_counters);

    if (TieredCompilation) {
      Label no_mdo;
      int increment = InvocationCounter::count_increment;
      int mask = ((1 << Tier0BackedgeNotifyFreqLog) - 1) << InvocationCounter::count_shift;
      if (ProfileInterpreter) {
        // Are we profiling?
        __ ldr(r1, Address(rmethod, in_bytes(Method::method_data_offset())));
        __ cbz(r1, no_mdo);
        // Increment the MDO backedge counter
        const Address mdo_backedge_counter(r1, in_bytes(MethodData::backedge_counter_offset()) +
                                           in_bytes(InvocationCounter::counter_offset()));
        __ increment_mask_and_jump(mdo_backedge_counter, increment, mask,
                                   r0, false, Assembler::EQ, &backedge_counter_overflow);
        __ b(dispatch);
      }
      __ bind(no_mdo);
      // Increment backedge counter in MethodCounters*
      __ ldr(rscratch1, Address(rmethod, Method::method_counters_offset()));
      __ increment_mask_and_jump(Address(rscratch1, be_offset), increment, mask,
                                 r0, false, Assembler::EQ, &backedge_counter_overflow);
    } else {
      // increment counter
      __ ldr(rscratch2, Address(rmethod, Method::method_counters_offset()));
      __ ldr(r0, Address(rscratch2, be_offset));        // load backedge counter
      __ add(rscratch1, r0, InvocationCounter::count_increment); // increment counter
      __ str(rscratch1, Address(rscratch2, be_offset));        // store counter

      __ ldr(r0, Address(rscratch2, inv_offset));    // load invocation counter
      __ mov(rscratch1, (unsigned)InvocationCounter::count_mask_value);
      __ andr(r0, r0, rscratch1); // and the status bits
      __ ldr(rscratch1, Address(rscratch2, be_offset));        // load backedge counter
      __ add(r0, r0, rscratch1);        // add both counters

      if (ProfileInterpreter) {
        // Test to see if we should create a method data oop
        __ lea(rscratch1, ExternalAddress((address) &InvocationCounter::InterpreterProfileLimit));
        __ ldr(rscratch1, rscratch1);
        __ cmp(r0, rscratch1);
        __ b(dispatch, Assembler::LT);

        // if no method data exists, go to profile method
        __ test_method_data_pointer(r0, profile_method);

        if (UseOnStackReplacement) {
          // check for overflow against w1 which is the MDO taken count
          __ lea(rscratch1, ExternalAddress((address) &InvocationCounter::InterpreterBackwardBranchLimit));
          __ ldr(rscratch1, rscratch1);
          __ cmp(r1, rscratch1);
          __ b(dispatch, Assembler::LO); // Intel == Assembler::below

          // When ProfileInterpreter is on, the backedge_count comes
          // from the MethodData*, which value does not get reset on
          // the call to frequency_counter_overflow().  To avoid
          // excessive calls to the overflow routine while the method is
          // being compiled, add a second test to make sure the overflow
          // function is called only once every overflow_frequency.
          const int overflow_frequency = 1024;
          const int of_mask_lsb = exact_log2(overflow_frequency);
          __ bfc(r1, of_mask_lsb, 32 - of_mask_lsb);
          __ cmp(r1, 0);
          __ b(backedge_counter_overflow, Assembler::EQ);

        }
      } else {
        if (UseOnStackReplacement) {
          // check for overflow against w0, which is the sum of the
          // counters
          __ lea(rscratch1, ExternalAddress((address) &InvocationCounter::InterpreterBackwardBranchLimit));
          __ ldr(rscratch1, rscratch1);
          __ cmp(r0, rscratch1);
          __ b(backedge_counter_overflow, Assembler::HS); // Intel == Assembler::aboveEqual
        }
      }
    }
  }
  __ bind(dispatch);

  // Pre-load the next target bytecode into rscratch1
  __ load_unsigned_byte(rscratch1, Address(rbcp, 0));

  // continue with the bytecode @ target
  // rscratch1: target bytecode
  // rbcp: target bcp
  __ dispatch_only(vtos);

  if (UseLoopCounter) {
    if (ProfileInterpreter) {
      // Out-of-line code to allocate method data oop.
      __ bind(profile_method);
      __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::profile_method));
      __ load_unsigned_byte(r1, Address(rbcp, 0));  // restore target bytecode
      __ set_method_data_pointer_for_bcp();
      __ b(dispatch);
    }

    if (TieredCompilation || UseOnStackReplacement) {
      // invocation counter overflow
      __ bind(backedge_counter_overflow);
      __ neg(r2, r2);
      __ add(r2, r2, rbcp);     // branch bcp
      // IcoResult frequency_counter_overflow([JavaThread*], address branch_bcp)
      __ call_VM(noreg,
                 CAST_FROM_FN_PTR(address,
                                  InterpreterRuntime::frequency_counter_overflow),
                 r2);
      if (!UseOnStackReplacement)
        __ b(dispatch);
    }

    if (UseOnStackReplacement) {
      __ load_unsigned_byte(r1, Address(rbcp, 0));  // restore target bytecode

      // r0: osr nmethod (osr ok) or NULL (osr not possible)
      // r1: target bytecode
      // r2: scratch
      __ cbz(r0, dispatch);     // test result -- no osr if null
      // nmethod may have been invalidated (VM may block upon call_VM return)
      __ ldr(r2, Address(r0, nmethod::entry_bci_offset()));
      // InvalidOSREntryBci == -2 which overflows cmpw as unsigned
      // use cmnw against -InvalidOSREntryBci which does the same thing
      __ cmn(r2, -InvalidOSREntryBci);
      __ b(dispatch, Assembler::EQ);

      // We have the address of an on stack replacement routine in r0
      // We need to prepare to execute the OSR method. First we must
      // migrate the locals and monitors off of the stack.

      __ mov(r4, r0);               // save the nmethod

      call_VM(noreg, CAST_FROM_FN_PTR(address, SharedRuntime::OSR_migration_begin));

      // r0 is OSR buffer, move it to expected parameter location
      __ mov(j_rarg0, r0);

      // remove activation
      // get sender sp
      __ ldr(rscratch1,
          Address(rfp, frame::interpreter_frame_sender_sp_offset * wordSize));
      // remove frame anchor
      __ leave();
      __ mov(sp, rscratch1);
      // Ensure compiled code always sees stack at proper alignment
      __ align_stack();

      // and begin the OSR nmethod
      __ ldr(rscratch1, Address(r4, nmethod::osr_entry_point_offset()));
      __ b(rscratch1);
    }
  }
}


void TemplateTable::if_0cmp(Condition cc)
{
  transition(itos, vtos);
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  /*if (cc == equal) {
    __ cmp(r0, 0);
    __ b(not_taken, Assembler::NE);
  } else if (cc == not_equal) {
    __ cmp(r0, 0);
    __ b(not_taken, Assembler::EQ);
  } else {
    __ ands(rscratch1, r0, r0);
    __ b(not_taken, j_not(cc));
  }*/
  __ cmp(r0, 0);
  __ b(not_taken, j_not(cc));

  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(r0);
}

void TemplateTable::if_icmp(Condition cc)
{
  transition(itos, vtos);
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  __ pop_i(r1);
  __ reg_printf("Comparing TOS = %p, and SOS = %p\n", r0, r1);
  __ cmp(r1, r0);
  __ b(not_taken, j_not(cc));
  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(r0);
}

void TemplateTable::if_nullcmp(Condition cc)
{
  transition(atos, vtos);
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  if (cc == equal)
    __ cbnz(r0, not_taken);
  else
    __ cbz(r0, not_taken);
  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(r0);
}

void TemplateTable::if_acmp(Condition cc)
{
  transition(atos, vtos);
  // assume branch is more often taken than not (loops use backward branches)
  Label not_taken;
  __ pop_ptr(r1);
  __ cmp(r1, r0);
  __ b(not_taken, j_not(cc));
  branch(false, false);
  __ bind(not_taken);
  __ profile_not_taken_branch(r0);
}

void TemplateTable::ret() {
  transition(vtos, vtos);
  // We might be moving to a safepoint.  The thread which calls
  // Interpreter::notice_safepoints() will effectively flush its cache
  // when it makes a system call, but we need to do something to
  // ensure that we see the changed dispatch table.
  __ membar(MacroAssembler::LoadLoad);

  locals_index(r1);
  __ ldr(r1, aaddress(r1)); // get return bci, compute return bcp
  __ profile_ret(r1, r2);
  __ ldr(rbcp, Address(rmethod, Method::const_offset()));
  __ lea(rbcp, Address(rbcp, r1));
  __ add(rbcp, rbcp, in_bytes(ConstMethod::codes_offset()));
  __ dispatch_next(vtos);
}

void TemplateTable::wide_ret() {
  transition(vtos, vtos);
  locals_index_wide(r1);
  __ ldr(r1, aaddress(r1)); // get return bci, compute return bcp
  __ profile_ret(r1, r2);
  __ ldr(rbcp, Address(rmethod, Method::const_offset()));
  __ lea(rbcp, Address(rbcp, r1));
  __ add(rbcp, rbcp, in_bytes(ConstMethod::codes_offset()));
  __ dispatch_next(vtos);
}

void TemplateTable::tableswitch() {
  Label default_case, continue_execution;
  transition(itos, vtos);
  // align rbcp
  __ lea(r1, at_bcp(BytesPerInt));
  __ bic(r1, r1, BytesPerInt - 1);
  // load lo & hi
  __ ldr(r2, Address(r1, BytesPerInt));
  __ ldr(r3, Address(r1, 2 * BytesPerInt));
  __ rev(r2, r2);
  __ rev(r3, r3);
  // check against lo & hi
  __ cmp(r0, r2);
  __ b(default_case, Assembler::LT);
  __ cmp(r0, r3);
  __ b(default_case, Assembler::GT);
  // lookup dispatch offset
  __ sub(r0, r0, r2);
  __ lea(r3, Address(r1, r0, lsl(2)));
  __ ldr(r3, Address(r3, 3 * BytesPerInt));
  __ profile_switch_case(r0, r1, r2);
  // continue execution
  __ bind(continue_execution);
  __ rev(r3, r3);
  __ load_unsigned_byte(rscratch1, Address(rbcp, r3));
  __ add(rbcp, rbcp, r3);
  __ dispatch_only(vtos);
  // handle default
  __ bind(default_case);
  __ profile_switch_default(r0);
  __ ldr(r3, Address(r1, 0));
  __ b(continue_execution);
}

void TemplateTable::lookupswitch() {
  transition(itos, itos);
  __ stop("lookupswitch bytecode should have been rewritten");
}

void TemplateTable::fast_linearswitch() {
  transition(itos, vtos);
  Label loop_entry, loop, found, continue_execution;

  __ reg_printf("Linearswitching to value %d\n", r0);

  // bswap r0 so we can avoid bswapping the table entries
  __ rev(r0, r0);
  // align rbcp
  __ lea(r14, at_bcp(BytesPerInt)); // btw: should be able to get rid of
                                    // this instruction (change offsets
                                    // below)
  __ bic(r14, r14, BytesPerInt - 1);
  // set counter
  __ ldr(r1, Address(r14, BytesPerInt));
  __ rev(r1, r1);
  __ b(loop_entry);
  // table search
  __ bind(loop);
  __ lea(rscratch1, Address(r14, r1, lsl(3)));
  __ ldr(rscratch1, Address(rscratch1, 2 * BytesPerInt));
  __ cmp(r0, rscratch1);
  __ b(found, Assembler::EQ);
  __ bind(loop_entry);
  __ subs(r1, r1, 1);
  __ b(loop, Assembler::PL);
  // default case
  __ profile_switch_default(r0);
  __ ldr(r3, Address(r14, 0));
  __ b(continue_execution);
  // entry found -> get offset
  __ bind(found);
  __ lea(rscratch1, Address(r14, r1, lsl(3)));
  __ ldr(r3, Address(rscratch1, 3 * BytesPerInt));
  __ profile_switch_case(r1, r0, r14);
  // continue execution
  __ bind(continue_execution);
  __ rev(r3, r3);
  __ add(rbcp, rbcp, r3);
  __ ldrb(rscratch1, Address(rbcp, 0));
  __ dispatch_only(vtos);
}

void TemplateTable::fast_binaryswitch() {
  transition(itos, vtos);
  // Implementation using the following core algorithm:
  //
  // int binary_search(int key, LookupswitchPair* array, int n) {
  //   // Binary search according to "Methodik des Programmierens" by
  //   // Edsger W. Dijkstra and W.H.J. Feijen, Addison Wesley Germany 1985.
  //   int i = 0;
  //   int j = n;
  //   while (i+1 < j) {
  //     // invariant P: 0 <= i < j <= n and (a[i] <= key < a[j] or Q)
  //     // with      Q: for all i: 0 <= i < n: key < a[i]
  //     // where a stands for the array and assuming that the (inexisting)
  //     // element a[n] is infinitely big.
  //     int h = (i + j) >> 1;
  //     // i < h < j
  //     if (key < array[h].fast_match()) {
  //       j = h;
  //     } else {
  //       i = h;
  //     }
  //   }
  //   // R: a[i] <= key < a[i+1] or Q
  //   // (i.e., if key is within array, i is the correct index)
  //   return i;
  // }

  // Register allocation
  const Register key   = r0; // already set (tosca)
  const Register array = r1;
  const Register i     = r2;
  const Register j     = r3;
  const Register h     = rscratch1;
  const Register temp  = rscratch2;

  // Find array start
  __ lea(array, at_bcp(3 * BytesPerInt)); // btw: should be able to
                                          // get rid of this
                                          // instruction (change
                                          // offsets below)
  __ bic(array, array, BytesPerInt - 1);

  // Initialize i & j
  __ mov(i, 0);                            // i = 0;
  __ ldr(j, Address(array, -BytesPerInt)); // j = length(array);

  // Convert j into native byteordering
  __ rev(j, j);

  // And start
  Label entry;
  __ b(entry);

  // binary search loop
  {
    Label loop;
    __ bind(loop);
    // int h = (i + j) >> 1;
    __ add(h, i, j);                           // h = i + j;
    __ lsr(h, h, 1);                                   // h = (i + j) >> 1;
    // if (key < array[h].fast_match()) {
    //   j = h;
    // } else {
    //   i = h;
    // }
    // Convert array[h].match to native byte-ordering before compare
    __ ldr(temp, Address(array, h, lsl(3)));
    __ rev(temp, temp);
    __ cmp(key, temp);
    // j = h if (key <  array[h].fast_match())
    __ mov(j, h, Assembler::LT);
    // i = h if (key >= array[h].fast_match())
    __ mov(i, h, Assembler::GE);
    // while (i+1 < j)
    __ bind(entry);
    __ add(h, i, 1);          // i+1
    __ cmp(h, j);             // i+1 < j
    __ b(loop, Assembler::LT);
  }

  // end of binary search, result index is i (must check again!)
  Label default_case;
  // Convert array[i].match to native byte-ordering before compare
  __ ldr(temp, Address(array, i, lsl(3)));
  __ rev(temp, temp);
  __ cmp(key, temp);
  __ b(default_case, Assembler::NE);

  // entry found -> j = offset
  __ add(j, array, i, lsl(3));
  __ ldr(j, Address(j, BytesPerInt));
  __ profile_switch_case(i, key, array);
  __ rev(j, j);
  __ load_unsigned_byte(rscratch1, Address(rbcp, j));
  __ lea(rbcp, Address(rbcp, j));
  __ dispatch_only(vtos);

  // default case -> j = default offset
  __ bind(default_case);
  __ profile_switch_default(i);
  __ ldr(j, Address(array, -2 * BytesPerInt));
  __ rev(j, j);
  __ load_unsigned_byte(rscratch1, Address(rbcp, j));
  __ lea(rbcp, Address(rbcp, j));
  __ dispatch_only(vtos);
}

void TemplateTable::_return(TosState state)
{
  __ reg_printf("STARTING RETURN\n");
  //__ stop("_return");
  transition(state, state);
  if(ltos == state) {
    __ reg_printf("Doing long return, tos value is 0x%08x%08x\n", r1, r0);
  } else if ( itos == state || atos == state) {
    __ reg_printf("Doing int/ref return, tos value is 0x%08x\n", r0);
  }

  assert(_desc->calls_vm(),
         "inconsistent calls_vm information"); // call in remove_activation

  if (_desc->bytecode() == Bytecodes::_return_register_finalizer) {
    assert(state == vtos, "only valid state");

    __ reg_printf("A\n");
    __ ldr(c_rarg1, aaddress(0));
    __ reg_printf("object is = %p\nB\n", c_rarg1);
    __ load_klass(r3, c_rarg1);
    __ reg_printf("C\n");
    __ ldr(r3, Address(r3, Klass::access_flags_offset()));
    __ reg_printf("D\n");
    __ tst(r3, JVM_ACC_HAS_FINALIZER);
    __ reg_printf("E\n");
    Label skip_register_finalizer;
    __ b(skip_register_finalizer, Assembler::EQ);
    __ reg_printf("About to call into the VM\n");
    __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::register_finalizer), c_rarg1);
    __ reg_printf("F\n");
    __ bind(skip_register_finalizer);
  }

  // Issue a StoreStore barrier after all stores but before return
  // from any constructor for any class with a final field.  We don't
  // know if this is a finalizer, so we always do so.
  if (_desc->bytecode() == Bytecodes::_return)
    __ membar(MacroAssembler::StoreStore);

  // Narrow result if state is itos but result type is smaller.
  // Need to narrow in the return bytecode rather than in generate_return_entry
  // since compiled code callers expect the result to already be narrowed.
  if (state == itos) {
    __ narrow(r0);
  }

  __ reg_printf("About to attmpt to remove activation with rfp = %p\n", rfp);
  __ remove_activation(state);
  __ reg_printf("Finshed _return, about to jump to lr = %p\n", lr);
  __ b(lr);
}

// ----------------------------------------------------------------------------
// Volatile variables demand their effects be made known to all CPU's
// in order.  Store buffers on most chips allow reads & writes to
// reorder; the JMM's ReadAfterWrite.java test fails in -Xint mode
// without some kind of memory barrier (i.e., it's not sufficient that
// the interpreter does not reorder volatile references, the hardware
// also must not reorder them).
//
// According to the new Java Memory Model (JMM):
// (1) All volatiles are serialized wrt to each other.  ALSO reads &
//     writes act as aquire & release, so:
// (2) A read cannot let unrelated NON-volatile memory refs that
//     happen after the read float up to before the read.  It's OK for
//     non-volatile memory refs that happen before the volatile read to
//     float down below it.
// (3) Similar a volatile write cannot let unrelated NON-volatile
//     memory refs that happen BEFORE the write float down to after the
//     write.  It's OK for non-volatile memory refs that happen after the
//     volatile write to float up before it.
//
// We only put in barriers around volatile refs (they are expensive),
// not _between_ memory refs (that would require us to track the
// flavor of the previous memory refs).  Requirements (2) and (3)
// require some barriers before volatile stores and after volatile
// loads.  These nearly cover requirement (1) but miss the
// volatile-store-volatile-load case.  This final case is placed after
// volatile-stores although it could just as well go before
// volatile-loads.

//Note none of these calls use rscratch1, well some do but are set again before return
// so index can be rscratch1 ( I think )
void TemplateTable::resolve_cache_and_index(int byte_no,
                                            Register Rcache,
                                            Register index,
                                            size_t index_size) {
  // Note none of the functions called here use any rscratch
  // call_VM may do but will save the argument first!
  const Register temp = rscratch2;
  assert_different_registers(Rcache, index, temp);

  Label resolved;
  assert(byte_no == f1_byte || byte_no == f2_byte, "byte_no out of range");
  __ get_cache_and_index_and_bytecode_at_bcp(Rcache, index, temp, byte_no, 1, index_size);
  __ cmp(temp, (int) bytecode());  // have we resolved this bytecode?
  __ b(resolved, Assembler::EQ);

  __ reg_printf("Not resolved, resolving, with rthread = %p, rfp = %p\n", rthread, rfp);
  // resolve first time through
  address entry;
  switch (bytecode()) {
  case Bytecodes::_getstatic:
  case Bytecodes::_putstatic:
  case Bytecodes::_getfield:
  case Bytecodes::_putfield:
    entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_get_put);
    break;
  case Bytecodes::_invokevirtual:
  case Bytecodes::_invokespecial:
  case Bytecodes::_invokestatic:
  case Bytecodes::_invokeinterface:
    entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_invoke);
    break;
  case Bytecodes::_invokehandle:
    entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_invokehandle);
    break;
  case Bytecodes::_invokedynamic:
    entry = CAST_FROM_FN_PTR(address, InterpreterRuntime::resolve_invokedynamic);
    break;
  default:
    fatal(err_msg("unexpected bytecode: %s", Bytecodes::name(bytecode())));
    break;
  }
  __ mov(temp, (int) bytecode());
  __ call_VM(noreg, entry, temp);
  __ reg_printf("Resolve complete\n");

  // Update registers with resolved info
  __ get_cache_and_index_at_bcp(Rcache, index, 1, index_size);
  // n.b. unlike x86 Rcache is now rcpool plus the indexed offset
  // so all clients ofthis method must be modified accordingly
  __ bind(resolved);
}

// The Rcache and index registers must be set before call
// n.b unlike x86 cache already includes the index offset
void TemplateTable::load_field_cp_cache_entry(Register obj,
                                              Register cache,
                                              Register index,
                                              Register off,
                                              Register flags,
                                              bool is_static = false) {
  assert_different_registers(cache, index, flags, off);

  ByteSize cp_base_offset = ConstantPoolCache::base_offset();
  // Field offset
  __ ldr(off, Address(cache, in_bytes(cp_base_offset +
                                      ConstantPoolCacheEntry::f2_offset())));
  // Flags
  __ ldr(flags, Address(cache, in_bytes(cp_base_offset +
                                        ConstantPoolCacheEntry::flags_offset())));

  // klass overwrite register
  if (is_static) {
    __ ldr(obj, Address(cache, in_bytes(cp_base_offset +
                                        ConstantPoolCacheEntry::f1_offset())));
    const int mirror_offset = in_bytes(Klass::java_mirror_offset());
    __ ldr(obj, Address(obj, mirror_offset));
  }
}


void TemplateTable::load_invoke_cp_cache_entry(int byte_no,
                                               Register method,
                                               Register itable_index,
                                               Register flags,
                                               bool is_invokevirtual,
                                               bool is_invokevfinal, /*unused*/
                                               bool is_invokedynamic) {
  //__ create_breakpoint();
  // setup registers
  const Register cache = rscratch1;
  const Register index = r14;
  assert_different_registers(method, flags);
  assert_different_registers(method, cache, index);
  assert_different_registers(itable_index, flags);
  assert_different_registers(itable_index, cache, index);
  // determine constant pool cache field offsets
  assert(is_invokevirtual == (byte_no == f2_byte), "is_invokevirtual flag redundant");
  const int method_offset = in_bytes(
    ConstantPoolCache::base_offset() +
      (is_invokevirtual
       ? ConstantPoolCacheEntry::f2_offset()
       : ConstantPoolCacheEntry::f1_offset()));
  const int flags_offset = in_bytes(ConstantPoolCache::base_offset() +
                                    ConstantPoolCacheEntry::flags_offset());
  // access constant pool cache fields
  const int index_offset = in_bytes(ConstantPoolCache::base_offset() +
                                    ConstantPoolCacheEntry::f2_offset());

  size_t index_size = (is_invokedynamic ? sizeof(u4) : sizeof(u2));
  resolve_cache_and_index(byte_no, cache, index, index_size);
  __ ldr(method, Address(cache, method_offset));

  if (itable_index != noreg) {
    __ ldr(itable_index, Address(cache, index_offset));
  }
  __ ldr(flags, Address(cache, flags_offset));

  __ reg_printf("Invocation, index = %d\n", index);
}


// The registers cache and index expected to be set before call.
// Correct values of the cache and index registers are preserved.
void TemplateTable::jvmti_post_field_access(Register cache, Register index,
                                            bool is_static, bool has_tos) {
  // do the JVMTI work here to avoid disturbing the register state below
  // We use c_rarg registers here because we want to use the register used in
  // the call to the VM
  if (JvmtiExport::can_post_field_access()) {
    // Check to see if a field access watch has been set before we
    // take the time to call into the VM.
    Label L1;
    assert_different_registers(cache, index, r0);
    __ lea(rscratch1, ExternalAddress((address) JvmtiExport::get_field_access_count_addr()));
    __ ldr(r0, Address(rscratch1));
    __ cmp(r0, 0);
    __ b(L1, Assembler::EQ);

    __ get_cache_and_index_at_bcp(c_rarg2, c_rarg3, 1);
    __ lea(c_rarg2, Address(c_rarg2, in_bytes(ConstantPoolCache::base_offset())));

    if (is_static) {
      __ mov(c_rarg1, 0); // NULL object reference
    } else {
      __ ldr(c_rarg1, at_tos()); // get object pointer without popping it
      __ verify_oop(c_rarg1);
    }
    // c_rarg1: object pointer or NULL
    // c_rarg2: cache entry pointer
    // c_rarg3: jvalue object on the stack
    __ call_VM(noreg, CAST_FROM_FN_PTR(address,
                                       InterpreterRuntime::post_field_access),
               c_rarg1, c_rarg2, c_rarg3);
    __ get_cache_and_index_at_bcp(cache, index, 1);
    __ bind(L1);
  }
}

void TemplateTable::pop_and_check_object(Register r)
{
  __ pop_ptr(r);
  __ null_check(r);  // for field access must check obj.
  __ verify_oop(r);
}

void TemplateTable::getfield_or_static(int byte_no, bool is_static) {
  //__ stop("getfield or static");
  //FIXME Find a better way than this!
  const Register cache = r2;
  const Register index = r3;
  const Register obj   = r14;
  const Register off   = rscratch2; //pop_and_check_object
  const Register flags = r0;
  const Register bc    = r14; // uses same reg as obj, so don't mix them

  resolve_cache_and_index(byte_no, cache, index, sizeof(u2));
  jvmti_post_field_access(cache, index, is_static, false);
  load_field_cp_cache_entry(obj, cache, index, off, flags, is_static);

  if (!is_static) {
    // obj is on the stack
    pop_and_check_object(obj);
  }

  const Address field(obj, off);


  Label Done, notByte, notBool, notInt, notShort, notChar,
              notLong, notFloat, notObj, notDouble;

  //__ bkpt(324);
  // x86 uses a shift and mask or wings it with a shift plus assert
  // the mask is not needed. aarch32 just uses bitfield extract
  __ extract_bits(flags, flags, ConstantPoolCacheEntry::tos_state_shift,  ConstantPoolCacheEntry::tos_state_bits);

  assert(btos == 0, "change code, btos != 0");
  __ cbnz(flags, notByte);

  // btos
  __ load_signed_byte(r0, field);
  __ push(btos);
  // Rewrite bytecode to be faster
  if (!is_static) {
    patch_bytecode(Bytecodes::_fast_bgetfield, bc, r1);
  }
  __ b(Done);

  __ bind(notByte);
  __ cmp(flags, ztos);
  __ b(notBool, Assembler::NE);

  // ztos (same code as btos)
  __ ldrsb(r0, field);
  __ push(ztos);
  // Rewrite bytecode to be faster
  if (!is_static) {
    // use btos rewriting, no truncating to t/f bit is needed for getfield.
    patch_bytecode(Bytecodes::_fast_bgetfield, bc, r1);
  }
  __ b(Done);

  __ bind(notBool);
  __ cmp(flags, atos);
  __ b(notObj, Assembler::NE);
  // atos
  __ load_heap_oop(r0, field);
  __ push(atos);
  __ reg_printf("Getfield or static, atos = 0x%08x\n", r0);
  if (!is_static) {
    patch_bytecode(Bytecodes::_fast_agetfield, bc, r1);
  }
  __ b(Done);

  __ bind(notObj);
  __ cmp(flags, itos);
  __ b(notInt, Assembler::NE);
  // itos
  __ ldr(r0, field);
  __ push(itos);
  __ reg_printf("Getfield or static, itos = 0x%08x\n", r0);
  // Rewrite bytecode to be faster
  if (!is_static) {
    patch_bytecode(Bytecodes::_fast_igetfield, bc, r1);
  }
  __ b(Done);

  __ bind(notInt);
  __ cmp(flags, ctos);
  __ b(notChar, Assembler::NE);
  // ctos
  __ load_unsigned_short(r0, field);
  __ push(ctos);
  // Rewrite bytecode to be faster
  if (!is_static) {
    patch_bytecode(Bytecodes::_fast_cgetfield, bc, r1);
  }
  __ b(Done);

  __ bind(notChar);
  __ cmp(flags, stos);
  __ b(notShort, Assembler::NE);
  // stos
  __ load_signed_short(r0, field);
  __ push(stos);
  // Rewrite bytecode to be faster
  if (!is_static) {
    patch_bytecode(Bytecodes::_fast_sgetfield, bc, r1);
  }
  __ b(Done);

  __ bind(notShort);
  __ cmp(flags, ltos);
  __ b(notLong, Assembler::NE);
  // ltos
  __ lea(rscratch1, field);
  __ atomic_ldrd(r0, r1, rscratch1);
  __ push(ltos);
  // Rewrite bytecode to be faster
  if (!is_static) {
    patch_bytecode(Bytecodes::_fast_lgetfield, bc, r1);
  }
  __ b(Done);

  __ bind(notLong);
  __ cmp(flags, ftos);
  __ b(notFloat, Assembler::NE);
  // ftos
  __ lea(rscratch1, field);
  __ vldr_f32(d0, Address(rscratch1));
  __ push(ftos);
  // Rewrite bytecode to be faster
  if (!is_static) {
    patch_bytecode(Bytecodes::_fast_fgetfield, bc, r1);
  }
  __ b(Done);

  __ bind(notFloat);
#ifdef ASSERT
  __ cmp(flags, dtos);
  __ b(notDouble, Assembler::NE);
#endif
  // dtos
  __ lea(rscratch1, field);
  __ atomic_ldrd(r0, r1, rscratch1);
  __ vmov_f64(d0, r0, r1);
  __ push(dtos);
  // Rewrite bytecode to be faster
  if (!is_static) {
    patch_bytecode(Bytecodes::_fast_dgetfield, bc, r1);
  }
#ifdef ASSERT
  __ b(Done);

  __ bind(notDouble);
  __ stop("Bad state");
#endif

  __ bind(Done);
  // It's really not worth bothering to check whether this field
  // really is volatile in the slow case.
  __ membar(MacroAssembler::LoadLoad | MacroAssembler::LoadStore);
}

void TemplateTable::getfield(int byte_no) {
  getfield_or_static(byte_no, false);
}

void TemplateTable::getstatic(int byte_no) {
  getfield_or_static(byte_no, true);
}

// The registers cache and index expected to be set before call.
// The function may destroy various registers, just not the cache and index registers.
void TemplateTable::jvmti_post_field_mod(Register cache, Register index, bool is_static) {
  transition(vtos, vtos);

  ByteSize cp_base_offset = ConstantPoolCache::base_offset();

  if (JvmtiExport::can_post_field_modification()) {
    // Check to see if a field modification watch has been set before
    // we take the time to call into the VM.
    Label L1;
    assert_different_registers(cache, index, r0);
    __ lea(rscratch1, ExternalAddress((address)JvmtiExport::get_field_modification_count_addr()));
    __ ldr(r0, Address(rscratch1));
    __ cbz(r0, L1);

    __ get_cache_and_index_at_bcp(c_rarg2, rscratch1, 1);

    if (is_static) {
      // Life is simple.  Null out the object pointer.
      __ mov(c_rarg1, 0);
    } else {
      // Life is harder. The stack holds the value on top, followed by
      // the object.  We don't know the size of the value, though; it
      // could be one or two words depending on its type. As a result,
      // we must find the type to determine where the object is.
      __ ldr(c_rarg3, Address(c_rarg2,
                              in_bytes(cp_base_offset +
                                       ConstantPoolCacheEntry::flags_offset())));
      __ lsr(c_rarg3, c_rarg3,
             ConstantPoolCacheEntry::tos_state_shift);
      ConstantPoolCacheEntry::verify_tos_state_shift();
      Label nope2, done, ok;
      __ ldr(c_rarg1, at_tos_p1());  // initially assume a one word jvalue
      __ cmp(c_rarg3, ltos);
      __ b(ok, Assembler::EQ);
      __ cmp(c_rarg3, dtos);
      __ b(nope2, Assembler::NE);
      __ bind(ok);
      __ ldr(c_rarg1, at_tos_p2()); // ltos (two word jvalue)
      __ bind(nope2);
    }
    // cache entry pointer
    __ add(c_rarg2, c_rarg2, in_bytes(cp_base_offset));
    // object (tos)
    __ mov(c_rarg3, sp);
    // c_rarg1: object pointer set up above (NULL if static)
    // c_rarg2: cache entry pointer
    // c_rarg3: jvalue object on the stack
    __ call_VM(noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::post_field_modification),
               c_rarg1, c_rarg2, c_rarg3);
    __ get_cache_and_index_at_bcp(cache, index, 1);
    __ bind(L1);
  }
}

void TemplateTable::putfield_or_static(int byte_no, bool is_static) {
  transition(vtos, vtos);
  const Register cache = r2;
  const Register index = rscratch1;
  const Register obj   = r2;
  const Register off   = rscratch2;
  const Register flags = r14;
  const Register bc    = r3;

  resolve_cache_and_index(byte_no, cache, index, sizeof(u2));
  __ reg_printf("Putfield or static, index = %d\n", index);
  jvmti_post_field_mod(cache, index, is_static);
  load_field_cp_cache_entry(obj, cache, index, off, flags, is_static);

  Label Done;
  {
    Label notVolatile;
    __ tbz(flags, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
    __ membar(MacroAssembler::StoreStore);
    __ bind(notVolatile);
  }
  __ reg_printf("Putfield or static B\n");

  // field address
  const Address field(obj, off);

  Label notByte, notBool, notInt, notShort, notChar,
        notLong, notFloat, notObj, notDouble;

  // x86 uses a shift and mask or wings it with a shift plus assert
  // the mask is not needed. aarch32 just uses bitfield extract
  __ extract_bits(rscratch1, flags, ConstantPoolCacheEntry::tos_state_shift, ConstantPoolCacheEntry::tos_state_bits);

  // Move the flags to rscratch2

  __ cmp(rscratch1, btos);
  __ b(notByte, Assembler::NE);

  // btos
  {
    __ pop(btos);
    if (!is_static) {
      pop_and_check_object(obj);
    }
    __ strb(r0, field);
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_bputfield, bc, r1, true, byte_no);
    }
    __ b(Done);
  }

  __ bind(notByte);
  __ cmp(rscratch1, ztos);
  __ b(notBool, Assembler::NE);

  // ztos
  {
    __ pop(ztos);
    if (!is_static) pop_and_check_object(obj);
    __ andr(r0, r0, 0x1);
    __ strb(r0, field);
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_zputfield, bc, r1, true, byte_no);
    }
    __ b(Done);
  }

  __ bind(notBool);
  __ cmp(rscratch1, atos);
  __ b(notObj, Assembler::NE);

  // atos
  {
    __ pop(atos);
    if (!is_static) {
      pop_and_check_object(obj);
    }
    // Store into the field
    do_oop_store(_masm, field, r0, _bs->kind(), false);
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_aputfield, bc, r1, true, byte_no);
    }
    __ b(Done);
  }

  __ bind(notObj);
  __ cmp(rscratch1, itos);
  __ b(notInt, Assembler::NE);

  // itos
  {
    __ pop(itos);
    if (!is_static) {
      pop_and_check_object(obj);
    }
    __ str(r0, field);
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_iputfield, bc, r1, true, byte_no);
    }
    __ b(Done);
  }

  __ bind(notInt);
  __ cmp(rscratch1, ctos);
  __ b(notChar, Assembler::NE);

  // ctos
  {
    __ pop(ctos);
    if (!is_static) {
      pop_and_check_object(obj);
    }
    __ strh(r0, field);
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_cputfield, bc, r1, true, byte_no);
    }
    __ b(Done);
  }

  __ bind(notChar);
  __ cmp(rscratch1, stos);
  __ b(notShort, Assembler::NE);

  // stos
  {
    __ pop(stos);
    if (!is_static) {
      pop_and_check_object(obj);
    }
    __ strh(r0, field);
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_sputfield, bc, r1, true, byte_no);
    }
    __ b(Done);
  }

  __ bind(notShort);
  __ cmp(rscratch1, ltos);
  __ b(notLong, Assembler::NE);

  // ltos
  {
    __ pop(ltos);
    if (!is_static) {
      pop_and_check_object(obj);
    }
    __ lea(rscratch1, field);
    __ atomic_strd(r0, r1, rscratch1, r2, r3);
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_lputfield, bc, r1, true, byte_no);
    }
    __ b(Done);
  }

  __ bind(notLong);
  __ cmp(rscratch1, ftos);
  __ b(notFloat, Assembler::NE);

  // ftos
  {
    __ pop(ftos);
    if (!is_static) {
      pop_and_check_object(obj);
    }
    __ lea(rscratch1, field);
    __ vstr_f32(d0, Address(rscratch1));
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_fputfield, bc, r1, true, byte_no);
    }
    __ b(Done);
  }

  __ bind(notFloat);
#ifdef ASSERT
  __ cmp(rscratch1, dtos);
  __ b(notDouble, Assembler::NE);
#endif // ASSERT

  // dtos
  {
    __ pop(dtos);
    if (!is_static) {
      pop_and_check_object(obj);
    }
    __ lea(rscratch1, field);
    __ vmov_f64(r0, r1, d0);
    __ atomic_strd(r0, r1, rscratch1, r2, r3);
    if (!is_static) {
      patch_bytecode(Bytecodes::_fast_dputfield, bc, r1, true, byte_no);
    }
  }

#ifdef ASSERT
  __ b(Done);

  __ bind(notDouble);
  __ stop("Bad state");
#endif // ASSERT

  __ bind(Done);

  {
    Label notVolatile;
    __ tbz(flags, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
    __ membar(MacroAssembler::StoreLoad);
    __ bind(notVolatile);
  }
}

void TemplateTable::putfield(int byte_no) {
  putfield_or_static(byte_no, false);
}

void TemplateTable::putstatic(int byte_no) {
  putfield_or_static(byte_no, true);
}

void TemplateTable::jvmti_post_fast_field_mod()
{
  if (JvmtiExport::can_post_field_modification()) {
    // Check to see if a field modification watch has been set before
    // we take the time to call into the VM.
    Label L2;
    __ lea(rscratch1, ExternalAddress((address)JvmtiExport::get_field_modification_count_addr()));
    __ ldr(c_rarg3, Address(rscratch1));
    __ cmp(c_rarg3, 0);
    __ b(L2, Assembler::EQ);
    __ pop_ptr(r14);                  // copy the object pointer from tos
    __ verify_oop(r14);
    __ push_ptr(r14);                 // put the object pointer back on tos
    // Save tos values before call_VM() clobbers them. Since we have
    // to do it for every data type, we use the saved values as the
    // jvalue object.
    switch (bytecode()) {          // load values into the jvalue object
    case Bytecodes::_fast_aputfield: __ push_ptr(r0); break;
    case Bytecodes::_fast_bputfield: // fall through
    case Bytecodes::_fast_zputfield: // fall through
    case Bytecodes::_fast_sputfield: // fall through
    case Bytecodes::_fast_cputfield: // fall through
    case Bytecodes::_fast_iputfield: __ push_i(r0); break;
    case Bytecodes::_fast_dputfield: __ push_d(); break;
    case Bytecodes::_fast_fputfield: __ push_f(); break;
    case Bytecodes::_fast_lputfield: __ push_l(r0); break;

    default:
      ShouldNotReachHere();
    }
    __ mov(c_rarg3, sp);             // points to jvalue on the stack
    // access constant pool cache entry
    __ get_cache_entry_pointer_at_bcp(c_rarg2, r0, 1);
    __ verify_oop(r14);
    // r14: object pointer copied above
    // c_rarg2: cache entry pointer
    // c_rarg3: jvalue object on the stack
    __ call_VM(noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::post_field_modification),
               r14, c_rarg2, c_rarg3);

    switch (bytecode()) {             // restore tos values
    case Bytecodes::_fast_aputfield: __ pop_ptr(r0); break;
    case Bytecodes::_fast_bputfield: // fall through
    case Bytecodes::_fast_zputfield: // fall through
    case Bytecodes::_fast_sputfield: // fall through
    case Bytecodes::_fast_cputfield: // fall through
    case Bytecodes::_fast_iputfield: __ pop_i(r0); break;
    case Bytecodes::_fast_dputfield: __ pop_d(); break;
    case Bytecodes::_fast_fputfield: __ pop_f(); break;
    case Bytecodes::_fast_lputfield: __ pop_l(r0); break;
    }
    __ bind(L2);
  }
}

void TemplateTable::fast_storefield(TosState state)
{
  transition(state, vtos);

  ByteSize base = ConstantPoolCache::base_offset();

  jvmti_post_fast_field_mod();

  // access constant pool cache
  __ get_cache_and_index_at_bcp(r2, rscratch1, 1); // index not used

  // test for volatile with r14
  __ ldr(r14, Address(r2, in_bytes(base +
                                   ConstantPoolCacheEntry::flags_offset())));

  // replace index with field offset from cache entry
  __ ldr(r3, Address(r2, in_bytes(base + ConstantPoolCacheEntry::f2_offset())));

  {
    Label notVolatile;
    __ tbz(r14, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
    __ membar(MacroAssembler::StoreStore);
    __ bind(notVolatile);
  }

  Label notVolatile;

  // Get object from stack
  pop_and_check_object(r2);

  // field address
  const Address field(r2, r3);

  // access field
  switch (bytecode()) {
  case Bytecodes::_fast_aputfield:
    do_oop_store(_masm, field, r0, _bs->kind(), false);
    break;
  case Bytecodes::_fast_lputfield:
    __ lea(rscratch1, field);
    __ atomic_strd(r0, r1, rscratch1, r2, r3);
    break;
  case Bytecodes::_fast_iputfield:
    __ str(r0, field);
    break;
  case Bytecodes::_fast_zputfield:
    __ andr(r0, r0, 0x1);  // boolean is true if LSB is 1
    // fall through to bputfield
  case Bytecodes::_fast_bputfield:
    __ strb(r0, field);
    break;
  case Bytecodes::_fast_sputfield:
    // fall through
  case Bytecodes::_fast_cputfield:
    __ strh(r0, field);
    break;
  case Bytecodes::_fast_fputfield:
    __ lea(rscratch1, field);
    __ vstr_f32(d0, Address(rscratch1));
    break;
  case Bytecodes::_fast_dputfield:
    __ lea(rscratch1, field);
    __ vmov_f64(r0, r1, d0);
    __ atomic_strd(r0, r1, rscratch1, r2, r3);
    break;
  default:
    ShouldNotReachHere();
  }

  {
    Label notVolatile;
    __ tbz(r14, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
    __ membar(MacroAssembler::StoreLoad);
    __ bind(notVolatile);
  }
}


void TemplateTable::fast_accessfield(TosState state)
{
  transition(atos, state);
  // Do the JVMTI work here to avoid disturbing the register state below
  if (JvmtiExport::can_post_field_access()) {
    // Check to see if a field access watch has been set before we
    // take the time to call into the VM.
    Label L1;
    __ lea(rscratch1, ExternalAddress((address) JvmtiExport::get_field_access_count_addr()));
    __ ldr(r2, Address(rscratch1));
    __ cmp(r2, 0);
    __ b(L1, Assembler::EQ);
    // access constant pool cache entry
    __ get_cache_entry_pointer_at_bcp(c_rarg2, rscratch2, 1);
    __ verify_oop(r0);
    __ push_ptr(r0);  // save object pointer before call_VM() clobbers it
    __ mov(c_rarg1, r0);
    // c_rarg1: object pointer copied above
    // c_rarg2: cache entry pointer
    __ call_VM(noreg,
               CAST_FROM_FN_PTR(address,
                                InterpreterRuntime::post_field_access),
               c_rarg1, c_rarg2);
    __ pop_ptr(r0); // restore object pointer
    __ bind(L1);
  }

  // access constant pool cache
  __ get_cache_and_index_at_bcp(r2, r1, 1);
  __ ldr(r1, Address(r2, in_bytes(ConstantPoolCache::base_offset() +
                                  ConstantPoolCacheEntry::f2_offset())));
  __ ldr(r3, Address(r2, in_bytes(ConstantPoolCache::base_offset() +
                                  ConstantPoolCacheEntry::flags_offset())));

  // r0: object
  __ verify_oop(r0);
  __ null_check(r0);
  const Address field(r0, r1);

  // access field
  switch (bytecode()) {
  case Bytecodes::_fast_agetfield:
    __ load_heap_oop(r0, field);
    __ verify_oop(r0);
    break;
  case Bytecodes::_fast_lgetfield:
    __ lea(rscratch1, field);
    __ atomic_ldrd(r0, r1, rscratch1);
    break;
  case Bytecodes::_fast_igetfield:
    __ ldr(r0, field);
    break;
  case Bytecodes::_fast_bgetfield:
    __ load_signed_byte(r0, field);
    break;
  case Bytecodes::_fast_sgetfield:
    __ load_signed_short(r0, field);
    break;
  case Bytecodes::_fast_cgetfield:
    __ load_unsigned_short(r0, field);
    break;
  case Bytecodes::_fast_fgetfield:
    __ lea(r0, field); // r0 <= field
    __ vldr_f32(d0, Address(r0));
    __ vmov_f32(rscratch1, d0);
    break;
  case Bytecodes::_fast_dgetfield:
    __ lea(rscratch1, field); // r0 <= field
    __ atomic_ldrd(r0, r1, rscratch1);
    __ vmov_f64(d0, r0, r1);
    break;
  default:
    ShouldNotReachHere();
  }
  {
    Label notVolatile;
    __ tbz(r3, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
    __ membar(MacroAssembler::LoadLoad | MacroAssembler::LoadStore);
    __ bind(notVolatile);
  }
}

void TemplateTable::fast_xaccess(TosState state)
{
  transition(vtos, state);

  // get receiver
  __ ldr(r0, aaddress(0));
  // access constant pool cache
  __ get_cache_and_index_at_bcp(r2, r3, 2);
  __ ldr(r1, Address(r2, in_bytes(ConstantPoolCache::base_offset() +
                                  ConstantPoolCacheEntry::f2_offset())));
  // make sure exception is reported in correct bcp range (getfield is
  // next instruction)
  __ add(rbcp, rbcp, 1);
  __ null_check(r0);

  Address field(r0, r1);
  switch (state) {
  case itos:
    __ ldr(r0, field);
    break;
  case atos:
    __ load_heap_oop(r0, field);
    __ verify_oop(r0);
    break;
  case ftos:
    __ lea(r0, field);
    __ vldr_f32(d0, Address(r0));
    break;
  default:
    ShouldNotReachHere();
  }

  {
    Label notVolatile;
    __ ldr(r3, Address(r2, in_bytes(ConstantPoolCache::base_offset() +
                                     ConstantPoolCacheEntry::flags_offset())));
    __ tbz(r3, ConstantPoolCacheEntry::is_volatile_shift, notVolatile);
    __ membar(MacroAssembler::LoadLoad);
    __ bind(notVolatile);
  }

  __ sub(rbcp, rbcp, 1);
}



//-----------------------------------------------------------------------------
// Calls

void TemplateTable::count_calls(Register method, Register temp) {
  // implemented elsewhere
  ShouldNotReachHere();
}

void TemplateTable::prepare_invoke(int byte_no,
                                   Register method, // linked method (or i-klass)
                                   Register index,  // itable index, MethodType, etc.
                                   Register recv,   // if caller wants to see it
                                   Register flags   // if caller wants to test it
                                   ) {
  // determine flags
  Bytecodes::Code code = bytecode();
  const bool is_invokeinterface  = code == Bytecodes::_invokeinterface;
  const bool is_invokedynamic    = code == Bytecodes::_invokedynamic;
  const bool is_invokehandle     = code == Bytecodes::_invokehandle;
  const bool is_invokevirtual    = code == Bytecodes::_invokevirtual;
  const bool is_invokespecial    = code == Bytecodes::_invokespecial;
  const bool load_receiver       = (recv  != noreg);
  const bool save_flags          = (flags != noreg);
  assert(load_receiver == (code != Bytecodes::_invokestatic && code != Bytecodes::_invokedynamic), "");
  assert(save_flags    == (is_invokeinterface || is_invokevirtual), "need flags for vfinal");
  assert(flags == noreg || flags == r3, "");
  assert(recv  == noreg || recv  == r2, "");

  // setup registers & access constant pool cache
  if (recv  == noreg)  recv  = r2;
  if (flags == noreg)  flags = r3;
  assert_different_registers(method, index, recv, flags);

  // save 'interpreter return address'
  __ save_bcp();

  load_invoke_cp_cache_entry(byte_no, method, index, flags, is_invokevirtual, false, is_invokedynamic);

  // maybe push appendix to arguments (just before return address)
  if (is_invokedynamic || is_invokehandle) {
    Label L_no_push;
    __ tbz(flags, ConstantPoolCacheEntry::has_appendix_shift, L_no_push);
    // Push the appendix as a trailing parameter.
    // This must be done before we get the receiver,
    // since the parameter_size includes it.
    __ push(r14); //NOT NEEDED?!
    __ mov(r14, index);
    assert(ConstantPoolCacheEntry::_indy_resolved_references_appendix_offset == 0, "appendix expected at index+0");
    __ load_resolved_reference_at_index(index, r14);
    __ pop(r14);
    __ push(index);  // push appendix (MethodType, CallSite, etc.)
    __ bind(L_no_push);
  }

  // load receiver if needed (note: no return address pushed yet)
  if (load_receiver) {
    __ andr(recv, flags, ConstantPoolCacheEntry::parameter_size_mask);
    // const int no_return_pc_pushed_yet = -1;  // argument slot correction before we push return address
    // const int receiver_is_at_end      = -1;  // back off one slot to get receiver
    // Address recv_addr = __ argument_address(recv, no_return_pc_pushed_yet + receiver_is_at_end);
    // __ movptr(recv, recv_addr);

    __ add(rscratch1, sp, recv, lsl(2));
    __ ldr(recv, Address(rscratch1, -Interpreter::expr_offset_in_bytes(1)));
    __ verify_oop(recv);
  }

  // compute return type
  // x86 uses a shift and mask or wings it with a shift plus assert
  // the mask is not needed. aarch32 just uses bitfield extract
  __ extract_bits(rscratch2, flags, ConstantPoolCacheEntry::tos_state_shift,  ConstantPoolCacheEntry::tos_state_bits);
  // load return address
  {
    const address table_addr = (address) Interpreter::invoke_return_entry_table_for(code);
    __ mov(rscratch1, table_addr);
    __ ldr(lr, Address(rscratch1, rscratch2, lsl(2)));
  }
}


void TemplateTable::invokevirtual_helper(Register index,
                                         Register recv,
                                         Register flags)
{
  // Uses temporary registers r0, r3
  assert_different_registers(index, recv, r0, r3);
  // Test for an invoke of a final method
  Label notFinal;
  __ tbz(flags, ConstantPoolCacheEntry::is_vfinal_shift, notFinal);

  __ reg_printf("It's a virtual final call\n");
  const Register method = index;  // method must be rmethod
  assert(method == rmethod,
         "methodOop must be rmethod for interpreter calling convention");

  // do the call - the index is actually the method to call
  // that is, f2 is a vtable index if !is_vfinal, else f2 is a Method*

  // It's final, need a null check here!
  __ null_check(recv);

  // profile this call
  __ profile_final_call(r0);
  __ profile_arguments_type(r0, method, r14, true);

  __ jump_from_interpreted(method, r0);

  __ bind(notFinal);
  __ reg_printf("It's not a virtual final call\n");
  // get receiver klass
  __ null_check(recv, oopDesc::klass_offset_in_bytes());
  __ load_klass(r0, recv);

  // profile this call
  __ profile_virtual_call(r0, rlocals, r3);

  // get target methodOop & entry point
  __ lookup_virtual_method(r0, index, method);
  __ profile_arguments_type(r3, method, r14, true);

  __ jump_from_interpreted(method, r3);
}

void TemplateTable::invokevirtual(int byte_no)
{
  transition(vtos, vtos);
  assert(byte_no == f2_byte, "use this argument");

  __ reg_printf("Invokevirtual, the sp is %p\n", sp);
  prepare_invoke(byte_no, rmethod, noreg, r2, r3);

  // rmethod: index (actually a Method*)
  // r2: receiver
  // r3: flags

  invokevirtual_helper(rmethod, r2, r3);
}

void TemplateTable::invokespecial(int byte_no)
{
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");
  __ ldr(rscratch1, Address(sp));
  __ reg_printf("Stack pointer is %p, tos word = %p\n", sp, rscratch1);

  prepare_invoke(byte_no, rmethod, noreg,  // get f1 Method*
                 r2);  // get receiver also for null check

  __ verify_oop(r2);
  __ null_check(r2);

  // do the call
  __ profile_call(r0);
  __ profile_arguments_type(r0, rmethod, rbcp, false);
  __ jump_from_interpreted(rmethod, r0);
}

void TemplateTable::invokestatic(int byte_no)
{
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");

  prepare_invoke(byte_no, rmethod);  // get f1 Method*
  // do the call
  __ profile_call(r0);
  __ profile_arguments_type(r0, rmethod, r14, false);
  __ jump_from_interpreted(rmethod, r0);
}

void TemplateTable::fast_invokevfinal(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f2_byte, "use this argument");
  __ stop("fast_invokevfinal not used on aarch32");}

void TemplateTable::invokeinterface(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");

  Register temp = rdispatch; //free at this point and reloaded later
  prepare_invoke(byte_no, r0, rmethod,  // get f1 Klass*, f2 itable index
                 r2, r3); // recv, flags

  // r0: interface klass (from f1)
  // rmethod: itable index (from f2)
  // r2: receiver
  // r3: flags

  // Special case of invokeinterface called for virtual method of
  // java.lang.Object.  See cpCacheOop.cpp for details.
  // This code isn't produced by javac, but could be produced by
  // another compliant java compiler.
  Label notMethod;
  __ tbz(r3, ConstantPoolCacheEntry::is_forced_virtual_shift, notMethod);

  __ reg_printf("ABC: Invoking invokevirtual_helper\n");
  invokevirtual_helper(rmethod, r2, r3); //loads lr too
  __ bind(notMethod);

  __ reg_printf("ABC: invokeinterface says 'It's not a method'\n");
  // Get receiver klass into r3 - also a null check
  __ restore_locals();
  __ null_check(r2, oopDesc::klass_offset_in_bytes());
  __ load_klass(r3, r2);

  // profile this call
  __ profile_virtual_call(r3, temp, r1);

  Label no_such_interface, no_such_method;

  __ lookup_interface_method(// inputs: rec. class, interface, itable index
                             r3, r0, rmethod,
                             // outputs: method, scan temp. reg
                             rmethod, temp,
                             no_such_interface);

  // rmethod,: methodOop to call
  // r2: receiver
  // Check for abstract method error
  // Note: This should be done more efficiently via a throw_abstract_method_error
  //       interpreter entry point and a conditional jump to it in case of a null
  //       method.
  __ cbz(rmethod, no_such_method);

  __ profile_arguments_type(r3, rmethod, temp, true);

  // do the call
  // r2: receiver
  // rmethod,: methodOop
  __ jump_from_interpreted(rmethod, r3);
  __ should_not_reach_here();

  // exception handling code follows...
  // note: must restore interpreter registers to canonical
  //       state for exception handling to work correctly!

  __ bind(no_such_method);
  __ reg_printf("ABC: invokeinterface says 'There's no such method'\n");
  // throw exception
  __ restore_bcp();      // bcp must be correct for exception handler   (was destroyed)
  __ restore_locals();   // make sure locals pointer is correct as well (was destroyed)
  __ call_VM(noreg, CAST_FROM_FN_PTR(address, InterpreterRuntime::throw_AbstractMethodError));
  // the call_VM checks for exception, so we should never return here.
  __ should_not_reach_here();

  __ bind(no_such_interface);
  __ reg_printf("ABC: invokeinterface says 'There's no such interface'\n");
  // throw exception
  __ restore_bcp();      // bcp must be correct for exception handler   (was destroyed)
  __ restore_locals();   // make sure locals pointer is correct as well (was destroyed)
  __ call_VM(noreg, CAST_FROM_FN_PTR(address,
                   InterpreterRuntime::throw_IncompatibleClassChangeError));
  // the call_VM checks for exception, so we should never return here.
  __ should_not_reach_here();
  return;
}

void TemplateTable::invokehandle(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");

  prepare_invoke(byte_no, rmethod, r0, r2);
  __ verify_method_ptr(r2);
  __ verify_oop(r2);
  __ null_check(r2);

  // FIXME: profile the LambdaForm also

  __ profile_final_call(r14);
  __ profile_arguments_type(r14, rmethod, rscratch2, true);

  __ jump_from_interpreted(rmethod, r0);
}

void TemplateTable::invokedynamic(int byte_no) {
  transition(vtos, vtos);
  assert(byte_no == f1_byte, "use this argument");

  prepare_invoke(byte_no, rmethod, r0);

  // r0: CallSite object (from cpool->resolved_references[])
  // rmethod: MH.linkToCallSite method (from f2)

  // Note:  r0_callsite is already pushed by prepare_invoke

  // %%% should make a type profile for any invokedynamic that takes a ref argument
  // profile this call
  __ profile_call(rbcp);
  __ profile_arguments_type(r3, rmethod, rscratch2, false);

  __ verify_oop(r0);

  __ jump_from_interpreted(rmethod, r0);
}


//-----------------------------------------------------------------------------
// Allocation

void TemplateTable::_new() {
  transition(vtos, atos);

  __ get_unsigned_2_byte_index_at_bcp(r3, 1);
  Label slow_case;
  Label done;
  Label initialize_header;
  Label initialize_object; // including clearing the fields
  Label allocate_shared;

  __ get_cpool_and_tags(r2, r0);
  // Make sure the class we're about to instantiate has been resolved.
  // This is done before loading InstanceKlass to be consistent with the order
  // how Constant Pool is updated (see ConstantPool::klass_at_put)
  const int tags_offset = Array<u1>::base_offset_in_bytes();
  __ lea(rscratch1, Address(r0, r3, lsl(0)));
  __ ldrb(rscratch1, Address(rscratch1, tags_offset));
  __ cmp(rscratch1, JVM_CONSTANT_Class);
  __ b(slow_case, Assembler::NE);

  // get InstanceKlass
  __ lea(r2, Address(r2, r3, lsl(2)));
  __ ldr(r2, Address(r2, sizeof(ConstantPool)));

  // make sure klass is initialized & doesn't have finalizer
  // make sure klass is fully initialized
  __ ldrb(rscratch1, Address(r2, InstanceKlass::init_state_offset()));
  __ cmp(rscratch1, InstanceKlass::fully_initialized);
  __ b(slow_case, Assembler::NE);

  // get instance_size in InstanceKlass (scaled to a count of bytes)
  __ ldr(r3, Address(r2, Klass::layout_helper_offset()));
  // test to see if it has a finalizer or is malformed in some way
  __ tbnz(r3, exact_log2(Klass::_lh_instance_slow_path_bit), slow_case);

  // Allocate the instance
  // 1) Try to allocate in the TLAB
  // 2) if fail and the object is large allocate in the shared Eden
  // 3) if the above fails (or is not applicable), go to a slow case
  // (creates a new TLAB, etc.)

  const bool allow_shared_alloc =
    Universe::heap()->supports_inline_contig_alloc();

  if (UseTLAB) {
    __ tlab_allocate(r0, r3, 0, noreg, r1,
                     allow_shared_alloc ? allocate_shared : slow_case);

    if (ZeroTLAB) {
      // the fields have been already cleared
      __ b(initialize_header);
    } else {
      // initialize both the header and fields
      __ b(initialize_object);
    }
  }

  // Allocation in the shared Eden, if allowed.
  //
  // r3: instance size in bytes
  if (allow_shared_alloc) {
    __ bind(allocate_shared);
    __ eden_allocate(r0, r3, 0, r14, slow_case);
    __ incr_allocated_bytes(rthread, r3, 0, rscratch1);
  }

  if (UseTLAB || Universe::heap()->supports_inline_contig_alloc()) {
    // The object is initialized before the header.  If the object size is
    // zero, go directly to the header initialization.
    __ bind(initialize_object);
    __ sub(r3, r3, sizeof(oopDesc));
    __ cbz(r3, initialize_header);

    // Initialize object fields
    {
      __ add(rscratch1, r0, sizeof(oopDesc));
      __ mov(rscratch2, 0);
      Label loop;
      __ bind(loop);
      __ str(rscratch2, Address(__ post(rscratch1, BytesPerInt)));
      __ sub(r3, r3, BytesPerInt);
      __ cbnz(r3, loop);
    }

    // initialize object header only.
    __ bind(initialize_header);
    if (UseBiasedLocking) {
      __ ldr(rscratch1, Address(r2, Klass::prototype_header_offset()));
    } else {
      __ mov(rscratch1, (intptr_t)markOopDesc::prototype());
    }
    __ str(rscratch1, Address(r0, oopDesc::mark_offset_in_bytes()));
    __ mov(rscratch2, 0);
    __ store_klass_gap(r0, rscratch2);  // zero klass gap for compressed oops - not using
    // not using compressed oops
    __ store_klass(r0, r2);      // store klass last

    {
      SkipIfEqual skip(_masm, &DTraceAllocProbes, false);
      // Trigger dtrace event for fastpath
      __ push(atos); // save the return value
      __ call_VM_leaf(
           CAST_FROM_FN_PTR(address, SharedRuntime::dtrace_object_alloc), r0);
      __ pop(atos); // restore the return value

    }
    __ b(done);
  }

  // slow case
  __ bind(slow_case);
  __ get_constant_pool(c_rarg1);
  __ get_unsigned_2_byte_index_at_bcp(c_rarg2, 1);
  call_VM(r0, CAST_FROM_FN_PTR(address, InterpreterRuntime::_new), c_rarg1, c_rarg2);
  __ verify_oop(r0);

  // continue
  __ bind(done);

  __ reg_printf("New object reference is %p\n", r0);
  // Must prevent reordering of stores for object initialization with stores that publish the new object.
  __ membar(Assembler::StoreStore);
}

void TemplateTable::newarray() {
  transition(itos, atos);
  __ load_unsigned_byte(c_rarg1, at_bcp(1));
  __ mov(c_rarg2, r0);
  call_VM(r0, CAST_FROM_FN_PTR(address, InterpreterRuntime::newarray),
          c_rarg1, c_rarg2);
  // Must prevent reordering of stores for object initialization with stores that publish the new object.
  __ membar(Assembler::StoreStore);
}

void TemplateTable::anewarray() {
  transition(itos, atos);
  __ get_unsigned_2_byte_index_at_bcp(c_rarg2, 1);
  __ reg_printf("Index = %d\n", c_rarg2);
  __ get_constant_pool(c_rarg1);
  __ mov(c_rarg3, r0);
  __ reg_printf("About to call InterpreterRuntime::anewarray\n");
  call_VM(r0, CAST_FROM_FN_PTR(address, InterpreterRuntime::anewarray),
          c_rarg1, c_rarg2, c_rarg3);
  __ reg_printf("Finshed call to InterpreterRuntime::anewarray\n");
  // Must prevent reordering of stores for object initialization with stores that publish the new object.
  __ membar(Assembler::StoreStore);
  __ reg_printf("Finshed anewarray\n");
}

void TemplateTable::arraylength() {
  transition(atos, itos);
  __ null_check(r0, arrayOopDesc::length_offset_in_bytes());
  __ ldr(r0, Address(r0, arrayOopDesc::length_offset_in_bytes()));
}

void TemplateTable::checkcast()
{
  transition(atos, atos);
  Label done, is_null, ok_is_subtype, quicked, resolved;
  __ cbz(r0, is_null);

  // Get cpool & tags index
  __ get_cpool_and_tags(r2, r3); // r2=cpool, r3=tags array
  __ get_unsigned_2_byte_index_at_bcp(r14, 1); // r14=index
  // See if bytecode has already been quicked
  __ add(rscratch1, r3, Array<u1>::base_offset_in_bytes());
  __ ldrb(r1, Address(rscratch1, r14));
  __ cmp(r1, JVM_CONSTANT_Class);
  __ b(quicked, Assembler::EQ);

  __ push(atos); // save receiver for result, and for GC
  call_VM(r0, CAST_FROM_FN_PTR(address, InterpreterRuntime::quicken_io_cc));
  // vm_result_2 has metadata result
  __ get_vm_result_2(r0, rthread);
  __ pop(r3); // restore receiver
  __ b(resolved);

  // Get superklass in r0 and subklass in r3
  __ bind(quicked);
  __ mov(r3, r0); // Save object in r3; r0 needed for subtype check
  __ lea(r0, Address(r2, r14, lsl(2)));
  __ ldr(r0, Address(r0, sizeof(ConstantPool)));

  __ bind(resolved);
  __ load_klass(r1, r3);

  // Generate subtype check.  Blows r2. Object in r3.
  // Superklass in r0. Subklass in r1.
  __ gen_subtype_check(r1, ok_is_subtype);

  // Come here on failure
  __ push(r3);
  // object is at TOS
  __ b(Interpreter::_throw_ClassCastException_entry);

  // Come here on success
  __ bind(ok_is_subtype);
  __ mov(r0, r3); // Restore object in r3

  // Collect counts on whether this test sees NULLs a lot or not.
  if (ProfileInterpreter) {
    __ b(done);
    __ bind(is_null);
    __ profile_null_seen(r2);
  } else {
    __ bind(is_null);   // same as 'done'
  }
  __ bind(done);
}

void TemplateTable::instanceof() {
  transition(atos, itos);
  Label done, is_null, ok_is_subtype, quicked, resolved;
  __ cbz(r0, is_null);

  // Get cpool & tags index
  __ get_cpool_and_tags(r2, r3); // r2=cpool, r3=tags array
  __ get_unsigned_2_byte_index_at_bcp(r14, 1); // r14=index

  // See if bytecode has already been quicked
  __ add(rscratch1, r3, Array<u1>::base_offset_in_bytes());
  __ ldrb(r1, Address(rscratch1, r14));
  __ cmp(r1, JVM_CONSTANT_Class);
  __ b(quicked, Assembler::EQ);

  __ push(atos); // save receiver for result, and for GC
  __ push_i(r14); // save index (used if profiling)
  call_VM(r0, CAST_FROM_FN_PTR(address, InterpreterRuntime::quicken_io_cc));
  // vm_result_2 has metadata result
  __ get_vm_result_2(r0, rthread);
  __ pop_i(r14); // restore index
  __ pop(r3); // restore receiver
  __ verify_oop(r3);
  __ load_klass(r3, r3);
  __ b(resolved);

  // Get superklass in r0 and subklass in r3
  __ bind(quicked);
  __ load_klass(r3, r0);
  __ lea(r0, Address(r2, r14, lsl(2)));
  __ ldr(r0, Address(r0, sizeof(ConstantPool)));

  __ bind(resolved);

  // Generate subtype check.  Blows r2.
  // Superklass in r0.  Subklass in r3.
  __ gen_subtype_check(r3, ok_is_subtype);

  // Come here on failure
  __ mov(r0, 0);
  __ b(done);
  // Come here on success
  __ bind(ok_is_subtype);
  __ mov(r0, 1);

  // Collect counts on whether this test sees NULLs a lot or not.
  if (ProfileInterpreter) {
    __ b(done);
    __ bind(is_null);
    __ profile_null_seen(r2);
  } else {
    __ bind(is_null);   // same as 'done'
  }
  __ bind(done);
  // r0 = 0: obj == NULL or  obj is not an instanceof the specified klass
  // r0 = 1: obj != NULL and obj is     an instanceof the specified klass
}

//-----------------------------------------------------------------------------
// Breakpoints
void TemplateTable::_breakpoint() {
  // Note: We get here even if we are single stepping..
  // jbug inists on setting breakpoints at every bytecode
  // even if we are in single step mode.

  transition(vtos, vtos);

  // get the unpatched byte code
  __ get_method(c_rarg1);
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address,
                              InterpreterRuntime::get_original_bytecode_at),
             c_rarg1, rbcp);
  __ push(r0);

  // post the breakpoint event
  __ call_VM(noreg,
             CAST_FROM_FN_PTR(address, InterpreterRuntime::_breakpoint),
             rmethod, rbcp);

  // complete the execution of original bytecode
  __ pop(rscratch1);
  __ dispatch_only_normal(vtos);
}

//-----------------------------------------------------------------------------
// Exceptions

void TemplateTable::athrow() {
  transition(atos, vtos);
  __ null_check(r0);
  __ b(Interpreter::throw_exception_entry());
}

//-----------------------------------------------------------------------------
// Synchronization
//
// Note: monitorenter & exit are symmetric routines; which is reflected
//       in the assembly code structure as well
//
// Stack layout:
//
// [expressions  ] <--- sp                = expression stack top
// ..
// [expressions  ]
// [monitor entry] <--- monitor block top = expression stack bot
// ..
// [monitor entry]
// [frame data   ] <--- monitor block bot
// ...
// [saved rbp    ] <--- rbp
void TemplateTable::monitorenter()
{
  transition(atos, vtos);

  // check for NULL object
  __ null_check(r0);

  const Address monitor_block_top(
        rfp, frame::interpreter_frame_monitor_block_top_offset * wordSize);
  const Address monitor_block_bot(
        rfp, frame::interpreter_frame_initial_sp_offset * wordSize);
  const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

  Label allocated;

  // initialize entry pointer
  __ mov(c_rarg1, 0); // points to free slot or NULL

  // find a free slot in the monitor block (result in c_rarg1)
  {
    Label entry, loop, exit;
    __ ldr(c_rarg3, monitor_block_top); // points to current entry,
                                        // starting with top-most entry
    __ lea(c_rarg2, monitor_block_bot); // points to word before bottom

    __ b(entry);

    __ bind(loop);
    // check if current entry is used
    // if not used then remember entry in c_rarg1
    __ ldr(rscratch1, Address(c_rarg3, BasicObjectLock::obj_offset_in_bytes()));
    __ cmp(rscratch1, 0);
    __ mov(c_rarg1, c_rarg3, Assembler::EQ);
    // check if current entry is for same object
    __ cmp(r0, rscratch1);
    // if same object then stop searching
    __ b(exit, Assembler::EQ);
    // otherwise advance to next entry
    __ add(c_rarg3, c_rarg3, entry_size);
    __ bind(entry);
    // check if bottom reached
    __ cmp(c_rarg3, c_rarg2);
    // if not at bottom then check this entry
    __ b(loop, Assembler::NE);
    __ bind(exit);
  }

  __ cbnz(c_rarg1, allocated); // check if a slot has been found and
                            // if found, continue with that on

  // allocate one if there's no free slot
  {
    Label entry, loop; //, no_adjust;
    // 1. compute new pointers            // rsp: old expression stack top
    __ ldr(c_rarg1, monitor_block_bot);   // c_rarg1: old expression stack bottom
    __ sub(sp, sp, entry_size);           // move expression stack top
    __ sub(c_rarg1, c_rarg1, entry_size); // move expression stack bottom
    __ mov(c_rarg3, sp);                  // set start value for copy loop
    __ str(c_rarg1, monitor_block_bot);   // set new monitor block bottom

    //__ cmp(sp, c_rarg3);                  // Check if we need to move sp
    //__ b(no_adjust, Assembler::LO);      // to allow more stack space
                                          // for our new sp
    //__ sub(sp, sp, 2 * wordSize);
    //__ bind(no_adjust);

    __ b(entry);
    // 2. move expression stack contents
    __ bind(loop);
    __ ldr(c_rarg2, Address(c_rarg3, entry_size)); // load expression stack
                                                   // word from old location
    __ str(c_rarg2, Address(c_rarg3, 0));          // and store it at new location
    __ add(c_rarg3, c_rarg3, wordSize);            // advance to next word
    __ bind(entry);
    __ cmp(c_rarg3, c_rarg1);        // check if bottom reached
    __ b(loop, Assembler::NE);      // if not at bottom then
                                     // copy next word
  }

  // call run-time routine
  // c_rarg1: points to monitor entry
  __ bind(allocated);

  // Increment bcp to point to the next bytecode, so exception
  // handling for async. exceptions work correctly.
  // The object has already been poped from the stack, so the
  // expression stack looks correct.
  __ add(rbcp, rbcp, 1); //inc

  // store object
  __ str(r0, Address(c_rarg1, BasicObjectLock::obj_offset_in_bytes()));
  __ lock_object(c_rarg1);

  // check to make sure this monitor doesn't cause stack overflow after locking
  __ save_bcp();  // in case of exception
  __ generate_stack_overflow_check(0);

  // The bcp has already been incremented. Just need to dispatch to
  // next instruction.
  __ dispatch_next(vtos);
}


void TemplateTable::monitorexit()
{
  transition(atos, vtos);

  // check for NULL object
  __ null_check(r0);

  const Address monitor_block_top(
        rfp, frame::interpreter_frame_monitor_block_top_offset * wordSize);
  const Address monitor_block_bot(
        rfp, frame::interpreter_frame_initial_sp_offset * wordSize);
  const int entry_size = frame::interpreter_frame_monitor_size() * wordSize;

  Label found;

  // find matching slot
  {
    Label entry, loop;
    __ ldr(c_rarg1, monitor_block_top); // points to current entry,
                                        // starting with top-most entry
    __ lea(c_rarg2, monitor_block_bot); // points to word before bottom
                                        // of monitor block
    __ b(entry);

    __ bind(loop);
    // check if current entry is for same object
    __ ldr(rscratch1, Address(c_rarg1, BasicObjectLock::obj_offset_in_bytes()));
    __ cmp(r0, rscratch1);
    // if same object then stop searching
    __ b(found, Assembler::EQ);
    // otherwise advance to next entry
    __ add(c_rarg1, c_rarg1, entry_size);
    __ bind(entry);
    // check if bottom reached
    __ cmp(c_rarg1, c_rarg2);
    // if not at bottom then check this entry
    __ b(loop, Assembler::NE);
  }

  // error handling. Unlocking was not block-structured
  __ call_VM(noreg, CAST_FROM_FN_PTR(address,
                   InterpreterRuntime::throw_illegal_monitor_state_exception));
  __ should_not_reach_here();

  // call run-time routine
  __ bind(found);
  __ push_ptr(r0); // make sure object is on stack (contract with oopMaps)
  __ unlock_object(c_rarg1);
  __ pop_ptr(r0); // discard object
}


// Wide instructions
//J_UPDATE
void TemplateTable::wide()
{
  __ load_unsigned_byte(r14, at_bcp(1));
  __ mov(rscratch1, (address)Interpreter::_wentry_point);
  __ ldr(rscratch1, Address(rscratch1, r14, lsl(2)));
  __ b(rscratch1);
}


// Multi arrays
//J_UPDATE
void TemplateTable::multianewarray() {
  transition(vtos, atos);
  __ load_unsigned_byte(r0, at_bcp(3)); // get number of dimensions
  // last dim is on top of stack; we want address of first one:
  // first_addr = last_addr + (ndims - 1) * wordSize
  __ lea(c_rarg1, Address(sp, r0, lsl(2)));
  __ sub(c_rarg1, c_rarg1, wordSize);
  call_VM(r0,
          CAST_FROM_FN_PTR(address, InterpreterRuntime::multianewarray),
          c_rarg1);
  __ load_unsigned_byte(r1, at_bcp(3));
  __ lea(sp, Address(sp, r1, lsl(2)));
}
#endif // !CC_INTERP
