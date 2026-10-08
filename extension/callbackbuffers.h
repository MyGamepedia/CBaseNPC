#ifndef CBASENPC_CALLBACK_BUFFERS_H
#define CBASENPC_CALLBACK_BUFFERS_H
#include <sp_vm_api.h>
#include <algorithm>
#include <initializer_list>
#include <memory>
#include <vector>

// PushArray borrows its buffer until Execute/Invoke. Keep each read-only array
// in invocation-local storage, including multiple vectors in the same callback.
class CBaseNPCCallbackBuffers
{
public:
  void PushArray(SourcePawn::IPluginFunction* callback, std::initializer_list<cell_t> values) {
    std::unique_ptr<cell_t[]> buffer(new cell_t[values.size()]);
    std::copy(values.begin(), values.end(), buffer.get());
    buffers_.push_back(std::move(buffer));
    callback->PushArray(buffers_.back().get(), static_cast<unsigned>(values.size()), 0);
  }
private:
  std::vector<std::unique_ptr<cell_t[]>> buffers_;
};
#endif
