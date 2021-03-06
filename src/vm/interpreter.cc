// Copyright (c) 2014, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#include "src/vm/interpreter.h"

#include <math.h>
#include <stdlib.h>

#include "src/shared/bytecodes.h"
#include "src/shared/flags.h"
#include "src/shared/names.h"
#include "src/shared/selectors.h"

#include "src/vm/natives.h"
#include "src/vm/port.h"
#include "src/vm/process.h"
#include "src/vm/session.h"
#include "src/vm/stack_walker.h"

#define GC_AND_RETRY_ON_ALLOCATION_FAILURE(var, exp)                    \
  Object* var = (exp);                                                  \
  if (var == Failure::retry_after_gc()) {                               \
    CollectGarbage();                                                   \
    /* Re-try interpreting the bytecode by re-dispatching. */           \
    DISPATCH();                                                         \
  }                                                                     \

namespace fletch {

const NativeFunction kNativeTable[] = {
#define N(e, c, n) &Native_##e,
  NATIVES_DO(N)
#undef N
};

class State {
 public:
  explicit State(Process* process)
      : process_(process),
        program_(process->program()) {
    RestoreState();
  }

  Process* process() const { return process_; }
  Program* program() const { return program_; }

  void SaveState() {
    Push(reinterpret_cast<Object*>(bcp_));
    process_->stack()->SetTopFromPointer(sp_);
  }

  void RestoreState() {
    Stack* stack = process_->stack();
    sp_ = stack->Pointer(stack->top());
    bcp_ = reinterpret_cast<uint8_t*>(Pop());
    ASSERT(bcp_ != NULL);
  }

  // Bytecode pointer related operations.
  uint8 ReadByte(int offset) { return bcp_[offset]; }
  int ReadInt32(int offset) { return Utils::ReadInt32(bcp_ + offset); }

  void PrintBytecode() {
    Bytecode::Print(bcp_);
  }

  Opcode ReadOpcode() const {
    uint8 opcode = *bcp_;
#ifdef DEBUG
    if (opcode >= Bytecode::kNumBytecodes) {
      FATAL1("Failed to interpret. Bad bytecode (opcode = %d).", opcode);
    }
#endif
    return static_cast<Opcode>(opcode);
  }

  Object* ReadConstant() const { return Function::ConstantForBytecode(bcp_); }

  void Goto(uint8* bcp) { ASSERT(bcp != NULL); bcp_ = bcp; }
  void Advance(int delta) { bcp_ += delta; }
  uint8* ComputeReturnAddress(int offset) { return bcp_ + offset; }

  // Stack pointer related operations.
  Object* Top() { return *sp_; }
  void SetTop(Object* value) { *sp_ = value; }

  Object* Local(int n) { return *(sp_ - n); }
  void SetLocal(int n, Object* value) { *(sp_ - n)  = value; }
  Object** LocalPointer(int n) { return sp_ - n; }

  Object* Pop() { return *(sp_--); }
  void Push(Object* value) { *(++sp_) = value; }
  void Drop(int n) { sp_ -= n; }

  bool HasStackSpaceFor(int size) const {
    return sp_ + size < process_->stack_limit();
  }

  Function* ComputeCurrentFunction() {
    return Function::FromBytecodePointer(bcp_);
  }

 protected:
  uint8* bcp() { return bcp_; }
  Object** sp() { return sp_; }

 private:
  Process* const process_;
  Program* const program_;
  Object** sp_;
  uint8* bcp_;
};

// TODO(kasperl): Should we call this interpreter?
class Engine : public State {
 public:
  explicit Engine(Process* process) : State(process) { }

  Interpreter::InterruptKind Interpret(Port** yield_target);

 private:
  void Branch(int true_offset, int false_offset);

  void PushReturnAddress(int offset);
  void PopReturnAddress();

  void PushDelta(int delta);
  int PopDelta();

  // Returns false if it was unable to grow the stack at this point, and the
  // immediate execution should halt.
  bool StackOverflowCheck(int size);
  void CollectGarbage();

