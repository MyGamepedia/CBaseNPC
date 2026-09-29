#ifndef CBASENPC_CLIENTCLASS_H
#define CBASENPC_CLIENTCLASS_H
#include <IGameConfigs.h>
#include <client_class.h>
#include "cbasenpcrecvtable.h"
#include "client/plugincliententityfactory.h"

// Do not invoke ClientClass's constructor: it would edit client.dll's list.
struct CBaseNPCOwnedClientClass
{
 CreateClientClassFn m_pCreateFn;
 CreateEventFn m_pCreateEventFn;
 const char* m_pNetworkName;
 RecvTable* m_pRecvTable;
 ClientClass* m_pNext;
 int m_ClassID;
 const char* m_pMapClassname;
};
struct CBaseNPCRuntimeClientClass
{
 std::unique_ptr<CBaseNPCRecvTable> table;
 CBaseNPCOwnedClientClass storage;
 ClientClass* physical = nullptr;
 CPluginClientEntityFactory* factory = nullptr;
 size_t SidecarSize() const { return table->GetSidecarSize(); }
 ClientClass* Get() { return reinterpret_cast<ClientClass*>(&storage); }
};
class CBaseNPCClientClassManager final
{
public:
 CBaseNPCClientClassManager();
 ~CBaseNPCClientClassManager();
 bool Init(SourceMod::IGameConfig* config, char* error, size_t maxlength);
 void Shutdown();
 bool Prepare(char* error, size_t maxlength);
 void Publish();
 bool Commit(char* error, size_t maxlength);
 bool IsFinalized() const;
 ClientClass* GetCombinedHead() const;
 ClientClass* FindStockOrCustomClass(const char* name) const;
 ClientClass* Hook_GetAllClasses();
 IClientNetworkable* Create(size_t slot, int entnum, int serial);
 CBaseNPCRuntimeClientClass* ActiveCreation(int entnum, int serial) const;
 void DumpSchema(const char* name = nullptr) const;
private:
 struct State;
 std::unique_ptr<State> m_State;
};
extern CBaseNPCClientClassManager g_CBaseNPCClientClassManager;
#endif
