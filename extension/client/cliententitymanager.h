#ifndef _INCLUDE_CBASENPC_CLIENT_ENTITY_MANAGER_H_
#define _INCLUDE_CBASENPC_CLIENT_ENTITY_MANAGER_H_

#include "smsdk_ext.h"
#include "shared/ICBaseNPCClientEntityManager.h"
#include "cliententityproperties.h"

#include <basehandle.h>
#include <client_class.h>
#include <cdll_int.h>
#include <datamap.h>
#include <dt_recv.h>
#include <icliententity.h>
#include <icliententitylist.h>
#include <iclientnetworkable.h>
#include <iclientunknown.h>
#include <string_t.h>
#include <tier1/utlvector.h>
#include <toolframework/itoolentity.h>

#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class C_BaseEntity;
struct CBaseNPCRuntimeClientClass;

struct CBaseNPCClientEntityAccess
{
  C_BaseEntity *entity = nullptr;
  IClientNetworkable *networkable = nullptr;
  ClientClass *clientClass = nullptr;
  datamap_t *dataMap = nullptr;
  unsigned char *recvBase = nullptr;
  unsigned char *sidecar = nullptr;
  size_t sidecarSize = 0;
  CBaseNPCRuntimeClientClass *runtimeClass = nullptr;
  const char *classname = nullptr;
  uint32_t handleValue = 0;
  int clientRef = -1;
};

// This internal client.dll callback contract is intentionally mirrored here.
// It is not part of the public IClientEntityList interface.
namespace CBaseNPCClient
{
class IClientEntityListener
{
public:
  virtual void OnEntityCreated(C_BaseEntity *entity) {}
  virtual void OnEntityDeleted(C_BaseEntity *entity) {}
};
}

