#include "nativecontext.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>

using namespace SourcePawn;
namespace {
// Never reuse a callback ID in another consumer or after its release.
int64_t nextFunction = 1;
constexpr cell_t NullVector = 4, NullString = 16, FirstBuffer = 32;
constexpr size_t MaxBuffer = 16 * 1024 * 1024;
}

void CBaseNPCNativeContext::Frame::Prepare(const sp::CallArgs& input)
{
  if (input.error || input.argc > SP_MAX_EXEC_PARAMS)
    throw std::runtime_error("Invalid native argument list");
  args = input;
  params[0] = input.argc;
  buffers.reserve(input.argc);
  int64_t nextLocal = FirstBuffer;
  for (unsigned i = 0; i < input.argc; ++i) {
    auto& arg = args.argv[i];
    if (arg.type == sp::CallArgs::ARG_CELL) { params[i + 1] = arg.u.value; continue; }
    Buffer buffer;
    bool copy = true;
    switch (arg.type) {
      case sp::CallArgs::ARG_CELL_BY_REF: buffer.bytes = sizeof(cell_t); break;
      case sp::CallArgs::ARG_ARRAY:
        if (arg.array_size > MaxBuffer / sizeof(cell_t))
          throw std::runtime_error("Native argument buffer is too large");
        buffer.bytes = size_t(arg.array_size) * sizeof(cell_t);
        break;
      case sp::CallArgs::ARG_CHAR_ARRAY:
        buffer.bytes = arg.array_size;
        copy = (arg.flags & SM_PARAM_STRING_COPY) != 0;
        break;
      case sp::CallArgs::ARG_INT64: buffer.bytes = sizeof(int64_t); break;
      default: throw std::runtime_error("Unsupported native argument type");
    }
    if (buffer.bytes > MaxBuffer || nextLocal + buffer.bytes + 4 > INT32_MAX)
      throw std::runtime_error("Native argument buffer is too large");
    if (arg.type != sp::CallArgs::ARG_INT64 && !arg.u.addr) {
      params[i + 1] = arg.type == sp::CallArgs::ARG_CHAR_ARRAY ? NullString : NullVector;
      continue;
    }
    buffer.local = cell_t(nextLocal);
    buffer.storage.resize((buffer.bytes + sizeof(cell_t) - 1) / sizeof(cell_t) + 1);
    buffer.destination = arg.type == sp::CallArgs::ARG_INT64 ? nullptr : arg.u.addr;
    buffer.copyback = buffer.destination && (arg.flags & SM_PARAM_COPYBACK);
    if (arg.type == sp::CallArgs::ARG_INT64)
      std::memcpy(buffer.storage.data(), &arg.u.i64, sizeof(int64_t));
    else if (copy && buffer.bytes)
      std::memcpy(buffer.storage.data(), arg.u.addr, buffer.bytes);
    if (arg.type != sp::CallArgs::ARG_INT64) arg.u.addr = buffer.storage.data();
    params[i + 1] = buffer.local;
    nextLocal += int64_t(buffer.storage.size() * sizeof(cell_t));
    buffers.push_back(std::move(buffer));
  }
}
void CBaseNPCNativeContext::Frame::CopyBack()
{
  for (const auto& buffer : buffers)
    if (buffer.copyback && buffer.bytes)
      std::memcpy(buffer.destination, buffer.storage.data(), buffer.bytes);
}

