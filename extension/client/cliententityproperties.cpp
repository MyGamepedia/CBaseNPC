#include "cliententitymanager.h"
#include "cliententitynatives.h"
#include "helpers.h"
#include "shared/datamaplookup.h"
#include "sourcesdk/cbasenpcclientlookup.h"
#include "sourcesdk/cbasenpcrecvtable.h"
#include "sourcesdk/cbasenpcnetworkschema.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>

#include <mathlib/vector.h>

namespace
{
constexpr int kPropSend = 0;
constexpr int kPropData = 1;
constexpr int kMaximumEntityOffset = 32768;
using namespace CBaseNPCClientNatives;
using DataMapInfo = ClientDataMapInfo;
using RecvPropInfo = ClientRecvPropInfo;

struct ResolvedRecvProp
{
  RecvProp *prop = nullptr;
  int offset = 0;
  int elementStride = 0;
};

void *ResolveEntity(IPluginContext *context, int clientRef)
{
  void *entity = g_ClientEntityManager.ResolveClientEntityRef(clientRef);
  if (!entity)
  {
    context->ThrowNativeError("Client entity reference 0x%08X is invalid", clientRef);
    return nullptr;
  }
  return entity;
}

bool ValidateOffset(IPluginContext *context, int64_t offset, bool allowZero = false)
{
  if (offset < (allowZero ? 0 : 1) || offset > kMaximumEntityOffset)
  {
    context->ThrowNativeError("Offset %lld is invalid", static_cast<long long>(offset));
    return false;
  }
  return true;
}

bool ValidateIntegerSize(IPluginContext *context, int size)
{
  if (size == 1 || size == 2 || size == 4)
    return true;
  context->ThrowNativeError("Integer size %d is invalid", size);
  return false;
}

size_t CopyString(char *destination, size_t maxlength, const char *source)
{
  if (!destination || maxlength == 0)
    return 0;

  if (!source)
    source = "";

  const size_t sourceLength = std::strlen(source);
  const size_t copyLength = (std::min)(sourceLength, maxlength - 1);
  if (copyLength)
    std::memcpy(destination, source, copyLength);
  destination[copyLength] = '\0';
  return copyLength;
}

bool FindDataMapInfo(datamap_t *map, const char *name, DataMapInfo *result)
{
  return g_ClientEntityManager.GetProperties().FindDataMapInfo(map, name, result);
}

bool FindRecvPropInfo(RecvTable *table, const char *name, RecvPropInfo *result)
{
  return g_ClientEntityManager.GetProperties().FindRecvPropInfo(table, name, result);
}

datamap_t *GetDataMap(IPluginContext *context, void *entity)
{
  datamap_t *map = g_ClientEntityManager.GetClientDataMap(entity);
  if (!map)
    context->ThrowNativeError("Could not retrieve client datamap");
  return map;
}

bool GetRecvRoot(IPluginContext *context, void *entity,
                 ClientClass **clientClass, RecvTable **table, uint8_t **base)
{
  *clientClass = g_ClientEntityManager.GetClientClass(entity);
  if (!*clientClass || !(*clientClass)->m_pRecvTable)
  {
    context->ThrowNativeError("Could not retrieve client network class/RecvTable");
    return false;
  }

  *table = (*clientClass)->m_pRecvTable;
  *base = static_cast<uint8_t *>(
    g_ClientEntityManager.GetClientRecvTableBase(entity));
  if (!*base)
  {
    context->ThrowNativeError("Client networkable returned a null data-table base pointer");
    return false;
  }
  return true;
}

bool GetPropertyName(IPluginContext *context, cell_t parameter, char **name)
{
  const int error = context->LocalToString(parameter, name);
  if (error == SP_ERROR_NONE)
    return true;
  context->ThrowNativeErrorEx(error, "Could not read property name");
  return false;
}

bool FindDataProperty(IPluginContext *context, void *entity, int clientRef,
                      const char *name, DataMapInfo *info)
{
  datamap_t *map = GetDataMap(context, entity);
  if (!map)
    return false;

  if (FindDataMapInfo(map, name, info))
    return true;

  const char *classname = g_ClientEntityManager.GetEntityClassnameClient(clientRef);
  context->ThrowNativeError("Property \"%s\" not found (client entity 0x%08X/%s)",
                            name, clientRef, classname ? classname : "");
  return false;
}

bool FindRecvProperty(IPluginContext *context, void *entity, int clientRef,
                      const char *name, RecvPropInfo *info, uint8_t **base)
{
  ClientClass *clientClass = nullptr;
  RecvTable *table = nullptr;
  if (!GetRecvRoot(context, entity, &clientClass, &table, base))
    return false;

  if (FindRecvPropInfo(table, name, info))
  {
    if (!strcmp(info->prop->GetName(), CBASENPC_CLASSNAME_PROP)) {
      context->ThrowNativeError("The synthetic classname property has no memory offset; use classname diagnostics");
      return false;
    }
    if (info->sidecar) {
      const auto field = info->sidecar;
      *base = g_ClientEntityManager.GetSidecarAddress(entity, 0, field->offset + field->elementCount * field->stride);
      if (!*base) { context->ThrowNativeError("Client sidecar is unavailable or field exceeds its allocation"); return false; }
    }
    return true;
  }

  const char *classname = g_ClientEntityManager.GetEntityClassnameClient(clientRef);
  context->ThrowNativeError("RecvProp \"%s\" not found (client entity 0x%08X/%s, netclass %s)",
                            name, clientRef, classname ? classname : "",
                            clientClass->GetName() ? clientClass->GetName() : "");
  return false;
}

bool ResolveDataElement(IPluginContext *context, const char *name,
                        const DataMapInfo &info, int element, int *offset)
{
  const int count = info.prop->fieldSize;
  if (count <= 0 || element < 0 || element >= count)
  {
    context->ThrowNativeError("Element %d is out of bounds (Prop %s has %d elements)",
                              element, name, count);
    return false;
  }

  if (info.prop->fieldSizeInBytes <= 0 || info.prop->fieldSizeInBytes % count != 0)
  {
    context->ThrowNativeError("Data field %s has an invalid byte size (%d for %d elements)",
                              name, info.prop->fieldSizeInBytes, count);
    return false;
  }

  *offset = info.actualOffset + element * (info.prop->fieldSizeInBytes / count);
  return true;
}

bool ResolveRecvElement(IPluginContext *context, const char *name,
                        const RecvPropInfo &info, int element,
                        SendPropType expectedType, const char *expectedName,
                        ResolvedRecvProp *resolved)
{
  RecvProp *prop = info.prop;
  resolved->offset = info.actualOffset;
  resolved->elementStride = 0;

  if (info.sidecar) {
    const auto field = info.sidecar;
    if (element < 0 || size_t(element) >= field->elementCount) {
      context->ThrowNativeError("Element %d is out of bounds (sidecar %s has %u elements)", element, name, unsigned(field->elementCount)); return false;
    }
    if (prop->GetType() == DPT_DataTable) prop = prop->GetDataTable()->GetProp(element);
    if (prop->GetType() != expectedType && !(expectedType == DPT_Vector && prop->GetType() == DPT_VectorXY)) {
      context->ThrowNativeError("Sidecar field %s is not %s", name, expectedName); return false;
    }
    if (!strcmp(expectedName, "entity handle") && field->kind != CBaseNPCSendFieldKind::EHandle) {
      context->ThrowNativeError("Sidecar field %s is not an EHANDLE", name); return false;
    }
    resolved->prop = prop;
    resolved->offset = int(field->offset + size_t(element) * field->stride);
    resolved->elementStride = int(field->stride);
    return true;
  }

  if (prop->GetType() == expectedType)
  {
    if (element != 0)
    {
      context->ThrowNativeError("RecvProp %s is not an array. Element %d is invalid",
                                name, element);
      return false;
    }
    resolved->prop = prop;
    return true;
  }

  if (prop->GetType() == DPT_Array)
  {
    const int count = prop->GetNumElements();
    const int stride = prop->GetElementStride();
    if (element < 0 || element >= count)
    {
      context->ThrowNativeError("Element %d is out of bounds (Prop %s has %d elements)",
                                element, name, count);
      return false;
    }

    RecvProp *arrayProp = prop->GetArrayProp();
    if (!arrayProp)
    {
      context->ThrowNativeError("RecvProp %s has no ArrayProp", name);
      return false;
    }
    if (arrayProp->GetType() != expectedType)
    {
      context->ThrowNativeError("RecvProp %s type is not %s (%d != %d)",
                                name, expectedName, arrayProp->GetType(), expectedType);
      return false;
    }

    resolved->prop = arrayProp;
    resolved->offset += arrayProp->GetOffset() + stride * element;
    resolved->elementStride = stride;
    return true;
  }

  if (prop->GetType() == DPT_DataTable)
  {
    RecvTable *table = prop->GetDataTable();
    if (!table)
    {
      context->ThrowNativeError("RecvProp %s has no nested RecvTable", name);
      return false;
    }

    const int count = table->GetNumProps();
    if (element < 0 || element >= count)
    {
      context->ThrowNativeError("Element %d is out of bounds (Prop %s has %d elements)",
                                element, name, count);
      return false;
    }

    RecvProp *elementProp = table->GetProp(element);
    if (!elementProp || elementProp->GetType() != expectedType)
    {
      context->ThrowNativeError("RecvProp %s element %d type is not %s",
                                name, element, expectedName);
      return false;
    }

    resolved->prop = elementProp;
    resolved->offset += elementProp->GetOffset();
    return true;
  }

  context->ThrowNativeError("RecvProp %s type is not %s (%d != %d)",
                            name, expectedName, prop->GetType(), expectedType);
  return false;
}

int MatchDataInteger(fieldtype_t type)
{
  switch (type)
  {
    case FIELD_TICK:
    case FIELD_MODELINDEX:
    case FIELD_MATERIALINDEX:
    case FIELD_INTEGER:
    case FIELD_COLOR32:
      return 32;
    case FIELD_SHORT:
      return 16;
    case FIELD_CHARACTER:
      return 8;
    case FIELD_BOOLEAN:
      return 1;
    default:
      return 0;
  }
}

int GetRecvIntegerBits(IPluginContext *context, const ResolvedRecvProp &resolved,
                       int fallbackSize)
{
  if (auto field = CBaseNPCRecvTable::FindField(resolved.prop))
    return field->kind == CBaseNPCSendFieldKind::Bool ? 1 : int(field->elementSize * 8);
  CStandardRecvProxies *proxies = g_ClientEntityManager.GetStandardRecvProxies();
  const RecvVarProxyFn proxy = resolved.prop->GetProxyFn();
  if (proxies)
  {
    if (proxy == proxies->m_Int32ToInt8)
      return 8;
    if (proxy == proxies->m_Int32ToInt16)
      return 16;
    if (proxy == proxies->m_Int32ToInt32)
      return 32;
  }

  if (resolved.elementStride == 1 || resolved.elementStride == 2 ||
      resolved.elementStride == 4)
  {
    return resolved.elementStride * 8;
  }

  if (!ValidateIntegerSize(context, fallbackSize))
    return 0;
  return fallbackSize * 8;
}

cell_t ReadInteger(uint8_t *address, int bits, bool isUnsigned)
{
  if (bits >= 17)
    return *reinterpret_cast<int32_t *>(address);
  if (bits >= 9)
    return isUnsigned ? *reinterpret_cast<uint16_t *>(address)
                      : *reinterpret_cast<int16_t *>(address);
  if (bits >= 2)
    return isUnsigned ? *reinterpret_cast<uint8_t *>(address)
                      : *reinterpret_cast<int8_t *>(address);
  return *reinterpret_cast<bool *>(address) ? 1 : 0;
}

void WriteInteger(uint8_t *address, int bits, cell_t value)
{
  if (bits >= 17)
    *reinterpret_cast<int32_t *>(address) = value;
  else if (bits >= 9)
    *reinterpret_cast<int16_t *>(address) = static_cast<int16_t>(value);
  else if (bits >= 2)
    *reinterpret_cast<int8_t *>(address) = static_cast<int8_t>(value);
  else
    *reinterpret_cast<bool *>(address) = value != 0;
}

cell_t Native_GetEntDataClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2]) ||
      !ValidateIntegerSize(context, params[3]))
    return 0;
  return ReadInteger(entity + params[2], params[3] * 8, params[3] == 1);
}

