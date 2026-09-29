#include "cliententitymanager.h"
#include "sourcesdk/cbasenpcclientlookup.h"
#include "sourcesdk/cbasenpcclientclass.h"
#include "sourcesdk/cbasenpcnetworkschema.h"
#include <cstdio>

#include <algorithm>
#include <cctype>
#include <const.h>
#include <cstring>

namespace
{
constexpr uint32_t kClientEntRefMask = uint32_t{1} << 31;

bool ClassnameMatches(const std::string &candidate, const char *pattern)
{
  if (!pattern)
    return false;

  size_t patternLength = std::strlen(pattern);
  const bool prefixMatch = patternLength > 0 && pattern[patternLength - 1] == '*';
  if (prefixMatch)
    --patternLength;

  if (candidate.size() < patternLength ||
      (!prefixMatch && candidate.size() != patternLength))
    return false;

  for (size_t i = 0; i < patternLength; ++i)
  {
    if (std::tolower(static_cast<unsigned char>(candidate[i])) !=
        std::tolower(static_cast<unsigned char>(pattern[i])))
      return false;
  }
  return true;
}
}

#if defined _WIN32
# include <Windows.h>
#endif

CClientEntityManager g_ClientEntityManager;

#ifndef CBASENPC_CLIENT_TESTS
SH_DECL_HOOK0(IClientNetworkable, GetClientClass, SH_NOATTRIB, 0, ClientClass*);
SH_DECL_HOOK0_void(IClientNetworkable, Release, SH_NOATTRIB, 0);
SH_DECL_HOOK1_void(IBaseClientDLL, FrameStageNotify, SH_NOATTRIB, 0, ClientFrameStage_t);
SH_DECL_HOOK0_void(IBaseClientDLL, LevelShutdown, SH_NOATTRIB, 0);
#endif

bool CClientEntityManager::Initialize(SourceMod::IGameConfig *gameConfig,
                                      char *error, size_t maxlength)
{
  if (available_)
    return true;
  Shutdown();
  // Absence of a client is expected on dedicated and unsupported platforms.
#if !defined(_WIN32) || defined(_WIN64)
  return true;
#else
  if (!engine)
  {
    if (error && maxlength) snprintf(error, maxlength, "IVEngineServer is not available");
    return false;
  }
  if (engine->IsDedicatedServer())
  {
    g_pSM->LogMessage(myself, "CBaseNPC client entity subsystem unavailable on dedicated server.");
    return true;
  }
  if (!gameConfig)
  {
    if (error && maxlength) snprintf(error, maxlength, "CBaseNPC gamedata is not available");
    return false;
  }

  if (!ResolveClientInterfaces(error, maxlength) ||
      !ResolvePropertyAccess(gameConfig, error, maxlength))
  {
    Shutdown();
    return false;
  }

  onEntityCreated_ = forwards->CreateForward("OnEntityCreatedClient", ET_Ignore, 3,
                                            nullptr, Param_Cell, Param_String, Param_Array);
  onEntityDestroyed_ = forwards->CreateForward("OnEntityDestroyedClient", ET_Ignore, 1,
                                              nullptr, Param_Cell);
  if (!onEntityCreated_ || !onEntityDestroyed_ ||
      !RegisterEngineListener(gameConfig, error, maxlength))
  {
    if ((!onEntityCreated_ || !onEntityDestroyed_) && error && maxlength)
      snprintf(error, maxlength, "Could not create CBaseNPC client entity forwards");
    Shutdown();
    return false;
  }

  available_ = true;
#ifndef CBASENPC_CLIENT_TESTS
  frameStageHook_ = SH_ADD_HOOK(IBaseClientDLL, FrameStageNotify, clientDll_, SH_MEMBER(this, &CClientEntityManager::Hook_FrameStageNotify), true);
  levelShutdownHook_ = SH_ADD_HOOK(IBaseClientDLL, LevelShutdown, clientDll_, SH_MEMBER(this, &CClientEntityManager::Hook_LevelShutdown), false);
  if (!frameStageHook_ || !levelShutdownHook_) {
    smutils->Format(error, maxlength, "Could not hook client frame/level lifecycle"); Shutdown(); return false;
  }
#endif
  SeedExistingEntities();
  g_pSM->LogMessage(myself,
    "CBaseNPC client entity subsystem initialized: %d existing client entities, "
    "listener offset 0x%X, GetDataDescMap slot %d, SUB_Remove slot %d",
    GetClientEntityCount(),
    static_cast<unsigned int>(reinterpret_cast<uint8_t *>(engineListeners_) -
                              reinterpret_cast<uint8_t *>(clientEntityList_)),
    getDataDescMapOffset_, subRemoveOffset_);
  return true;
#endif
}

