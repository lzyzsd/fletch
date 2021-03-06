#!/bin/bash

# Copyright (c) 2015, the Fletch project authors. Please see the AUTHORS file
# for details. All rights reserved. Use of this source code is governed by a
# BSD-style license that can be found in the LICENSE.md file.

set -ue

DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SERVICEC_DIR=$DIR/../../../tools/servicec
JAVA_DIR=$DIR/../java
FLETCH_DIR=$DIR/../../..

# Regenerate java and jni sources.
cd $SERVICEC_DIR
dart bin/servicec.dart --out=../../samples/todomvc/ ../../samples/todomvc/todomvc_service.idl

# Build the native interpreter src for arm and x86.
cd $FLETCH_DIR
ninja
ninja -C out/ReleaseXARMAndroid fletch_vm_generator
ninja -C out/ReleaseIA32 fletch_vm_generator
mkdir -p out/ReleaseXARMAndroid/obj/src/vm/fletch_vm.gen
mkdir -p out/ReleaseIA32/obj/src/vm/fletch_vm.gen
out/ReleaseXARMAndroid/fletch_vm_generator > out/ReleaseXARMAndroid/obj/src/vm/fletch_vm.gen/generated.S
out/ReleaseIA32/fletch_vm_generator > out/ReleaseIA32/obj/src/vm/fletch_vm.gen/generated.S

# Compile Fletch runtime and jni code into libfletch.so.
cd $JAVA_DIR
NDK_MODULE_PATH=. ndk-build

# Copy Java source and fletch library to the right places.
mkdir -p $DIR/TodoMVC/app/src/main/java/fletch
cp -R fletch/*.java $DIR/TodoMVC/app/src/main/java/fletch/
mkdir -p $DIR/TodoMVC/app/src/main/jniLibs/
cp -R libs/* $DIR/TodoMVC/app/src/main/jniLibs/

# Build snapshot.
cd $FLETCH_DIR
ninja -C out/ReleaseIA32 fletch fletchc
mkdir -p $DIR/TodoMVC/app/src/main/res/raw
./out/ReleaseIA32/fletch $DIR/../todomvc.dart --out=$DIR/TodoMVC/app/src/main/res/raw/todomvc_snapshot
