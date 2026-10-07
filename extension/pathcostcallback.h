#ifndef CBASENPC_PATH_COST_CALLBACK_H
#define CBASENPC_PATH_COST_CALLBACK_H
#include <sp_vm_api.h>

// A legitimate callback cost of zero/negative is preserved. Unavailable or
// failed callbacks must instead let the caller calculate its usual fallback.
inline bool CBaseNPCTryPathCost(SourcePawn::IPluginFunction* callback,
                               const sp::CallArgs& args, float& cost)
{
  if (!callback || !callback->IsRunnable()) return false;
  cell_t result = 0;
  try {
    if (!callback->Invoke(args, &result)) return false;
  } catch (...) { return false; }
  cost = sp_ctof(result);
  return true;
}
#endif