bool CClientEntityManager::ResolveClientInterfaces(char *error, size_t maxlength)
{
#if defined _WIN32
  HMODULE clientModule = GetModuleHandleA("client.dll");
  if (!clientModule)
  {
    smutils->Format(error, maxlength,
                    "client.dll is not loaded although SourceMod has started");
    return false;
  }

  auto clientFactory = reinterpret_cast<CreateInterfaceFn>(
    GetProcAddress(clientModule, CREATEINTERFACE_PROCNAME));
  if (!clientFactory)
  {
    smutils->Format(error, maxlength, "client.dll does not export CreateInterface");
    return false;
  }

  clientEntityList_ = static_cast<IClientEntityList *>(
    clientFactory(VCLIENTENTITYLIST_INTERFACE_VERSION, nullptr));
  clientDll_ = static_cast<IBaseClientDLL *>(
    clientFactory(CLIENT_DLL_INTERFACE_VERSION, nullptr));
  clientTools_ = static_cast<IClientTools *>(
    clientFactory(VCLIENTTOOLS_INTERFACE_VERSION, nullptr));

  if (!clientEntityList_)
  {
    smutils->Format(error, maxlength, "client.dll does not expose %s",
                    VCLIENTENTITYLIST_INTERFACE_VERSION);
    return false;
  }
  if (!clientTools_)
  {
    smutils->Format(error, maxlength, "client.dll does not expose %s",
                    VCLIENTTOOLS_INTERFACE_VERSION);
    return false;
  }
  if (!clientDll_)
  {
    smutils->Format(error, maxlength, "client.dll does not expose %s",
                    CLIENT_DLL_INTERFACE_VERSION);
    return false;
  }

  ClientClass *head = clientDll_->GetAllClasses();
  if (!head || !CBaseNPCClientLookup::ValidateClasses(head, classCount_))
  {
    smutils->Format(error, maxlength,
      "%s::GetAllClasses returned null, invalid metadata, or more than 4096 classes",
      CLIENT_DLL_INTERFACE_VERSION);
    return false;
  }
  return true;
#else
  return false;
#endif
}

bool CClientEntityManager::ResolvePropertyAccess(SourceMod::IGameConfig *gameConfig, char *error, size_t maxlength)
{
  if (!gameConfig->GetOffset("C_BaseEntity::GetDataDescMap", &getDataDescMapOffset_) ||
      getDataDescMapOffset_ < 0 || getDataDescMapOffset_ > 512)
  {
    smutils->Format(error, maxlength,
                    "Missing or implausible C_BaseEntity::GetDataDescMap vtable offset");
    return false;
  }

  if (!gameConfig->GetOffset("C_BaseEntity::SUB_Remove", &subRemoveOffset_) ||
      subRemoveOffset_ < 0 || subRemoveOffset_ > 512)
  {
    smutils->Format(error, maxlength,
                    "Missing or implausible C_BaseEntity::SUB_Remove vtable offset");
    return false;
  }

  standardRecvProxies_ = clientDll_->GetStandardRecvProxies();
  if (!standardRecvProxies_)
  {
    smutils->Format(error, maxlength,
                    "%s::GetStandardRecvProxies returned null",
                    CLIENT_DLL_INTERFACE_VERSION);
    return false;
  }

  void *allocPooledString = nullptr;
  if (!gameConfig->GetMemSig("AllocPooledStringClient", &allocPooledString) ||
      !allocPooledString)
  {
    smutils->Format(error, maxlength,
                    "Could not resolve client.dll AllocPooledString signature");
    return false;
  }

  allocPooledStringClient_ =
    reinterpret_cast<string_t (*)(const char *)>(allocPooledString);
  return true;
}

bool CClientEntityManager::RegisterEngineListener(SourceMod::IGameConfig *gameConfig, char *error, size_t maxlength)
{
  int listenersOffset = -1;
  if (!gameConfig->GetOffset("ClientEntityListeners", &listenersOffset))
  {
    smutils->Format(error, maxlength,
                    "Missing ClientEntityListeners offset in gamedata");
    return false;
  }
  if (listenersOffset <= 0 || listenersOffset > 0x1000)
  {
    smutils->Format(error, maxlength,
                    "Implausible ClientEntityListeners offset: 0x%X", listenersOffset);
    return false;
  }

  engineListeners_ = reinterpret_cast<EngineListenerVector *>(
    reinterpret_cast<uint8_t *>(clientEntityList_) + listenersOffset);

  const int count = engineListeners_->Count();
  if (count < 0 || count > 1024)
  {
    smutils->Format(error, maxlength,
                    "ClientEntityListeners offset 0x%X produced implausible count %d",
                    listenersOffset, count);
    engineListeners_ = nullptr;
    return false;
  }

  if (engineListeners_->Find(this) == engineListeners_->InvalidIndex())
    engineListeners_->AddToTail(this);

  engineListenerRegistered_ = true;
  return true;
}

void CClientEntityManager::UnregisterEngineListener()
{
  if (!engineListenerRegistered_ || !engineListeners_)
    return;

  engineListeners_->FindAndRemove(this);
  engineListenerRegistered_ = false;
  engineListeners_ = nullptr;
}

void CClientEntityManager::SeedExistingEntities()
{
  if (!clientTools_)
    return;

  for (EntitySearchResult current = clientTools_->FirstEntity(); current;
       current = clientTools_->NextEntity(current))
  {
    TrackEntity(reinterpret_cast<C_BaseEntity *>(current), false);
  }
}

