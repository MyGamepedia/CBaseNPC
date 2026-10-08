#ifndef CBASENPC_INTERFACES_H
#define CBASENPC_INTERFACES_H
#include "smsdk_ext.h"

void CBaseNPCPublishInterfaces();
void CBaseNPCShutdownInterfaces();
Handle_t CBaseNPCTrackNativeHandle(IPluginContext* context, Handle_t handle, IdentityToken_t* owner);
IPlugin* CBaseNPCGetOwningPlugin(IPluginContext* context);
void CBaseNPCCheckNativeHandleAccess(IPluginContext* context, void* object, bool mutating);
void CBaseNPCForgetNativeHandleObject(void* object);
void CBaseNPCRevokeNativeHandleIdentity(IdentityToken_t* identity);
cell_t CBaseNPCDispatchForward(const char* name, const sp::CallArgs& args, cell_t initial = 0);
#endif
