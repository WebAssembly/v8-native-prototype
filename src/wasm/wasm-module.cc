// Copyright 2015 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/v8.h"
#include "src/macro-assembler.h"
#include "src/objects.h"

#include "src/simulator.h"

// TODO(titzer): wasm-module shouldn't need anything from the compiler.
#include "src/compiler/common-operator.h"
#include "src/compiler/js-graph.h"
#include "src/compiler/pipeline.h"
#include "src/compiler/machine-operator.h"

#include "src/wasm/decoder.h"
#include "src/wasm/tf-builder.h"
#include "src/wasm/wasm-module.h"
#include "src/wasm/wasm-result.h"
#include "src/wasm/wasm-wrapper.h"

namespace v8 {
namespace internal {
namespace wasm {

#if DEBUG
#define TRACE(...)                                     \
  do {                                                 \
    if (FLAG_trace_wasm_compiler) PrintF(__VA_ARGS__); \
  } while (false)
#else
#define TRACE(...)
#endif


std::ostream& operator<<(std::ostream& os, const WasmModule& module) {
  os << "WASM module with ";
  os << (1 << module.mem_size_log2) << " mem bytes";
  if (module.functions) os << module.functions->size() << " functions";
  if (module.globals) os << module.functions->size() << " globals";
  if (module.data_segments) os << module.functions->size() << " data segments";
  return os;
}


std::ostream& operator<<(std::ostream& os, const WasmFunction& function) {
  os << "WASM function with signature ";

  // TODO(titzer): factor out rendering of signatures.
  if (function.sig->return_count() == 0) os << "v";
  for (size_t i = 0; i < function.sig->return_count(); i++) {
    os << WasmOpcodes::ShortNameOf(function.sig->GetReturn(i));
  }
  os << "_";
  if (function.sig->parameter_count() == 0) os << "v";
  for (size_t i = 0; i < function.sig->parameter_count(); i++) {
    os << WasmOpcodes::ShortNameOf(function.sig->GetParam(i));
  }
  os << " locals: ";
  if (function.local_int32_count)
    os << function.local_int32_count << " int32s ";
  if (function.local_int64_count)
    os << function.local_int64_count << " int64s ";
  if (function.local_float32_count)
    os << function.local_float32_count << " float32s ";
  if (function.local_float64_count)
    os << function.local_float64_count << " float64s ";

  os << " code bytes: "
     << (function.code_end_offset - function.code_start_offset);
  return os;
}


// A helper class for compiling multiple wasm functions that offers
// placeholder code objects for calling functions that are not yet compiled.
class WasmLinker {
 public:
  WasmLinker(Isolate* isolate, size_t size)
      : isolate_(isolate), placeholder_code_(size), function_code_(size) {}

  // Get the code object for a function, allocating a placeholder if it has
  // not yet been compiled.
  Handle<Code> GetFunctionCode(uint32_t index) {
    DCHECK(index < function_code_.size());
    if (function_code_[index].is_null()) {
      // Create a placeholder code object and encode the corresponding index in
      // the {constant_pool_offset} field of the code object.
      // TODO(titzer): placeholder code objects are somewhat dangerous.
      Handle<Code> self(nullptr, isolate_);
      byte buffer[] = {0, 0, 0, 0, 0, 0, 0, 0};  // fake instructions.
      CodeDesc desc = {buffer, 8, 8, 0, 0, nullptr};
      Handle<Code> code = isolate_->factory()->NewCode(
          desc, Code::KindField::encode(Code::WASM_FUNCTION), self);
      code->set_constant_pool_offset(index + kPlaceholderMarker);
      placeholder_code_[index] = code;
      function_code_[index] = code;
    }
    return function_code_[index];
  }

  void Finish(uint32_t index, Handle<Code> code) {
    DCHECK(index < function_code_.size());
    function_code_[index] = code;
  }

  void Link() {
    for (size_t i = 0; i < function_code_.size(); i++) {
      LinkFunction(function_code_[i]);
    }
  }

 private:
  static const int kPlaceholderMarker = 1000000000;

  Isolate* isolate_;
  std::vector<Handle<Code>> placeholder_code_;
  std::vector<Handle<Code>> function_code_;

