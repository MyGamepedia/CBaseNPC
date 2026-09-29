#include "cbasenpcclientclass.h"
#include "cbasenpcserverclass.h"
#include "cbasenpcrecvproxy.h"
#include "extension.h"
#include "pluginentityfactory.h"
#include "client/cliententitymanager.h"
#include <algorithm>
#include <array>
#include <functional>
#include <set>
#include <stdexcept>
#include <utility>
#ifdef GetProp
#undef GetProp
#endif

SH_DECL_HOOK0(IBaseClientDLL, GetAllClasses, SH_NOATTRIB, 0, ClientClass*);
static_assert(sizeof(CBaseNPCOwnedClientClass) == sizeof(ClientClass), "ClientClass ABI mismatch");
#ifdef _MSC_VER
#define CC_OFFSET(type, member) offsetof(type, member)
#else
#define CC_OFFSET(type, member) __builtin_offsetof(type, member)
#endif
#define CC_CHECK(member) static_assert(CC_OFFSET(CBaseNPCOwnedClientClass, member) == CC_OFFSET(ClientClass, member), "ClientClass member ABI mismatch")
CC_CHECK(m_pCreateFn); CC_CHECK(m_pCreateEventFn); CC_CHECK(m_pNetworkName); CC_CHECK(m_pRecvTable);
CC_CHECK(m_pNext); CC_CHECK(m_ClassID); CC_CHECK(m_pMapClassname);
#undef CC_CHECK
#undef CC_OFFSET

namespace {
template<size_t Slot> IClientNetworkable* CreateThunk(int index, int serial)
{ return g_CBaseNPCClientClassManager.Create(Slot, index, serial); }
template<size_t... Slots> auto Thunks(std::index_sequence<Slots...>)
{ return std::array<CreateClientClassFn, sizeof...(Slots)>{{&CreateThunk<Slots>...}}; }
const auto createThunks = Thunks(std::make_index_sequence<MAX_SERVER_CLASSES>{});
bool Error(char* out, size_t size, const std::string& message)
{ if (out && size) snprintf(out, size, "%s", message.c_str()); return false; }
bool ContainsTable(RecvTable* table, RecvTable* target, std::set<RecvTable*>& seen)
{
 if (table == target) return true;
 if (!table || !seen.insert(table).second) return false;
 // Only inheritance, not arbitrary member datatables, establishes a family.
 for (int i = 0; i < table->GetNumProps(); ++i) {
   auto prop = table->GetProp(i);
   if (prop->GetType() == DPT_DataTable && !Q_stricmp(prop->GetName(), "baseclass") && ContainsTable(prop->GetDataTable(), target, seen)) return true;
 }
 return false;
}
void ValidateTable(RecvTable* table, std::set<RecvTable*>& path, std::set<RecvTable*>& all,
 std::set<std::string, CaseInsensitiveCompare>& names)
{
 if (!table || !table->GetName() || !*table->GetName() || !path.insert(table).second)
   throw std::runtime_error("invalid/cyclic RecvTable graph");
 if (path.size() > 64) throw std::runtime_error("RecvTable inheritance exceeds 64 levels");
 if (all.insert(table).second) names.insert(table->GetName());
 if (all.size() > MAX_DATATABLES || table->GetNumProps() < 0 || table->GetNumProps() > MAX_DATATABLE_PROPS)
   throw std::runtime_error("RecvTable graph exceeds engine limits");
 for (int i = 0; i < table->GetNumProps(); ++i) {
   auto prop = table->GetProp(i);
   if (!prop->GetName()) throw std::runtime_error("unnamed RecvProp");
   if (prop->GetType() == DPT_DataTable) ValidateTable(prop->GetDataTable(), path, all, names);
 }
 path.erase(table);
}
void MatchFields(SendTable* send, RecvTable* recv)
{
 if (!send || !recv || Q_stricmp(send->GetName(), recv->GetName()) || send->GetNumProps() != recv->GetNumProps())
   throw std::runtime_error("custom Send/Recv table shape mismatch");
 for (int i = 0; i < send->GetNumProps(); ++i) {
   auto sp = send->GetProp(i); auto rp = recv->GetProp(i);
   if (Q_stricmp(sp->GetName(), rp->GetName()) || sp->GetType() != rp->GetType())
     throw std::runtime_error("custom Send/Recv property mismatch");
   if (sp->GetType() == DPT_DataTable) {
     if (i != 0) MatchFields(sp->GetDataTable(), rp->GetDataTable());
     else if (Q_stricmp(sp->GetDataTable()->GetName(), rp->GetDataTable()->GetName()))
       throw std::runtime_error("base SendTable/RecvTable name mismatch");
   }
 }
}
void CollectRecvNames(RecvTable* table, std::set<RecvTable*>& seen, std::set<std::string, CaseInsensitiveCompare>& names)
{
 if (!table || !seen.insert(table).second) return;
 for (int i = 0; i < table->GetNumProps(); ++i) {
   auto prop = table->GetProp(i); names.insert(prop->GetName());
   if (prop->GetType() == DPT_DataTable) CollectRecvNames(prop->GetDataTable(), seen, names);
 }
}
}

