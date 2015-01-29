// Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE.md file.

// Generated file. Do not edit.

#include "person_counter.h"
#include "include/service_api.h"
#include <stdlib.h>

static ServiceId _service_id = kNoServiceId;

void PersonCounter::Setup() {
  _service_id = ServiceApiLookup("PersonCounter");
}

void PersonCounter::TearDown() {
  ServiceApiTerminate(_service_id);
  _service_id = kNoServiceId;
}

static const MethodId _kGetAgeId = reinterpret_cast<MethodId>(1);
static const MethodId _kCountId = reinterpret_cast<MethodId>(2);

int PersonCounter::GetAge(Person person) {
  int offset = person.offset() - 32;
  char* buffer = reinterpret_cast<char*>(person.segment()->At(offset));
  ServiceApiInvoke(_service_id, _kGetAgeId, buffer, person.segment()->used());
  return *reinterpret_cast<int*>(buffer + 32);
}

int PersonCounter::Count(Person person) {
  int offset = person.offset() - 32;
  char* buffer = reinterpret_cast<char*>(person.segment()->At(offset));
  ServiceApiInvoke(_service_id, _kCountId, buffer, person.segment()->used());
  return *reinterpret_cast<int*>(buffer + 32);
}

MessageBuilder::MessageBuilder(int space)
    : segment_(space) {
}

Person MessageBuilder::Root() {
  // TODO(kasperl): Mark the person as a root somehow so we know
  // it has a "header" before it.
  return Person(&segment_, 32);
}

PersonBuilder MessageBuilder::NewRoot() {
  int offset = segment_.Allocate(32 + Person::kSize);
  return PersonBuilder(&segment_, offset + 32);
}

Segment::Segment(int capacity)
    : memory_(static_cast<char*>(malloc(capacity))),
      capacity_(capacity),
      used_(0) {
}

Segment::~Segment() {
  free(memory_);
}

int Segment::Allocate(int size) {
  if (used_ + size > capacity_) {
    abort();  // Cannot deal with this yet.
    return -1;
  }
  int result = used_;
  used_ += size;
  return result;
}

List<PersonBuilder> PersonBuilder::NewChildren(int length) {
  int offset = offset_ + Person::kChildrenOffset;
  int* lo = reinterpret_cast<int*>(segment_->At(offset + 0));
  int* hi = reinterpret_cast<int*>(segment_->At(offset + 4));

  int list = segment_->Allocate(PersonBuilder::kSize * length);
  *lo = list;
  *hi = length;
  return List<PersonBuilder>(segment_, list);
}