cell_t Native_SetEntDataClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2]) ||
      !ValidateIntegerSize(context, params[4]))
    return 0;
  WriteInteger(entity + params[2], params[4] * 8, params[3]);
  return 1;
}

cell_t Native_GetEntDataFloatClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2]))
    return 0;
  return sp_ftoc(*reinterpret_cast<float *>(entity + params[2]));
}

cell_t Native_SetEntDataFloatClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2]))
    return 0;
  *reinterpret_cast<float *>(entity + params[2]) = sp_ctof(params[3]);
  return 1;
}

cell_t Native_GetEntDataVectorClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2]))
    return 0;

  cell_t *output = nullptr;
  const int error = context->LocalToPhysAddr(params[3], &output);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not write vector");

  const Vector &vector = *reinterpret_cast<Vector *>(entity + params[2]);
  VectorToPawnVector(output, vector);
  return 1;
}

cell_t Native_SetEntDataVectorClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2]))
    return 0;

  cell_t *input = nullptr;
  const int error = context->LocalToPhysAddr(params[3], &input);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not read vector");

  Vector &vector = *reinterpret_cast<Vector *>(entity + params[2]);
  PawnVectorToVector(input, vector);
  return 1;
}

cell_t Native_GetEntDataStringClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2], true))
    return 0;

  size_t length = 0;
  const int error = context->StringToLocalUTF8(
    params[3], params[4], reinterpret_cast<char *>(entity + params[2]), &length);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not write string");
  return static_cast<cell_t>(length);
}