class CClientEntityManager final :
  public CBaseNPCClient::IClientEntityListener,
  public SourceMod::ICBaseNPCClientEntityManager
{
public:
  bool Initialize(SourceMod::IGameConfig *gameConfig, char *error, size_t maxlength);
  void Shutdown();
  void DetachPluginConsumers();
  bool IsAvailable() const override { return available_; }

  IBaseClientDLL *GetBaseClientDLL() const { return clientDll_; }
  IClientEntityList *GetClientEntityList() const { return clientEntityList_; }
  size_t GetClassCount() const { return classCount_; }
  ClientClass *GetAllClasses() const;
  ClientClass *FindClientClass(const char *networkName) const;
  RecvProp *FindRecvProp(const char *networkClassName, const char *propName,
                         int *actualOffset = nullptr) const;
  CClientEntityProperties &GetProperties() { return properties_; }

  void OnEntityCreated(C_BaseEntity *entity) override;
  void OnEntityDeleted(C_BaseEntity *entity) override;

  void *ResolveClientEntityRef(int clientRef) override;
  bool ResolveAccess(int clientRef, CBaseNPCClientEntityAccess& access);
  int EntityToClientRef(void *entity) override;
  int EntityToClientHandleRef(void *entity) override;
  bool IsSameClientEntity(void *entity, int clientHandleRef) override;
  int ClientHandleToEntityRef(const void *handleAddress) override;
  bool EntityRefToClientHandle(int clientRef, void *handleAddress) override;
  int GetClientEntityCount() const override;
  int GetEntityRefByOrdinalClient(int ordinal) const override;
  const char *GetEntityClassnameClient(int clientRef) override;
  void AddClientEntityListener(SourceMod::ICBaseNPCClientEntityListener *listener) override;
  void RemoveClientEntityListener(SourceMod::ICBaseNPCClientEntityListener *listener) override;

  int FindClientEntityByClassname(int startRef, const char *classname);
  bool GetEntityClassnameDiagnostics(int clientRef, int *entIndex,
                                     const char **networkName, const char **mapClassname,
                                     const char **clientClassname, const char **replicatedClassname,
                                     const char **classname);
  int EntRefToEntIndexClient(int clientRef) const;
  int EntIndexToEntRefClient(int entIndex) const;

  datamap_t *GetClientDataMap(void *entity) const;
  ClientClass *GetClientClass(void *entity) const;
  void *GetClientRecvTableBase(void *entity) const;
  CStandardRecvProxies *GetStandardRecvProxies() const;
  string_t AllocPooledStringClient(const char *value) const;
  bool RemoveClientOnlyEntity(void *entity) const;

  bool AttachRuntime(C_BaseEntity* entity, CBaseNPCRuntimeClientClass* runtime);
  void RunPostConstructor(C_BaseEntity* entity);
  unsigned char* GetSidecarAddress(void* entity, size_t offset, size_t size);
  unsigned char* GetNetworkSidecarAddress(int objectId, void* entity, size_t offset, size_t size);
  void RecordSlowRecvMetadataLookup();
  void RecordSidecarPoolUse(bool reused);
  void ResetNetworkStats();
  void DumpNetworkStats() const;
  void ReceiveClassname(void* entity, const char* classname);
  bool GetRuntimeDiagnostics(int ref, bool& runtime, size_t& sidecarSize, const char*& table, const char*& physical);
  void FlushPendingCreates();
  void PurgeEntities();
  ClientClass* Hook_GetClientClass();
  void Hook_Release();
  void Hook_FrameStageNotify(ClientFrameStage_t stage);
  void Hook_LevelShutdown();

private:
#if defined(CBASENPC_CLIENT_TESTS)
  friend struct CClientEntityManagerTestAccess;
#endif
  struct EntityRecord
  {
    int clientRef;
    uint32_t handleValue;
    bool deleting = false;
    std::string clientClassname;
    std::string replicatedClassname;
    CBaseNPCRuntimeClientClass* runtimeClass = nullptr;
    unsigned char* sidecar = nullptr;
    size_t sidecarSize = 0;
    int networkSlot = -1;
    int classHook = 0, releaseHook = 0;
    bool createdNotified = false, postConstructed = false, removeNotified = false;
  };

  using EngineListenerVector = CUtlVector<CBaseNPCClient::IClientEntityListener *>;

  bool ResolveClientInterfaces(char *error, size_t maxlength);
  bool ResolvePropertyAccess(SourceMod::IGameConfig *gameConfig, char *error, size_t maxlength);
  bool RegisterEngineListener(SourceMod::IGameConfig *gameConfig, char *error, size_t maxlength);
  void UnregisterEngineListener();
  void SeedExistingEntities();
  void TrackEntity(C_BaseEntity *entity, bool notify);
  std::string ReadClientClassname(C_BaseEntity *entity) const;
  const std::string& EnsureEffectiveClassname(C_BaseEntity* entity, EntityRecord& record);
  void NotifyCreated(C_BaseEntity* entity, uint32_t handleValue);
  void CleanupRuntime(C_BaseEntity* entity);
  bool IsTrackedEntity(void *entity) const;
  EntityRecord* ResolveRecord(int clientRef, C_BaseEntity** entity = nullptr);
  datamap_t* GetDataMapUnchecked(void* entity) const;
  const std::string& EffectiveClassname(const EntityRecord& record) const;
  void ClearNetworkSlot(EntityRecord& record, C_BaseEntity* entity);

private:
  bool available_ = false;
  bool purging_ = false;
  size_t classCount_ = 0;
  CClientEntityProperties properties_;
  IClientEntityList *clientEntityList_ = nullptr;
  IBaseClientDLL *clientDll_ = nullptr;
  IClientTools *clientTools_ = nullptr;
  CStandardRecvProxies *standardRecvProxies_ = nullptr;
  EngineListenerVector *engineListeners_ = nullptr;
  bool engineListenerRegistered_ = false;
  IForward *onEntityCreated_ = nullptr;
  IForward *onEntityDestroyed_ = nullptr;
  std::unordered_map<C_BaseEntity *, EntityRecord> entities_;
  struct NetworkSidecarSlot {
    C_BaseEntity* entity = nullptr;
    unsigned char* sidecar = nullptr;
    size_t sidecarSize = 0;
    uint32_t handleValue = 0;
  };
  std::array<NetworkSidecarSlot, MAX_EDICTS> networkSidecars_{};
  std::vector<C_BaseEntity *> entityOrder_;
  std::vector<SourceMod::ICBaseNPCClientEntityListener *> listeners_;
  struct PendingCreate { C_BaseEntity* entity; uint32_t handleValue; };
  std::vector<PendingCreate> pendingCreates_;
  int frameStageHook_ = 0, levelShutdownHook_ = 0;
  int getDataDescMapOffset_ = -1;
  int subRemoveOffset_ = -1;
  string_t (*allocPooledStringClient_)(const char *) = nullptr;
  uint64_t recvProxyCalls_ = 0, objectIdSlotMisses_ = 0;
  uint64_t slowRecvMetadataLookups_ = 0, sidecarPointerLookups_ = 0;
  uint64_t sidecarPoolAllocations_ = 0, sidecarPoolReuses_ = 0;
  uint64_t clientToolsClassnameFallbacks_ = 0;
  uint64_t networkStatsGeneration_ = 0;
#if defined(CBASENPC_CLIENT_TESTS)
  bool testClassnameBridgeEnabled_ = false;
  std::string (*testClassnameLookup_)(C_BaseEntity*) = nullptr;
#endif
};

extern CClientEntityManager g_ClientEntityManager;

#endif
