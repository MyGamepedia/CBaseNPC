#ifndef CBASENPC_PLUGIN_CLIENT_FACTORY_H
#define CBASENPC_PLUGIN_CLIENT_FACTORY_H
#include "smsdk_ext.h"
#include <string>
#include <vector>

// Declaration/callback owner only. No allocation or server-factory inheritance.
struct CPluginClientEntityFactory
{
 std::string classname, networkName, tableName, networkBase, physicalBase;
 IPlugin* plugin = nullptr;
 IPluginFunction* postConstructor = nullptr;
 IPluginFunction* onRemove = nullptr;
 bool installed = false, frozen = false;
 void Detach() { plugin = nullptr; postConstructor = onRemove = nullptr; }
 void Constructed(int ref) const { if (postConstructor) { postConstructor->PushCell(ref); postConstructor->Execute(nullptr); } }
 void Removed(int ref) const { if (onRemove) { onRemove->PushCell(ref); onRemove->Execute(nullptr); } }
};

class CPluginClientEntityFactories final : public IHandleTypeDispatch, public IPluginsListener
{
public:
 bool Init(char* error, size_t maxlength);
 void Shutdown();
 void OnHandleDestroy(HandleType_t type, void* object) override;
 void OnPluginUnloaded(IPlugin* plugin) override;
 HandleType_t Type() const { return type_; }
 const std::vector<CPluginClientEntityFactory*>& All() const { return factories_; }
 void Add(CPluginClientEntityFactory* factory) { factories_.push_back(factory); }
private:
 HandleType_t type_ = 0;
 std::vector<CPluginClientEntityFactory*> factories_;
};
extern CPluginClientEntityFactories g_PluginClientEntityFactories;
namespace natives { void setupClientFactoryNatives(std::vector<sp_nativeinfo_t>& list); }
#endif