void CClientEntityManager::TrackEntity(C_BaseEntity *entity, bool notify)
{
  if (!available_ || purging_ || !entity || entities_.find(entity) != entities_.end())
    return;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  const CBaseHandle &handle = unknown->GetRefEHandle();
  if (!handle.IsValid())
    return;

  IClientNetworkable *networkable = unknown->GetClientNetworkable();
  const int entIndex = networkable ? networkable->entindex() : -1;
  EntityRecord record;
  record.clientRef = entIndex >= 0 ? entIndex : handle.ToInt();
  record.handleValue = static_cast<uint32_t>(handle.ToInt());

  const uint32_t handleValue = record.handleValue;
  record.createdNotified = !notify;
  entities_.emplace(entity, std::move(record));
  entityOrder_.push_back(entity);

#ifndef CBASENPC_CLIENT_TESTS
  if (auto runtime = g_CBaseNPCClientClassManager.ActiveCreation(entIndex, handle.GetSerialNumber()))
    AttachRuntime(entity, runtime);
#endif

  // Runtime identity must be installed before consulting IClientTools: its
  // classname accessor can itself call the virtual GetClientClass().
#if defined(CBASENPC_CLIENT_TESTS)
  const bool classnameBridgeEnabled = false;
#else
  const bool classnameBridgeEnabled = g_CBaseNPCNetworkSchemaManager.IsClassnameBridgeEnabled();
#endif
  auto tracked = entities_.find(entity);
  if (tracked != entities_.end() && !tracked->second.runtimeClass &&
      (entIndex < 0 || !classnameBridgeEnabled)) {
    const auto clientName = ReadClientClassname(entity);
    tracked = entities_.find(entity);
    if (tracked == entities_.end() || tracked->second.handleValue != handleValue) return;
    tracked->second.clientClassname = clientName;
    if (CBaseNPCNetworkDebugEnabled()) ++clientToolsClassnameFallbacks_;
  }

  if (!notify)
    return;

  if (entIndex >= 0) {
    pendingCreates_.push_back({entity, handleValue});
    return;
  }
  NotifyCreated(entity, handleValue);
}

void CClientEntityManager::NotifyCreated(C_BaseEntity* entity, uint32_t handleValue)
{
  auto found = entities_.find(entity);
  if (!available_ || found == entities_.end() || found->second.handleValue != handleValue ||
      found->second.deleting || found->second.createdNotified) return;
  found->second.createdNotified = true;
  const int clientRef = found->second.clientRef;
  const std::string classname = EffectiveClassname(found->second);

  if (onEntityCreated_)
  {
    onEntityCreated_->PushCell(clientRef);
    onEntityCreated_->PushString(classname.c_str());
    int64_t clientAddress = static_cast<int64_t>(reinterpret_cast<uintptr_t>(entity));
    onEntityCreated_->PushArray(reinterpret_cast<cell_t *>(&clientAddress), 2);
    onEntityCreated_->Execute(nullptr);
  }

  const auto snapshot = listeners_;
  for (auto *listener : snapshot)
  {
    const auto live = entities_.find(entity);
    if (!available_ || live == entities_.end() || live->second.handleValue != handleValue)
      break; // A prior callback may have deleted the entity or stopped the manager.
    if (available_ && listener && std::find(listeners_.begin(), listeners_.end(), listener) != listeners_.end())
      listener->OnClientEntityCreated(entity, clientRef, classname.c_str());
  }
}

std::string CClientEntityManager::ReadClientClassname(C_BaseEntity *entity) const
{
  if (!entity || !clientTools_)
    return {};

  const int recordablesBefore = clientTools_->GetNumRecordables();
  const HTOOLHANDLE handle = clientTools_->AttachToEntity(entity);
  const bool attachedByUs =
    handle != HTOOLHANDLE_INVALID &&
    clientTools_->GetNumRecordables() == recordablesBefore + 1;

  std::string classname;
  if (handle != HTOOLHANDLE_INVALID && clientTools_->IsValidHandle(handle))
  {
    const char *rawClassname = clientTools_->GetClassname(handle);
    if (rawClassname && rawClassname[0] != '\0')
      classname.assign(rawClassname);
  }

  if (attachedByUs)
    clientTools_->DetachFromEntity(entity);

  constexpr const char classPrefix[] = "class ";
  constexpr const char structPrefix[] = "struct ";
  if (classname.compare(0, sizeof(classPrefix) - 1, classPrefix) == 0)
    classname.erase(0, sizeof(classPrefix) - 1);
  else if (classname.compare(0, sizeof(structPrefix) - 1, structPrefix) == 0)
    classname.erase(0, sizeof(structPrefix) - 1);

  return classname;
}

void CClientEntityManager::OnEntityCreated(C_BaseEntity *entity)
{
  TrackEntity(entity, true);
}

void CClientEntityManager::OnEntityDeleted(C_BaseEntity *entity)
{
  auto found = entities_.find(entity);
  if (!available_ || found == entities_.end() || found->second.deleting)
    return;
  found->second.deleting = true;
  const int clientRef = found->second.clientRef;
  const uint32_t handleValue = found->second.handleValue;
  const bool notify = found->second.createdNotified;
  CleanupRuntime(entity);

  if (notify && onEntityDestroyed_)
  {
    onEntityDestroyed_->PushCell(clientRef);
    onEntityDestroyed_->Execute(nullptr);
  }
  const auto snapshot = listeners_;
  for (auto *listener : snapshot)
  {
    if (!notify) break;
    if (!available_) break;
    if (listener && std::find(listeners_.begin(), listeners_.end(), listener) != listeners_.end())
      listener->OnClientEntityDestroyed(entity, clientRef);
  }

  // Plugin/listener callbacks can insert entities and rehash the map.
  found = entities_.find(entity);
  if (found != entities_.end() && found->second.handleValue == handleValue)
  {
    entityOrder_.erase(std::remove(entityOrder_.begin(), entityOrder_.end(), entity), entityOrder_.end());
    entities_.erase(found);
  }
}