  void ValidateStack();

  bool ShouldBreak();
  bool IsAtBreakPoint();

  Object* ToBool(bool value) const {
    return value ? program()->true_object() : program()->false_object();
  }
};

// Dispatching helper macros.
#define DISPATCH()                                        \
  if (ShouldBreak()) return Interpreter::kBreakPoint;     \
  goto *kDispatchTable[ReadOpcode()]
#define DISPATCH_NO_BREAK()                               \
  goto *kDispatchTable[ReadOpcode()]
#define DISPATCH_TO(opcode)                               \
  goto opcode##Label

// Opcode definition macros.
#define OPCODE_BEGIN(opcode)                              \
  opcode##Label: {                                        \
    if (Flags::validate_stack) ValidateStack()
#define OPCODE_END() }                                    \
  DISPATCH()

Interpreter::InterruptKind Engine::Interpret(Port** yield_target) {
#define LABEL(name, branching, format, length, stack_diff, print) &&name##Label,
  static void* kDispatchTable[] = {
    BYTECODES_DO(LABEL)
  };
#undef LABEL

  // Dispatch to the first bytecode.
  if (IsAtBreakPoint()) {
    DISPATCH_NO_BREAK();
  } else {
    DISPATCH();
  }

  OPCODE_BEGIN(LoadLocal0);
    Object* local = Local(0);
    Push(local);
    Advance(kLoadLocal0Length);
  OPCODE_END();

  OPCODE_BEGIN(LoadLocal1);
    Object* local = Local(1);
    Push(local);
    Advance(kLoadLocal1Length);
  OPCODE_END();

  OPCODE_BEGIN(LoadLocal2);
    Object* local = Local(2);
    Push(local);
    Advance(kLoadLocal2Length);
  OPCODE_END();

  OPCODE_BEGIN(LoadLocal);
    int offset = ReadByte(1);
    Object* local = Local(offset);
    Push(local);
    Advance(kLoadLocalLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadBoxed);
    int offset = ReadByte(1);
    Boxed* boxed = Boxed::cast(Local(offset));
    Push(boxed->value());
    Advance(kLoadBoxedLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadStatic);
    int index = ReadInt32(1);
    Object* value = process()->statics()->get(index);
    Push(value);
    Advance(kLoadStaticLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadStaticInit);
    int index = ReadInt32(1);
    Object* value = process()->statics()->get(index);
    if (value->IsInitializer()) {
      Function* target = Initializer::cast(value)->function();
      PushReturnAddress(kLoadStaticInitLength);
      Goto(target->bytecode_address_for(0));
      if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
    } else {
      Push(value);
      Advance(kLoadStaticInitLength);
    }
  OPCODE_END();

  OPCODE_BEGIN(LoadField);
    Instance* target = Instance::cast(Pop());
    Push(target->GetInstanceField(ReadByte(1)));
    Advance(kLoadFieldLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadConst);
    int index = ReadInt32(1);
    Push(program()->constant_at(index));
    Advance(kLoadConstLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadConstUnfold);
    Push(ReadConstant());
    Advance(kLoadConstUnfoldLength);
  OPCODE_END();

  OPCODE_BEGIN(StoreLocal);
    int offset = ReadByte(1);
    Object* value = Local(0);
    SetLocal(offset, value);
    Advance(kStoreLocalLength);
  OPCODE_END();

  OPCODE_BEGIN(StoreBoxed);
    int offset = ReadByte(1);
    Object* value = Local(0);
    Boxed* boxed = Boxed::cast(Local(offset));
    boxed->set_value(value);
    Advance(kStoreBoxedLength);
  OPCODE_END();

  OPCODE_BEGIN(StoreStatic);
    int index = ReadInt32(1);
    Object* value = Local(0);
    process()->statics()->set(index, value);
    Advance(kStoreStaticLength);
  OPCODE_END();

  OPCODE_BEGIN(StoreField);
    Object* value = Pop();
    Instance* target = Instance::cast(Pop());
    target->SetInstanceField(ReadByte(1), value);
    Push(value);
    Advance(kStoreFieldLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadLiteralNull);
    Push(program()->null_object());
    Advance(kLoadLiteralNullLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadLiteralTrue);
    Push(program()->true_object());
    Advance(kLoadLiteralTrueLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadLiteralFalse);
    Push(program()->false_object());
    Advance(kLoadLiteralFalseLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadLiteral0);
    Push(Smi::FromWord(0));
    Advance(kLoadLiteral0Length);
  OPCODE_END();

  OPCODE_BEGIN(LoadLiteral1);
    Push(Smi::FromWord(1));
    Advance(kLoadLiteral1Length);
  OPCODE_END();

  OPCODE_BEGIN(LoadLiteral);
    Push(Smi::FromWord(ReadByte(1)));
    Advance(kLoadLiteralLength);
  OPCODE_END();

  OPCODE_BEGIN(LoadLiteralWide);
    int value = ReadInt32(1);
    ASSERT(Smi::IsValid(value));
    Push(Smi::FromWord(value));
    Advance(kLoadLiteralWideLength);
  OPCODE_END();

  OPCODE_BEGIN(InvokeMethod);
    int selector = ReadInt32(1);
    int arity = Selector::ArityField::decode(selector);
    Object* receiver = Local(arity);
    PushReturnAddress(kInvokeMethodLength);
    Function* target = process()->LookupEntry(receiver, selector)->target;
    Goto(target->bytecode_address_for(0));
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
  OPCODE_END();

  OPCODE_BEGIN(InvokeMethodFast);
    int index = ReadInt32(1);
    Array* table = program()->dispatch_table();
    int selector = Smi::cast(table->get(index + 1))->value();
    int arity = Selector::ArityField::decode(selector);
    Object* receiver = Local(arity);
    PushReturnAddress(kInvokeMethodFastLength);

    Class* clazz = receiver->IsSmi()
        ? program()->smi_class()
        : HeapObject::cast(receiver)->get_class();
    int class_id = clazz->id();

    Function* target = NULL;
    for (int offset = 4; true; offset += 4) {
      Smi* lower = Smi::cast(table->get(index + offset));
      if (class_id < lower->value()) continue;
      Smi* upper = Smi::cast(table->get(index + offset + 1));
      if (class_id >= upper->value()) continue;
      target = Function::cast(table->get(index + offset + 3));
      break;
    }

    Goto(target->bytecode_address_for(0));
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
  OPCODE_END();

  OPCODE_BEGIN(InvokeMethodVtable);
    int selector = ReadInt32(1);
    int arity = Selector::ArityField::decode(selector);
    int offset = Selector::IdField::decode(selector);
    Object* receiver = Local(arity);
    PushReturnAddress(kInvokeMethodVtableLength);

    Class* clazz = receiver->IsSmi()
        ? program()->smi_class()
        : HeapObject::cast(receiver)->get_class();

    int index = clazz->id() + offset;
    Array* entry = Array::cast(program()->vtable()->get(index));
    if (Smi::cast(entry->get(0))->value() != offset) {
      entry = Array::cast(program()->vtable()->get(0));
    }
    Function* target = Function::cast(entry->get(2));
    Goto(target->bytecode_address_for(0));
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
  OPCODE_END();

  OPCODE_BEGIN(InvokeStatic);
    int index = ReadInt32(1);
    Function* target = program()->static_method_at(index);
    PushReturnAddress(kInvokeStaticLength);
    Goto(target->bytecode_address_for(0));
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
  OPCODE_END();

  OPCODE_BEGIN(InvokeFactory);
    DISPATCH_TO(InvokeStatic);
  OPCODE_END();

  OPCODE_BEGIN(InvokeStaticUnfold);
    Function* target = Function::cast(ReadConstant());
    PushReturnAddress(kInvokeStaticLength);
    Goto(target->bytecode_address_for(0));
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
  OPCODE_END();

  OPCODE_BEGIN(InvokeFactoryUnfold);
    DISPATCH_TO(InvokeStaticUnfold);
  OPCODE_END();

  OPCODE_BEGIN(InvokeNative);
    int arity = ReadByte(1);
    Native native = static_cast<Native>(ReadByte(2));
    Object** arguments = LocalPointer(arity);
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(
        result, kNativeTable[native](process(), arguments));
    if (result->IsFailure()) {
      Push(program()->ObjectFromFailure(Failure::cast(result)));
      Advance(kInvokeNativeLength);
    } else {
      PopReturnAddress();
      Drop(arity);
      Push(result);
    }
  OPCODE_END();

#define INVOKE_BUILTIN(kind)           \
  OPCODE_BEGIN(Invoke##kind);          \
    DISPATCH_TO(InvokeMethod);         \
  OPCODE_END();                        \
  OPCODE_BEGIN(Invoke##kind##Fast);    \
    DISPATCH_TO(InvokeMethodFast);     \
  OPCODE_END();                        \
  OPCODE_BEGIN(Invoke##kind##Vtable);  \
    DISPATCH_TO(InvokeMethodVtable);   \
  OPCODE_END();

  INVOKE_BUILTIN(Eq);
  INVOKE_BUILTIN(Lt);
  INVOKE_BUILTIN(Le);
  INVOKE_BUILTIN(Gt);
  INVOKE_BUILTIN(Ge);

  INVOKE_BUILTIN(Add);
  INVOKE_BUILTIN(Sub);
  INVOKE_BUILTIN(Mod);
  INVOKE_BUILTIN(Mul);
  INVOKE_BUILTIN(TruncDiv);

  INVOKE_BUILTIN(BitNot);
  INVOKE_BUILTIN(BitAnd);
  INVOKE_BUILTIN(BitOr);
  INVOKE_BUILTIN(BitXor);
  INVOKE_BUILTIN(BitShr);
  INVOKE_BUILTIN(BitShl);

#undef INVOKE_BUILTIN

  OPCODE_BEGIN(InvokeNativeYield);
    int arity = ReadByte(1);
    Native native = static_cast<Native>(ReadByte(2));
    Object** arguments = LocalPointer(arity);
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(
        result, kNativeTable[native](process(), arguments));
    if (result->IsFailure()) {
      Push(program()->ObjectFromFailure(Failure::cast(result)));
      Advance(kInvokeNativeYieldLength);
    } else {
      PopReturnAddress();
      Drop(arity);
      Object* null = program()->null_object();
      Push(null);
      if (result != null) {
        SaveState();
        *yield_target = reinterpret_cast<Port*>(result);
        ASSERT((*yield_target)->IsLocked());
        return Interpreter::kTargetYield;
      }
    }
  OPCODE_END();

  OPCODE_BEGIN(InvokeTest);
    int selector = ReadInt32(1);
    Object* receiver = Local(0);
    SetTop(ToBool(process()->LookupEntry(receiver, selector)->tag != 0));
    Advance(kInvokeTestLength);
  OPCODE_END();

  OPCODE_BEGIN(InvokeTestFast);
    int index = ReadInt32(1);
    Array* table = program()->dispatch_table();
    Object* receiver = Local(0);

    Class* clazz = receiver->IsSmi()
        ? program()->smi_class()
        : HeapObject::cast(receiver)->get_class();
    int class_id = clazz->id();

    for (int offset = 4; true; offset += 4) {
      Smi* lower = Smi::cast(table->get(index + offset));
      if (class_id < lower->value()) continue;
      Smi* upper = Smi::cast(table->get(index + offset + 1));
      if (class_id >= upper->value()) continue;
      SetTop(ToBool(upper != Smi::FromWord(Smi::kMaxValue)));
      break;
    }

    Advance(kInvokeTestFastLength);
  OPCODE_END();

  OPCODE_BEGIN(InvokeTestVtable);
    int selector = ReadInt32(1);
    int offset = Selector::IdField::decode(selector);
    Object* receiver = Local(0);

    Class* clazz = receiver->IsSmi()
        ? program()->smi_class()
        : HeapObject::cast(receiver)->get_class();

    int index = clazz->id() + offset;
    Array* entry = Array::cast(program()->vtable()->get(index));
    SetTop(ToBool(Smi::cast(entry->get(0))->value() == offset));

    Advance(kInvokeTestVtableLength);
  OPCODE_END();

  OPCODE_BEGIN(Pop);
    Drop(1);
    Advance(kPopLength);
  OPCODE_END();

  OPCODE_BEGIN(Return);
    int locals = ReadByte(1);
    int arguments = ReadByte(2);
    Object* result = Local(0);
    Drop(locals);
    PopReturnAddress();
    Drop(arguments);
    Push(result);
  OPCODE_END();

  OPCODE_BEGIN(BranchLong);
    int delta = ReadInt32(1);
    Advance(delta);
  OPCODE_END();

  OPCODE_BEGIN(BranchIfTrueLong);
    int delta = ReadInt32(1);
    Branch(delta, kBranchIfTrueLongLength);
  OPCODE_END();

  OPCODE_BEGIN(BranchIfFalseLong);
    int delta = ReadInt32(1);
    Branch(kBranchIfFalseLongLength, delta);
  OPCODE_END();

  OPCODE_BEGIN(BranchBack);
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
    Advance(-ReadByte(1));
  OPCODE_END();

  OPCODE_BEGIN(BranchBackIfTrue);
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
    int delta = -ReadByte(1);
    Branch(delta, kBranchBackIfTrueLength);
  OPCODE_END();

  OPCODE_BEGIN(BranchBackIfFalse);
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
    int delta = -ReadByte(1);
    Branch(kBranchBackIfTrueLength, delta);
  OPCODE_END();

  OPCODE_BEGIN(BranchBackLong);
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
    int delta = ReadInt32(1);
    Advance(-delta);
  OPCODE_END();

  OPCODE_BEGIN(BranchBackIfTrueLong);
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
    int delta = -ReadInt32(1);
    Branch(delta, kBranchBackIfTrueLongLength);
  OPCODE_END();

  OPCODE_BEGIN(BranchBackIfFalseLong);
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
    int delta = -ReadInt32(1);
    Branch(kBranchBackIfFalseLongLength, delta);
  OPCODE_END();

  OPCODE_BEGIN(PopAndBranchLong);
    int pop_count = ReadByte(1);
    int delta = ReadInt32(2);
    Drop(pop_count);
    Advance(delta);
  OPCODE_END();

  OPCODE_BEGIN(PopAndBranchBackLong);
    if (!StackOverflowCheck(0)) return Interpreter::kInterrupt;
    int pop_count = ReadByte(1);
    int delta = -ReadInt32(2);
    Drop(pop_count);
    Advance(delta);
  OPCODE_END();

  OPCODE_BEGIN(Allocate);
    int index = ReadInt32(1);
    Class* klass = program()->class_at(index);
    ASSERT(klass->id() == index);
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(result,
        process()->NewInstance(klass));
    Instance* instance = Instance::cast(result);
    int fields = klass->NumberOfInstanceFields();
    for (int i = fields - 1; i >= 0; --i) {
      instance->SetInstanceField(i, Pop());
    }
    Push(instance);
    Advance(kAllocateLength);
  OPCODE_END();

  OPCODE_BEGIN(AllocateUnfold);
    Class* klass = Class::cast(ReadConstant());
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(result,
        process()->NewInstance(klass));
    Instance* instance = Instance::cast(result);
    int fields = klass->NumberOfInstanceFields();
    for (int i = fields - 1; i >= 0; --i) {
      instance->SetInstanceField(i, Pop());
    }
    Push(instance);
    Advance(kAllocateLength);
  OPCODE_END();

  OPCODE_BEGIN(AllocateImmutable);
    int index = ReadInt32(1);
    Class* klass = program()->class_at(index);
    ASSERT(klass->id() == index);
    int fields = klass->NumberOfInstanceFields();
    bool immutable = true;
    for (int i = 0; i < fields; i++) {
      if (!Local(i)->IsImmutable()) {
        immutable = false;
        break;
      }
    }
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(result,
        process()->NewInstance(klass, immutable));
    Instance* instance = Instance::cast(result);
    for (int i = fields - 1; i >= 0; --i) {
      instance->SetInstanceField(i, Pop());
    }
    Push(instance);
    Advance(kAllocateImmutableLength);
  OPCODE_END();

  OPCODE_BEGIN(AllocateImmutableUnfold);
    Class* klass = Class::cast(ReadConstant());
    int fields = klass->NumberOfInstanceFields();
    bool immutable = true;
    for (int i = 0; i < fields; i++) {
      if (!Local(i)->IsImmutable()) {
        immutable = false;
        break;
      }
    }
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(result,
        process()->NewInstance(klass, immutable));
    Instance* instance = Instance::cast(result);
    for (int i = fields - 1; i >= 0; --i) {
      instance->SetInstanceField(i, Pop());
    }
    Push(instance);
    Advance(kAllocateImmutableUnfoldLength);
  OPCODE_END();

  OPCODE_BEGIN(AllocateBoxed);
    Object* value = Local(0);
    GC_AND_RETRY_ON_ALLOCATION_FAILURE(raw_boxed,
        process()->NewBoxed(value));
    Boxed* boxed = Boxed::cast(raw_boxed);
    SetTop(boxed);
    Advance(kAllocateBoxedLength);
  OPCODE_END();

  OPCODE_BEGIN(Negate);
    Object* condition = Local(0);
    if (condition == program()->true_object()) {
      SetTop(program()->false_object());
    } else if (condition == program()->false_object()) {
      SetTop(program()->true_object());
    } else {
      UNIMPLEMENTED();
    }
    Advance(kNegateLength);
  OPCODE_END();

  OPCODE_BEGIN(StackOverflowCheck);
    int size = ReadInt32(1);
    if (!StackOverflowCheck(size)) return Interpreter::kInterrupt;
    Advance(kStackOverflowCheckLength);
  OPCODE_END();

  OPCODE_BEGIN(Throw);
    // TODO(kasperl): We assume that the stack walker code will not
    // cause any GCs so it's safe to hold onto the exception reference.
    Object* exception = Local(0);

    // Push next address, to make the frame look complete.
    SaveState();

    // Find the catch block address.
    int stack_delta = 0;
    uint8* catch_bcp = HandleThrow(process(), exception, &stack_delta);
    if (catch_bcp == NULL) return Interpreter::kUncaughtException;

    // Restore stack pointer and bcp.
    RestoreState();

    Goto(catch_bcp);

    // The delta is computed given that bcp is pushed on the
    // stack. We have already pop'ed bcp as part of RestoreState.
    Drop(stack_delta - 1);

    SetTop(exception);
  OPCODE_END();

  OPCODE_BEGIN(ProcessYield);
    Object* value = Local(0);
    SetTop(program()->null_object());
    Advance(kProcessYieldLength);
    SaveState();
    bool terminated = value == program()->true_object();
    return terminated ? Interpreter::kTerminate : Interpreter::kYield;
  OPCODE_END();

  OPCODE_BEGIN(CoroutineChange);
    Object* argument = Local(0);
    SetLocal(0, program()->null_object());
    Coroutine* coroutine = Coroutine::cast(Local(1));
    SetLocal(1, program()->null_object());

    SaveState();
    process()->UpdateCoroutine(coroutine);
    RestoreState();

    Advance(kCoroutineChangeLength);

    Drop(1);
    SetTop(argument);
  OPCODE_END();

  OPCODE_BEGIN(Identical);
    Object* result = HandleIdentical(process(), Local(1), Local(0));
    Drop(1);
    SetTop(result);
    Advance(kIdenticalLength);
  OPCODE_END();

  OPCODE_BEGIN(IdenticalNonNumeric);
    bool identical = Local(0) == Local(1);
    Drop(1);
    SetTop(ToBool(identical));
    Advance(kIdenticalNonNumericLength);
  OPCODE_END();

  OPCODE_BEGIN(EnterNoSuchMethod);
    uint8* return_address = reinterpret_cast<uint8*>(Local(0));
    Opcode opcode = static_cast<Opcode>(*(return_address - 5));

    int selector;
    if (Bytecode::IsInvokeFast(opcode)) {
      int index = Utils::ReadInt32(return_address - 4);
      Array* table = program()->dispatch_table();
      selector = Smi::cast(table->get(index + 1))->value();
    } else if (Bytecode::IsInvokeVtable(opcode)) {
      // TODO(kasperl): The id encoded in the selector is
      // wrong because it is an offset.
      selector = Utils::ReadInt32(return_address - 4);
    } else {
      ASSERT(Bytecode::IsInvokeNormal(opcode));
      selector = Utils::ReadInt32(return_address - 4);
    }

    int arity = Selector::ArityField::decode(selector);
    Smi* selector_smi = Smi::FromWord(selector);
    Object* receiver = Local(arity + 1);

    Push(selector_smi);
    Push(receiver);
    Push(selector_smi);
    Advance(kEnterNoSuchMethodLength);
  OPCODE_END();

  OPCODE_BEGIN(ExitNoSuchMethod);
    Object* result = Pop();
    word selector = Smi::cast(Pop())->value();
    PopReturnAddress();

    // The result of invoking setters must be the assigned value,
    // even in the presence of noSuchMethod.
    if (Selector::KindField::decode(selector) == Selector::SETTER) {
      result = Local(0);
    }

    int arity = Selector::ArityField::decode(selector);
    Drop(arity + 1);
    Push(result);
  OPCODE_END();

  OPCODE_BEGIN(SubroutineCall);
    int delta = ReadInt32(1);
    int return_delta = ReadInt32(5);
    PushDelta(return_delta);
    Advance(delta);
  OPCODE_END();

  OPCODE_BEGIN(SubroutineReturn);
    Advance(-PopDelta());
  OPCODE_END();

  OPCODE_BEGIN(FrameSize);
    Advance(kFrameSizeLength);
  OPCODE_END();

  OPCODE_BEGIN(MethodEnd);
    FATAL("Cannot interpret 'method-end' bytecodes.");
  OPCODE_END();
} // NOLINT

void Engine::Branch(int true_offset, int false_offset) {
  int offset = (Pop() == program()->true_object())
      ? true_offset
      : false_offset;
  Advance(offset);
}

void Engine::PushReturnAddress(int offset) {
  Push(reinterpret_cast<Object*>(ComputeReturnAddress(offset)));
}

void Engine::PopReturnAddress() {
  Goto(reinterpret_cast<uint8*>(Pop()));
}

void Engine::PushDelta(int delta) {
  Push(Smi::FromWord(delta));
}

int Engine::PopDelta() {
  return Smi::cast(Pop())->value();
}

bool Engine::StackOverflowCheck(int size) {
  if (HasStackSpaceFor(size)) return true;
  SaveState();
  if (!process()->HandleStackOverflow(size)) return false;
  RestoreState();
  return true;
}

void Engine::CollectGarbage() {
  // Push bcp so that we can traverse frames when we get
  // to the point where we GC methods.
  SaveState();
  process()->CollectGarbage();
  RestoreState();
}

void Engine::ValidateStack() {
  SaveState();
  StackWalker walker(process(), process()->stack());
  int computed_stack_size = 0;
  int last_arity = 0;
  while (walker.MoveNext()) {
    // Return address
    computed_stack_size += 1;
    computed_stack_size += walker.frame_size();
    last_arity = walker.function()->arity();
  }
  if (process()->stack()->top() != computed_stack_size + last_arity) {
    FATAL("Wrong stack height");
  }
  RestoreState();
}

bool Engine::ShouldBreak() {
  DebugInfo* debug_info = process()->debug_info();
  if (debug_info != NULL) {
    bool should_break = debug_info->ShouldBreak(bcp(), sp());
    if (should_break) SaveState();
    return should_break;
  }
  return false;
}

bool Engine::IsAtBreakPoint() {
  DebugInfo* debug_info = process()->debug_info();
  if (process()->debug_info() != NULL) {
    bool result = debug_info->is_at_breakpoint();
    debug_info->set_is_at_breakpoint(false);
    return result;
  }
  return false;
}

extern "C"
int InterpretFast(Process* process, Port** yield_target) __attribute__((weak));
int InterpretFast(Process* process, Port** yield_target) { return -1; }

void Interpreter::Run() {
  ASSERT(interruption_ == kReady);
  process_->RestoreErrno();
  process_->TakeLookupCache();
  int result = -1;
  if (!process_->is_debugging()) result = InterpretFast(process_, &target_);
  if (result < 0) {
    Engine engine(process_);
    interruption_ = engine.Interpret(&target_);
  } else {
    interruption_ = static_cast<InterruptKind>(result);
  }

  process_->ReleaseLookupCache();
  process_->StoreErrno();
  ASSERT(interruption_ != kReady);
}

// -------------------- Native interpreter support --------------------

bool HandleStackOverflow(Process* process, int size) {
  return process->HandleStackOverflow(size);
}

bool HandleIsInvokeFast(int opcode) {
  return Bytecode::IsInvokeFast(static_cast<Opcode>(opcode));
}

void HandleGC(Process* process) {
  process->CollectGarbage();
}

Object* HandleObjectFromFailure(Process* process, Failure* failure) {
  return process->program()->ObjectFromFailure(failure);
}

Object* HandleAllocate(Process* process, Class* clazz, int immutable) {
  return process->NewInstance(clazz, immutable == 1);
}

Object* HandleAllocateBoxed(Process* process, Object* value) {
  return process->NewBoxed(value);
}

void HandleCoroutineChange(Process* process, Coroutine* coroutine) {
  process->UpdateCoroutine(coroutine);
}

Object* HandleIdentical(Process* process, Object* left, Object* right) {
  bool identical;
  if (left->IsDouble() && right->IsDouble()) {
    double left_value = Double::cast(left)->value();
    double right_value = Double::cast(right)->value();
    if (isnan(left_value) && isnan(right_value)) {
      identical = true;
    } else {
      identical = (left_value == right_value);
    }
  } else if (left->IsLargeInteger() && right->IsLargeInteger()) {
    int64 left_value = LargeInteger::cast(left)->value();
    int64 right_value = LargeInteger::cast(right)->value();
    identical = (left_value == right_value);
  } else {
    identical = (left == right);
  }
  Program* program = process->program();
  return identical ? program->true_object() : program->false_object();
}

LookupCache::Entry* HandleLookupEntry(Process* process,
                                      LookupCache::Entry* primary,
                                      Class* clazz,
                                      int selector) {
  // TODO(kasperl): Can we inline the definition here? This is
  // performance critical.
  return process->LookupEntrySlow(primary, clazz, selector);
}

uint8* HandleThrow(Process* process, Object* exception, int* stack_delta) {
  while (true) {
    uint8* catch_bcp = StackWalker::ComputeCatchBlock(process, stack_delta);
    if (catch_bcp != NULL) return catch_bcp;

    // Unwind the coroutine caller stack by one level.
    Coroutine* current = process->coroutine();
    if (!current->has_caller()) {
      // Uncaught exception.
      printf("Uncaught exception:\n");
      exception->Print();

      // Send stack trace information to attached session if debugging.
      Session* session = process->program()->session();
      if (session != NULL && session->is_debugging()) {
        session->UncaughtException();
        return NULL;
      }
      exit(1);
    }

    Coroutine* caller = current->caller();
    process->UpdateCoroutine(caller);

    // Mark the coroutine that didn't catch the exception as 'done' and
    // make sure to clear its stack reference so we don't unnecessarily
    // hold onto the memory.
    current->set_stack(process->program()->null_object());
    current->set_caller(current);
  }
}

}  // namespace fletch