cell_t Native_SetEntDataStringClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2], true))
    return 0;
  if (params[4] <= 0)
    return context->ThrowNativeError("String buffer size %d is invalid", params[4]);

  char *input = nullptr;
  const int error = context->LocalToString(params[3], &input);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not read string");
  return static_cast<cell_t>(CopyString(
    reinterpret_cast<char *>(entity + params[2]), params[4], input));
}

cell_t Native_GetEntDataEnt2Client(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2]))
    return -1;
  return g_ClientEntityManager.ClientHandleToEntityRef(entity + params[2]);
}

cell_t Native_SetEntDataEnt2Client(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  if (!entity || !ValidateOffset(context, params[2]))
    return 0;
  if (!g_ClientEntityManager.EntityRefToClientHandle(params[3], entity + params[2]))
    return context->ThrowNativeError("Client entity reference 0x%08X is invalid", params[3]);
  return 1;
}

cell_t Native_GetEntDataArrayClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  const int offset = params[2];
  const int count = params[4];
  const int size = params[5];
  if (!entity || !ValidateIntegerSize(context, size) || count < 0)
    return count < 0 ? context->ThrowNativeError("Array size %d is invalid", count) : 0;
  if (count && (!ValidateOffset(context, offset) ||
                !ValidateOffset(context, static_cast<int64_t>(offset) + static_cast<int64_t>(count) * size - 1)))
    return 0;

  cell_t *output = nullptr;
  const int error = context->LocalToPhysAddr(params[3], &output);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not write output array");
  for (int i = 0; i < count; ++i)
    output[i] = ReadInteger(entity + offset + i * size, size * 8, size == 1);
  return 1;
}