struct CBaseNPCClientClassManager::State
{
 FCall<bool, RecvTable**, int> init;
 FCall<void, bool> term;
 IBaseClientDLL* dll = nullptr;
 std::vector<ClientClass*> stock;
 std::vector<std::unique_ptr<CBaseNPCRuntimeClientClass>> classes;
 std::vector<RecvTable*> roots;
 struct Creation { CBaseNPCRuntimeClientClass* runtime; int index, serial; };
 std::vector<Creation> creationStack;
 ClientClass* head = nullptr;
 int hook = 0;
 bool prepared = false, published = false, finalized = false, failed = false, inert = false;
};
CBaseNPCClientClassManager g_CBaseNPCClientClassManager;
CBaseNPCClientClassManager::CBaseNPCClientClassManager() = default;
CBaseNPCClientClassManager::~CBaseNPCClientClassManager()
{ if (m_State && m_State->published) m_State.release(); }

bool CBaseNPCClientClassManager::Init(SourceMod::IGameConfig* config, char* error, size_t maxlength)
{
 if (m_State && m_State->published) return Error(error, maxlength, "Client schema already published; PROCESS RESTART REQUIRED");
 Shutdown();
 if (!g_ClientEntityManager.IsAvailable()) return true;
 m_State.reset(new State);
 auto& s = *m_State;
 try {
   s.init.Init(config, "RecvTable_Init"); s.term.Init(config, "RecvTable_Term");
   void *init = nullptr, *term = nullptr;
   config->GetMemSig("RecvTable_Init", &init); config->GetMemSig("RecvTable_Term", &term);
   if (!init || !term || init == term) throw std::runtime_error("Invalid RecvTable_Init/Term signatures (identical addresses)");
   s.dll = g_ClientEntityManager.GetBaseClientDLL();
   std::set<ClientClass*> seen;
   std::set<std::string, CaseInsensitiveCompare> names;
   for (auto cc = s.dll->GetAllClasses(); cc; cc = cc->m_pNext) {
     if (!seen.insert(cc).second || seen.size() > MAX_SERVER_CLASSES || !cc->m_pNetworkName || !cc->m_pRecvTable || !names.insert(cc->m_pNetworkName).second)
       throw std::runtime_error("invalid, duplicate or cyclic stock ClientClass registry");
     s.stock.push_back(cc);
   }
   if (s.stock.empty()) throw std::runtime_error("empty stock ClientClass registry");
   s.hook = SH_ADD_HOOK(IBaseClientDLL, GetAllClasses, s.dll, SH_MEMBER(this, &CBaseNPCClientClassManager::Hook_GetAllClasses), false);
   if (!s.hook) throw std::runtime_error("Could not hook IBaseClientDLL::GetAllClasses");
   return true;
 } catch (const std::exception& ex) { Shutdown(); return Error(error, maxlength, ex.what()); }
}
void CBaseNPCClientClassManager::Shutdown()
{
 if (!m_State) return;
 if (m_State->published) return; // Process-lifetime metadata, thunks and public list.
 if (m_State->hook) SH_REMOVE_HOOK_ID(m_State->hook);
 m_State.reset();
}
ClientClass* CBaseNPCClientClassManager::FindStockOrCustomClass(const char* name) const
{
 if (!m_State || !name) return nullptr;
 for (auto cc : m_State->stock) if (!Q_stricmp(name, cc->m_pNetworkName)) return cc;
 for (auto& cc : m_State->classes) if (!Q_stricmp(name, cc->storage.m_pNetworkName)) return cc->Get();
 return nullptr;
}
bool CBaseNPCClientClassManager::HasInstalledNetworkDeclarations() const
{
 for (auto factory : g_PluginClientEntityFactories.All())
   if (factory && factory->installed) return true;
 return false;
}
bool CBaseNPCClientClassManager::Prepare(bool inert, char* error, size_t maxlength)
{
 if (!m_State) return true; // Dedicated: declarations are inert, no client module access.
 auto& s = *m_State;
 if (s.prepared || s.failed) return Error(error, maxlength, "Client schema already prepared or failed; restart required");
 try {
   if (inert) {
     s.inert = true;
     s.prepared = true;
     return true;
   }
   if (engine->GetEntityCount() > 0 || g_ClientEntityManager.GetClientEntityCount())
     throw std::runtime_error("Client schema registration is too late: entities already exist; restart required");
   std::map<std::string, CPluginEntityFactory*, CaseInsensitiveCompare> servers;
   for (int i = 0; i < g_pPluginEntityFactories->m_Factories.Count(); ++i) {
     auto factory = g_pPluginEntityFactories->m_Factories[i];
     if (factory && factory->m_bInstalled && factory->HasServerClassDeclaration()) servers.emplace(factory->m_NetworkName, factory);
   }
   std::map<std::string, CPluginClientEntityFactory*, CaseInsensitiveCompare> declarations;
   std::set<std::string, CaseInsensitiveCompare> tableNames;
   std::set<RecvTable*> all, path;
   for (auto cc : s.stock) ValidateTable(cc->m_pRecvTable, path, all, tableNames);
   for (auto factory : g_PluginClientEntityFactories.All()) {
     if (!factory->installed) continue;
     if (FindStockOrCustomClass(factory->networkName.c_str()) || !declarations.emplace(factory->networkName, factory).second || !tableNames.insert(factory->tableName).second)
       throw std::runtime_error("duplicate ClientClass/RecvTable declaration: " + factory->networkName);
     auto server = servers.find(factory->networkName);
     if (server == servers.end() || server->second->m_SendTableName != factory->tableName || server->second->m_BaseNetworkName != factory->networkBase || server->second->m_iClassname != factory->classname)
       throw std::runtime_error("client declaration has no matching server declaration: " + factory->networkName);
   }
   for (const auto& server : servers) if (!declarations.count(server.first))
     throw std::runtime_error("missing CEntityFactoryClient for " + server.first);
   if (s.stock.size() + declarations.size() > MAX_SERVER_CLASSES) throw std::runtime_error("too many ClientClasses");
   std::map<CPluginClientEntityFactory*, int> visit;
   std::map<CPluginClientEntityFactory*, CBaseNPCRuntimeClientClass*> built;
   std::function<CBaseNPCRuntimeClientClass*(CPluginClientEntityFactory*)> build = [&](CPluginClientEntityFactory* f) {
     if (visit[f] == 2) return built[f];
     if (visit[f] == 1) throw std::runtime_error("ClientClass inheritance cycle at " + f->networkName);
     visit[f] = 1;
     ClientClass* physical = nullptr;
     for (auto cc : s.stock) if (!Q_stricmp(cc->m_pNetworkName, f->physicalBase.c_str())) physical = cc;
     if (!physical || !physical->m_pCreateFn) throw std::runtime_error("missing stock physical create function: " + f->physicalBase);
     ClientClass* base = nullptr;
     size_t inheritedSize = 0;
     auto custom = declarations.find(f->networkBase);
     if (custom != declarations.end()) {
       auto parent = build(custom->second); base = parent->Get(); inheritedSize = parent->SidecarSize();
       // Exact create family avoids reusing offsets from an unrelated C++ type.
       if (parent->physical != physical) throw std::runtime_error("custom parent/child physical client classes differ: " + f->networkName);
     } else {
       base = FindStockOrCustomClass(f->networkBase.c_str());
       std::set<RecvTable*> seen;
       if (!base || !ContainsTable(physical->m_pRecvTable, base->m_pRecvTable, seen))
         throw std::runtime_error("physical client class does not contain network base: " + f->networkName);
     }
     auto serverFactory = servers.at(f->networkName);
     std::set<std::string, CaseInsensitiveCompare> fieldNames;
     std::set<RecvTable*> inheritedTables;
     CollectRecvNames(base->m_pRecvTable, inheritedTables, fieldNames);
     std::unique_ptr<CBaseNPCRuntimeClientClass> result(new CBaseNPCRuntimeClientClass);
     result->physical = physical; result->factory = f;
     result->table.reset(new CBaseNPCRecvTable(f->tableName.c_str(), serverFactory->m_SendFields.size(), base->m_pRecvTable, inheritedSize));
     for (size_t i = 0; i < serverFactory->m_SendFields.size(); ++i) {
       const auto& desc = serverFactory->m_SendFields[i];
       auto td = serverFactory->GetFieldDescriptor(desc.dataDescIndex);
       if (td && td->fieldName && !fieldNames.insert(td->fieldName).second)
         throw std::runtime_error(f->networkName + ": duplicate/inherited client property " + td->fieldName);
       std::string detail;
       if (!td || !result->table->BuildField(i, *td, desc, detail)) throw std::runtime_error(f->networkName + ": " + detail);
     }
     auto table = result->table.get();
     result->storage = {createThunks[s.classes.size()], nullptr, table->CopyString(f->networkName.c_str()), table->GetTable(), nullptr, -1, table->CopyString(f->classname.c_str())};
     auto server = g_CBaseNPCServerClassManager.FindStockOrCustomClass(f->networkName.c_str());
     MatchFields(server ? server->m_pTable : nullptr, table->GetTable());
     ValidateTable(table->GetTable(), path, all, tableNames);
     auto ptr = result.get(); s.classes.push_back(std::move(result)); built[f] = ptr; visit[f] = 2;
     return ptr;
   };
   for (auto& declaration : declarations) build(declaration.second);
   // Stable custom order, followed by the untouched stock linked list.
   std::vector<ClientClass*> customList;
   for (auto& cc : s.classes) customList.push_back(cc->Get());
   std::sort(customList.begin(), customList.end(), [](ClientClass* a, ClientClass* b) { return Q_stricmp(a->m_pNetworkName, b->m_pNetworkName) < 0; });
   for (size_t i = 0; i < customList.size(); ++i) customList[i]->m_pNext = i + 1 < customList.size() ? customList[i + 1] : s.stock.front();
   s.head = customList.empty() ? s.stock.front() : customList.front();
   for (auto cc = s.head; cc; cc = cc->m_pNext) s.roots.push_back(cc->m_pRecvTable);
   s.prepared = true; return true;
 } catch (const std::exception& ex) { s.failed = true; s.classes.clear(); return Error(error, maxlength, ex.what()); }
}
void CBaseNPCClientClassManager::Publish()
{
 if (!m_State || !m_State->prepared || m_State->failed) return;
 if (m_State->inert) return;
 m_State->published = true;
 for (auto& cc : m_State->classes) cc->factory->frozen = true;
}
bool CBaseNPCClientClassManager::Commit(char* error, size_t maxlength)
{
 if (!m_State) return true;
 auto& s = *m_State;
 if (s.finalized) return true;
 if (s.inert) { s.finalized = true; return true; }
 if (!s.published || s.failed) return Error(error, maxlength, "client schema was not published");
 s.term(true);
 if (!s.init(s.roots.data(), int(s.roots.size()))) {
   s.failed = true; return Error(error, maxlength, "RecvTable_Init failed AFTER RecvTable_Term; PROCESS RESTART REQUIRED");
 }
 s.finalized = true;
 auto actual = s.dll->GetAllClasses();
 for (auto table : s.roots) {
   if (!actual || actual->m_pRecvTable != table) {
     s.failed = true; return Error(error, maxlength, "public ClientClass registry differs from prepared list; PROCESS RESTART REQUIRED");
   }
   actual = actual->m_pNext;
 }
 if (actual) { s.failed = true; return Error(error, maxlength, "unexpected public ClientClass entries; PROCESS RESTART REQUIRED"); }
 g_ClientEntityManager.GetProperties().ClearCaches();
 for (auto& cc : s.classes) DumpSchema(cc->storage.m_pNetworkName);
 g_pSM->LogMessage(myself, "Finalized client schema: %u stock, %u custom classes, %u ordered RecvTable roots (classname bridge enabled).",
   unsigned(s.stock.size()), unsigned(s.classes.size()), unsigned(s.roots.size()));
 return true;
}
bool CBaseNPCClientClassManager::IsFinalized() const { return m_State && m_State->finalized && !m_State->failed; }
ClientClass* CBaseNPCClientClassManager::GetCombinedHead() const { return m_State ? m_State->head : nullptr; }
ClientClass* CBaseNPCClientClassManager::Hook_GetAllClasses()
{
 if (m_State && m_State->published) RETURN_META_VALUE(MRES_SUPERCEDE, m_State->head);
 RETURN_META_VALUE(MRES_IGNORED, nullptr);
}
CBaseNPCRuntimeClientClass* CBaseNPCClientClassManager::ActiveCreation(int index, int serial) const
{
 if (!m_State || m_State->creationStack.empty()) return nullptr;
 auto& top = m_State->creationStack.back();
 return top.index == index && top.serial == serial ? top.runtime : nullptr;
}
IClientNetworkable* CBaseNPCClientClassManager::Create(size_t slot, int index, int serial)
{
 if (!IsFinalized() || slot >= m_State->classes.size()) return nullptr;
 auto runtime = m_State->classes[slot].get();
 m_State->creationStack.push_back({runtime, index, serial});
 struct Scope { State& s; ~Scope() { s.creationStack.pop_back(); } } scope{*m_State};
 // Exactly one stock creation call. It performs Init(entnum, serial) itself.
 auto net = runtime->physical->m_pCreateFn(index, serial);
 if (!net) return nullptr;
 auto entity = net->GetIClientUnknown()->GetBaseEntity();
 if (!g_ClientEntityManager.AttachRuntime(entity, runtime)) { net->Release(); return nullptr; }
 const auto handleRef = g_ClientEntityManager.EntityToClientHandleRef(entity);
 g_ClientEntityManager.RunPostConstructor(entity);
 // A plugin can delete the corresponding server entity in its callback.
 // Do not dereference a potentially destroyed client networkable afterward.
 if (!g_ClientEntityManager.IsSameClientEntity(entity, handleRef)) return nullptr;
 return net;
}
void CBaseNPCClientClassManager::DumpSchema(const char* name) const
{
 if (!m_State) { Msg("[CBASENPC] Client runtime unavailable (dedicated/unsupported platform).\n"); return; }
 for (auto cc : m_State->stock) {
   if (name && *name && Q_stricmp(name, cc->m_pNetworkName)) continue;
   Msg("[CBASENPC] Stock ClientClass %s / %s map=%s ClassID=%d create=%p\n", cc->m_pNetworkName,
     cc->m_pRecvTable->GetName(), cc->m_pMapClassname ? cc->m_pMapClassname : "", cc->m_ClassID, cc->m_pCreateFn);
 }
 for (auto& cc : m_State->classes) {
   if (name && *name && Q_stricmp(name, cc->storage.m_pNetworkName)) continue;
   Msg("[CBASENPC] ClientClass %s / %s map=%s physical=%s sidecar=%u Send/Recv=MATCH ClassID=%d\n",
     cc->storage.m_pNetworkName, cc->storage.m_pRecvTable->GetName(), cc->storage.m_pMapClassname,
     cc->physical->m_pNetworkName, unsigned(cc->SidecarSize()), cc->storage.m_ClassID);
   for (int i = 1; i < cc->storage.m_pRecvTable->GetNumProps(); ++i) {
     auto prop = cc->storage.m_pRecvTable->GetProp(i); auto field = CBaseNPCRecvTable::FindField(prop);
     if (field) Msg("  %s kind=%d sidecarOffset=%u elementSize=%u count=%u stride=%u stringBuffer=%d\n", prop->GetName(), int(field->kind), unsigned(field->offset), unsigned(field->elementSize), unsigned(field->elementCount), unsigned(field->stride), field->stringBufferSize);
   }
 }
 Msg("[CBASENPC] %u stock + %u custom ClientClasses. prepared=%d published=%d finalized=%d failed=%d\n",
   unsigned(m_State->stock.size()), unsigned(m_State->classes.size()), m_State->prepared, m_State->published, m_State->finalized, m_State->failed);
}
CON_COMMAND(cbasenpc_dump_clientclasses, "Inspect dynamic ClientClass/RecvTable/sidecar schema")
{ g_CBaseNPCClientClassManager.DumpSchema(args.ArgC() > 1 ? args[1] : nullptr); }
