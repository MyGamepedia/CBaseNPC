#ifndef CBASENPC_SERVER_API_H
#define CBASENPC_SERVER_API_H
#include "ICBaseNPCShared.h"
class CBaseEntity;
class ServerClass;
struct datamap_t;
#define SMINTERFACE_CBASENPC_SERVER_NAME "ICBaseNPCServer"
#define SMINTERFACE_CBASENPC_SERVER_VERSION 1
namespace SourceMod
{
// Server-only primitives, usable on Windows/Linux dedicated servers. BMS-only
// methods report unavailable in TF2; the native catalog reflects the build.
class ICBaseNPCServer : public SMInterface, public ICBaseNPCNativeAPI
{
public:
  const char* GetInterfaceName() override { return SMINTERFACE_CBASENPC_SERVER_NAME; }
  unsigned int GetInterfaceVersion() override { return SMINTERFACE_CBASENPC_SERVER_VERSION; }
  virtual bool IsCoreInitialized() const = 0;
  virtual bool IsRegistrationOpen() const = 0;
  virtual bool IsFinalized() const = 0;
  virtual bool IsPublished() const = 0;
  virtual bool IsClassnameBridgeEnabled() const = 0;
  virtual const char* GetRegistrationError() const = 0;
  virtual bool SetDTPropBits(IdentityToken_t* owner, const char* dataTable,
                             const char* property, int bits, char* error,
                             size_t maxlength) = 0;
  virtual CBaseEntity* ResolveEntityRef(int entityRef) const = 0;
  virtual int EntityToRef(CBaseEntity* entity) const = 0;
  virtual datamap_t* GetDataMap(CBaseEntity* entity) const = 0;
  virtual ServerClass* FindServerClass(const char* networkName) const = 0;
  // SubscribeForward also accepts OnCBaseNPCInitialized() and
  // OnCBaseNPCNetworkSchemaFinalized(bool success), plus the server forwards
  // declared in the includes. State getters cover already-delivered events.
};
}
#endif
