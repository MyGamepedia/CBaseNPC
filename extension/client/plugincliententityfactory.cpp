#include "plugincliententityfactory.h"
#include "cbasenpcinterfaces.h"
#include "sourcesdk/cbasenpcserverclass.h"
#include <algorithm>
#include <cstdio>
#include <memory>
#include <tier1/strtools.h>

CPluginClientEntityFactories g_PluginClientEntityFactories;
bool CPluginClientEntityFactories::Init(char* error, size_t maxlength)
{
 if (type_) return true;
 type_ = handlesys->CreateType("CEntityFactoryClient", this, 0, nullptr, nullptr, myself->GetIdentity(), nullptr);
 if (!type_) { snprintf(error, maxlength, "Could not create CEntityFactoryClient handle type"); return false; }
 plsys->AddPluginsListener(this);
 return true;
}
void CPluginClientEntityFactories::OnHandleDestroy(HandleType_t, void* object)
{
 CBaseNPCForgetNativeHandleObject(object);
 auto factory = static_cast<CPluginClientEntityFactory*>(object);
 factory->Detach();
 // Published runtime classes retain declarations, never plugin functions.
 if (factory->frozen) return;
 factories_.erase(std::remove(factories_.begin(), factories_.end(), factory), factories_.end());
 delete factory;
}
void CPluginClientEntityFactories::OnPluginUnloaded(IPlugin* plugin)
{
 CBaseNPCRevokeNativeHandleIdentity(plugin->GetIdentity());
 for (auto factory : factories_) if (factory->plugin == plugin) factory->Detach();
}
void CPluginClientEntityFactories::Shutdown()
{
 for (auto factory : factories_) factory->Detach();
 if (!type_) return;
 plsys->RemovePluginsListener(this);
 handlesys->RemoveType(type_, myself->GetIdentity()); type_ = 0;
}
namespace {
bool Editable(IPluginContext* context)
{
 if (g_CBaseNPCServerClassManager.IsRegistrationOpen()) return true;
 context->ThrowNativeError("%s", g_CBaseNPCServerClassManager.RegistrationError()); return false;
}
CPluginClientEntityFactory* Get(IPluginContext* context, cell_t handle)
{
 if (!Editable(context)) return nullptr;
 HandleSecurity security(context->GetIdentity(), myself->GetIdentity());
 CPluginClientEntityFactory* factory = nullptr;
 auto error = handlesys->ReadHandle(handle, g_PluginClientEntityFactories.Type(), &security, reinterpret_cast<void**>(&factory));
 if (error != HandleError_None) { context->ThrowNativeError("Invalid client factory handle (%d)", error); return nullptr; }
 CBaseNPCCheckNativeHandleAccess(context, factory, true);
 if (factory->installed || factory->frozen) { context->ThrowNativeError("Client factory is installed/frozen; restart required to change it"); return nullptr; }
 return factory;
}
bool Name(IPluginContext* context, cell_t arg, std::string& target)
{
 char* text = nullptr;
 if (context->LocalToString(arg, &text) != SP_ERROR_NONE || !text || !*text || strlen(text) > 255)
 { context->ThrowNativeError("A nonempty name of at most 255 bytes is required"); return false; }
 target = text; return true;
}
cell_t New(IPluginContext* context, const cell_t* params)
{
 if (!Editable(context)) return 0;
 // Validation can throw in an extension consumer. Do it before allocation,
 // then use RAII until the handle system accepts ownership of the object.
 std::string classname;
 if (!Name(context, params[1], classname)) return 0;
 auto postConstructor = context->GetFunctionById(params[2]);
 auto onRemove = context->GetFunctionById(params[3]);
 std::unique_ptr<CPluginClientEntityFactory> factory(new CPluginClientEntityFactory);
 factory->classname = std::move(classname);
 factory->plugin = CBaseNPCGetOwningPlugin(context);
 factory->postConstructor = postConstructor;
 factory->onRemove = onRemove;
 auto handle = handlesys->CreateHandle(g_PluginClientEntityFactories.Type(), factory.get(), context->GetIdentity(), myself->GetIdentity(), nullptr);
 if (!handle) return context->ThrowNativeError("Could not create client factory handle");
 auto cleanup = [context, handle](void*) {
   HandleSecurity security(context->GetIdentity(), myself->GetIdentity());
   handlesys->FreeHandle(handle, &security);
 };
 std::unique_ptr<void, decltype(cleanup)> handleGuard(factory.release(), cleanup);
 g_PluginClientEntityFactories.Add(static_cast<CPluginClientEntityFactory*>(handleGuard.get()));
 auto tracked = CBaseNPCTrackNativeHandle(context, handle, context->GetIdentity());
 handleGuard.release();
 return tracked;
}
cell_t Define(IPluginContext* context, const cell_t* params)
{
 auto factory = Get(context, params[1]); if (!factory) return 0;
 return Name(context, params[2], factory->networkName) && Name(context, params[3], factory->tableName) && Name(context, params[4], factory->networkBase);
}
cell_t Derive(IPluginContext* context, const cell_t* params)
{ auto factory = Get(context, params[1]); return factory && Name(context, params[2], factory->physicalBase); }
cell_t Install(IPluginContext* context, const cell_t* params)
{
 auto factory = Get(context, params[1]); if (!factory) return 0;
 if (factory->networkName.empty() || factory->tableName.empty() || factory->networkBase.empty() || factory->physicalBase.empty())
   return context->ThrowNativeError("DefineClientClass and DeriveFromNetworkClass are required before Install");
 for (auto other : g_PluginClientEntityFactories.All())
   if (other != factory && other->installed && (!Q_stricmp(other->networkName.c_str(), factory->networkName.c_str()) || !Q_stricmp(other->classname.c_str(), factory->classname.c_str())))
     return context->ThrowNativeError("Duplicate client classname/network name");
 factory->installed = true; return 1;
}
}
void natives::setupClientFactoryNatives(std::vector<sp_nativeinfo_t>& list)
{
 const sp_nativeinfo_t entries[] = {
  {"CEntityFactoryClient.CEntityFactoryClient", New}, {"CEntityFactoryClient.DefineClientClass", Define},
  {"CEntityFactoryClient.DeriveFromNetworkClass", Derive}, {"CEntityFactoryClient.Install", Install}
 };
 list.insert(list.end(), std::begin(entries), std::end(entries));
}