cell_t Native_SetEntDataArrayClient(IPluginContext *context, const cell_t *params)
{
  auto *entity = static_cast<uint8_t *>(ResolveEntity(context, params[1]));
  const int offset = params[2];
  const int count = params[4];
  const int size = params[5];
  if (!entity || !ValidateIntegerSize(context, size) || count < 0)
    return count < 0 ? context->ThrowNativeError("Array size %d is invalid", count) : 0;
  if (count && (!ValidateOffset(context, offset) ||
                !ValidateOffset(context, static_cast<int64_t>(offset) + static_cast<int64_t>(count) * size - 1)))
    return 0;

  cell_t *input = nullptr;
  const int error = context->LocalToPhysAddr(params[3], &input);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not read input array");
  for (int i = 0; i < count; ++i)
    WriteInteger(entity + offset + i * size, size * 8, input[i]);
  return 1;
}

cell_t Native_StoreEntityToHandleAddressClient(IPluginContext *context, const cell_t *params)
{
  void *address = nullptr;
  if (!ReadAddressArgument(context, params[2], &address))
    return 0;
  if (!address)
    return context->ThrowNativeError("Address cannot be null");
  if (reinterpret_cast<uintptr_t>(address) < kMinimumValidAddress)
    return context->ThrowNativeError("Invalid address %p is pointing to reserved memory", address);
  if (!g_ClientEntityManager.EntityRefToClientHandle(params[1], address))
    return context->ThrowNativeError("Client entity reference 0x%08X is invalid", params[1]);
  return 1;
}

cell_t Native_GetEntityNetClassClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  ClientClass *clientClass = g_ClientEntityManager.GetClientClass(entity);
  if (!clientClass || !clientClass->GetName())
    return 0;
  const int error = context->StringToLocalUTF8(
    params[2], params[3], clientClass->GetName(), nullptr);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not write network class name");
  return 1;
}

cell_t Native_IsEntNetworkableClient(IPluginContext *, const cell_t *params)
{
  void *entity = g_ClientEntityManager.ResolveClientEntityRef(params[1]);
  if (!entity)
    return 0;
  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  IClientNetworkable *networkable = unknown->GetClientNetworkable();
  return networkable && networkable->entindex() != -1 ? 1 : 0;
}