class CBaseNPCNativeContext::Function final : public IPluginFunction
{
public:
  Function(CBaseNPCNativeContext& context, funcid_t id, SourceMod::ICBaseNPCCallback* target)
    : context_(context), id_(id), target_(target) {}
  bool Invoke(const sp::CallArgs& args, cell_t* result) override {
    if (result) *result = 0;
    if (!IsRunnable()) return false;
    // Engine callbacks also use Invoke directly, outside InvokeNative. Never
    // let an extension exception cross the engine/SourceHook boundary.
    try {
      Frame frame;
      frame.Prepare(args);
      context_.EnterCall();
      struct Guard { CBaseNPCNativeContext& c; ~Guard() { c.LeaveCall(); } } guard{context_};
      cell_t value = 0;
      bool success = target_->Invoke(frame.args, &value);
      if (success) { frame.CopyBack(); if (result) *result = value; }
      return success;
    } catch (...) { return false; }
  }
  int Execute(cell_t* result) override {
    sp::CallArgs args = pending_; pending_.Reset();
    try { return Invoke(args, result) ? SP_ERROR_NONE : SP_ERROR_NATIVE; }
    catch (...) { return SP_ERROR_NATIVE; }
  }
  bool Invoke(cell_t* result) override { return Execute(result) == SP_ERROR_NONE; }
  IPluginContext* GetParentContext() override { return &context_; }
  IPluginRuntime* GetParentRuntime() override { return &context_; }
  bool IsRunnable() override { return target_ && !context_.IsPaused(); }
  funcid_t GetFunctionID() override { return id_; }
  const char* DebugName() override { return "CBaseNPC C++ callback"; }
  void Cancel() override { pending_.Reset(); }
  int PushCell(cell_t value) override { pending_.PushCell(value); return Status(); }
  int PushCellByRef(cell_t* value, int flags) override { pending_.PushCellByRef(value, flags); return Status(); }
  int PushFloat(float value) override { pending_.PushFloat(value); return Status(); }
  int PushFloatByRef(float* value, int flags) override { pending_.PushFloatByRef(value, flags); return Status(); }
  int PushArray(cell_t* value, unsigned count, int flags) override { pending_.PushArray(value, count, flags); return Status(); }
  int PushString(const char* value) override { pending_.PushString(value ? value : ""); return Status(); }
  int PushStringEx(char* value, size_t length, int stringFlags, int copyFlags) override {
    pending_.PushString(value, length, stringFlags | copyFlags); return Status();
  }
  int PushInt64(int64_t value) override { pending_.PushInt64(value); return Status(); }
  void Detach() { target_ = nullptr; Cancel(); }
private:
  int Status() const { return pending_.error ? SP_ERROR_PARAMS_MAX : SP_ERROR_NONE; }
  CBaseNPCNativeContext& context_;
  funcid_t id_;
  SourceMod::ICBaseNPCCallback* target_;
  sp::CallArgs pending_;
};

