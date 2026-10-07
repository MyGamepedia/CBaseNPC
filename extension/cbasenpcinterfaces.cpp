#include "cbasenpcinterfaces.h"
#include "nativecontext.h"
#include "nativehandlepolicy.h"
#include "shared/ICBaseNPCServer.h"
#include "shared/ICBaseNPCClient.h"
#include "extension.h"
#include "sourcesdk/cbasenpcnetworkschema.h"
#include "sourcesdk/cbasenpcserverclass.h"
#if SOURCE_ENGINE == SE_BMS
#include "client/cliententitymanager.h"
#endif
#include <algorithm>
#include <cstring>
#include <map>

extern std::vector<sp_nativeinfo_t> gNatives;
extern bool m_bInitialized;
using namespace SourceMod;
namespace {
bool Error(char* error, size_t length, const char* message)
{ if (error && length) std::snprintf(error, length, "%s", message); return false; }
enum class Side { Server, Client };
bool ClientNative(const char* name)
{
  return !std::strncmp(name, "CClientEntity.", std::strlen("CClientEntity.")) ||
    !std::strncmp(name, "CClientEntityManager.", std::strlen("CClientEntityManager.")) ||
    !std::strncmp(name, "CEntityFactoryClient.", std::strlen("CEntityFactoryClient."));
}
bool ForwardOnSide(const char* name, Side side)
{
  if (!name) return false;
  if (side == Side::Client)
    return !std::strcmp(name, "OnEntityCreatedClient") || !std::strcmp(name, "OnEntityDestroyedClient");
  return !std::strcmp(name, "CEntityFactory_OnInstalled") ||
    !std::strcmp(name, "CEntityFactory_OnUninstalled") ||
    !std::strcmp(name, "CBaseCombatCharacter_EventKilled") ||
    !std::strcmp(name, "OnCBaseNPCInitialized") ||
    !std::strcmp(name, "OnCBaseNPCNetworkSchemaFinalized");
}
struct Consumer;
std::vector<Consumer*> consumers;
CBaseNPCNativeHandlePolicy handlePolicy;
bool running = false;

struct Consumer final : ICBaseNPCConsumer
{
  explicit Consumer(IdentityToken_t* owner, Side side) : context(owner), side(side) {}
  CBaseNPCNativeContext context;
  Side side;
  bool closing = false;
  struct OwnedHandle { Handle_t handle; IdentityToken_t* owner; };
  std::vector<OwnedHandle> handles;
  std::vector<std::pair<std::string, cell_t>> subscriptions;
  CBaseNPCInitializationDelivery initializedCallbacks;
  bool InvokeNative(const char* name, const sp::CallArgs& args, cell_t* result,
                     char* error, size_t maxlength) override {
    if (result) *result = 0;
    if (error && maxlength) *error = '\0';
    if (!running || closing) return Error(error, maxlength, "CBaseNPC consumer is stopped");
    if (!name || !*name || ClientNative(name) != (side == Side::Client))
      return Error(error, maxlength, "Native does not belong to this interface side");
    auto entry = std::find_if(gNatives.begin(), gNatives.end(), [name](const sp_nativeinfo_t& e) {
      return e.name && !std::strcmp(name, e.name);
    });
    if (entry == gNatives.end()) return Error(error, maxlength, "Native is unavailable in this build");
    // Network declarations and schema queries are independent of deferred core
    // initialization; ordinary server natives require the initialized core.
    if (side == Side::Server && !m_bInitialized && std::strcmp(name, "SetDTPropBits"))
      return Error(error, maxlength, "CBaseNPC core is not initialized yet");
    context.EnterCall();
    struct Guard {
      CBaseNPCNativeContext& c;
      CBaseNPCNativeContext::Frame* previous;
      ~Guard() { c.SetFrame(previous); c.LeaveCall(); }
    } guard{context, context.SetFrame(nullptr)};
    try {
      CBaseNPCNativeContext::Frame frame;
      frame.Prepare(args);
      context.SetFrame(&frame);
      cell_t value = entry->func(&context, frame.params);
      frame.CopyBack();
      if (result) *result = value;
      return true;
    } catch (const std::exception& ex) { return Error(error, maxlength, ex.what()); }
      catch (...) { return Error(error, maxlength, "Unexpected C++ exception in CBaseNPC native"); }
  }
  cell_t RegisterCallback(ICBaseNPCCallback* callback) override {
    return running && !closing ? context.RegisterCallback(callback) : 0;
  }
  bool RemoveCallback(cell_t callback) override { return context.RemoveCallback(callback); }
  bool SubscribeForward(const char* name, cell_t callback, char* error, size_t maxlength) override {
    if (!running || closing || !ForwardOnSide(name, side))
      return Error(error, maxlength, "Unknown/unavailable CBaseNPC forward on this side");
    try {
      auto function = context.GetFunctionById(callback);
      if (!function || !function->IsRunnable()) return Error(error, maxlength, "Callback is null or disabled");
      std::pair<std::string, cell_t> entry{name, callback};
      if (std::find(subscriptions.begin(), subscriptions.end(), entry) == subscriptions.end()) {
        subscriptions.push_back(std::move(entry));
        // Initialization may precede interface publication during SDK_OnLoad.
        // Replay only this new subscription, never all existing listeners.
        if (!std::strcmp(name, "OnCBaseNPCInitialized") && m_bInitialized && initializedCallbacks.Mark(callback)) {
          cell_t result = 0;
          if (!context.InvokeCallback(callback, sp::CallArgs(), &result)) {
            UnsubscribeForward(name, callback);
            return Error(error, maxlength, "Initialization callback failed during state replay");
          }
        }
      }
      return true;
    } catch (const std::exception& ex) { return Error(error, maxlength, ex.what()); }
  }
  void UnsubscribeForward(const char* name, cell_t callback) override {
    if (!name) return;
    if (!std::strcmp(name, "OnCBaseNPCInitialized")) initializedCallbacks.Remove(callback);
    std::pair<std::string, cell_t> entry{name, callback};
    subscriptions.erase(std::remove(subscriptions.begin(), subscriptions.end(), entry), subscriptions.end());
  }
  bool CloseHandle(Handle_t handle, char* error, size_t maxlength) override {
    if (context.ActiveCalls()) return Error(error, maxlength, "Cannot close a consumer handle during its invocation");
    auto found = std::find_if(handles.begin(), handles.end(), [handle](const OwnedHandle& owned) { return owned.handle == handle; });
    if (found == handles.end()) return Error(error, maxlength, "Handle was not created by this consumer");
    HandleSecurity security(found->owner, myself->GetIdentity());
    auto status = handlesys->FreeHandle(handle, &security);
    if (status != HandleError_None) return Error(error, maxlength, HandleErrorToString(status));
    handles.erase(std::remove_if(handles.begin(), handles.end(), [handle](const OwnedHandle& owned) { return owned.handle == handle; }), handles.end());
    return true;
  }
  IdentityToken_t* GetOwnerIdentity() const override {
    void* identity = nullptr;
    const_cast<CBaseNPCNativeContext&>(context).GetKey(1, &identity);
    return static_cast<IdentityToken_t*>(identity);
  }
  cell_t ToAddress(const void* address) const override { return PtrToPawnAddress(address); }
  void* FromAddress(cell_t address) const override { return PawnAddressToPtr(address); }
  bool SetHandleGrant(Handle_t handle, ICBaseNPCConsumer* recipient, bool grant,
                       char* error, size_t maxlength) {
    auto found = std::find_if(consumers.begin(), consumers.end(), [recipient](Consumer* c) { return c == recipient; });
    if (closing || !running || found == consumers.end() || (*found)->closing || (*found)->side != side)
      return Error(error, maxlength, "An active recipient consumer on the same side is required");
    HandleSecurity security(GetOwnerIdentity(), myself->GetIdentity());
    void* object = nullptr;
    auto status = handlesys->ReadHandle(handle, 0, &security, &object);
    if (status != HandleError_None) return Error(error, maxlength, HandleErrorToString(status));
    // ReadHandle's default identity-only access is not an owner check.
    bool success = grant ? handlePolicy.Grant(object, GetOwnerIdentity(), *found)
                         : handlePolicy.Revoke(object, GetOwnerIdentity(), *found);
    return success || Error(error, maxlength, "Only the original CBaseNPC object owner can grant or revoke mutation rights");
  }
  bool GrantHandleMutation(Handle_t handle, ICBaseNPCConsumer* recipient, char* error, size_t maxlength) override {
    return SetHandleGrant(handle, recipient, true, error, maxlength);
  }
  bool RevokeHandleMutation(Handle_t handle, ICBaseNPCConsumer* recipient, char* error, size_t maxlength) override {
    return SetHandleGrant(handle, recipient, false, error, maxlength);
  }
  void Close() {
    closing = true;
    context.Deactivate();
    subscriptions.clear();
    initializedCallbacks.Clear();
    handlePolicy.RemoveScope(this);
    // Metadata copies have no extension callbacks. A published schema must
    // never be reverted when an extension consumer releases its declarations.
    if (side == Side::Server) g_CBaseNPCServerClassManager.RemoveDTPropBitsPatches(GetOwnerIdentity());
    auto owned = std::move(handles);
    for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
      HandleSecurity security(it->owner, myself->GetIdentity());
      handlesys->FreeHandle(it->handle, &security);
    }
  }
};