cell_t Native_RemoveEntityClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  IClientNetworkable *networkable = unknown->GetClientNetworkable();
  if (networkable && networkable->entindex() != -1)
  {
    const char *classname = g_ClientEntityManager.GetEntityClassnameClient(params[1]);
    g_pSM->LogMessage(myself,
      "[CBASENPC] CClientEntity.Remove ignored networked client entity 0x%08X (%s, entindex %d)",
      params[1], classname ? classname : "", networkable->entindex());
    return 0;
  }

  if (!g_ClientEntityManager.RemoveClientOnlyEntity(entity))
  {
    g_pSM->LogMessage(myself,
      "[CBASENPC] CClientEntity.Remove could not call SUB_Remove for client entity 0x%08X",
      params[1]);
    return 0;
  }
  return 1;
}

cell_t Native_GetEntSendPropOffsClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return -1;

  char *name = nullptr;
  if (!GetPropertyName(context, params[2], &name))
    return -1;

  ClientClass *clientClass = g_ClientEntityManager.GetClientClass(entity);
  if (!clientClass || !clientClass->m_pRecvTable)
    return -1;

  RecvPropInfo info;
  if (!FindRecvPropInfo(clientClass->m_pRecvTable, name, &info))
    return -1;
  if (info.sidecar || !strcmp(info.prop->GetName(), CBASENPC_CLASSNAME_PROP)) return -1;

  if (info.prop->GetType() == DPT_Array && info.prop->GetArrayProp())
  {
    return params[3]
      ? info.actualOffset + info.prop->GetArrayProp()->GetOffset()
      : info.prop->GetArrayProp()->GetOffset();
  }
  return params[3] ? info.actualOffset : info.prop->GetOffset();
}

cell_t Native_HasEntPropClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;

  if (params[2] == kPropData)
  {
    datamap_t *map = g_ClientEntityManager.GetClientDataMap(entity);
    DataMapInfo info;
    return map && FindDataMapInfo(map, name, &info) ? 1 : 0;
  }
  if (params[2] == kPropSend)
  {
    ClientClass *clientClass = g_ClientEntityManager.GetClientClass(entity);
    RecvPropInfo info;
    return clientClass && FindRecvPropInfo(clientClass->m_pRecvTable, name, &info) ? 1 : 0;
  }
  return 0;
}

cell_t Native_GetEntPropArraySizeClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    return info.prop->fieldSize;
  }
  if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    if (info.prop->GetType() == DPT_Array)
      return info.prop->GetNumElements();
    if (info.prop->GetType() == DPT_DataTable && info.prop->GetDataTable())
      return info.prop->GetDataTable()->GetNumProps();
    return 0;
  }
  return context->ThrowNativeError("Invalid Property type %d", params[2]);
}

cell_t Native_GetEntPropClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  const int element = params[5];

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    const int bits = MatchDataInteger(info.prop->fieldType);
    if (!bits)
      return context->ThrowNativeError("Data field %s is not an integer (%d)",
                                       name, info.prop->fieldType);
    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return 0;
    return ReadInteger(static_cast<uint8_t *>(entity) + offset, bits, false);
  }

  if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_Int, "integer", &resolved))
      return 0;
    const int bits = GetRecvIntegerBits(context, resolved, params[4]);
    if (!bits)
      return 0;
    const bool isUnsigned = (resolved.prop->GetFlags() & SPROP_UNSIGNED) != 0;
    return ReadInteger(base + resolved.offset, bits, isUnsigned);
  }

  return context->ThrowNativeError("Invalid Property type %d", params[2]);
}

cell_t Native_SetEntPropClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  const int element = params[6];

  uint8_t *address = nullptr;
  int bits = 0;
  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    bits = MatchDataInteger(info.prop->fieldType);
    if (!bits)
      return context->ThrowNativeError("Data field %s is not an integer (%d)",
                                       name, info.prop->fieldType);
    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return 0;
    address = static_cast<uint8_t *>(entity) + offset;
  }
  else if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_Int, "integer", &resolved))
      return 0;
    bits = GetRecvIntegerBits(context, resolved, params[5]);
    if (!bits)
      return 0;
    address = base + resolved.offset;
  }
  else
  {
    return context->ThrowNativeError("Invalid Property type %d", params[2]);
  }

  WriteInteger(address, bits, params[4]);
  return 1;
}

