#include "cliententitymanager.h"
#include "cliententitynatives.h"

#include <cstdint>

namespace
{
using namespace CBaseNPCClientNatives;

cell_t Native_IsAvailable(IPluginContext *, const cell_t *)
{
  return g_ClientEntityManager.IsAvailable();
}

cell_t Native_GetHandleRef(IPluginContext *, const cell_t *params)
{
  return g_ClientEntityManager.EntityToClientHandleRef(
    g_ClientEntityManager.ResolveClientEntityRef(params[1]));
}

cell_t Native_GetEntityAddressClient(IPluginContext *context, const cell_t *params)
{
  const cell_t shift = UsesInt64Address(context) ? 1 : 0;
  const int clientRef = params[shift + 1];
  void *entity = g_ClientEntityManager.ResolveClientEntityRef(clientRef);
  if (!entity)
    return context->ThrowNativeError("Client entity reference 0x%08X is invalid", clientRef);

  const uintptr_t address = reinterpret_cast<uintptr_t>(entity);
  if (shift)
  {
    cell_t *output = nullptr;
    const int error = context->LocalToPhysAddr(params[1], &output);
    if (error != SP_ERROR_NONE)
      return context->ThrowNativeErrorEx(error, "Could not write Address return value");
    *reinterpret_cast<int64_t *>(output) = static_cast<int64_t>(address);
  }
  return static_cast<cell_t>(address);
}

cell_t Native_GetEntityRefFromAddressClient(IPluginContext *context, const cell_t *params)
{
  void *address = nullptr;
  if (!ReadAddressArgument(context, params[1], &address))
    return -1;

  if (!address)
    return -1;
  return g_ClientEntityManager.EntityToClientRef(address);
}

cell_t Native_GetEntityCountClient(IPluginContext *, const cell_t *)
{
  return g_ClientEntityManager.GetClientEntityCount();
}

cell_t Native_GetEntityRefByOrdinalClient(IPluginContext *context, const cell_t *params)
{
  const int clientRef = g_ClientEntityManager.GetEntityRefByOrdinalClient(params[1]);
  if (clientRef == -1)
    return context->ThrowNativeError("Client entity ordinal %d is invalid", params[1]);
  return clientRef;
}

cell_t Native_LoadEntityFromHandleAddressClient(IPluginContext *context, const cell_t *params)
{
  void *address = nullptr;
  if (!ReadAddressArgument(context, params[1], &address))
    return 0;

  if (!address)
    return context->ThrowNativeError("Address cannot be null");
  if (reinterpret_cast<uintptr_t>(address) < kMinimumValidAddress)
  {
    return context->ThrowNativeError(
      "Invalid address %p is pointing to reserved memory", address);
  }

  return g_ClientEntityManager.ClientHandleToEntityRef(address);
}

cell_t Native_IsValidEntityClient(IPluginContext *, const cell_t *params)
{
  return g_ClientEntityManager.ResolveClientEntityRef(params[1]) ? 1 : 0;
}

cell_t Native_EntIndexToEntRefClient(IPluginContext *, const cell_t *params)
{
  return g_ClientEntityManager.EntIndexToEntRefClient(params[1]);
}

cell_t Native_EntRefToEntIndexClient(IPluginContext *, const cell_t *params)
{
  const int clientRef = params[1];
  if (!g_ClientEntityManager.ResolveClientEntityRef(clientRef))
    return -1;

  const int entIndex = g_ClientEntityManager.EntRefToEntIndexClient(clientRef);
  if (entIndex == -1 && params[2])
  {
    g_pSM->LogMessage(myself,
      "Client entity 0x%08X is client-only and has no server entindex", clientRef);
  }
  return entIndex;
}

cell_t Native_GetEntityClassnameClient(IPluginContext *context, const cell_t *params)
{
  const char *classname = g_ClientEntityManager.GetEntityClassnameClient(params[1]);
  const char *output = classname ? classname : "";
  const int error = context->StringToLocalUTF8(params[2], params[3], output, nullptr);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not write classname");
  return classname ? 1 : 0;
}

cell_t Native_GetEntityClassnameDiagnosticsClient(IPluginContext *context,
                                                   const cell_t *params)
{
  int entIndex = -1;
  const char *networkName = nullptr;
  const char *mapClassname = nullptr;
  const char *clientClassname = nullptr;
  const char *serverClassname = nullptr;
  const char *classname = nullptr;
  if (!g_ClientEntityManager.GetEntityClassnameDiagnostics(
        params[1], &entIndex, &networkName, &mapClassname,
        &clientClassname, &serverClassname, &classname))
  {
    return 0;
  }

  cell_t *entIndexOutput = nullptr;
  int error = context->LocalToPhysAddr(params[2], &entIndexOutput);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not write client entindex");
  *entIndexOutput = entIndex;

  const char *outputs[] = {
    networkName, mapClassname, clientClassname, serverClassname, classname
  };
  for (size_t i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i)
  {
    const size_t parameter = 3 + i * 2;
    error = context->StringToLocalUTF8(
      params[parameter], params[parameter + 1], outputs[i] ? outputs[i] : "", nullptr);
    if (error != SP_ERROR_NONE)
      return context->ThrowNativeErrorEx(error, "Could not write classname diagnostics");
  }
  return 1;
}

cell_t Native_FindEntityByClassnameClient(IPluginContext *context, const cell_t *params)
{
  const int startRef = params[1];
  if (startRef != -1 && !g_ClientEntityManager.ResolveClientEntityRef(startRef))
    return context->ThrowNativeError("Client start entity reference 0x%08X is invalid", startRef);

  char *classname = nullptr;
  const int error = context->LocalToString(params[2], &classname);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not read classname");

  return g_ClientEntityManager.FindClientEntityByClassname(startRef, classname);
}

}

void natives::setupClientEntityNatives(std::vector<sp_nativeinfo_t> &natives)
{
  const sp_nativeinfo_t list[] = {
    {"CClientEntity.GetAddress", Native_GetEntityAddressClient},
    {"CClientEntityManager.FromAddress", Native_GetEntityRefFromAddressClient},
    {"CClientEntityManager.GetCount", Native_GetEntityCountClient},
    {"CClientEntityManager.GetByOrdinal", Native_GetEntityRefByOrdinalClient},
    {"CClientEntityManager.FromHandleAddress", Native_LoadEntityFromHandleAddressClient},
    {"CClientEntity.IsValid", Native_IsValidEntityClient},
    {"CClientEntityManager.FromEntIndex", Native_EntIndexToEntRefClient},
    {"CClientEntity.GetEntIndex", Native_EntRefToEntIndexClient},
    {"CClientEntity.GetClassname", Native_GetEntityClassnameClient},
    {"CClientEntity.GetClassnameDiagnostics", Native_GetEntityClassnameDiagnosticsClient},
    {"CClientEntityManager.FindByClassname", Native_FindEntityByClassnameClient},
    {"CClientEntityManager.IsAvailable", Native_IsAvailable},
    {"CClientEntity.GetHandleRef", Native_GetHandleRef}
  };
  natives.insert(natives.end(), std::begin(list), std::end(list));
  setupClientEntityPropertyNatives(natives);
}
