#ifndef _INCLUDE_ICBASENPC_CLIENT_ENTITY_MANAGER_H_
#define _INCLUDE_ICBASENPC_CLIENT_ENTITY_MANAGER_H_

#include <IShareSys.h>

#define SMINTERFACE_CBASENPC_CLIENT_ENTITY_MANAGER_NAME "ICBaseNPCClientEntityManager"
#define SMINTERFACE_CBASENPC_CLIENT_ENTITY_MANAGER_VERSION 1

// ResolveClientEntityRef accepts a networked entindex, a client-only raw
// CBaseHandle::ToInt(), or a proper high-bit-marked client handle containing
// both the entry index and serial.

namespace SourceMod
{
class ICBaseNPCClientEntityListener
{
public:
  virtual ~ICBaseNPCClientEntityListener() = default;
  // Networked creation is delivered after initial decode at FRAME_NET_UPDATE_END.
  // Client-only creation is immediate; seeding existing entities is silent.
  // Classname prefers the per-instance value received from the server, not a
  // direct server-entity lookup. Hold a proper handle ref across callbacks.
  virtual void OnClientEntityCreated(void *entity, int clientRef, const char *classname) {}
  virtual void OnClientEntityDestroyed(void *entity, int clientRef) {}
};

class ICBaseNPCClientEntityManager : public SMInterface
{
public:
  const char *GetInterfaceName() override
  {
    return SMINTERFACE_CBASENPC_CLIENT_ENTITY_MANAGER_NAME;
  }

  unsigned int GetInterfaceVersion() override
  {
    return SMINTERFACE_CBASENPC_CLIENT_ENTITY_MANAGER_VERSION;
  }

public:
  virtual bool IsAvailable() const = 0;
  virtual void *ResolveClientEntityRef(int clientRef) = 0;
  virtual int EntityToClientRef(void *entity) = 0;
  virtual int EntityToClientHandleRef(void *entity) = 0;
  virtual bool IsSameClientEntity(void *entity, int clientHandleRef) = 0;
  virtual int ClientHandleToEntityRef(const void *handleAddress) = 0;
  virtual bool EntityRefToClientHandle(int clientRef, void *handleAddress) = 0;
  virtual int GetClientEntityCount() const = 0;
  virtual int GetEntityRefByOrdinalClient(int ordinal) const = 0;
  virtual const char *GetEntityClassnameClient(int clientRef) = 0;
  virtual void AddClientEntityListener(ICBaseNPCClientEntityListener *listener) = 0;
  virtual void RemoveClientEntityListener(ICBaseNPCClientEntityListener *listener) = 0;
};
}

#endif