cell_t Native_GetEntPropFloatClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  const int element = params[4];
  uint8_t *address = nullptr;

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    if (info.prop->fieldType != FIELD_FLOAT && info.prop->fieldType != FIELD_TIME)
      return context->ThrowNativeError("Data field %s is not a float (%d)",
                                       name, info.prop->fieldType);
    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return 0;
    address = static_cast<uint8_t *>(entity) + offset;
  }
  else if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_Float, "float", &resolved))
      return 0;
    address = base + resolved.offset;
  }
  else
  {
    return context->ThrowNativeError("Invalid Property type %d", params[2]);
  }
  return sp_ftoc(*reinterpret_cast<float *>(address));
}

cell_t Native_SetEntPropFloatClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  const int element = params[5];
  uint8_t *address = nullptr;

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    if (info.prop->fieldType != FIELD_FLOAT && info.prop->fieldType != FIELD_TIME)
      return context->ThrowNativeError("Data field %s is not a float (%d)",
                                       name, info.prop->fieldType);
    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return 0;
    address = static_cast<uint8_t *>(entity) + offset;
  }
  else if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_Float, "float", &resolved))
      return 0;
    address = base + resolved.offset;
  }
  else
  {
    return context->ThrowNativeError("Invalid Property type %d", params[2]);
  }
  *reinterpret_cast<float *>(address) = sp_ctof(params[4]);
  return 1;
}

cell_t Native_GetEntPropEntClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return -1;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return -1;
  const int element = params[4];

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return -1;
    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return -1;
    uint8_t *address = static_cast<uint8_t *>(entity) + offset;
    if (info.prop->fieldType == FIELD_EHANDLE)
      return g_ClientEntityManager.ClientHandleToEntityRef(address);
    if (info.prop->fieldType == FIELD_CLASSPTR)
      return g_ClientEntityManager.EntityToClientRef(
        *reinterpret_cast<void **>(address));
    if (info.prop->fieldType == FIELD_EDICT)
      return context->ThrowNativeError(
        "Data field %s is FIELD_EDICT, which has no client entity equivalent", name);
    return context->ThrowNativeError("Data field %s is not a client entity (%d)",
                                     name, info.prop->fieldType);
  }

  if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return -1;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_Int, "entity handle", &resolved))
      return -1;
    return g_ClientEntityManager.ClientHandleToEntityRef(base + resolved.offset);
  }
  return context->ThrowNativeError("Invalid Property type %d", params[2]);
}

cell_t Native_SetEntPropEntClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  const int otherRef = params[4];
  const int element = params[5];

  void *other = nullptr;
  if (otherRef != -1)
  {
    other = g_ClientEntityManager.ResolveClientEntityRef(otherRef);
    if (!other)
      return context->ThrowNativeError("Client entity reference 0x%08X is invalid", otherRef);
  }

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return 0;
    uint8_t *address = static_cast<uint8_t *>(entity) + offset;
    if (info.prop->fieldType == FIELD_EHANDLE)
    {
      if (!g_ClientEntityManager.EntityRefToClientHandle(otherRef, address))
        return context->ThrowNativeError("Client entity reference 0x%08X is invalid", otherRef);
      return 1;
    }
    if (info.prop->fieldType == FIELD_CLASSPTR)
    {
      *reinterpret_cast<void **>(address) = other;
      return 1;
    }
    if (info.prop->fieldType == FIELD_EDICT)
      return context->ThrowNativeError(
        "Data field %s is FIELD_EDICT, which has no client entity equivalent", name);
    return context->ThrowNativeError("Data field %s is not a client entity (%d)",
                                     name, info.prop->fieldType);
  }

  if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_Int, "entity handle", &resolved))
      return 0;
    if (!g_ClientEntityManager.EntityRefToClientHandle(otherRef, base + resolved.offset))
      return context->ThrowNativeError("Client entity reference 0x%08X is invalid", otherRef);
    return 1;
  }
  return context->ThrowNativeError("Invalid Property type %d", params[2]);
}

cell_t Native_GetEntPropVectorClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  const int element = params[5];
  uint8_t *address = nullptr;
  int components = 3;

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    if (info.prop->fieldType != FIELD_VECTOR &&
        info.prop->fieldType != FIELD_POSITION_VECTOR)
      return context->ThrowNativeError("Data field %s is not a vector (%d)",
                                       name, info.prop->fieldType);
    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return 0;
    address = static_cast<uint8_t *>(entity) + offset;
  }
  else if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_Vector, "vector", &resolved))
      return 0;
    if (resolved.prop->GetType() == DPT_VectorXY) components = 2;
    address = base + resolved.offset;
  }
  else
  {
    return context->ThrowNativeError("Invalid Property type %d", params[2]);
  }

  cell_t *output = nullptr;
  const int error = context->LocalToPhysAddr(params[4], &output);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not write vector");
  for (int i = 0; i < components; ++i) output[i] = sp_ftoc(reinterpret_cast<float*>(address)[i]);
  if (components == 2) output[2] = sp_ftoc(0.0f);
  return 1;
}