void *CClientEntityManager::ResolveClientEntityRef(int clientRef)
{
  C_BaseEntity* entity = nullptr;
  return ResolveRecord(clientRef, &entity) ? entity : nullptr;
}

CClientEntityManager::EntityRecord* CClientEntityManager::ResolveRecord(
  int clientRef, C_BaseEntity** resolvedEntity)
{
  const uint32_t encodedRef = static_cast<uint32_t>(clientRef);
  if (!available_ || !clientEntityList_ || encodedRef == INVALID_EHANDLE_INDEX)
    return nullptr;

  // BCompat refs for networked entities are raw entindices. Proper refs carry
  // the high-bit marker and encode the client CBaseHandle plus its serial.
  const bool properRef = (encodedRef & kClientEntRefMask) != 0;
  if (!properRef && clientRef >= 0 && clientRef < MAX_EDICTS)
  {
    IClientEntity *clientEntity = clientEntityList_->GetClientEntity(clientRef);
    if (!clientEntity)
      return nullptr;

    C_BaseEntity *entity = clientEntity->GetBaseEntity();
    const auto found = entities_.find(entity);
    if (found == entities_.end() || found->second.clientRef != clientRef ||
        found->second.handleValue != static_cast<uint32_t>(clientEntity->GetRefEHandle().ToInt()))
      return nullptr;
    if (resolvedEntity) *resolvedEntity = entity;
    return &found->second;
  }

  const uint32_t handleValue = properRef
    ? encodedRef & ~kClientEntRefMask
    : encodedRef;
  const CBaseHandle handle(static_cast<unsigned long>(handleValue));
  IClientUnknown *unknown = clientEntityList_->GetClientUnknownFromHandle(handle);
  if (!unknown || unknown->GetRefEHandle() != handle)
    return nullptr;

  C_BaseEntity *entity = unknown->GetBaseEntity();
  const auto found = entities_.find(entity);
  if (found == entities_.end() || found->second.handleValue != handleValue)
    return nullptr;
  if (resolvedEntity) *resolvedEntity = entity;
  return &found->second;
}

bool CClientEntityManager::ResolveAccess(int clientRef, CBaseNPCClientEntityAccess& access)
{
  access = {};
  C_BaseEntity* entity = nullptr;
  EntityRecord* record = ResolveRecord(clientRef, &entity);
  if (!record) return false;
  access.entity = entity;
  access.handleValue = record->handleValue;
  access.clientRef = record->clientRef;
  access.runtimeClass = record->runtimeClass;
  access.classname = EffectiveClassname(*record).c_str();
  access.sidecar = record->sidecar;
  access.sidecarSize = record->sidecarSize;
  auto unknown = reinterpret_cast<IClientUnknown*>(entity);
  access.networkable = unknown->GetClientNetworkable();
  access.clientClass = record->runtimeClass ? record->runtimeClass->Get() :
    (access.networkable ? access.networkable->GetClientClass() : nullptr);
  access.recvBase = access.networkable
    ? static_cast<unsigned char*>(access.networkable->GetDataTableBasePtr()) : nullptr;
  access.dataMap = GetDataMapUnchecked(entity);
  return true;
}

int CClientEntityManager::EntityToClientRef(void *entityAddress)
{
  auto *entity = static_cast<C_BaseEntity *>(entityAddress);
  const auto found = entities_.find(entity);
  return found == entities_.end() ? -1 : found->second.clientRef;
}

int CClientEntityManager::EntityToClientHandleRef(void *entityAddress)
{
  auto *entity = static_cast<C_BaseEntity *>(entityAddress);
  const auto found = entities_.find(entity);
  if (found == entities_.end())
    return -1;

  return static_cast<int>(found->second.handleValue | kClientEntRefMask);
}

bool CClientEntityManager::IsSameClientEntity(void *entityAddress,
                                                int clientHandleRef)
{
  if (!IsTrackedEntity(entityAddress))
    return false;

  const uint32_t encodedRef = static_cast<uint32_t>(clientHandleRef);
  if ((encodedRef & kClientEntRefMask) == 0 ||
      encodedRef == INVALID_EHANDLE_INDEX)
  {
    return false;
  }

  const uint32_t expectedHandle = encodedRef & ~kClientEntRefMask;
  if (ResolveClientEntityRef(clientHandleRef) != entityAddress)
    return false;
  auto *unknown = reinterpret_cast<IClientUnknown *>(entityAddress);
  const CBaseHandle &currentHandle = unknown->GetRefEHandle();
  return currentHandle.IsValid() &&
         static_cast<uint32_t>(currentHandle.ToInt()) == expectedHandle;
}

int CClientEntityManager::GetClientEntityCount() const
{
  return static_cast<int>(entities_.size());
}

int CClientEntityManager::GetEntityRefByOrdinalClient(int ordinal) const
{
  if (ordinal < 0 || ordinal >= static_cast<int>(entityOrder_.size()))
    return -1;

  const auto found = entities_.find(entityOrder_[ordinal]);
  return found == entities_.end() ? -1 : found->second.clientRef;
}

const char *CClientEntityManager::GetEntityClassnameClient(int clientRef)
{
  auto *entity = static_cast<C_BaseEntity *>(ResolveClientEntityRef(clientRef));
  if (!entity)
    return nullptr;
  const auto found = entities_.find(entity);
  if (found == entities_.end())
    return nullptr;

  return EffectiveClassname(found->second).c_str();
}

