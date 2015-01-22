// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

#include "src/vm/ffi.h"

#include <dlfcn.h>
#include <errno.h>

#include "src/vm/natives.h"
#include "src/vm/object.h"
#include "src/vm/port.h"
#include "src/vm/process.h"

namespace fletch {

class DefaultLibraryEntry {
 public:
  DefaultLibraryEntry(char* library, DefaultLibraryEntry* next)
      : library_(library), next_(next) {
  }

  ~DefaultLibraryEntry() {
    free(library_);
  }

  const char* library() const { return library_; }
  DefaultLibraryEntry* next() const { return next_; }

 private:
  char* library_;
  DefaultLibraryEntry* next_;
};

DefaultLibraryEntry* ForeignFunctionInterface::libraries_ = NULL;
Mutex* ForeignFunctionInterface::mutex_ = NULL;

void ForeignFunctionInterface::Setup() {
  mutex_ = Platform::CreateMutex();
}

void ForeignFunctionInterface::TearDown() {
  DefaultLibraryEntry* current = libraries_;
  while (current != NULL) {
    DefaultLibraryEntry* next = current->next();
    delete current;
    current = next;
  }
  delete mutex_;
}

void ForeignFunctionInterface::AddDefaultSharedLibrary(const char* library) {
  ScopedLock lock(mutex_);
  libraries_ = new DefaultLibraryEntry(strdup(library), libraries_);
}

static void* PerformForeignLookup(const char* library, const char* name) {
  void* handle = dlopen(library, RTLD_LOCAL | RTLD_LAZY);
  if (handle == NULL) return NULL;
  void* result = dlsym(handle, name);
  if (dlclose(handle) != 0) return NULL;
  return result;
}

void* ForeignFunctionInterface::LookupInDefaultLibraries(const char* symbol) {
  ScopedLock lock(mutex_);
  for (DefaultLibraryEntry* current = libraries_;
       current != NULL;
       current = current->next()) {
    void* result = PerformForeignLookup(current->library(), symbol);
    if (result != NULL) return result;
  }
  return NULL;
}

NATIVE(ForeignLookup) {
  char* library = arguments[1]->IsString()
      ? AsForeignString(String::cast(arguments[1]))
      : NULL;

  char* name = AsForeignString(String::cast(arguments[0]));
  void* result = PerformForeignLookup(library, name);

  if (result == NULL) {
    result = ForeignFunctionInterface::LookupInDefaultLibraries(name);
  }

  if (result == NULL) fprintf(stderr, "Failed foreign lookup: %s\n", name);

  free(library);
  free(name);

  return result != NULL
      ? process->ToInteger(reinterpret_cast<intptr_t>(result))
      : Failure::index_out_of_bounds();
}

NATIVE(ForeignAllocate) {
  word size = AsForeignWord(arguments[0]);
  void* result = calloc(1, size);
  return process->ToInteger(reinterpret_cast<intptr_t>(result));
}

NATIVE(ForeignFree) {
  word address = AsForeignWord(arguments[0]);
  free(reinterpret_cast<void*>(address));
  return process->program()->null_object();
}

NATIVE(ForeignMarkForFinalization) {
  HeapObject* foreign = HeapObject::cast(arguments[0]);
  process->RegisterFinalizer(foreign, Process::FinalizeForeign);
  return process->program()->null_object();
}

NATIVE(ForeignBitsPerWord) {
  return Smi::FromWord(kBitsPerWord);
}

NATIVE(ForeignErrno) {
  return Smi::FromWord(errno);
}

NATIVE(ForeignPlatform) {
  return Smi::FromWord(Platform::OS());
}

NATIVE(ForeignConvertPort) {
  if (!arguments[0]->IsInstance()) return Smi::zero();
  Instance* instance = Instance::cast(arguments[0]);
  if (!instance->IsPort()) return Smi::zero();
  Object* field = instance->GetInstanceField(0);
  uword address = AsForeignWord(field);
  if (address == 0) return Smi::zero();
  Port* port = reinterpret_cast<Port*>(address);
  port->IncrementRef();
  return process->ToInteger(reinterpret_cast<intptr_t>(port));
}

typedef int (*F0)();
typedef int (*F1)(word);
typedef int (*F2)(word, word);
typedef int (*F3)(word, word, word);
typedef int (*F4)(word, word, word, word);
typedef int (*F5)(word, word, word, word, word);
typedef int (*F6)(word, word, word, word, word, word);

NATIVE(ForeignCall0) {
  word address = AsForeignWord(arguments[0]);
  F0 function = reinterpret_cast<F0>(address);
  int result = function();
  return process->ToInteger(result);
}

NATIVE(ForeignCall1) {
  word address = AsForeignWord(arguments[0]);
  word a0 = AsForeignWord(arguments[1]);
  F1 function = reinterpret_cast<F1>(address);
  int result = function(a0);
  return process->ToInteger(result);
}

NATIVE(ForeignCall2) {
  word address = AsForeignWord(arguments[0]);
  word a0 = AsForeignWord(arguments[1]);
  word a1 = AsForeignWord(arguments[2]);
  F2 function = reinterpret_cast<F2>(address);
  int result = function(a0, a1);
  return process->ToInteger(result);
}

NATIVE(ForeignCall3) {
  word address = AsForeignWord(arguments[0]);
  word a0 = AsForeignWord(arguments[1]);
  word a1 = AsForeignWord(arguments[2]);
  word a2 = AsForeignWord(arguments[3]);
  F3 function = reinterpret_cast<F3>(address);
  int result = function(a0, a1, a2);
  return process->ToInteger(result);
}

NATIVE(ForeignCall4) {
  word address = AsForeignWord(arguments[0]);
  word a0 = AsForeignWord(arguments[1]);
  word a1 = AsForeignWord(arguments[2]);
  word a2 = AsForeignWord(arguments[3]);
  word a3 = AsForeignWord(arguments[4]);
  F4 function = reinterpret_cast<F4>(address);
  int result = function(a0, a1, a2, a3);
  return process->ToInteger(result);
}

NATIVE(ForeignCall5) {
  word address = AsForeignWord(arguments[0]);
  word a0 = AsForeignWord(arguments[1]);
  word a1 = AsForeignWord(arguments[2]);
  word a2 = AsForeignWord(arguments[3]);
  word a3 = AsForeignWord(arguments[4]);
  word a4 = AsForeignWord(arguments[5]);
  F5 function = reinterpret_cast<F5>(address);
  int result = function(a0, a1, a2, a3, a4);
  return process->ToInteger(result);
}

NATIVE(ForeignCall6) {
  word address = AsForeignWord(arguments[0]);
  word a0 = AsForeignWord(arguments[1]);
  word a1 = AsForeignWord(arguments[2]);
  word a2 = AsForeignWord(arguments[3]);
  word a3 = AsForeignWord(arguments[4]);
  word a4 = AsForeignWord(arguments[5]);
  word a5 = AsForeignWord(arguments[6]);
  F6 function = reinterpret_cast<F6>(address);
  int result = function(a0, a1, a2, a3, a4, a5);
  return process->ToInteger(result);
}

#define DEFINE_FOREIGN_ACCESSORS(suffix, type)                           \
                                                                         \
NATIVE(ForeignGet##suffix) {                                             \
  type* address = reinterpret_cast<type*>(AsForeignWord(arguments[0]));  \
  return process->ToInteger(*address);                                   \
}                                                                        \
                                                                         \
NATIVE(ForeignSet##suffix) {                                             \
  Object* value = arguments[1];                                          \
  type* address = reinterpret_cast<type*>(AsForeignWord(arguments[0]));  \
  *address = AsForeignWord(value);                                       \
  return value;                                                          \
}

DEFINE_FOREIGN_ACCESSORS(Int8, int8)
DEFINE_FOREIGN_ACCESSORS(Int16, int16)
DEFINE_FOREIGN_ACCESSORS(Int32, int32)
DEFINE_FOREIGN_ACCESSORS(Int64, int64)

DEFINE_FOREIGN_ACCESSORS(Uint8, uint8)
DEFINE_FOREIGN_ACCESSORS(Uint16, uint16)
DEFINE_FOREIGN_ACCESSORS(Uint32, uint32)
DEFINE_FOREIGN_ACCESSORS(Uint64, uint64)

#undef DEFINE_FOREIGN_ACCESSORS

}  // namespace fletch