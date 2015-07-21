// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/v8.h"

#include "src/wasm/wasm-module.h"
#include "src/wasm/decoder.h"

#include "src/compiler/common-operator.h"
#include "src/compiler/machine-operator.h"
#include "src/compiler/pipeline.h"
#include "src/compiler/js-graph.h"
#include "src/wasm/tf-builder.h"

namespace v8 {
namespace internal {
namespace wasm {

// Internal constants for the layout of the module object.
static const int kWasmModuleInternalFieldCount = 4;
static const int kWasmModuleFunctionTable = 0;
static const int kWasmModuleCodeTable = 1;
static const int kWasmMemArrayBuffer = 2;
static const int kWasmGlobalsArrayBuffer = 3;

namespace {
// Helper function to allocate a placeholder code object that will be the
// target of direct calls until the function is compiled.
Handle<Code> CreatePlaceholderCode(Isolate* isolate, uint32_t index) {
  return Handle<Code>::null();  // TODO
}

// Helper function to compile a single function.
Handle<Code> CompileFunction(Isolate* isolate, ModuleEnv* module_env,
                             const WasmFunction& function) {
  // Initialize the function environment for decoding.
  FunctionEnv env;
  env.module = module_env;
  env.sig = function.sig;
  env.local_int32_count = function.local_int32_count;
  env.local_int64_count = function.local_int64_count;
  env.local_float32_count = function.local_float32_count;
  env.local_float64_count = function.local_float64_count;

  env.total_locals =                                      // --
      env.local_int32_count + env.local_int64_count +     // --
      env.local_float32_count + env.local_float64_count;  // --

  // Create a TF graph during decoding.
  Zone zone;
  compiler::Graph graph(&zone);
  compiler::CommonOperatorBuilder common(&zone);
  compiler::MachineOperatorBuilder machine(&zone);
  compiler::JSGraph jsgraph(isolate, &graph, &common, nullptr, &machine);
  Result result = BuildTFGraph(
      &jsgraph, &env,                                                 // --
      module_env->module->module_start + function.code_start_offset,  // --
      module_env->module->module_start + function.code_end_offset);   // --

  if (result.error_code != kSuccess) {
    // TODO(titzer): render an appropriate error message and throw.
    return Handle<Code>::null();
  }

  // Run the compiler pipeline to generate machine code.
  compiler::CallDescriptor* descriptor = const_cast<compiler::CallDescriptor*>(
      module_env->GetWasmCallDescriptor(&zone, function.sig));
  return compiler::Pipeline::GenerateCodeForTesting(isolate, descriptor,
                                                    &graph);
}

// Patch the direct calls in a function to other functions.
void PatchDirectCalls(ModuleEnv* module_env, const WasmFunction& function,
                      Handle<Code> code) {
  if (function.external) return;
  // TODO
}
}


// Instantiates a wasm module as a JSObject.
//  * allocates a backing store of {mem_size} bytes.
//  * installs a named property for that buffer if exported
//  * installs named properties on the object for exported functions
//  * compiles wasm code to machine code
MaybeHandle<JSObject> WasmModule::Instantiate(Isolate* isolate) {
  this->shared_isolate = isolate;  // TODO: have a real shared isolate.

  Factory* factory = isolate->factory();
  Handle<Map> map = factory->NewMap(
      JS_OBJECT_TYPE,
      JSObject::kHeaderSize + kWasmModuleInternalFieldCount * kPointerSize);

  // Allocate the module object.
  Handle<JSObject> module = factory->NewJSObjectFromMap(map, TENURED);
  Handle<FixedArray> code_table =
      factory->NewFixedArray(static_cast<int>(functions->size()), TENURED);

  // Allocate the raw memory for the mem.
  if (mem_size_log2 > kMaxMemSize) {
    // Memory is bigger than maximum supported size.
    return isolate->Throw<JSObject>(
        factory->InternalizeUtf8String("Out of memory: wasm memory too large"));
  }
  uint32_t size = 1 << mem_size_log2;
  void* backing_store = isolate->array_buffer_allocator()->Allocate(size);
  if (!backing_store) {
    // Not enough space for backing store of mem
    return isolate->Throw<JSObject>(
        factory->InternalizeUtf8String("Out of memory: wasm memory"));
  }

#if DEBUG
  // Double check the API allocator actually zero-initialized the memory.
  for (uint32_t i = 0; i < size; i++) {
    DCHECK_EQ(0, static_cast<byte*>(backing_store)[i]);
  }
#endif

  // Load initialized data segments.
  for (const WasmDataSegment& segment : *data_segments) {
    if (!segment.init) continue;
    CHECK_LT(segment.dest_addr, size);
    CHECK_LT(segment.source_size, size);
    CHECK_LT(segment.dest_addr + segment.source_size, size);
    byte* addr = reinterpret_cast<byte*>(backing_store) + segment.dest_addr;
    memcpy(addr, module_start + segment.source_offset, segment.source_size);
  }

  // Create the JSArrayBuffer backed by the raw memory.
  Handle<JSArrayBuffer> buffer = factory->NewJSArrayBuffer();
  isolate->heap()->RegisterNewArrayBuffer(isolate->heap()->InNewSpace(*buffer),
                                          backing_store, size);
  buffer->set_backing_store(backing_store);
  buffer->set_is_external(false);
  buffer->set_is_neuterable(false);
  buffer->set_byte_length(Smi::FromInt(size));
  module->SetInternalField(kWasmMemArrayBuffer, *buffer);

  // TODO(titzer): allocate storage for the globals.
  module->SetInternalField(kWasmGlobalsArrayBuffer, Smi::FromInt(0));

  if (mem_export) {
    // Export the memory as a named property.
    Handle<String> name = factory->InternalizeUtf8String("memory");
    JSObject::AddProperty(module, name, buffer, READ_ONLY);
  }

  // Compile all functions in the module.
  int index = 0;
  ModuleEnv module_env;
  module_env.module = this;
  module_env.mem_start = reinterpret_cast<uintptr_t>(backing_store);
  module_env.mem_end = reinterpret_cast<uintptr_t>(backing_store) + size;
  std::vector<Handle<Code>> function_code(functions->size());
  module_env.function_code = &function_code;

  // First pass: compile each function.
  for (const WasmFunction& func : *functions) {
    // Compile each function and initialize the code table.
    Handle<String> name =
        factory->InternalizeUtf8String(GetName(func.name_offset));
    if (func.external) {
      // External functions are read-write properties on this object.
      Handle<Object> undefined = isolate->factory()->undefined_value();
      JSObject::AddProperty(module, name, undefined, DONT_DELETE);
    } else {
      // Compile the function and install it in the code table.
      Handle<Code> code = CompileFunction(isolate, &module_env, func);
      if (!code.is_null()) {
        function_code[index] = code;
        code_table->set(index, *code);
      }
    }
    if (func.exported) {
      // Export functions are installed as read-only properties on the module.
      Handle<JSFunction> function = factory->NewFunction(name);
      JSObject::AddProperty(module, name, function, READ_ONLY);
      // TODO: create adapter code object for exported functions.
    }
    index++;
  }

  // Second pass: patch all direct call sites.
  for (index = 0; index < functions->size(); index++) {
    PatchDirectCalls(&module_env, functions->at(index), function_code[index]);
  }

  module->SetInternalField(kWasmModuleFunctionTable, Smi::FromInt(0));
  module->SetInternalField(kWasmModuleCodeTable, *code_table);
  return module;
}


Handle<Code> ModuleEnv::GetFunctionCode(uint32_t index) {
  DCHECK(IsValidFunction(index));
  Handle<Code> result = Handle<Code>::null();
  if (function_code) {
    result = function_code->at(index);
    if (result.is_null()) {
      // Allocate placeholder code that will be patched later.
      result = CreatePlaceholderCode(module->shared_isolate, index);
      function_code->at(index) = result;
    }
  }
  return result;
}


const compiler::CallDescriptor* ModuleEnv::GetWasmCallDescriptor(
    Zone* zone, FunctionSig* sig) {
  return nullptr;  // TODO
}


const compiler::CallDescriptor* ModuleEnv::GetCallDescriptor(Zone* zone,
                                                             uint32_t index) {
  return nullptr;  // TODO
}
}
}
}