int CClientEntityManager::FindClientEntityByClassname(
  int startRef, const char *classname) const
{
  if (!available_) return -1;
  C_BaseEntity *startEntity = static_cast<C_BaseEntity *>(
    const_cast<CClientEntityManager *>(this)->ResolveClientEntityRef(startRef));
  size_t startOrdinal = 0;
  if (startRef != -1)
  {
    bool foundStart = false;
    for (size_t i = 0; i < entityOrder_.size(); ++i)
    {
      const auto found = entities_.find(entityOrder_[i]);
      if (found != entities_.end() && entityOrder_[i] == startEntity)
      {
        startOrdinal = i + 1;
        foundStart = true;
        break;
      }
    }
    if (!foundStart)
      return -1;
  }

  for (size_t i = startOrdinal; i < entityOrder_.size(); ++i)
  {
    const auto found = entities_.find(entityOrder_[i]);
    if (found == entities_.end())
      continue;

    const std::string &candidate = EffectiveClassname(found->second);
    if (!candidate.empty() && ClassnameMatches(candidate, classname))
      return found->second.clientRef;
  }
  return -1;
}

bool CClientEntityManager::GetEntityClassnameDiagnostics(
  int clientRef, int *entIndex, const char **networkName, const char **mapClassname,
  const char **clientClassname, const char **replicatedClassname, const char **classname)
{
  auto *entity = static_cast<C_BaseEntity *>(ResolveClientEntityRef(clientRef));
  if (!entity)
    return false;

  const auto found = entities_.find(entity);
  if (found == entities_.end())
    return false;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  IClientNetworkable *networkable = unknown->GetClientNetworkable();
  ClientClass *clientClass = networkable ? networkable->GetClientClass() : nullptr;

  if (entIndex)
    *entIndex = networkable ? networkable->entindex() : -1;
  if (networkName)
    *networkName = clientClass && clientClass->m_pNetworkName
      ? clientClass->m_pNetworkName : "";
  if (mapClassname)
    *mapClassname = clientClass && clientClass->m_pMapClassname
      ? clientClass->m_pMapClassname : "";
  if (clientClassname)
    *clientClassname = found->second.clientClassname.c_str();
  if (replicatedClassname)
    *replicatedClassname = found->second.replicatedClassname.c_str();
  if (classname)
    *classname = EffectiveClassname(found->second).c_str();
  return true;
}

int CClientEntityManager::EntRefToEntIndexClient(int clientRef) const
{
  auto *self = const_cast<CClientEntityManager *>(this);
  auto *entity = static_cast<C_BaseEntity *>(self->ResolveClientEntityRef(clientRef));
  if (!entity)
    return -1;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  IClientNetworkable *networkable = unknown->GetClientNetworkable();
  return networkable ? networkable->entindex() : -1;
}

int CClientEntityManager::EntIndexToEntRefClient(int entIndex) const
{
  if (!available_ || !clientEntityList_ || entIndex < 0 || entIndex > clientEntityList_->GetHighestEntityIndex())
    return -1;

  IClientEntity *clientEntity = clientEntityList_->GetClientEntity(entIndex);
  if (!clientEntity)
    return -1;

  C_BaseEntity *entity = clientEntity->GetBaseEntity();
  const auto found = entities_.find(entity);
  if (found == entities_.end())
    return -1;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  const CBaseHandle &handle = unknown->GetRefEHandle();
  if (!handle.IsValid() ||
      found->second.handleValue != static_cast<uint32_t>(handle.ToInt()))
    return -1;

  return static_cast<int>(found->second.handleValue | kClientEntRefMask);
}

datamap_t *CClientEntityManager::GetClientDataMap(void *entity) const
{
  if (!IsTrackedEntity(entity)) return nullptr;
  return GetDataMapUnchecked(entity);
}

datamap_t *CClientEntityManager::GetDataMapUnchecked(void *entity) const
{
  if (!entity || getDataDescMapOffset_ < 0) return nullptr;

  void **vtable = *reinterpret_cast<void ***>(entity);
  if (!vtable || !vtable[getDataDescMapOffset_])
    return nullptr;

#if defined _WIN32
  using GetDataDescMapFn = datamap_t *(__thiscall *)(void *);
#else
  using GetDataDescMapFn = datamap_t *(*)(void *);
#endif
  return reinterpret_cast<GetDataDescMapFn>(vtable[getDataDescMapOffset_])(entity);
}

ClientClass *CClientEntityManager::GetClientClass(void *entity) const
{
  if (!IsTrackedEntity(entity))
    return nullptr;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  IClientNetworkable *networkable = unknown->GetClientNetworkable();
  return networkable ? networkable->GetClientClass() : nullptr;
}

void *CClientEntityManager::GetClientRecvTableBase(void *entity) const
{
  if (!IsTrackedEntity(entity))
    return nullptr;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  IClientNetworkable *networkable = unknown->GetClientNetworkable();
  return networkable ? networkable->GetDataTableBasePtr() : nullptr;
}

CStandardRecvProxies *CClientEntityManager::GetStandardRecvProxies() const
{
  return standardRecvProxies_;
}

string_t CClientEntityManager::AllocPooledStringClient(const char *value) const
{
  return allocPooledStringClient_ ? allocPooledStringClient_(value) : NULL_STRING;
}