  void LinkFunction(Handle<Code> code) {
    bool modified = false;
    int mode_mask = RelocInfo::kCodeTargetMask;
    AllowDeferredHandleDereference embedding_raw_address;
    for (RelocIterator it(*code, mode_mask); !it.done(); it.next()) {
      RelocInfo::Mode mode = it.rinfo()->rmode();
      if (RelocInfo::IsCodeTarget(mode)) {
        Code* target =
            Code::GetCodeFromTargetAddress(it.rinfo()->target_address());
        if (target->kind() == Code::WASM_FUNCTION &&
            target->constant_pool_offset() >= kPlaceholderMarker) {
          // Patch direct calls to placeholder code objects.
          uint32_t index = target->constant_pool_offset() - kPlaceholderMarker;
          CHECK(index < function_code_.size());
          Handle<Code> new_target = function_code_[index];
          if (target != *new_target) {
            CHECK_EQ(*placeholder_code_[index], target);
            it.rinfo()->set_target_address(new_target->instruction_start(),
                                           SKIP_WRITE_BARRIER,
                                           SKIP_ICACHE_FLUSH);
            modified = true;
          }
        }
      }
    }
    if (modified) {
      CpuFeatures::FlushICache(code->instruction_start(),
                               code->instruction_size());
    }
  }
};


namespace {
// Internal constants for the layout of the module object.
const int kWasmModuleInternalFieldCount = 4;
const int kWasmModuleFunctionTable = 0;
const int kWasmModuleCodeTable = 1;
const int kWasmMemArrayBuffer = 2;
const int kWasmGlobalsArrayBuffer = 3;


// Helper function to compile a single function.
Handle<Code> CompileFunction(ErrorThrower& thrower, Isolate* isolate,
                             ModuleEnv* module_env,
                             const WasmFunction& function, int index) {
  if (FLAG_trace_wasm_compiler) {
    // TODO(titzer): clean me up a bit.
    OFStream os(stdout);
    os << "Compiling WASM function #" << index << ":";
    if (function.name_offset > 0) {
      os << module_env->module->GetName(function.name_offset);
    }
    os << std::endl;
  }
  // Initialize the function environment for decoding.
  FunctionEnv env;
  env.module = module_env;
  env.sig = function.sig;
  env.local_int32_count = function.local_int32_count;
  env.local_int64_count = function.local_int64_count;
  env.local_float32_count = function.local_float32_count;
  env.local_float64_count = function.local_float64_count;
  env.SumLocals();

  // Create a TF graph during decoding.
  Zone zone;
  compiler::Graph graph(&zone);
  compiler::CommonOperatorBuilder common(&zone);
  compiler::MachineOperatorBuilder machine(&zone);
  compiler::JSGraph jsgraph(isolate, &graph, &common, nullptr, &machine);
  TreeResult result = BuildTFGraph(
      &jsgraph, &env,                                                 // --
      module_env->module->module_start,                               // --
      module_env->module->module_start + function.code_start_offset,  // --
      module_env->module->module_start + function.code_end_offset);   // --

  if (result.failed()) {
    if (FLAG_trace_wasm_compiler) {
      OFStream os(stdout);
      os << "Compilation failed: " << result << std::endl;
    }
    // Add the function as another context for the exception
    char buffer[256];
    snprintf(buffer, 256, "Compiling WASM function #%d:%s failed:", index,
             module_env->module->GetName(function.name_offset));
    thrower.Failed(buffer, result);
    return Handle<Code>::null();
  }

  // Run the compiler pipeline to generate machine code.
  compiler::CallDescriptor* descriptor = const_cast<compiler::CallDescriptor*>(
      module_env->GetWasmCallDescriptor(&zone, function.sig));
  CompilationInfo info("wasm", isolate, &zone);
  Handle<Code> code =
      compiler::Pipeline::GenerateCodeForTesting(&info, descriptor, &graph);

#ifdef ENABLE_DISASSEMBLER
  // Disassemble the code for debugging.
  if (!code.is_null() && FLAG_print_opt_code) {
    static const int kBufferSize = 128;
    char buffer[kBufferSize];
    const char* name = "";
    if (function.name_offset > 0) {
      const byte* ptr = module_env->module->module_start + function.name_offset;
      name = reinterpret_cast<const char*>(ptr);
    }
    snprintf(buffer, kBufferSize, "WASM function #%d:%s", index, name);
    OFStream os(stdout);
    code->Disassemble(buffer, os);
  }
#endif
  return code;
}


Handle<JSArrayBuffer> NewArrayBuffer(Isolate* isolate, int size,
                                     byte** backing_store) {
  void* memory = isolate->array_buffer_allocator()->Allocate(size);
  if (!memory) return Handle<JSArrayBuffer>::null();
  *backing_store = reinterpret_cast<byte*>(memory);

#if DEBUG
  // Double check the API allocator actually zero-initialized the memory.
  for (uint32_t i = 0; i < size; i++) {
    DCHECK_EQ(0, (*backing_store)[i]);
  }
#endif

  Handle<JSArrayBuffer> buffer = isolate->factory()->NewJSArrayBuffer();
  JSArrayBuffer::Setup(buffer, isolate, true, memory, size);
  buffer->set_is_neuterable(false);
  return buffer;
}

// The main logic for decoding the bytes of a module.
class ModuleDecoder {
 public:
  ModuleDecoder(Zone* zone, const byte* module_start, const byte* module_end)
      : module_zone(zone),
        start_(module_start),
        cur_(module_start),
        end_(module_end) {
    result_.start = start_;
    if (end_ < start_) {
      error(start_, "end is less than start");
      end_ = start_;
    }
  }