cell_t Native_SetEntPropVectorClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  const int element = params[5];
  uint8_t *address = nullptr;
  int components = 3;

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    if (info.prop->fieldType != FIELD_VECTOR &&
        info.prop->fieldType != FIELD_POSITION_VECTOR)
      return context->ThrowNativeError("Data field %s is not a vector (%d)",
                                       name, info.prop->fieldType);
    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return 0;
    address = static_cast<uint8_t *>(entity) + offset;
  }
  else if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_Vector, "vector", &resolved))
      return 0;
    if (resolved.prop->GetType() == DPT_VectorXY) components = 2;
    address = base + resolved.offset;
  }
  else
  {
    return context->ThrowNativeError("Invalid Property type %d", params[2]);
  }

  cell_t *input = nullptr;
  const int error = context->LocalToPhysAddr(params[4], &input);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not read vector");
  for (int i = 0; i < components; ++i) reinterpret_cast<float*>(address)[i] = sp_ctof(input[i]);
  return 1;
}

cell_t Native_GetEntPropStringClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  const int element = params[6];
  const char *source = nullptr;

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    const fieldtype_t type = info.prop->fieldType;
    if (type != FIELD_CHARACTER && type != FIELD_STRING &&
        type != FIELD_MODELNAME && type != FIELD_SOUNDNAME)
      return context->ThrowNativeError("Data field %s is not a string (%d)", name, type);

    if (type == FIELD_CHARACTER)
    {
      if (element != 0)
        return context->ThrowNativeError("Prop %s is a character buffer, not an array of strings", name);
      source = reinterpret_cast<char *>(entity) + info.actualOffset;
    }
    else
    {
      int offset = 0;
      if (!ResolveDataElement(context, name, info, element, &offset))
        return 0;
      const string_t value = *reinterpret_cast<string_t *>(
        static_cast<uint8_t *>(entity) + offset);
      source = STRING(value);
    }
  }
  else if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_String, "string", &resolved))
      return 0;
    source = reinterpret_cast<char *>(base + resolved.offset);
  }
  else
  {
    return context->ThrowNativeError("Invalid Property type %d", params[2]);
  }

  size_t length = 0;
  const int error = context->StringToLocalUTF8(
    params[4], params[5], source ? source : "", &length);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not write string");
  return static_cast<cell_t>(length);
}

cell_t Native_SetEntPropStringClient(IPluginContext *context, const cell_t *params)
{
  void *entity = ResolveEntity(context, params[1]);
  if (!entity)
    return 0;
  char *name = nullptr;
  char *input = nullptr;
  if (!GetPropertyName(context, params[3], &name))
    return 0;
  int error = context->LocalToString(params[4], &input);
  if (error != SP_ERROR_NONE)
    return context->ThrowNativeErrorEx(error, "Could not read string");
  const int element = params[5];

  if (params[2] == kPropData)
  {
    DataMapInfo info;
    if (!FindDataProperty(context, entity, params[1], name, &info))
      return 0;
    const fieldtype_t type = info.prop->fieldType;
    if (type != FIELD_CHARACTER && type != FIELD_STRING &&
        type != FIELD_MODELNAME && type != FIELD_SOUNDNAME)
      return context->ThrowNativeError("Data field %s is not a string (%d)", name, type);

    if (type == FIELD_CHARACTER)
    {
      if (element != 0)
        return context->ThrowNativeError("Prop %s is a character buffer, not an array of strings", name);
      if (info.prop->fieldSize <= 0)
        return context->ThrowNativeError("Data field %s has an invalid character buffer size", name);
      return static_cast<cell_t>(CopyString(
        reinterpret_cast<char *>(entity) + info.actualOffset,
        info.prop->fieldSize, input));
    }

    int offset = 0;
    if (!ResolveDataElement(context, name, info, element, &offset))
      return 0;
    *reinterpret_cast<string_t *>(static_cast<uint8_t *>(entity) + offset) =
      g_ClientEntityManager.AllocPooledStringClient(input);
    return static_cast<cell_t>(std::strlen(input));
  }

  if (params[2] == kPropSend)
  {
    RecvPropInfo info;
    uint8_t *base = nullptr;
    if (!FindRecvProperty(context, entity, params[1], name, &info, &base))
      return 0;
    ResolvedRecvProp resolved;
    if (!ResolveRecvElement(context, name, info, element, DPT_String, "string", &resolved))
      return 0;
    const int maxlength = resolved.prop->m_StringBufferSize;
    if (maxlength <= 0)
      return context->ThrowNativeError("RecvProp %s has an invalid string buffer size %d",
                                       name, maxlength);
    return static_cast<cell_t>(CopyString(
      reinterpret_cast<char *>(base + resolved.offset), maxlength, input));
  }
  return context->ThrowNativeError("Invalid Property type %d", params[2]);
}

} // namespace

