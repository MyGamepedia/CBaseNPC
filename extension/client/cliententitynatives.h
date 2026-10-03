#ifndef CBASENPC_CLIENT_ENTITY_NATIVES_H
#define CBASENPC_CLIENT_ENTITY_NATIVES_H
#include "smsdk_ext.h"
#include <cstdint>
#include <vector>
namespace CBaseNPCClientNatives
{
constexpr uintptr_t kMinimumValidAddress = 0x10000;

inline bool UsesInt64Address(IPluginContext *context)
{
  return context->GetRuntime()->FindPubvarByName("__Int64_Address__", nullptr) == SP_ERROR_NONE;
}

inline bool ReadAddressArgument(IPluginContext *context, cell_t parameter, void **address)
{
  *address = reinterpret_cast<void *>(static_cast<uintptr_t>(
    static_cast<ucell_t>(parameter)));

  if (!UsesInt64Address(context))
    return true;

  cell_t *sourcePawnAddress = nullptr;
  const int error = context->LocalToPhysAddr(parameter, &sourcePawnAddress);
  if (error != SP_ERROR_NONE)
  {
    context->ThrowNativeErrorEx(error, "Could not read Address argument");
    return false;
  }

  const int64_t value = *reinterpret_cast<int64_t *>(sourcePawnAddress);
  *address = reinterpret_cast<void *>(static_cast<uintptr_t>(value));
  return true;
}


}
namespace natives
{
void setupClientEntityNatives(std::vector<sp_nativeinfo_t> &natives);
void setupClientEntityPropertyNatives(std::vector<sp_nativeinfo_t> &natives);
}
#endif