  // Decodes an entire module.
  ModuleResult DecodeModule(WasmModule* module, bool verify_functions = true) {
    cur_ = start_;
    result_.val = module;
    module->module_start = start_;
    module->module_end = end_;
    module->mem_size_log2 = 0;
    module->mem_export = false;
    module->mem_external = false;
    module->functions = new std::vector<WasmFunction>();
    module->globals = new std::vector<WasmGlobal>();
    module->data_segments = new std::vector<WasmDataSegment>();

    // Decode the module header.
    module->mem_size_log2 = u8();    // read the memory size
    module->mem_export = u8() != 0;  // read memory export option

    uint32_t globals_count = u16();        // read number of globals
    uint32_t functions_count = u16();      // read number of functions
    uint32_t data_segments_count = u16();  // read number of data segments

    // Decode globals.
    for (uint32_t i = 0; i < globals_count; i++) {
      if (result_.failed()) break;
      module->globals->push_back({0, kMemI32, 0, false});
      WasmGlobal* global = &module->globals->back();
      DecodeGlobalInModule(global);
    }

    // Set up module environment for verification.
    ModuleEnv menv;
    menv.module = module;
    menv.globals_area = 0;
    menv.mem_start = 0;
    menv.mem_end = 0;
    menv.function_code = nullptr;

    // Decode functions.
    for (uint32_t i = 0; i < functions_count; i++) {
      if (result_.failed()) break;
      module->functions->push_back({nullptr, 0, 0, 0, 0, 0, 0, false, false});
      WasmFunction* function = &module->functions->back();
      DecodeFunctionInModule(function, verify_functions);

      if (result_.ok() && verify_functions) {
        if (!function->external) VerifyFunctionBody(i, &menv, function);
      }
    }

    // Decode data segments.
    for (uint32_t i = 0; i < data_segments_count; i++) {
      if (result_.failed()) break;
      module->data_segments->push_back({0, 0, 0});
      WasmDataSegment* segment = &module->data_segments->back();
      DecodeDataSegmentInModule(segment);
    }

    return result_;
  }

  // Decodes a single anonymous function starting at {start_}.
  FunctionResult DecodeSingleFunction(ModuleEnv* module_env,
                                      WasmFunction* function) {
    cur_ = start_;
    function->sig = sig();                        // read signature
    function->name_offset = 0;                    // ---- name
    function->code_start_offset = off(cur_ + 8);  // ---- code start
    function->code_end_offset = off(end_);        // ---- code end
    function->local_int32_count = u16();          // read u16
    function->local_int64_count = u16();          // read u16
    function->local_float32_count = u16();        // read u16
    function->local_float64_count = u16();        // read u16
    function->exported = false;                   // ---- exported
    function->external = false;                   // ---- external

    if (result_.ok()) {
      VerifyFunctionBody(0, module_env, function);
    }

    FunctionResult result;
    // Copy error code and location.
    result.CopyFrom(result_);
    result.val = function;
    return result;
  }

