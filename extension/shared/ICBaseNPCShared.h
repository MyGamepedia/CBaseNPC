#ifndef CBASENPC_SHARED_API_H
#define CBASENPC_SHARED_API_H

#include <IShareSys.h>
#include <IExtensionSys.h>
#include <IHandleSys.h>
#include <sp_vm_api.h>
#include <cstddef>

namespace SourceMod
{
// Main-thread API. All parameters are explicit and ordered as in the .inc
// declaration, including a methodmap's `this` argument. Float values use
// CallArgs::PushFloat; buffers/arrays use SourcePawn cells and COPYBACK flags.
// No plugin, plugin runtime or private CBaseNPC class is required by consumers.
class ICBaseNPCCallback
{
public:
  virtual ~ICBaseNPCCallback() = default;
  // References are private copies; only parameters marked COPYBACK are copied
  // back on success. args and its buffers are borrowed for this invocation.
  virtual bool Invoke(const sp::CallArgs& args, cell_t* result) = 0;
};

class ICBaseNPCConsumer
{
public:
  virtual bool InvokeNative(const char* name, const sp::CallArgs& args,
                            cell_t* result, char* error, size_t maxlength) = 0;
  // Pass this ID with PushCell wherever a native expects a function. 0 and -1
  // are null functions. Callback objects remain owned by the consuming module.
  virtual cell_t RegisterCallback(ICBaseNPCCallback* callback) = 0;
  // Disables a callback without invalidating adapters retained by live objects.
  virtual bool RemoveCallback(cell_t callback) = 0;
  virtual bool SubscribeForward(const char* name, cell_t callback,
                                char* error, size_t maxlength) = 0;
  virtual void UnsubscribeForward(const char* name, cell_t callback) = 0;
  virtual bool CloseHandle(Handle_t handle, char* error, size_t maxlength) = 0;
  virtual IdentityToken_t* GetOwnerIdentity() const = 0;
  // Legacy server pointer parameters/results use SourceMod pseudo-addresses on
  // x64, raw addresses on x86. Client Address natives use the .inc Address ABI.
  virtual cell_t ToAddress(const void* address) const = 0;
  virtual void* FromAddress(cell_t address) const = 0;
  // Only the original object owner can explicitly share mutation rights.
  // Reads/derivation remain public; a grant does not transfer handle ownership
  // or permit CloseHandle. Rights disappear when either consumer unloads.
  virtual bool GrantHandleMutation(Handle_t handle, ICBaseNPCConsumer* recipient,
                                   char* error, size_t maxlength) = 0;
  virtual bool RevokeHandleMutation(Handle_t handle, ICBaseNPCConsumer* recipient,
                                    char* error, size_t maxlength) = 0;
protected:
  virtual ~ICBaseNPCConsumer() = default;
};

// These are ordinary abstract services, not additional ShareSys interfaces.
class ICBaseNPCNativeAPI
{
public:
  virtual size_t GetNativeCount() const = 0;
  virtual const char* GetNativeName(size_t index) const = 0;
  virtual bool HasNative(const char* name) const = 0;
  // One consumer per (extension identity, side). Extension handles and gamedata
  // can be passed directly; newly created consumer-owned handles are tracked
  // here. Handles explicitly created for a SourcePawn plugin remain its own.
  virtual ICBaseNPCConsumer* CreateConsumer(IExtension* owner, char* error,
                                           size_t maxlength) = 0;
  // Call from SDK_OnUnload, before destroying callback objects. Refused during
  // an active invocation of this consumer. Disables callbacks first, then frees
  // handles; published network metadata remains immutable/process-lifetime.
  // Detached adapters are tombstones until process exit, since
  // callbacks may have been attached to objects owned by another consumer.
  virtual bool ReleaseConsumer(ICBaseNPCConsumer* consumer, char* error,
                                size_t maxlength) = 0;
};
}
#endif