bool CClientEntityManager::RemoveClientOnlyEntity(void *entity) const
{
  if (!IsTrackedEntity(entity) || subRemoveOffset_ < 0)
    return false;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  IClientNetworkable *networkable = unknown->GetClientNetworkable();
  if (networkable && networkable->entindex() != -1)
    return false;

  void **vtable = *reinterpret_cast<void ***>(entity);
  if (!vtable || !vtable[subRemoveOffset_])
    return false;

#if defined _WIN32
  using SubRemoveFn = void (__thiscall *)(void *);
#else
  using SubRemoveFn = void (*)(void *);
#endif
  reinterpret_cast<SubRemoveFn>(vtable[subRemoveOffset_])(entity);
  return true;
}

int CClientEntityManager::ClientHandleToEntityRef(const void *address)
{
  if (!available_ || !address || !clientEntityList_)
    return -1;

  const CBaseHandle &handle = *static_cast<const CBaseHandle *>(address);
  if (!handle.IsValid())
    return -1;

  IClientUnknown *unknown = clientEntityList_->GetClientUnknownFromHandle(handle);
  if (!unknown || unknown->GetRefEHandle() != handle)
    return -1;

  C_BaseEntity *entity = unknown->GetBaseEntity();
  const auto found = entities_.find(entity);
  return found == entities_.end() ||
         found->second.handleValue != static_cast<uint32_t>(handle.ToInt())
    ? -1 : found->second.clientRef;
}

bool CClientEntityManager::EntityRefToClientHandle(int clientRef, void *address)
{
  if (!available_ || !address)
    return false;

  auto *destination = static_cast<CBaseHandle *>(address);
  if (clientRef == -1)
  {
    destination->Term();
    return true;
  }

  auto *entity = static_cast<C_BaseEntity *>(ResolveClientEntityRef(clientRef));
  if (!entity)
    return false;

  auto *unknown = reinterpret_cast<IClientUnknown *>(entity);
  const CBaseHandle &source = unknown->GetRefEHandle();
  if (!source.IsValid())
    return false;

  *destination = source;
  return true;
}

void CClientEntityManager::AddClientEntityListener(
  SourceMod::ICBaseNPCClientEntityListener *listener)
{
  if (available_ && listener && std::find(listeners_.begin(), listeners_.end(), listener) == listeners_.end())
    listeners_.push_back(listener);
}

void CClientEntityManager::RemoveClientEntityListener(
  SourceMod::ICBaseNPCClientEntityListener *listener)
{
  listeners_.erase(std::remove(listeners_.begin(), listeners_.end(), listener),
                   listeners_.end());
}

bool CClientEntityManager::IsTrackedEntity(void *entity) const
{
  return available_ && entity && entities_.find(static_cast<C_BaseEntity *>(entity)) != entities_.end();
}

const std::string& CClientEntityManager::EffectiveClassname(const EntityRecord& record) const
{
  static const std::string fallback("C_BaseEntity");
  if (!record.replicatedClassname.empty()) return record.replicatedClassname;
  if (!record.clientClassname.empty()) return record.clientClassname;
  return fallback;
}

void CClientEntityManager::ClearNetworkSlot(EntityRecord& record, C_BaseEntity* entity)
{
  if (record.networkSlot >= 0 && record.networkSlot < static_cast<int>(networkSidecars_.size())) {
    auto& slot = networkSidecars_[record.networkSlot];
    if (slot.entity == entity && slot.handleValue == record.handleValue) slot = {};
  }
  record.networkSlot = -1;
}

ClientClass *CClientEntityManager::GetAllClasses() const
{
  return available_ && clientDll_ ? clientDll_->GetAllClasses() : nullptr;
}

ClientClass *CClientEntityManager::FindClientClass(const char *networkName) const
{
  return CBaseNPCClientLookup::FindClass(GetAllClasses(), networkName);
}

RecvProp *CClientEntityManager::FindRecvProp(const char *networkClassName,
                                            const char *propName, int *actualOffset) const
{
  ClientClass *cc = FindClientClass(networkClassName);
  auto prop = CBaseNPCClientLookup::FindProp(cc ? cc->m_pRecvTable : nullptr, propName, actualOffset);
  if (actualOffset && CBaseNPCRecvTable::IsCustom(prop)) *actualOffset = -1;
  return prop;
}

void CClientEntityManager::Shutdown()
{
  PurgeEntities();
  available_ = false;
#ifndef CBASENPC_CLIENT_TESTS
  if (frameStageHook_) SH_REMOVE_HOOK_ID(frameStageHook_);
  if (levelShutdownHook_) SH_REMOVE_HOOK_ID(levelShutdownHook_);
#endif
  frameStageHook_ = levelShutdownHook_ = 0;
  UnregisterEngineListener();
  if (onEntityCreated_)
  {
    forwards->ReleaseForward(onEntityCreated_);
    onEntityCreated_ = nullptr;
  }
  if (onEntityDestroyed_)
  {
    forwards->ReleaseForward(onEntityDestroyed_);
    onEntityDestroyed_ = nullptr;
  }
  listeners_.clear();
  properties_.ClearCaches();
  entityOrder_.clear();
  entities_.clear();
  networkSidecars_.fill({});
  clientTools_ = nullptr;
  standardRecvProxies_ = nullptr;
  clientDll_ = nullptr;
  clientEntityList_ = nullptr;
  classCount_ = 0;
  getDataDescMapOffset_ = -1;
  subRemoveOffset_ = -1;
  allocPooledStringClient_ = nullptr;
}