void RetireConsumer(Consumer* consumer)
{
  consumer->Close();
  // A consumer may attach its callback to somebody else's locomotion/action
  // factory, or its input delegate to a shared datamap. We cannot free those
  // objects on consumer unload. Keep detached adapters alive as process-lifetime
  // tombstones; no consumer extension code or owned handles remain. Never
  // reclaim/reuse these adapter addresses.
  static auto* retired = new std::vector<std::unique_ptr<Consumer>>;
  retired->emplace_back(consumer);
}

template<Side side, typename Interface>
class NativeAPI : public Interface
{
public:
  size_t GetNativeCount() const override {
    size_t count = 0;
    for (const auto& n : gNatives) if (n.name && ClientNative(n.name) == (side == Side::Client)) ++count;
    return count;
  }
  const char* GetNativeName(size_t index) const override {
    for (const auto& n : gNatives) if (n.name && ClientNative(n.name) == (side == Side::Client))
      if (!index--) return n.name;
    return nullptr;
  }
  bool HasNative(const char* name) const override {
    if (!name || ClientNative(name) != (side == Side::Client)) return false;
    for (const auto& n : gNatives) if (n.name && !std::strcmp(n.name, name)) return true;
    return false;
  }
  ICBaseNPCConsumer* CreateConsumer(IExtension* owner, char* error, size_t maxlength) override {
    if (!running || !owner || !owner->GetIdentity()) {
      Error(error, maxlength, "An extension owner identity and a running CBaseNPC are required"); return nullptr;
    }
    for (auto consumer : consumers) if (consumer->side == side && consumer->GetOwnerIdentity() == owner->GetIdentity())
      return consumer;
    auto consumer = new Consumer(owner->GetIdentity(), side);
    consumers.push_back(consumer);
    return consumer;
  }
  bool ReleaseConsumer(ICBaseNPCConsumer* consumer, char* error, size_t maxlength) override {
    auto found = std::find_if(consumers.begin(), consumers.end(), [consumer](Consumer* entry) { return entry == consumer; });
    if (found == consumers.end() || (*found)->side != side)
      return Error(error, maxlength, "Consumer does not belong to this interface");
    auto owned = *found;
    if (owned->context.ActiveCalls()) return Error(error, maxlength, "Cannot release a consumer during its invocation");
    consumers.erase(found);
    RetireConsumer(owned);
    return true;
  }
};
class ServerAPI final : public NativeAPI<Side::Server, ICBaseNPCServer>
{
public:
  bool IsCoreInitialized() const override { return running && m_bInitialized; }
  bool IsRegistrationOpen() const override { return running && g_CBaseNPCServerClassManager.IsRegistrationOpen(); }
  bool IsFinalized() const override { return running && g_CBaseNPCNetworkSchemaManager.IsFinalized(); }
  bool IsPublished() const override { return g_CBaseNPCNetworkSchemaManager.IsPublished(); }
  bool IsClassnameBridgeEnabled() const override { return g_CBaseNPCNetworkSchemaManager.IsClassnameBridgeEnabled(); }
  const char* GetRegistrationError() const override { return g_CBaseNPCServerClassManager.RegistrationError(); }
  bool SetDTPropBits(IdentityToken_t* owner, const char* table, const char* prop, int bits,
                     char* error, size_t maxlength) override {
#if SOURCE_ENGINE == SE_BMS
    auto found = std::find_if(consumers.begin(), consumers.end(), [owner](Consumer* c) {
      return c->side == Side::Server && !c->closing && c->GetOwnerIdentity() == owner;
    });
    if (!running || !owner || found == consumers.end())
      return Error(error, maxlength, "Create a server consumer for the extension before registering a bit patch");
    return g_CBaseNPCServerClassManager.RegisterDTPropBitsPatch(table, prop, bits, error, maxlength, owner);
#else
    return Error(error, maxlength, "SetDTPropBits is only available in the Black Mesa build");
#endif
  }
  CBaseEntity* ResolveEntityRef(int ref) const override { return running ? gamehelpers->ReferenceToEntity(ref) : nullptr; }
  int EntityToRef(CBaseEntity* entity) const override { return running && entity ? gamehelpers->EntityToReference(entity) : -1; }
  datamap_t* GetDataMap(CBaseEntity* entity) const override { return IsCoreInitialized() && entity ? gamehelpers->GetDataMap(entity) : nullptr; }
  ServerClass* FindServerClass(const char* name) const override { return running ? g_CBaseNPCServerClassManager.FindStockOrCustomClass(name) : nullptr; }
} server;
class ClientAPI final : public NativeAPI<Side::Client, ICBaseNPCClient>
{
public:
#if SOURCE_ENGINE == SE_BMS
  bool IsAvailable() const override { return running && g_ClientEntityManager.IsAvailable(); }
  void* ResolveClientEntityRef(int ref) override { return IsAvailable() ? g_ClientEntityManager.ResolveClientEntityRef(ref) : nullptr; }
  int EntityToClientRef(void* entity) override { return IsAvailable() ? g_ClientEntityManager.EntityToClientRef(entity) : -1; }
  int EntityToClientHandleRef(void* entity) override { return IsAvailable() ? g_ClientEntityManager.EntityToClientHandleRef(entity) : -1; }
  bool IsSameClientEntity(void* entity, int ref) override { return IsAvailable() && g_ClientEntityManager.IsSameClientEntity(entity, ref); }
  int ClientHandleToEntityRef(const void* address) override { return IsAvailable() ? g_ClientEntityManager.ClientHandleToEntityRef(address) : -1; }
  bool EntityRefToClientHandle(int ref, void* address) override { return IsAvailable() && g_ClientEntityManager.EntityRefToClientHandle(ref, address); }
  int GetClientEntityCount() const override { return IsAvailable() ? g_ClientEntityManager.GetClientEntityCount() : 0; }
  int GetEntityRefByOrdinalClient(int ordinal) const override { return IsAvailable() ? g_ClientEntityManager.GetEntityRefByOrdinalClient(ordinal) : -1; }
  const char* GetEntityClassnameClient(int ref) override { return IsAvailable() ? g_ClientEntityManager.GetEntityClassnameClient(ref) : nullptr; }
  int FindClientEntityByClassname(int start, const char* name) override { return IsAvailable() ? g_ClientEntityManager.FindClientEntityByClassname(start, name) : -1; }
  int EntRefToEntIndexClient(int ref) const override { return IsAvailable() ? g_ClientEntityManager.EntRefToEntIndexClient(ref) : -1; }
  int EntIndexToEntRefClient(int index) const override { return IsAvailable() ? g_ClientEntityManager.EntIndexToEntRefClient(index) : -1; }
  void AddClientEntityListener(ICBaseNPCClientEntityListener* listener) override { if (IsAvailable()) g_ClientEntityManager.AddClientEntityListener(listener); }
  void RemoveClientEntityListener(ICBaseNPCClientEntityListener* listener) override { g_ClientEntityManager.RemoveClientEntityListener(listener); }
#else
  bool IsAvailable() const override { return false; }
  void* ResolveClientEntityRef(int) override { return nullptr; }
  int EntityToClientRef(void*) override { return -1; }
  int EntityToClientHandleRef(void*) override { return -1; }
  bool IsSameClientEntity(void*, int) override { return false; }
  int ClientHandleToEntityRef(const void*) override { return -1; }
  bool EntityRefToClientHandle(int, void*) override { return false; }
  int GetClientEntityCount() const override { return 0; }
  int GetEntityRefByOrdinalClient(int) const override { return -1; }
  const char* GetEntityClassnameClient(int) override { return nullptr; }
  int FindClientEntityByClassname(int, const char*) override { return -1; }
  int EntRefToEntIndexClient(int) const override { return -1; }
  int EntIndexToEntRefClient(int) const override { return -1; }
  void AddClientEntityListener(ICBaseNPCClientEntityListener*) override {}
  void RemoveClientEntityListener(ICBaseNPCClientEntityListener*) override {}
#endif
} client;
}