  // Decodes a single function signature at {start}.
  FunctionSig* DecodeFunctionSignature(const byte* start) {
    cur_ = start;
    FunctionSig* result = sig();
    return result_.ok() ? result : nullptr;
  }

 private:
  Zone* module_zone;
  const byte* start_;
  const byte* cur_;
  const byte* end_;
  ModuleResult result_;

  uint32_t off(const byte* ptr) { return static_cast<uint32_t>(ptr - start_); }

  // Decodes a single global entry inside a module starting at {cur_}.
  void DecodeGlobalInModule(WasmGlobal* global) {
    global->name_offset = string();  // read global name
    global->type = mem_type();       // read global memory type
    global->offset = 0;              // ---- offset is computed later
    global->exported = u8() != 0;    // read exported flag
  }

  // Decodes a single function entry inside a module starting at {cur_}.
  void DecodeFunctionInModule(WasmFunction* function, bool verify_body = true) {
    function->sig = sig();                   // read function signature
    function->name_offset = string();        // read function name
    function->code_start_offset = offset();  // read code start offset
    function->code_end_offset = offset();    // read code end offset
    function->local_int32_count = u16();     // read local int32 count
    function->local_int64_count = u16();     // read local int64 count
    function->local_float32_count = u16();   // read local float32 count
    function->local_float64_count = u16();   // read local float64 count
    function->exported = u8() != 0;          // read exported flag
    function->external = u8() != 0;          // read external flag
  }

  // Decodes a single data segment entry inside a module starting at {cur_}.
  void DecodeDataSegmentInModule(WasmDataSegment* segment) {
    segment->dest_addr = u32();  // TODO: check it's within the memory size.
    segment->source_offset = offset();
    segment->source_size = u32();  // TODO: check the size is reasonable.
    segment->init = u8();
  }

  // Verifies the body (code) of a given function.
  void VerifyFunctionBody(uint32_t func_num, ModuleEnv* menv,
                          WasmFunction* function) {
    FunctionEnv fenv;
    fenv.module = menv;
    fenv.sig = function->sig;
    fenv.local_int32_count = function->local_int32_count;
    fenv.local_int64_count = function->local_int64_count;
    fenv.local_float32_count = function->local_float32_count;
    fenv.local_float64_count = function->local_float64_count;
    fenv.SumLocals();

    TreeResult result =
        VerifyWasmCode(&fenv, start_, start_ + function->code_start_offset,
                       start_ + function->code_end_offset);
    if (result.failed()) {
      // Wrap the error message from the function decoder.
      std::ostringstream str;
      str << "in function #" << func_num << ": ";
      // TODO(titzer): add function name for the user?
      str << result;
      const char* raw = str.str().c_str();
      size_t len = strlen(raw);
      char* buffer = new char[len];
      strncpy(buffer, raw, len);
      buffer[len - 1] = 0;

      // Copy error code and location.
      result_.CopyFrom(result);
      result_.error_msg.Reset(buffer);
    }
  }

  // Reads a single 8-bit unsigned integer (byte) and advances.
  uint8_t u8() { return checkAvailable(1) ? *(cur_++) : 0; }

  // Reads a single 16-bit unsigned integer (little endian) and advances.
  uint16_t u16() {
    if (checkAvailable(2)) {
#ifdef V8_TARGET_LITTLE_ENDIAN
      // TODO(titzer): lift endianness out to a utility (none found so far).
      byte b0 = cur_[0];
      byte b1 = cur_[1];
#else
      byte b1 = cur_[0];
      byte b0 = cur_[1];
#endif
      cur_ += 2;
      return static_cast<uint16_t>(b1 << 8) | b0;
    }
    return 0;
  }