void CClientEntityManager::DetachPluginConsumers()
{
  listeners_.clear();
  if (onEntityCreated_) forwards->ReleaseForward(onEntityCreated_);
  if (onEntityDestroyed_) forwards->ReleaseForward(onEntityDestroyed_);
  onEntityCreated_ = onEntityDestroyed_ = nullptr;
}

unsigned char* CClientEntityManager::GetSidecarAddress(void* entity, size_t offset, size_t size)
{
  if (CBaseNPCNetworkDebugEnabled()) ++sidecarPointerLookups_;
  auto found = entities_.find(static_cast<C_BaseEntity*>(entity));
  if (!available_ || found == entities_.end() || !found->second.sidecar ||
      offset > found->second.sidecarSize || size > found->second.sidecarSize - offset) return nullptr;
  return found->second.sidecar + offset;
}

unsigned char* CClientEntityManager::GetNetworkSidecarAddress(
  int objectId, void* object, size_t offset, size_t size)
{
  if (CBaseNPCNetworkDebugEnabled()) ++recvProxyCalls_;
  if (!available_ || objectId < 0 || objectId >= static_cast<int>(networkSidecars_.size())) {
    if (CBaseNPCNetworkDebugEnabled()) ++objectIdSlotMisses_;
    return nullptr;
  }
  const auto& slot = networkSidecars_[objectId];
  if (!slot.sidecar || slot.entity != object ||
      offset > slot.sidecarSize || size > slot.sidecarSize - offset) {
    if (CBaseNPCNetworkDebugEnabled()) ++objectIdSlotMisses_;
    return nullptr;
  }
  const auto& handle = reinterpret_cast<IClientUnknown*>(object)->GetRefEHandle();
  if (!handle.IsValid() || static_cast<uint32_t>(handle.ToInt()) != slot.handleValue) {
    if (CBaseNPCNetworkDebugEnabled()) ++objectIdSlotMisses_;
    return nullptr;
  }
  return slot.sidecar + offset;
}

void CClientEntityManager::RecordSlowRecvMetadataLookup()
{
  if (CBaseNPCNetworkDebugEnabled()) ++slowRecvMetadataLookups_;
}

void CClientEntityManager::RecordSidecarPoolUse(bool reused)
{
  if (!CBaseNPCNetworkDebugEnabled()) return;
  if (reused) ++sidecarPoolReuses_;
  else ++sidecarPoolAllocations_;
}

bool CClientEntityManager::AttachRuntime(C_BaseEntity* entity, CBaseNPCRuntimeClientClass* runtime)
{
  if (!entity || !runtime) return false;
  // Normally attached from the raw client-list callback inside stock Init().
  if (entities_.find(entity) == entities_.end()) TrackEntity(entity, true);
  auto found = entities_.find(entity);
  if (found == entities_.end() || found->second.deleting || found->second.removeNotified) return false;
  auto& record = found->second;
  if (record.runtimeClass) return record.runtimeClass == runtime && record.classHook && record.releaseHook;
  auto net = reinterpret_cast<IClientUnknown*>(entity)->GetClientNetworkable();
  if (!net) return false;
  record.sidecarSize = runtime->SidecarSize();
  bool reused = false;
  record.sidecar = runtime->AcquireSidecar(&reused);
  RecordSidecarPoolUse(reused);
  record.runtimeClass = runtime;
  const int networkSlot = net->entindex();
  if (networkSlot >= 0 && networkSlot < static_cast<int>(networkSidecars_.size())) {
    record.networkSlot = networkSlot;
    networkSidecars_[networkSlot] = {entity, record.sidecar, record.sidecarSize, record.handleValue};
  }
#ifndef CBASENPC_CLIENT_TESTS
  record.classHook = SH_ADD_HOOK(IClientNetworkable, GetClientClass, net, SH_MEMBER(this, &CClientEntityManager::Hook_GetClientClass), false);
  record.releaseHook = SH_ADD_HOOK(IClientNetworkable, Release, net, SH_MEMBER(this, &CClientEntityManager::Hook_Release), false);
#else
  record.classHook = record.releaseHook = 1;
#endif
  if (!record.classHook || !record.releaseHook) { CleanupRuntime(entity); return false; }
  record.clientClassname = runtime->storage.m_pMapClassname;
  return true;
}
void CClientEntityManager::RunPostConstructor(C_BaseEntity* entity)
{
  auto found = entities_.find(entity);
  if (found == entities_.end() || !found->second.runtimeClass || found->second.postConstructed || found->second.deleting) return;
  found->second.postConstructed = true;
  auto factory = found->second.runtimeClass->factory;
  if (factory) factory->Constructed(found->second.clientRef);
}
void CClientEntityManager::CleanupRuntime(C_BaseEntity* entity)
{
  auto found = entities_.find(entity);
  if (found == entities_.end() || found->second.removeNotified) return;
  found->second.removeNotified = true;
  const auto handle = found->second.handleValue;
  auto runtime = found->second.runtimeClass;
  if (runtime && runtime->factory && found->second.postConstructed) runtime->factory->Removed(found->second.clientRef);
  found = entities_.find(entity); // callbacks can rehash or recursively delete
  if (found == entities_.end() || found->second.handleValue != handle) return;
  auto& record = found->second;
  ClearNetworkSlot(record, entity);
#ifndef CBASENPC_CLIENT_TESTS
  if (record.classHook) SH_REMOVE_HOOK_ID(record.classHook);
  if (record.releaseHook) SH_REMOVE_HOOK_ID(record.releaseHook);
#endif
  record.classHook = record.releaseHook = 0;
  if (record.runtimeClass) record.runtimeClass->ReleaseSidecar(record.sidecar);
  record.sidecar = nullptr; record.sidecarSize = 0; record.runtimeClass = nullptr;
}
#ifndef CBASENPC_CLIENT_TESTS
ClientClass* CClientEntityManager::Hook_GetClientClass()
{
  auto entity = META_IFACEPTR(IClientNetworkable)->GetIClientUnknown()->GetBaseEntity();
  auto found = entities_.find(entity);
  if (found != entities_.end() && found->second.runtimeClass)
    RETURN_META_VALUE(MRES_SUPERCEDE, found->second.runtimeClass->Get());
  RETURN_META_VALUE(MRES_IGNORED, nullptr);
}
void CClientEntityManager::Hook_Release()
{
  auto entity = META_IFACEPTR(IClientNetworkable)->GetIClientUnknown()->GetBaseEntity();
  CleanupRuntime(entity);
  RETURN_META(MRES_IGNORED);
}
#endif
void CClientEntityManager::ReceiveClassname(void* object, const char* classname)
{
  auto found = entities_.find(static_cast<C_BaseEntity*>(object));
  if (found == entities_.end() || found->second.deleting) return;
  auto& record = found->second;
  record.replicatedClassname.assign(classname ? classname : "", classname ? strnlen(classname, CBASENPC_NETWORK_CLASSNAME_LENGTH - 1) : 0);
}
void CClientEntityManager::FlushPendingCreates()
{
  auto pending = std::move(pendingCreates_); pendingCreates_.clear();
  for (const auto& item : pending) {
    auto found = entities_.find(item.entity);
    if (found != entities_.end() && found->second.handleValue == item.handleValue &&
        found->second.replicatedClassname.empty() && found->second.clientClassname.empty()) {
      found->second.clientClassname = ReadClientClassname(item.entity);
      if (CBaseNPCNetworkDebugEnabled()) ++clientToolsClassnameFallbacks_;
    }
    NotifyCreated(item.entity, item.handleValue);
  }
}
void CClientEntityManager::PurgeEntities()
{
  if (purging_) return;
  purging_ = true;
  pendingCreates_.clear();
  const auto snapshot = entityOrder_;
  for (auto entity : snapshot) OnEntityDeleted(entity);
  entityOrder_.clear(); entities_.clear();
  networkSidecars_.fill({});
  purging_ = false;
}