void CBaseNPCPublishInterfaces()
{
  running = true;
  sharesys->AddInterface(myself, &server);
  sharesys->AddInterface(myself, &client);
}
void CBaseNPCShutdownInterfaces()
{
  running = false;
  // Dependency unload normally releases consumers first. If CBaseNPC itself
  // is being forcibly unloaded, detach all extension callbacks before handles.
  auto owned = std::move(consumers);
  consumers.clear();
  for (auto consumer : owned) consumer->context.Deactivate();
  for (auto consumer : owned) RetireConsumer(consumer);
}
Handle_t CBaseNPCTrackNativeHandle(IPluginContext* context, Handle_t handle, IdentityToken_t* owner)
{
  if (!handle) return handle;
  Consumer* creator = nullptr;
  auto identity = owner ? owner : context->GetIdentity();
  for (auto consumer : consumers) if (&consumer->context == context) {
    creator = consumer;
    // A factory explicitly created for a SourcePawn plugin belongs to that
    // plugin, including cleanup. Detach this consumer's callbacks on release,
    // but do not free the plugin's handle or revoke its mutation authority.
    if (identity == context->GetIdentity()) consumer->handles.push_back({handle, identity});
    break;
  }
  void* object = nullptr;
  HandleSecurity security(identity, myself->GetIdentity());
  if (handlesys->ReadHandle(handle, 0, &security, &object) == HandleError_None)
    handlePolicy.Register(object, identity, context->GetIdentity(), creator);
  return handle;
}
void CBaseNPCCheckNativeHandleAccess(IPluginContext* context, void* object, bool mutating)
{
  if (!mutating) return;
  // Keep established SourcePawn sharing semantics. C++ consumers must own the
  // object or hold a grant from its original owner, including cloned handles.
  for (auto consumer : consumers) if (&consumer->context == context) {
    if (!handlePolicy.CanMutate(object, context->GetIdentity(), consumer))
      context->ThrowNativeError("Cannot mutate a foreign CBaseNPC handle object without an owner grant");
    return;
  }
}
void CBaseNPCForgetNativeHandleObject(void* object) { handlePolicy.Forget(object); }
void CBaseNPCRevokeNativeHandleIdentity(IdentityToken_t* identity) { handlePolicy.RemoveIdentity(identity); }
IPlugin* CBaseNPCGetOwningPlugin(IPluginContext* context)
{
  // The public SourceMod adapter calls GetBaseRuntime() and then downcasts to
  // its private VM implementation. C++ consumers have no such VM object.
  return context->GetBaseRuntime() ? plsys->FindPluginByContext(context) : nullptr;
}
cell_t CBaseNPCDispatchForward(const char* name, const sp::CallArgs& args, cell_t initial)
{
  const auto snapshot = consumers;
  for (auto consumer : snapshot) {
    if (std::find(consumers.begin(), consumers.end(), consumer) == consumers.end() || consumer->closing) continue;
    const auto subscriptions = consumer->subscriptions;
    for (const auto& entry : subscriptions) {
      if (entry.first != name || std::find(consumer->subscriptions.begin(), consumer->subscriptions.end(), entry) == consumer->subscriptions.end()) continue;
      if (!std::strcmp(name, "OnCBaseNPCInitialized") && !consumer->initializedCallbacks.Mark(entry.second)) continue;
#if SOURCE_ENGINE == SE_BMS
      if (!std::strcmp(name, "OnEntityCreatedClient") &&
          !g_ClientEntityManager.ResolveClientEntityRef(args.argv[0].u.value)) return initial;
#endif
      cell_t result = 0;
      if (consumer->context.InvokeCallback(entry.second, args, &result)) initial = (std::max)(initial, result);
    }
  }
  return initial;
}
