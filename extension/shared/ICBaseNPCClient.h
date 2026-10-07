#ifndef CBASENPC_CLIENT_API_H
#define CBASENPC_CLIENT_API_H
#include "ICBaseNPCShared.h"
#define SMINTERFACE_CBASENPC_CLIENT_NAME "ICBaseNPCClient"
#define SMINTERFACE_CBASENPC_CLIENT_VERSION 2
namespace SourceMod
{
class ICBaseNPCClientEntityListener
{
public:
  virtual ~ICBaseNPCClientEntityListener() = default;
  // Creation follows initial decode at FRAME_NET_UPDATE_END. Existing-entity
  // seeding is silent. References are opaque tokens, never raw engine handles.
  virtual void OnClientEntityCreated(void*, int, const char*) {}
  virtual void OnClientEntityDestroyed(void*, int) {}
};
class ICBaseNPCClient : public SMInterface, public ICBaseNPCNativeAPI
{
public:
  const char* GetInterfaceName() override { return SMINTERFACE_CBASENPC_CLIENT_NAME; }
  unsigned int GetInterfaceVersion() override { return SMINTERFACE_CBASENPC_CLIENT_VERSION; }
  bool IsVersionCompatible(unsigned int version) override { return version == 1 || version == 2; }
  // Published on every supported server build. On dedicated/TF2 IsAvailable
  // is false; identity calls return their invalid/empty result. BMS client
  // declarations remain usable on dedicated as with SourcePawn.
  virtual bool IsAvailable() const = 0;
  virtual void* ResolveClientEntityRef(int clientRef) = 0;
  virtual int EntityToClientRef(void* entity) = 0;
  virtual int EntityToClientHandleRef(void* entity) = 0;
  virtual bool IsSameClientEntity(void* entity, int clientHandleRef) = 0;
  virtual int ClientHandleToEntityRef(const void* handleAddress) = 0;
  virtual bool EntityRefToClientHandle(int clientRef, void* handleAddress) = 0;
  virtual int GetClientEntityCount() const = 0;
  virtual int GetEntityRefByOrdinalClient(int ordinal) const = 0;
  virtual const char* GetEntityClassnameClient(int clientRef) = 0;
  virtual int FindClientEntityByClassname(int startRef, const char* classname) = 0;
  virtual int EntRefToEntIndexClient(int clientRef) const = 0;
  virtual int EntIndexToEntRefClient(int entIndex) const = 0;
  // Remove the listener before its owner module unloads. SubscribeForward
  // provides consumer-owned equivalents of both SourcePawn client forwards.
  virtual void AddClientEntityListener(ICBaseNPCClientEntityListener* listener) = 0;
  virtual void RemoveClientEntityListener(ICBaseNPCClientEntityListener* listener) = 0;
};
}
#endif