void CClientEntityManager::DumpNetworkStats() const
{
#if defined(CBASENPC_CLIENT_TESTS)
  std::printf("[CBASENPC] network stats: RecvProxy=%llu slow-metadata-lookups=%llu ObjectID-misses=%llu sidecar-map-lookups=%llu pool-allocations=%llu pool-reuses=%llu client-tools-classname-fallbacks=%llu property-cache-hits=%llu property-cache-misses=%llu\n",
#else
  Msg("[CBASENPC] network stats: RecvProxy=%llu slow-metadata-lookups=%llu ObjectID-misses=%llu sidecar-map-lookups=%llu pool-allocations=%llu pool-reuses=%llu client-tools-classname-fallbacks=%llu property-cache-hits=%llu property-cache-misses=%llu\n",
#endif
    static_cast<unsigned long long>(recvProxyCalls_),
    static_cast<unsigned long long>(slowRecvMetadataLookups_),
    static_cast<unsigned long long>(objectIdSlotMisses_),
    static_cast<unsigned long long>(sidecarPointerLookups_),
    static_cast<unsigned long long>(sidecarPoolAllocations_),
    static_cast<unsigned long long>(sidecarPoolReuses_),
    static_cast<unsigned long long>(clientToolsClassnameFallbacks_),
    static_cast<unsigned long long>(properties_.CacheHits()),
    static_cast<unsigned long long>(properties_.CacheMisses()));
}

bool CClientEntityManager::GetRuntimeDiagnostics(int ref, bool& runtime, size_t& sidecarSize,
 const char*& table, const char*& physical)
{
  auto entity = static_cast<C_BaseEntity*>(ResolveClientEntityRef(ref));
  auto found = entities_.find(entity);
  if (found == entities_.end()) return false;
  auto cc = GetClientClass(entity);
  runtime = found->second.runtimeClass != nullptr;
  sidecarSize = found->second.sidecarSize;
  table = cc && cc->m_pRecvTable ? cc->m_pRecvTable->GetName() : "";
  physical = runtime ? found->second.runtimeClass->physical->m_pNetworkName : (cc ? cc->m_pNetworkName : "");
  return true;
}
#ifndef CBASENPC_CLIENT_TESTS
void CClientEntityManager::Hook_FrameStageNotify(ClientFrameStage_t stage)
{ if (stage == FRAME_NET_UPDATE_END) FlushPendingCreates(); RETURN_META(MRES_IGNORED); }
void CClientEntityManager::Hook_LevelShutdown()
{ PurgeEntities(); RETURN_META(MRES_IGNORED); }
CON_COMMAND(cbasenpc_dump_network_stats, "Dump CBaseNPC client networking hot-path counters")
{ g_ClientEntityManager.DumpNetworkStats(); }
#endif