  // Reads a single 32-bit unsigned integer (little endian) and advances.
  uint32_t u32() {
    if (checkAvailable(4)) {
#ifdef V8_TARGET_LITTLE_ENDIAN
      // TODO(titzer): lift endianness out to a utility (none found so far).
      byte b0 = cur_[0];
      byte b1 = cur_[1];
      byte b2 = cur_[2];
      byte b3 = cur_[3];
#else
      byte b3 = cur_[0];
      byte b2 = cur_[1];
      byte b1 = cur_[2];
      byte b0 = cur_[3];
#endif
      cur_ += 4;
      return static_cast<uint32_t>(b3 << 24) | static_cast<uint32_t>(b2 << 16) |
             static_cast<uint32_t>(b1 << 8) | b0;
    }
    return 0;
  }

  // Reads a single 32-bit unsigned integer interpreted as an offset, checking
  // the offset is within bounds and advances.
  uint32_t offset() {
    uint32_t offset = u32();
    if (offset > (end_ - start_)) {
      error(cur_ - sizeof(uint32_t), "offset out of bounds of module");
    }
    return offset;
  }

  // Reads a single 32-bit unsigned integer interpreted as an offset into the
  // data and validating the string there and advances.
  uint32_t string() { return offset(); }  // TODO: validate string

  // Reads a single 8-bit integer, interpreting it as a local type.
  LocalType local_type() {
    byte val = u8();
    LocalType t = static_cast<LocalType>(val);
    switch (t) {
      case kAstStmt:
      case kAstI32:
      case kAstI64:
      case kAstF32:
      case kAstF64:
        return t;
      default:
        error(cur_ - 1, "invalid local type");
        return kAstStmt;
    }
  }

  // Reads a single 8-bit integer, interpreting it as a memory type.
  MemType mem_type() {
    byte val = u8();
    MemType t = static_cast<MemType>(val);
    switch (t) {
      case kMemI8:
      case kMemU8:
      case kMemI16:
      case kMemU16:
      case kMemI32:
      case kMemU32:
      case kMemI64:
      case kMemU64:
      case kMemF32:
      case kMemF64:
        return t;
      default:
        error(cur_ - 1, "invalid memory type");
        return kMemI32;
    }
  }

  // Parses an inline function signature.
  FunctionSig* sig() {
    byte count = u8();
    LocalType ret = local_type();
    FunctionSig::Builder builder(module_zone, ret == kAstStmt ? 0 : 1, count);
    if (ret != kAstStmt) builder.AddReturn(ret);

    for (int i = 0; i < count; i++) {
      LocalType param = local_type();
      if (param == kAstStmt) error(cur_ - 1, "invalid void parameter type");
      builder.AddParam(param);
    }
    return builder.Build();
  }

  bool checkAvailable(int size) {
    if (cur_ < start_ || cur_ + size > end_) {
      char buf[128];
      snprintf(buf, 128, "expected %d bytes, fell off end", size);
      error(cur_, buf);
      return false;
    } else {
      return true;
    }
  }