CBaseNPCNativeContext::CBaseNPCNativeContext(SourceMod::IdentityToken_t* owner) : owner_(owner) {}
CBaseNPCNativeContext::~CBaseNPCNativeContext() = default;
cell_t CBaseNPCNativeContext::RegisterCallback(SourceMod::ICBaseNPCCallback* callback)
{
  if (!available_ || !callback || nextFunction > INT32_MAX) return 0;
  cell_t id = cell_t(nextFunction++);
  functions_.emplace_back(new Function(*this, id, callback));
  return id;
}
bool CBaseNPCNativeContext::RemoveCallback(cell_t id)
{
  for (auto& function : functions_) if (function->GetFunctionID() == funcid_t(id)) {
    function->Detach(); return true;
  }
  return false;
}
void CBaseNPCNativeContext::Deactivate()
{
  available_ = false;
  for (auto& function : functions_) function->Detach();
}
bool CBaseNPCNativeContext::InvokeCallback(cell_t id, const sp::CallArgs& args, cell_t* result)
{
  try { auto function = GetFunctionById(id); return function && function->Invoke(args, result); }
  catch (...) { return false; }
}
IPluginFunction* CBaseNPCNativeContext::GetFunctionById(funcid_t id)
{
  if (IsNullFunctionId(id)) return nullptr;
  for (auto& function : functions_) if (function->GetFunctionID() == id) return function.get();
  ThrowNativeError("Callback ID %u does not belong to this CBaseNPC consumer", id);
}
bool CBaseNPCNativeContext::GetFunctionByIdOrNull(funcid_t id, IPluginFunction** out)
{ *out = GetFunctionById(id); return true; }
IPluginFunction* CBaseNPCNativeContext::GetFunctionByIdOrError(funcid_t id)
{
  auto function = GetFunctionById(id);
  if (!function) ThrowNativeError("A non-null C++ callback is required");
  return function;
}
int CBaseNPCNativeContext::FindPubvarByName(const char* name, uint32_t* index)
{
  if (Int64Address() && name && !std::strcmp(name, "__Int64_Address__")) {
    if (index) *index = 0;
    return SP_ERROR_NONE;
  }
  return SP_ERROR_NOT_FOUND;
}
CBaseNPCNativeContext::Buffer& CBaseNPCNativeContext::FindBuffer(cell_t local, size_t bytes, size_t& offset)
{
  if (frame_) for (auto& buffer : frame_->buffers) {
    if (local < buffer.local) continue;
    offset = size_t(local - buffer.local);
    if (offset <= buffer.bytes && bytes <= buffer.bytes - offset) return buffer;
  }
  ThrowNativeError("Invalid C++ native buffer reference %d (size %u)", local, unsigned(bytes));
}
int CBaseNPCNativeContext::LocalToPhysAddr(cell_t local, cell_t** out)
{
  if (!out) ThrowNativeError("Null buffer output pointer");
  if (local == NullVector) { *out = nullVector_; return SP_ERROR_NONE; }
  if (local == NullString) { *out = &nullString_; return SP_ERROR_NONE; }
  if (local % sizeof(cell_t)) ThrowNativeError("Unaligned cell buffer reference");
  size_t offset;
  auto& buffer = FindBuffer(local, sizeof(cell_t), offset);
  *out = reinterpret_cast<cell_t*>(reinterpret_cast<unsigned char*>(buffer.storage.data()) + offset);
  return SP_ERROR_NONE;
}
int CBaseNPCNativeContext::LocalToString(cell_t local, char** out)
{
  if (!out) ThrowNativeError("Null string output pointer");
  if (local == NullString) { *out = reinterpret_cast<char*>(&nullString_); return SP_ERROR_NONE; }
  size_t offset;
  auto& buffer = FindBuffer(local, 1, offset);
  *out = reinterpret_cast<char*>(buffer.storage.data()) + offset;
  if (!std::memchr(*out, '\0', buffer.bytes - offset)) ThrowNativeError("Input string has no terminator");
  return SP_ERROR_NONE;
}
int CBaseNPCNativeContext::LocalToStringNULL(cell_t local, char** out)
{
  if (local == NullString) { *out = nullptr; return SP_ERROR_NONE; }
  return LocalToString(local, out);
}
int CBaseNPCNativeContext::StringToLocal(cell_t local, size_t bytes, const char* source)
{
  if (!bytes) return SP_ERROR_NONE;
  size_t offset;
  auto& buffer = FindBuffer(local, bytes, offset);
  char* out = reinterpret_cast<char*>(buffer.storage.data()) + offset;
  source = source ? source : "";
  size_t count = std::min(bytes - 1, std::strlen(source));
  std::memcpy(out, source, count); out[count] = '\0';
  return SP_ERROR_NONE;
}
int CBaseNPCNativeContext::StringToLocalUTF8(cell_t local, size_t bytes, const char* source, size_t* written)
{
  if (written) *written = 0;
  if (!bytes) return SP_ERROR_NONE;
  size_t offset;
  auto& buffer = FindBuffer(local, bytes, offset);
  char* out = reinterpret_cast<char*>(buffer.storage.data()) + offset;
  source = source ? source : "";
  size_t count = std::min(bytes - 1, std::strlen(source));
  // If truncated, do not leave a partial UTF-8 codepoint in the destination.
  if (source[count]) while (count && (static_cast<unsigned char>(source[count]) & 0xc0) == 0x80) --count;
  std::memcpy(out, source, count); out[count] = '\0';
  if (written) *written = count;
  return SP_ERROR_NONE;
}
cell_t* CBaseNPCNativeContext::GetNullRef(SP_NULL_TYPE type)
{ return type == SP_NULL_VECTOR ? nullVector_ : &nullString_; }
bool CBaseNPCNativeContext::GetKey(int key, void** value)
{ if (key != 1 || !value) return false; *value = owner_; return true; }
void CBaseNPCNativeContext::ReportErrorVA(const char* fmt, va_list ap)
{
  char message[2048];
  std::vsnprintf(message, sizeof(message), fmt ? fmt : "CBaseNPC native error", ap);
  error_ = SP_ERROR_NATIVE;
  throw std::runtime_error(message);
}
#define CBASENPC_ERROR_METHOD(ret, name, signature, format) \
  ret CBaseNPCNativeContext::name signature { \
    va_list ap; va_start(ap, format); \
    try { ReportErrorVA(format, ap); } catch (...) { va_end(ap); throw; } \
  }
CBASENPC_ERROR_METHOD(cell_t, ThrowNativeError, (const char* fmt, ...), fmt)
CBASENPC_ERROR_METHOD(cell_t, ThrowNativeErrorEx, (int, const char* fmt, ...), fmt)
CBASENPC_ERROR_METHOD(void, ReportError, (const char* fmt, ...), fmt)
CBASENPC_ERROR_METHOD(void, ReportFatalError, (const char* fmt, ...), fmt)
CBASENPC_ERROR_METHOD(cell_t, BlamePluginError, (IPluginFunction*, const char* fmt, ...), fmt)
#undef CBASENPC_ERROR_METHOD
void CBaseNPCNativeContext::ReportErrorNumber(int code) { ThrowNativeError("Native error %d", code); }
bool CBaseNPCNativeContext::HeapAlloc2dArray(unsigned int, unsigned int, cell_t*, const cell_t*)
{ ThrowNativeError("VM heap allocation is unavailable in a C++ consumer"); }
int CBaseNPCNativeContext::LocalToArrayPtr(cell_t, ARRAY_PTR*)
{ ThrowNativeError("VM array handles are unavailable in a C++ consumer"); }
void* CBaseNPCNativeContext::GetArrayData(ARRAY_PTR, uint32_t*)
{ ThrowNativeError("VM array handles are unavailable in a C++ consumer"); }
