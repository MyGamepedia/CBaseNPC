#ifndef CBASENPC_CLIENTCLASS_H
#define CBASENPC_CLIENTCLASS_H
#include <IGameConfigs.h>
#include <client_class.h>
#include <cstring>
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
 CBaseNPCRuntimeClientClass() = default;
 std::unique_ptr<CBaseNPCRecvTable> table;
 CBaseNPCOwnedClientClass storage;
 ClientClass* physical = nullptr;
 CPluginClientEntityFactory* factory = nullptr;
 std::vector<CBaseNPCEHandleInitRange> ehandleInitRanges;
 std::vector<std::unique_ptr<unsigned char[]>> sidecarBlocks;
 std::vector<unsigned char*> freeSidecars;
 size_t SidecarSize() const { return table->GetSidecarSize(); }
 ClientClass* Get() { return reinterpret_cast<ClientClass*>(&storage); }
 unsigned char* AcquireSidecar(bool* reused = nullptr)
 {
  unsigned char* block = nullptr;
  if (!freeSidecars.empty()) {
   block = freeSidecars.back();
   freeSidecars.pop_back();
   if (reused) *reused = true;
  } else {
   std::unique_ptr<unsigned char[]> owned(new unsigned char[SidecarSize() ? SidecarSize() : 1]);
   block = owned.get();
   sidecarBlocks.push_back(std::move(owned));
   if (reused) *reused = false;
  }
  InitializeSidecar(block);
  return block;
 }
 void ReleaseSidecar(unsigned char* sidecar)
 {
  if (sidecar) freeSidecars.push_back(sidecar);
 }
 void InitializeSidecar(unsigned char* sidecar) const
 {
  if (!sidecar) return;
  std::memset(sidecar, 0, SidecarSize());
  for (const auto& range : ehandleInitRanges) {
   if (!range.stride || range.offset > SidecarSize() ||
       range.count > (SidecarSize() - range.offset) / range.stride) continue;
   for (size_t i = 0; i < range.count; ++i)
    reinterpret_cast<CBaseHandle*>(sidecar + range.offset + i * range.stride)->Term();
  }
 }
};
class CBaseNPCClientClassManager final
{
public:
 CBaseNPCClientClassManager();
 ~CBaseNPCClientClassManager();
 bool Init(SourceMod::IGameConfig* config, char* error, size_t maxlength);
 void Shutdown();
 bool Prepare(bool inert, char* error, size_t maxlength);
 void Publish();
 bool Commit(char* error, size_t maxlength);
 bool IsFinalized() const;
 bool HasInstalledNetworkDeclarations() const;
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