  void error(const byte* pc, const char* msg, const byte* pt = nullptr) {
    if (result_.error_code == kSuccess) {
#if DEBUG
      if (FLAG_wasm_break_on_decoder_error) {
        base::OS::DebugBreak();
      }
#endif
      result_.error_code = kError;  // TODO(titzer): better error code
      size_t len = strlen(msg) + 1;
      char* result = new char[len];
      strncpy(result, msg, len);
      result[len - 1] = 0;
      result_.error_msg.Reset(result);
      result_.error_pc = pc;
      result_.error_pt = pt;
    }
  }
};


size_t AllocateGlobalsOffsets(std::vector<WasmGlobal>* globals) {
  uint32_t offset = 0;
  if (!globals) return 0;
  for (WasmGlobal& global : *globals) {
    byte size = WasmOpcodes::MemSize(global.type);
    offset = (offset + size - 1) & ~(size - 1);  // align
    global.offset = offset;
    offset += size;
  }
  return offset;
}


size_t ComputeGlobalsSize(std::vector<WasmGlobal>* globals) {
  uint32_t globals_size = 0;
  if (!globals) return 0;
  for (const WasmGlobal& global : *globals) {  // maximum of all globals.
    uint32_t end = global.offset + WasmOpcodes::MemSize(global.type);
    if (end > globals_size) globals_size = end;
  }
  return globals_size;
}

void LoadDataSegments(WasmModule* module, byte* mem_addr, size_t mem_size) {
  for (const WasmDataSegment& segment : *module->data_segments) {
    if (!segment.init) continue;
    CHECK_LT(segment.dest_addr, mem_size);
    CHECK_LT(segment.source_size, mem_size);
    CHECK_LT(segment.dest_addr + segment.source_size, mem_size);
    byte* addr = mem_addr + segment.dest_addr;
    memcpy(addr, module->module_start + segment.source_offset,
           segment.source_size);
  }
}
}  // namespace


// Instantiates a wasm module as a JSObject.
//  * allocates a backing store of {mem_size} bytes.
//  * installs a named property "memory" for that buffer if exported
//  * installs named properties on the object for exported functions
//  * compiles wasm code to machine code
MaybeHandle<JSObject> WasmModule::Instantiate(Isolate* isolate,
                                              Handle<JSObject> ffi) {
  this->shared_isolate = isolate;  // TODO: have a real shared isolate.
  ErrorThrower thrower(isolate, "WasmModule::Instantiate()");

  Factory* factory = isolate->factory();
  // Memory is bigger than maximum supported size.
  if (mem_size_log2 > kMaxMemSize) {
    thrower.Error("Out of memory: wasm memory too large");
    return MaybeHandle<JSObject>();
  }

  Handle<Map> map = factory->NewMap(
      JS_OBJECT_TYPE,
      JSObject::kHeaderSize + kWasmModuleInternalFieldCount * kPointerSize);

  //-------------------------------------------------------------------------
  // Allocate the module object.
  //-------------------------------------------------------------------------
  Handle<JSObject> module = factory->NewJSObjectFromMap(map, TENURED);
  Handle<FixedArray> code_table =
      factory->NewFixedArray(static_cast<int>(functions->size()), TENURED);

  //-------------------------------------------------------------------------
  // Allocate the linear memory.
  //-------------------------------------------------------------------------
  uint32_t mem_size = 1 << mem_size_log2;
  byte* mem_addr = nullptr;
  Handle<JSArrayBuffer> mem_buffer =
      NewArrayBuffer(isolate, mem_size, &mem_addr);
  if (!mem_addr) {
    // Not enough space for backing store of memory
    thrower.Error("Out of memory: wasm memory");
    return MaybeHandle<JSObject>();
  }

  // Load initialized data segments.
  LoadDataSegments(this, mem_addr, mem_size);

  module->SetInternalField(kWasmMemArrayBuffer, *mem_buffer);

  if (mem_export) {
    // Export the memory as a named property.
    Handle<String> name = factory->InternalizeUtf8String("memory");
    JSObject::AddProperty(module, name, mem_buffer, READ_ONLY);
  }

  //-------------------------------------------------------------------------
  // Allocate the globals area if necessary.
  //-------------------------------------------------------------------------
  size_t globals_size = ComputeGlobalsSize(globals);
  byte* globals_addr = nullptr;
  if (globals_size > 0) {
    Handle<JSArrayBuffer> globals_buffer =
        NewArrayBuffer(isolate, mem_size, &globals_addr);
    if (!globals_addr) {
      // Not enough space for backing store of globals.
      thrower.Error("Out of memory: wasm globals");
      return MaybeHandle<JSObject>();
    }

    module->SetInternalField(kWasmGlobalsArrayBuffer, *globals_buffer);
  } else {
    module->SetInternalField(kWasmGlobalsArrayBuffer, Smi::FromInt(0));
  }

  //-------------------------------------------------------------------------
  // Compile all functions in the module.
  //-------------------------------------------------------------------------
  int index = 0;
  WasmLinker linker(isolate, functions->size());
  ModuleEnv module_env;
  module_env.module = this;
  module_env.mem_start = reinterpret_cast<uintptr_t>(mem_addr);
  module_env.mem_end = reinterpret_cast<uintptr_t>(mem_addr) + mem_size;
  module_env.globals_area = reinterpret_cast<uintptr_t>(globals_addr);
  module_env.linker = &linker;
  module_env.function_code = nullptr;

  // First pass: compile each function and initialize the code table.
  for (const WasmFunction& func : *functions) {
    if (thrower.error()) break;

    const char* cstr = GetName(func.name_offset);
    Handle<String> name = factory->InternalizeUtf8String(cstr);
    Handle<Code> code = Handle<Code>::null();
    Handle<JSFunction> function = Handle<JSFunction>::null();
    if (func.external) {
      // Lookup external function in FFI object.
      if (!ffi.is_null()) {
        MaybeHandle<Object> result = Object::GetProperty(ffi, name);
        if (!result.is_null()) {
          Handle<Object> obj = result.ToHandleChecked();
          if (obj->IsJSFunction()) {
            function = Handle<JSFunction>::cast(obj);
            code =
                CompileWasmToJSWrapper(isolate, &module_env, function, index);
          } else {
            thrower.Error("FFI function #%d:%s is not a JSFunction.", index,
                          cstr);
            return MaybeHandle<JSObject>();
          }
        } else {
          thrower.Error("FFI function #%d:%s not found.", index, cstr);
          return MaybeHandle<JSObject>();
        }
      } else {
        thrower.Error("FFI table is not an object.");
        return MaybeHandle<JSObject>();
      }
    } else {
      // Compile the function.
      code = CompileFunction(thrower, isolate, &module_env, func, index);
      if (code.is_null()) {
        thrower.Error("Compilation of #%d:%s failed.", index, cstr);
        return MaybeHandle<JSObject>();
      }
      if (func.exported) {
        function =
            CompileJSToWasmWrapper(isolate, &module_env, name, code, index);
      }
    }
    if (!code.is_null()) {
      // Install the code into the linker table.
      linker.Finish(index, code);
      code_table->set(index, *code);
    }
    if (func.exported) {
      // Exported functions are installed as read-only properties on the module.
      JSObject::AddProperty(module, name, function, READ_ONLY);
    }
    index++;
  }

  // Second pass: patch all direct call sites.
  linker.Link();

  module->SetInternalField(kWasmModuleFunctionTable, Smi::FromInt(0));
  module->SetInternalField(kWasmModuleCodeTable, *code_table);
  return module;
}


Handle<Code> ModuleEnv::GetFunctionCode(uint32_t index) {
  DCHECK(IsValidFunction(index));
  if (linker) return linker->GetFunctionCode(index);
  if (function_code) return function_code->at(index);
  return Handle<Code>::null();
}


compiler::CallDescriptor* ModuleEnv::GetCallDescriptor(Zone* zone,
                                                       uint32_t index) {
  DCHECK(IsValidFunction(index));
  // Always make a direct call to whatever is in the table at that location.
  // A wrapper will be generated for FFI calls.
  WasmFunction* function = &module->functions->at(index);
  return GetWasmCallDescriptor(zone, function->sig);
}


// Helpers for nice error messages.
class ModuleError : public ModuleResult {
 public:
  explicit ModuleError(const char* msg) {
    error_code = kError;
    size_t len = strlen(msg) + 1;
    char* result = new char[len];
    strncpy(result, msg, len);
    result[len - 1] = 0;
    error_msg.Reset(result);
  }
};


// Helpers for nice error messages.
class FunctionError : public FunctionResult {
 public:
  explicit FunctionError(const char* msg) {
    error_code = kError;
    size_t len = strlen(msg) + 1;
    char* result = new char[len];
    strncpy(result, msg, len);
    result[len - 1] = 0;
    error_msg.Reset(result);
  }
};


ModuleResult DecodeWasmModule(Isolate* isolate, Zone* zone,
                              const byte* module_start, const byte* module_end,
                              bool verify_functions) {
  size_t size = module_end - module_start;
  if (module_start > module_end) return ModuleError("start > end");
  if (size < kMinModuleSize) return ModuleError("size < minimum module size");
  if (size >= kMaxModuleSize) return ModuleError("size > maximum module size");
  WasmModule* module = new WasmModule();
  ModuleDecoder decoder(zone, module_start, module_end);
  return decoder.DecodeModule(module, verify_functions);
}


FunctionSig* DecodeFunctionSignatureForTesting(Zone* zone, const byte* start,
                                               const byte* end) {
  ModuleDecoder decoder(zone, start, end);
  return decoder.DecodeFunctionSignature(start);
}


FunctionResult DecodeWasmFunction(Isolate* isolate, Zone* zone,
                                  ModuleEnv* module_env,
                                  const byte* function_start,
                                  const byte* function_end) {
  size_t size = function_end - function_start;
  if (function_start > function_end) return FunctionError("start > end");
  if (size > kMaxFunctionSize)
    return FunctionError("size > maximum function size");
  WasmFunction* function = new WasmFunction();
  ModuleDecoder decoder(zone, function_start, function_end);
  return decoder.DecodeSingleFunction(module_env, function);
}


int32_t CompileAndRunWasmModule(Isolate* isolate, const byte* module_start,
                                const byte* module_end) {
  HandleScope scope(isolate);
  Zone zone;
  // Decode the module, but don't verify function bodies, since we'll
  // be compiling them anyway.
  ModuleResult result =
      DecodeWasmModule(isolate, &zone, module_start, module_end, false);
  if (result.failed()) {
    // Module verification failed. throw.
    std::ostringstream str;
    str << "WASM.compileRun() failed: " << result;
    isolate->Throw(
        *isolate->factory()->NewStringFromAsciiChecked(str.str().c_str()));
    return -1;
  }

  int32_t retval = CompileAndRunWasmModule(isolate, result.val);
  delete result.val;
  return retval;
}


int32_t CompileAndRunWasmModule(Isolate* isolate, WasmModule* module) {
  ErrorThrower thrower(isolate, "CompileAndRunWasmModule");

  // Allocate temporary linear memory and globals.
  size_t mem_size = 1 << module->mem_size_log2;
  size_t globals_size = AllocateGlobalsOffsets(module->globals);

  base::SmartArrayPointer<byte> mem_addr(new byte[mem_size]);
  base::SmartArrayPointer<byte> globals_addr(new byte[globals_size]);

  memset(mem_addr.get(), 0, mem_size);
  memset(globals_addr.get(), 0, globals_size);

  // Create module environment.
  WasmLinker linker(isolate, module->functions->size());
  ModuleEnv module_env;
  module_env.module = module;
  module_env.mem_start = reinterpret_cast<uintptr_t>(mem_addr.get());
  module_env.mem_end = reinterpret_cast<uintptr_t>(mem_addr.get()) + mem_size;
  module_env.globals_area = reinterpret_cast<uintptr_t>(globals_addr.get());
  module_env.linker = &linker;
  module_env.function_code = nullptr;

  // Load data segments.
  // TODO(titzer): throw instead of crashing if segments don't fit in memory?
  LoadDataSegments(module, mem_addr.get(), mem_size);

  // Compile all functions.
  Handle<Code> main_code = Handle<Code>::null();  // record last code.
  int index = 0;
  for (const WasmFunction& func : *module->functions) {
    if (!func.external) {
      // Compile the function and install it in the code table.
      Handle<Code> code =
          CompileFunction(thrower, isolate, &module_env, func, index);
      if (!code.is_null()) {
        if (func.exported) main_code = code;
        linker.Finish(index, code);
      }
      if (thrower.error()) return -1;
    }
    index++;
  }

  if (!main_code.is_null()) {
    linker.Link();
#if USE_SIMULATOR && V8_TARGET_ARCH_ARM64
    // Run the main code on arm64 simulator.
    Simulator* simulator = Simulator::current(isolate);
    Simulator::CallArgument args[] = {Simulator::CallArgument(0),
                                      Simulator::CallArgument::End()};
    return static_cast<int32_t>(simulator->CallInt64(main_code->entry(), args));
#elif USE_SIMULATOR
    // Run the main code on simulator.
    Simulator* simulator = Simulator::current(isolate);
    return static_cast<int32_t>(
        simulator->Call(main_code->entry(), 4, 0, 0, 0, 0));
#else
    // Run the main code as raw machine code.
    int32_t (*raw_func)() = reinterpret_cast<int (*)()>(main_code->entry());
    return raw_func();
#endif
  } else {
    // No main code was found.
    isolate->Throw(*isolate->factory()->NewStringFromStaticChars(
        "WASM.compileRun() failed: no valid main code produced."));
  }
  return -1;
}
}
}
}
