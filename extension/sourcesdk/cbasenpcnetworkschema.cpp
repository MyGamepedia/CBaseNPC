#include "cbasenpcnetworkschema.h"
#include "cbasenpcserverclass.h"
#include "cbasenpcsendproxy.h"
#include "baseentity.h"
#include "extension.h"
#ifdef GetProp
#undef GetProp
#endif
#if SOURCE_ENGINE == SE_BMS
#include "cbasenpcclientclass.h"
#include "client/cliententitymanager.h"
#endif
#include <memory>
#include <set>
#include <stdexcept>
#ifdef GetProp
#undef GetProp
#endif

CBaseNPCNetworkSchemaManager g_CBaseNPCNetworkSchemaManager;
static_assert(CBASENPC_NETWORK_CLASSNAME_LENGTH == DT_MAX_STRING_BUFFERSIZE,
 "Classname bridge must match the engine string encoding limit");
namespace {
struct ClassnameBridge
{
 SendTable* send = nullptr;
 SendProp* oldSend = nullptr;
 int oldSendCount = 0;
 std::unique_ptr<SendProp[]> sendProps;
#if SOURCE_ENGINE == SE_BMS
 RecvTable* recv = nullptr;
 RecvProp* oldRecv = nullptr;
 int oldRecvCount = 0;
 std::unique_ptr<RecvProp[]> recvProps;
#endif
};
SendTable* FindBase(SendTable* table, std::set<SendTable*>& seen)
{
 if (!table || !seen.insert(table).second) return nullptr;
 if (!strcmp(table->GetName(), "DT_BaseEntity")) return table;
 for (int i = 0; i < table->GetNumProps(); ++i) if (table->GetProp(i)->GetType() == DPT_DataTable)
   if (auto found = FindBase(table->GetProp(i)->GetDataTable(), seen)) return found;
 return nullptr;
}
void SendClassname(const SendProp*, const void* object, const void*, DVariant* out, int, int)
{
 const char* name = static_cast<const CBaseEntity*>(object)->GetClassname();
 out->m_pString = name ? name : "";
}
#if SOURCE_ENGINE == SE_BMS
RecvTable* FindBase(RecvTable* table, std::set<RecvTable*>& seen)
{
 if (!table || !seen.insert(table).second) return nullptr;
 if (!strcmp(table->GetName(), "DT_BaseEntity")) return table;
 for (int i = 0; i < table->GetNumProps(); ++i) if (table->GetProp(i)->GetType() == DPT_DataTable)
   if (auto found = FindBase(table->GetProp(i)->GetDataTable(), seen)) return found;
 return nullptr;
}
void ReceiveClassname(const CRecvProxyData* data, void* object, void*)
{ if (data) g_ClientEntityManager.ReceiveClassname(object, data->m_Value.m_pString); }
#endif
}
bool CBaseNPCNetworkSchemaManager::Finalize(char* error, size_t maxlength)
{
 if (finalized_) return true;
 if (attempted_) { snprintf(error, maxlength, "Network schema already attempted; PROCESS RESTART REQUIRED"); return false; }
 attempted_ = true;
 std::unique_ptr<ClassnameBridge> bridge;
 try {
   bool localClient = false;
#if SOURCE_ENGINE == SE_BMS
   localClient = g_ClientEntityManager.IsAvailable();
#endif
   char detail[1024] = {};
   if (!g_CBaseNPCServerClassManager.Prepare(localClient, detail, sizeof(detail))) throw std::runtime_error(detail);
#if SOURCE_ENGINE == SE_BMS
   if (!g_CBaseNPCClientClassManager.Prepare(detail, sizeof(detail))) throw std::runtime_error(detail);
#endif
   auto head = g_CBaseNPCServerClassManager.GetCombinedHead();
   if (head) {
     bridge.reset(new ClassnameBridge);
     std::set<SendTable*> seen;
     for (auto sc = head; sc && !bridge->send; sc = sc->m_pNext) bridge->send = FindBase(sc->m_pTable, seen);
     // The prospective sorted head may be a custom class; baseclass traversal
     // still reaches the original stock table without publishing the list.
     if (!bridge->send) throw std::runtime_error("DT_BaseEntity SendTable was not found");
     auto send = bridge->send;
     bridge->oldSend = send->m_pProps; bridge->oldSendCount = send->m_nProps;
     if (send->m_nProps + 1 >= MAX_DATATABLE_PROPS) throw std::runtime_error("DT_BaseEntity has no capacity for classname bridge");
     bridge->sendProps.reset(new SendProp[send->m_nProps + 1]);
     for (int i = 0; i < send->m_nProps; ++i) {
       if (!strcmp(send->GetProp(i)->GetName(), CBASENPC_CLASSNAME_PROP)) throw std::runtime_error("classname bridge SendProp already exists; restart required");
       bridge->sendProps[i] = *send->GetProp(i);
     }
     auto& prop = bridge->sendProps[send->m_nProps];
     prop.m_pVarName = CBASENPC_CLASSNAME_PROP; prop.m_Type = DPT_String;
     prop.SetOffset(0); prop.SetProxyFn(SendClassname);
#if SOURCE_ENGINE == SE_BMS
     if (localClient) {
       std::set<RecvTable*> recvSeen;
       for (auto cc = g_CBaseNPCClientClassManager.GetCombinedHead(); cc && !bridge->recv; cc = cc->m_pNext)
         bridge->recv = FindBase(cc->m_pRecvTable, recvSeen);
       if (!bridge->recv) throw std::runtime_error("DT_BaseEntity RecvTable was not found");
       auto recv = bridge->recv;
       bridge->oldRecv = recv->m_pProps; bridge->oldRecvCount = recv->m_nProps;
       if (recv->m_nProps + 1 >= MAX_DATATABLE_PROPS) throw std::runtime_error("DT_BaseEntity RecvTable has no capacity for classname bridge");
       bridge->recvProps.reset(new RecvProp[recv->m_nProps + 1]);
       for (int i = 0; i < recv->m_nProps; ++i) {
         if (!strcmp(recv->GetProp(i)->GetName(), CBASENPC_CLASSNAME_PROP)) throw std::runtime_error("classname bridge RecvProp already exists; restart required");
         bridge->recvProps[i] = *recv->GetProp(i);
       }
       auto& rp = bridge->recvProps[recv->m_nProps];
       rp.m_pVarName = CBASENPC_CLASSNAME_PROP; rp.m_RecvType = DPT_String;
       rp.m_StringBufferSize = CBASENPC_NETWORK_CLASSNAME_LENGTH; rp.SetProxyFn(ReceiveClassname);
     }
#endif
   }
   // Everything above is allocation/validation only. Publication retains the
   // code module first; all pointers installed below live until process exit.
   if (!g_CBaseNPCServerClassManager.Publish(detail, sizeof(detail))) throw std::runtime_error(detail);
   published_ = g_CBaseNPCServerClassManager.IsPublished();
   if (bridge) {
     bridge->send->m_pProps = bridge->sendProps.get(); bridge->send->m_nProps = bridge->oldSendCount + 1;
#if SOURCE_ENGINE == SE_BMS
     if (bridge->recv) { bridge->recv->m_pProps = bridge->recvProps.get(); bridge->recv->m_nProps = bridge->oldRecvCount + 1; }
#endif
     bridge.release(); // Both original and replacement arrays are process-lifetime.
   }
#if SOURCE_ENGINE == SE_BMS
   g_CBaseNPCClientClassManager.Publish();
#endif
   if (!g_CBaseNPCServerClassManager.Commit(detail, sizeof(detail))) throw std::runtime_error(detail);
#if SOURCE_ENGINE == SE_BMS
   if (!g_CBaseNPCClientClassManager.Commit(detail, sizeof(detail))) throw std::runtime_error(detail);
#endif
   finalized_ = true;
   if (published_ && !localClient) g_pSM->LogMessage(myself, "Dynamic SendTables/classname replication published on dedicated server. Remote clients REQUIRE the identical CBaseNPC client schema.");
   return true;
 } catch (const std::exception& ex) {
   failed_ = true;
   snprintf(error, maxlength, "%s%s", ex.what(), published_ ? " -- NETWORKING UNSAFE, PROCESS RESTART REQUIRED" : " -- schema rejected before publication; restart required");
   // Never let entity creation or a second schema attempt proceed after failure.
   return false;
 }
}
