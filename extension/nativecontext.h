#ifndef CBASENPC_NATIVE_CONTEXT_H
#define CBASENPC_NATIVE_CONTEXT_H
#include "shared/ICBaseNPCShared.h"
#include <memory>
#include <vector>
#include <string>
#include <stdexcept>
#include <set>

// A narrow adapter for CBaseNPC's existing native entry points. It never runs
// bytecode, impersonates a SourceMod plugin, or enters the SourcePawn VM.
class CBaseNPCNativeContext final : public SourcePawn::IPluginRuntime
{
public:
  struct Buffer {
    cell_t local = 0;
    std::vector<cell_t> storage;
    void* destination = nullptr;
    size_t bytes = 0;
    bool copyback = false;
  };
  struct Frame {
    sp::CallArgs args;
    cell_t params[SP_MAX_EXEC_PARAMS + 1] = {};
    std::vector<Buffer> buffers;
    void Prepare(const sp::CallArgs& input);
    void CopyBack();
  };
  explicit CBaseNPCNativeContext(SourceMod::IdentityToken_t* owner);
  ~CBaseNPCNativeContext() override;
  // Frame swapping makes callback -> native reentry independent of its caller.
  Frame* SetFrame(Frame* frame) { auto old = frame_; frame_ = frame; return old; }
  cell_t RegisterCallback(SourceMod::ICBaseNPCCallback* callback);
  bool RemoveCallback(cell_t id);
  void Deactivate();
  bool InvokeCallback(cell_t id, const sp::CallArgs& args, cell_t* result);
  unsigned ActiveCalls() const { return active_; }
  void EnterCall() { ++active_; }
  void LeaveCall() { --active_; }
  bool Int64Address() const { return sizeof(void*) > sizeof(cell_t); }

  int GetPubvarByIndex(uint32_t, sp_pubvar_t**) override { return SP_ERROR_NOT_FOUND; }
  int FindPubvarByName(const char* name, uint32_t* index) override;
  int GetPubvarAddrs(uint32_t, cell_t*, cell_t**) override { return SP_ERROR_NOT_FOUND; }
  uint32_t GetPubVarsNum() override { return 0; }
  SourcePawn::IPluginFunction* GetFunctionByName(const char*) override { return nullptr; }
  SourcePawn::IPluginFunction* GetFunctionById(funcid_t id) override;
  bool IsDebugging() override { return false; }
  bool IsPaused() override { return !available_; }
  size_t GetMemUsage() override { return 0; }
  const char* GetFilename() override { return "CBaseNPC C++ consumer"; }
  bool UsesDirectArrays() override { return false; }
  int LocalToPhysAddr(cell_t local, cell_t** out) override;
  int LocalToString(cell_t local, char** out) override;
  int StringToLocal(cell_t local, size_t bytes, const char* source) override;
  int StringToLocalUTF8(cell_t local, size_t bytes, const char* source, size_t* written) override;
  cell_t ThrowNativeErrorEx(int code, const char* fmt, ...) override;
  cell_t ThrowNativeError(const char* fmt, ...) override;
  cell_t* GetNullRef(SourcePawn::SP_NULL_TYPE type) override;
  int LocalToStringNULL(cell_t local, char** out) override;
  bool IsInExec() override { return active_ != 0; }
  SourcePawn::IPluginRuntime* GetRuntime() override { return this; }
  int GetLastNativeError() override { return error_; }
  bool GetKey(int key, void** value) override;
  [[noreturn]] void ReportError(const char* fmt, ...) override;
  [[noreturn]] void ReportErrorVA(const char* fmt, va_list ap) override;
  [[noreturn]] void ReportFatalError(const char* fmt, ...) override;
  [[noreturn]] void ReportFatalErrorVA(const char* fmt, va_list ap) override { ReportErrorVA(fmt, ap); }
  [[noreturn]] void ReportErrorNumber(int code) override;
  cell_t BlamePluginError(SourcePawn::IPluginFunction*, const char* fmt, ...) override;
  SourcePawn::IFrameIterator* CreateFrameIterator() override { return nullptr; }
  void DestroyFrameIterator(SourcePawn::IFrameIterator*) override {}
  bool HeapAlloc2dArray(unsigned int, unsigned int, cell_t*, const cell_t*) override;
  void EnterHeapScope() override {}
  void LeaveHeapScope() override {}
  cell_t GetNullFunctionValue() override { return 0; }
  bool IsNullFunctionId(funcid_t id) override { return id == 0 || id == static_cast<funcid_t>(-1); }
  bool GetFunctionByIdOrNull(funcid_t id, SourcePawn::IPluginFunction** out) override;
  SourcePawn::IPluginFunction* GetFunctionByIdOrError(funcid_t id) override;
  int LocalToArrayPtr(cell_t, SourcePawn::ARRAY_PTR*) override;
  void* GetArrayData(SourcePawn::ARRAY_PTR, uint32_t*) override;
  sp::BaseRuntime* GetBaseRuntime() override { return nullptr; }
  SourcePawn::ISourcePawnEnvironment* GetEnvironment() override { return nullptr; }
private:
  class Function;
  Buffer& FindBuffer(cell_t local, size_t bytes, size_t& offset);
  SourceMod::IdentityToken_t* owner_;
  Frame* frame_ = nullptr;
  std::vector<std::unique_ptr<Function>> functions_;
  // A few server natives retain reason/name pointers past the immediate call
  // (notably NextBot action results). Match plugin memory lifetime for input
  // strings, instead of handing them a pointer into a temporary call frame.
  std::set<std::string> strings_;
  cell_t nullVector_[3] = {}, nullString_ = 0;
  unsigned active_ = 0;
  int error_ = SP_ERROR_NONE;
  bool available_ = true;
};
#endif