bool CClientEntityProperties::FindDataMapInfo(datamap_t *map, const char *name,
                                             ClientDataMapInfo *result)
{
  if (!result || !map || !name || !*name) return false;
  auto &cache = dataMaps_[map];
  auto found = cache.find(name);
  if (found == cache.end())
  {
    ClientDataMapInfo info;
    info.prop = CBaseNPCDataMapLookup::Find(map, name, &info.actualOffset);
    found = cache.emplace(name, info).first;
  }
  *result = found->second;
  return result->prop != nullptr;
}

bool CClientEntityProperties::FindRecvPropInfo(RecvTable *table, const char *name,
                                              ClientRecvPropInfo *result)
{
  if (!result || !table || !name || !*name) return false;
  auto &cache = recvTables_[table];
  auto found = cache.find(name);
  if (found == cache.end())
  {
    ClientRecvPropInfo info;
    info.prop = CBaseNPCClientLookup::FindProp(table, name, &info.actualOffset);
    info.sidecar = CBaseNPCRecvTable::FindField(info.prop);
    if (info.sidecar) info.actualOffset = -1; // Not a physical C_BaseEntity offset.
    found = cache.emplace(name, info).first;
  }
  *result = found->second;
  return result->prop != nullptr;
}

void CClientEntityProperties::ClearCaches()
{
  dataMaps_.clear();
  recvTables_.clear();
}

void natives::setupClientEntityPropertyNatives(std::vector<sp_nativeinfo_t> &natives)
{
  const sp_nativeinfo_t list[] = {
    {"CClientEntity.GetData", Native_GetEntDataClient},
    {"CClientEntity.GetDataEntity", Native_GetEntDataEnt2Client},
    {"CClientEntity.GetDataFloat", Native_GetEntDataFloatClient},
    {"CClientEntity.GetDataString", Native_GetEntDataStringClient},
    {"CClientEntity.GetDataVector", Native_GetEntDataVectorClient},
    {"CClientEntity.GetProp", Native_GetEntPropClient},
    {"CClientEntity.GetPropArraySize", Native_GetEntPropArraySizeClient},
    {"CClientEntity.GetPropEntity", Native_GetEntPropEntClient},
    {"CClientEntity.GetPropFloat", Native_GetEntPropFloatClient},
    {"CClientEntity.GetPropString", Native_GetEntPropStringClient},
    {"CClientEntity.GetPropVector", Native_GetEntPropVectorClient},
    {"CClientEntity.SetData", Native_SetEntDataClient},
    {"CClientEntity.SetDataEntity", Native_SetEntDataEnt2Client},
    {"CClientEntity.SetDataFloat", Native_SetEntDataFloatClient},
    {"CClientEntity.SetDataString", Native_SetEntDataStringClient},
    {"CClientEntity.SetDataVector", Native_SetEntDataVectorClient},
    {"CClientEntity.SetProp", Native_SetEntPropClient},
    {"CClientEntity.SetPropEntity", Native_SetEntPropEntClient},
    {"CClientEntity.SetPropFloat", Native_SetEntPropFloatClient},
    {"CClientEntity.SetPropString", Native_SetEntPropStringClient},
    {"CClientEntity.SetPropVector", Native_SetEntPropVectorClient},
    {"CClientEntity.StoreHandle", Native_StoreEntityToHandleAddressClient},
    {"CClientEntity.GetDataArray", Native_GetEntDataArrayClient},
    {"CClientEntity.GetRecvPropOffset", Native_GetEntSendPropOffsClient},
    {"CClientEntity.HasProp", Native_HasEntPropClient},
    {"CClientEntity.SetDataArray", Native_SetEntDataArrayClient},
    {"CClientEntity.GetNetworkClass", Native_GetEntityNetClassClient},
    {"CClientEntity.IsNetworkable", Native_IsEntNetworkableClient},
    {"CClientEntity.Remove", Native_RemoveEntityClient}
  };
  natives.insert(natives.end(), std::begin(list), std::end(list));
}
