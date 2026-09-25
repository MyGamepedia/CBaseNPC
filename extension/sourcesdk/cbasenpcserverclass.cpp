#include "cbasenpcserverclass.h"
#include "cbasenpcsendtable.h"
#include "cbasenpcsendproxy.h"
#include "extension.h"
#include "pluginentityfactory.h"
#include <server_class.h>
#include <algorithm>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#ifdef _WIN32
#include <windows.h>
#undef GetProp
#undef min
#undef max
#else
#include <dlfcn.h>
#endif

SH_DECL_HOOK0(IServerGameDLL, GetAllServerClasses, SH_NOATTRIB, 0, ServerClass*);

namespace
{
struct OwnedServerClass
{
	const char* m_pNetworkName;
	SendTable* m_pTable;
	ServerClass* m_pNext;
	int m_ClassID;
	int m_InstanceBaselineIndex;
};
static_assert(sizeof(OwnedServerClass) == sizeof(ServerClass), "ServerClass ABI changed");
#ifdef _MSC_VER
#define CLASS_OFFSET(type, member) offsetof(type, member)
#else
// dt_common.h replaces offsetof on Linux with a non-constant expression.
#define CLASS_OFFSET(type, member) __builtin_offsetof(type, member)
#endif
#define CHECK_CLASS_LAYOUT(member) static_assert(CLASS_OFFSET(OwnedServerClass, member) == CLASS_OFFSET(ServerClass, member), "ServerClass ABI: " #member)
CHECK_CLASS_LAYOUT(m_pNetworkName);
CHECK_CLASS_LAYOUT(m_pTable);
CHECK_CLASS_LAYOUT(m_pNext);
CHECK_CLASS_LAYOUT(m_ClassID);
CHECK_CLASS_LAYOUT(m_InstanceBaselineIndex);
#undef CHECK_CLASS_LAYOUT
#undef CLASS_OFFSET

struct FinalClass
{
	std::unique_ptr<CBaseNPCSendTable> table;
	OwnedServerClass storage;
	size_t requiredEntitySize = 0;
	ServerClass* Get() { return reinterpret_cast<ServerClass*>(&storage); }
};

bool SetError(char* error, size_t maxlength, const std::string& message)
{
	if (error && maxlength) snprintf(error, maxlength, "%s", message.c_str());
	return false;
}

// Metadata contains local proxy function pointers AND SendProp vtables. Keeping
// just the heap objects is not sufficient if SourceMod unloads the library.
// There is no unload-veto callback in IExtensionInterface. Retain a loader
// reference for process lifetime before publishing anything to the engine.
bool RetainCodeModule()
{
#ifdef _WIN32
	HMODULE module;
	return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		reinterpret_cast<LPCSTR>(&g_CBaseNPCServerClassManager), &module) != 0;
#else
	Dl_info info = {};
	if (!dladdr(&g_CBaseNPCServerClassManager, &info) || !info.dli_fname) return false;
	return dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD) != nullptr;
#endif
}

// Bound the complete flattened hierarchy before the destructive engine call.
// Counting excluded props too is conservative; never undercount array leaves.
size_t CountProps(SendTable* table, std::set<SendTable*>& path)
{
	if (!table || !path.insert(table).second) throw std::runtime_error("cyclic/null SendTable");
	size_t count = 0;
	for (int i = 0; i < table->GetNumProps(); ++i)
	{
		auto prop = table->GetProp(i);
		// Some engine encoders split vectors into component props.
		count += prop->GetType() == DPT_DataTable ? CountProps(prop->GetDataTable(), path) :
			(prop->GetType() == DPT_Vector ? 3 : (prop->GetType() == DPT_VectorXY ? 2 : 1));
		if (count > MAX_DATATABLE_PROPS) throw std::runtime_error("flattened SendTable exceeds MAX_DATATABLE_PROPS");
	}
	path.erase(table);
	return count;
}

void CollectTableNames(SendTable* table, std::set<SendTable*>& visited,
	std::set<std::string, CaseInsensitiveCompare>& names)
{
	if (!table || !visited.insert(table).second) return;
	if (table->GetName()) names.insert(table->GetName());
	for (int i = 0; i < table->GetNumProps(); ++i)
		if (table->GetProp(i)->GetType() == DPT_DataTable)
			CollectTableNames(table->GetProp(i)->GetDataTable(), visited, names);
}

void CollectFieldNames(SendTable* table, std::set<SendTable*>& visited,
	std::set<std::string, CaseInsensitiveCompare>& names)
{
	if (!table || !visited.insert(table).second) return;
	for (int i = 0; i < table->GetNumProps(); ++i)
	{
		auto prop = table->GetProp(i);
		if (prop->GetFlags() & SPROP_EXCLUDE) continue;
		if (prop->GetName()) names.insert(prop->GetName());
		if (prop->GetType() == DPT_DataTable && !prop->GetArrayProp())
			CollectFieldNames(prop->GetDataTable(), visited, names);
	}
}
}

struct CBaseNPCServerClassManager::State
{
	FCall<bool, SendTable**, int> init;
	FCall<void> term;
	std::vector<ServerClass*> stock;
	std::vector<std::unique_ptr<FinalClass>> classes;
	ServerClass* head = nullptr;
	int hook = 0;
	bool available = false;
	bool attempted = false;
	bool finalized = false;
	bool failed = false;
	bool published = false;
	bool stopped = false;
	std::string failure;
};

CBaseNPCServerClassManager g_CBaseNPCServerClassManager;
CBaseNPCServerClassManager::CBaseNPCServerClassManager() = default;
CBaseNPCServerClassManager::~CBaseNPCServerClassManager()
{
	// Even static destruction must not invalidate engine-owned precalcs.
	if (m_State && m_State->published) m_State.release();
}

bool CBaseNPCServerClassManager::Init(SourceMod::IGameConfig* config, char* error, size_t maxlength)
{
	if (m_State && m_State->published)
		return SetError(error, maxlength, "CBaseNPC networking cannot be reloaded in this process. Restart required.");
	Shutdown();
	m_State.reset(new State);
#if SOURCE_ENGINE == SE_BMS
	try
	{
		m_State->init.Init(config, "SendTable_Init");
		m_State->term.Init(config, "SendTable_Term");
		if (!g_CBaseNPCSendProxy.IsInitialized()) throw std::runtime_error("SendProxy subsystem is not initialized");
		std::set<ServerClass*> seen;
		for (ServerClass* sc = gamedll->GetAllServerClasses(); sc; sc = sc->m_pNext)
		{
			if (!seen.insert(sc).second) throw std::runtime_error("cycle in stock ServerClass registry");
			if (!sc->m_pNetworkName || !sc->m_pTable) throw std::runtime_error("invalid stock ServerClass");
			m_State->stock.push_back(sc);
		}
		if (m_State->stock.empty()) throw std::runtime_error("empty stock ServerClass registry");
		m_State->hook = SH_ADD_HOOK(IServerGameDLL, GetAllServerClasses, gamedll,
			SH_MEMBER(this, &CBaseNPCServerClassManager::Hook_GetAllServerClasses), false);
		if (!m_State->hook) throw std::runtime_error("failed to hook GetAllServerClasses");
		m_State->available = true;
	}
	catch (const std::exception& ex)
	{
		Shutdown();
		return SetError(error, maxlength, ex.what());
	}
#endif
	// TF2's datamap-only path stays loadable without BMS engine signatures.
	return true;
}

void CBaseNPCServerClassManager::Shutdown()
{
	if (!m_State) return;
	if (m_State->hook) SH_REMOVE_HOOK_ID(m_State->hook);
	m_State->hook = 0;
	if (m_State->published)
	{
		m_State->stopped = true;
		g_pSM->LogError(myself, "CBaseNPC runtime unload after network publication is unsupported. Metadata/code retained; restart the process before continuing.");
		return;
	}
	m_State.reset();
}

bool CBaseNPCServerClassManager::IsRegistrationOpen() const
{
	return m_State && m_State->available && !m_State->attempted && !m_State->stopped;
}
const char* CBaseNPCServerClassManager::RegistrationError() const
{
	if (!m_State || !m_State->available) return "Dynamic networking is available only in the BMS build with SendTable_Init/Term gamedata";
	return "CBaseNPC network tables are finalized or failed. Register during initial plugin startup; restart the game/server after installing this plugin.";
}
bool CBaseNPCServerClassManager::IsFinalized() const { return m_State && m_State->finalized && !m_State->failed && !m_State->stopped; }
bool CBaseNPCServerClassManager::HasFailed() const { return m_State && m_State->failed; }
ServerClass* CBaseNPCServerClassManager::GetCombinedHead() const { return m_State ? m_State->head : nullptr; }
ServerClass* CBaseNPCServerClassManager::Hook_GetAllServerClasses()
{
	if (m_State && m_State->published) RETURN_META_VALUE(MRES_SUPERCEDE, m_State->head);
	RETURN_META_VALUE(MRES_IGNORED, nullptr);
}
ServerClass* CBaseNPCServerClassManager::FindStockOrCustomClass(const char* name) const
{
	if (!m_State || !name) return nullptr;
	for (auto sc : m_State->stock) if (!Q_stricmp(sc->m_pNetworkName, name)) return sc;
	for (auto& sc : m_State->classes) if (!Q_stricmp(sc->storage.m_pNetworkName, name)) return sc->Get();
	return nullptr;
}

bool CBaseNPCServerClassManager::Finalize(char* error, size_t maxlength)
{
	if (!m_State) return SetError(error, maxlength, "network manager is not initialized");
	auto& state = *m_State;
	if (state.failed) return SetError(error, maxlength, state.failure);
	if (state.finalized) return true;
	if (state.attempted) return SetError(error, maxlength, "reentrant network finalization");
	state.attempted = true;
	try
	{
		std::map<std::string, CPluginEntityFactory*, CaseInsensitiveCompare> declarations;
		std::set<std::string, CaseInsensitiveCompare> classNames, tableNames;
		std::set<SendTable*> stockTables;
		for (auto sc : state.stock)
		{
			if (!classNames.insert(sc->m_pNetworkName).second) throw std::runtime_error("duplicate stock ServerClass name");
			CollectTableNames(sc->m_pTable, stockTables, tableNames);
		}
		for (int i = 0; i < g_pPluginEntityFactories->m_Factories.Count(); ++i)
		{
			auto factory = g_pPluginEntityFactories->m_Factories[i];
			if (!factory->m_SendFields.empty() && !factory->HasServerClassDeclaration())
				throw std::runtime_error(factory->m_iClassname + ": network fields require DefineServerClass()");
			if (!factory->HasServerClassDeclaration()) continue;
			if (!classNames.insert(factory->m_NetworkName).second)
				throw std::runtime_error(factory->m_iClassname + ": duplicate ServerClass network name " + factory->m_NetworkName);
			if (!tableNames.insert(factory->m_SendTableName).second)
				throw std::runtime_error(factory->m_iClassname + ": duplicate SendTable name " + factory->m_SendTableName);
			declarations.emplace(factory->m_NetworkName, factory);
		}
		if (declarations.empty())
		{
			state.finalized = true; // No Term/Init, no relinking, no published hook.
			return true;
		}
		if (!state.available || !g_CBaseNPCSendProxy.IsInitialized()) throw std::runtime_error("networking dependencies are unavailable");
		if (engine->GetEntityCount() > 0) throw std::runtime_error("too late to register network classes: edict pool already exists; restart required");
		if (state.stock.size() + declarations.size() > MAX_SERVER_CLASSES) throw std::runtime_error("too many ServerClasses (MAX_SERVER_CLASSES)");

		std::map<CPluginEntityFactory*, int> visit;
		std::map<CPluginEntityFactory*, FinalClass*> built;
		std::function<FinalClass*(CPluginEntityFactory*)> build = [&](CPluginEntityFactory* factory) -> FinalClass*
		{
			if (visit[factory] == 2) return built[factory];
			if (visit[factory] == 1) throw std::runtime_error("ServerClass inheritance cycle at " + factory->m_NetworkName);
			visit[factory] = 1;
			// Validate before any recursive GetEntitySize() or engine mutation.
			std::set<CPluginEntityFactory*> allocationAncestors;
			for (auto cur = factory; cur; cur = CPluginEntityFactory::ToPluginEntityFactory(cur->FindBaseFactory()))
				if (!allocationAncestors.insert(cur).second)
					throw std::runtime_error(factory->m_iClassname + ": entity allocation inheritance cycle");
			ServerClass* base = nullptr;
			size_t baseRequiredSize = 0;
			auto customBase = declarations.find(factory->m_BaseNetworkName);
			if (customBase != declarations.end())
			{
				auto parent = build(customBase->second);
				base = parent->Get();
				baseRequiredSize = parent->requiredEntitySize;
				if (baseRequiredSize && !allocationAncestors.count(customBase->second))
					throw std::runtime_error(factory->m_iClassname + ": custom network base with fields must also be an entity allocation ancestor");
			}
			else base = FindStockOrCustomClass(factory->m_BaseNetworkName.c_str());
			if (!base) throw std::runtime_error(factory->m_iClassname + ": missing base ServerClass " + factory->m_BaseNetworkName);
			if (factory->m_bDefiningDataDesc || (!factory->m_SendFields.empty() && !factory->HasDataDesc()))
				throw std::runtime_error(factory->m_iClassname + ": call EndDataMapDesc() before network finalization");
			if (factory->m_SendFields.size() >= MAX_DATATABLE_PROPS) throw std::runtime_error(factory->m_iClassname + ": too many fields");
			auto allocationBase = factory->FindBaseFactory();
			if (factory->DoesNotDerive() || (!allocationBase && factory->IsBaseFactoryRequired()))
				throw std::runtime_error(factory->m_iClassname + ": unresolved entity allocation base");
			const size_t baseSize = allocationBase ? allocationBase->GetEntitySize() : factory->GetBaseEntitySize();
			const size_t entitySize = baseSize + factory->GetDataDescSize();
			if (!baseSize || entitySize < baseRequiredSize || (factory->HasDataDesc() && factory->GetDataDescOffset() != baseSize))
				throw std::runtime_error(factory->m_iClassname + ": entity/datamap layout does not contain its network base");
			std::unique_ptr<FinalClass> result(new FinalClass);
			result->requiredEntitySize = baseRequiredSize;
			result->table.reset(new CBaseNPCSendTable(factory->m_SendTableName.c_str(), factory->m_SendFields.size(), base->m_pTable));
			std::set<std::string, CaseInsensitiveCompare> fields;
			std::set<SendTable*> inheritedTables;
			CollectFieldNames(base->m_pTable, inheritedTables, fields);
			for (size_t n = 0; n < factory->m_SendFields.size(); ++n)
			{
				const auto& desc = factory->m_SendFields[n];
				auto td = factory->GetFieldDescriptor(desc.dataDescIndex);
				if (!td || !td->fieldName) throw std::runtime_error(factory->m_iClassname + ": invalid field descriptor index");
				if (!fields.insert(td->fieldName).second) throw std::runtime_error(factory->m_iClassname + ": duplicate or inherited field " + td->fieldName);
				std::string detail;
				if (!result->table->BuildField(n, *td, desc, detail)) throw std::runtime_error(factory->m_iClassname + "." + td->fieldName + ": " + detail);
				size_t end = static_cast<size_t>(td->fieldOffset[TD_OFFSET_NORMAL]) + td->fieldSizeInBytes;
				if (end > entitySize) throw std::runtime_error(factory->m_iClassname + ": SendProp exceeds entity allocation");
				result->requiredEntitySize = std::max(result->requiredEntitySize, end);
			}
			std::set<SendTable*> path;
			CountProps(result->table->GetTable(), path);
			result->storage = {result->table->CopyString(factory->m_NetworkName.c_str()), result->table->GetTable(), nullptr, -1, INVALID_STRING_INDEX};
			auto ptr = result.get();
			state.classes.push_back(std::move(result));
			built[factory] = ptr;
			visit[factory] = 2;
			factory->m_FinalNetworkEntitySize = entitySize;
			return ptr;
		};
		for (auto& decl : declarations) build(decl.second);
		// Include array tables and inherited stock tables, not just class roots.
		for (auto& sc : state.classes) CollectTableNames(sc->table->GetTable(), stockTables, tableNames);
		if (stockTables.size() > MAX_DATATABLES) throw std::runtime_error("too many SendTables (MAX_DATATABLES)");

		std::vector<ServerClass*> combined = state.stock;
		for (auto& sc : state.classes) combined.push_back(sc->Get());
		std::sort(combined.begin(), combined.end(), [](ServerClass* a, ServerClass* b) { return Q_stricmp(a->m_pNetworkName, b->m_pNetworkName) < 0; });
		std::vector<SendTable*> roots;
		for (auto sc : combined) roots.push_back(sc->m_pTable); // Intentionally NOT deduplicated.
		if (!RetainCodeModule()) throw std::runtime_error("cannot retain network proxy module for process lifetime");
		for (size_t i = 0; i < combined.size(); ++i) combined[i]->m_pNext = i + 1 < combined.size() ? combined[i + 1] : nullptr;
		state.head = combined.front();
		state.published = true;
		state.term();
		if (!state.init(roots.data(), static_cast<int>(roots.size())))
			throw std::runtime_error("SendTable_Init failed AFTER SendTable_Term. Engine networking is unsafe; PROCESS RESTART REQUIRED");
		size_t fields = 0;
		for (auto& item : built)
		{
			auto factory = item.first;
			factory->m_pServerClass = item.second->Get();
			fields += factory->m_SendFields.size();
			// Freeze allocation ancestors too, including datamap-only bases.
			for (auto cur = factory; cur; cur = CPluginEntityFactory::ToPluginEntityFactory(cur->FindBaseFactory())) cur->m_bNetworkLayoutFrozen = true;
			g_pSM->LogMessage(myself, "Network class %s: %s / %s, base %s (%u fields)", factory->m_iClassname.c_str(), factory->m_NetworkName.c_str(), factory->m_SendTableName.c_str(), factory->m_BaseNetworkName.c_str(), unsigned(factory->m_SendFields.size()));
			auto table = item.second->table->GetTable();
			for (int i = 1; i < table->GetNumProps(); ++i)
			{
				auto prop = table->GetProp(i);
				g_pSM->LogMessage(myself, "  SendProp %s: offset 0x%x, type %d, bits %d, flags 0x%x, array elements %d",
					prop->GetName(), prop->GetOffset(), int(prop->GetType()), prop->m_nBits, prop->GetFlags(),
					prop->GetArrayProp() ? prop->GetDataTable()->GetNumProps() : 0);
			}
		}
		// Check the public interface, not merely the vector used to relink it.
		auto actual = gamedll->GetAllServerClasses();
		for (auto expected : combined)
		{
			if (actual != expected) throw std::runtime_error("public ServerClass registry differs from the finalized sorted registry; restart required");
			actual = actual->m_pNext;
		}
		if (actual) throw std::runtime_error("public ServerClass registry has unexpected entries; restart required");
		state.finalized = true;
		g_pSM->LogMessage(myself, "Finalized %u custom ServerClasses with %u custom SendProps; %u stock classes, %u ordered root SendTables.", unsigned(built.size()), unsigned(fields), unsigned(state.stock.size()), unsigned(roots.size()));
		return true;
	}
	catch (const std::exception& ex)
	{
		state.failed = true;
		state.failure = ex.what();
		if (!state.published) state.classes.clear();
		return SetError(error, maxlength, state.failure);
	}
}

void CBaseNPCServerClassManager::DumpSchema(const char* name) const
{
	if (!m_State || !m_State->finalized || m_State->failed)
	{
		Msg("[CBASENPC] Network schema is not successfully finalized.\n");
		return;
	}
	std::set<ServerClass*> seen;
	ServerClass* previous = nullptr;
	for (auto sc = gamedll->GetAllServerClasses(); sc; sc = sc->m_pNext)
	{
		if (!seen.insert(sc).second) { Msg("[CBASENPC] ERROR: ServerClass cycle\n"); break; }
		if (previous && Q_stricmp(previous->m_pNetworkName, sc->m_pNetworkName) >= 0)
			Msg("[CBASENPC] ERROR: unsorted/duplicate class %s\n", sc->m_pNetworkName);
		previous = sc;
		if (name && *name && Q_stricmp(name, sc->m_pNetworkName)) continue;
		Msg("[CBASENPC] %s / %s: ClassID=%d baseline=%d\n", sc->m_pNetworkName,
			sc->m_pTable->GetName(), sc->m_ClassID, sc->m_InstanceBaselineIndex);
		for (auto& owned : m_State->classes)
		{
			if (owned->Get() != sc) continue;
			auto table = sc->m_pTable;
			for (int i = 0; i < table->GetNumProps(); ++i)
			{
				auto prop = table->GetProp(i);
				sm_sendprop_info_t info = {};
				bool found = gamehelpers->FindSendPropInfo(sc->m_pNetworkName, prop->GetName(), &info);
				Msg("  %s: offset=%d type=%d bits=%d flags=0x%x lookup=%s\n", prop->GetName(),
					prop->GetOffset(), int(prop->GetType()), prop->m_nBits, prop->GetFlags(),
					found && info.actual_offset == prop->GetOffset() ? "OK" : "ERROR");
				if (prop->GetType() == DPT_DataTable)
					Msg("    table=%s elements=%d\n", prop->GetDataTable()->GetName(), prop->GetDataTable()->GetNumProps());
			}
		}
	}
	Msg("[CBASENPC] %u ServerClasses in public registry. ClassIDs are assigned by the engine at map start.\n", unsigned(seen.size()));
}

CON_COMMAND(cbasenpc_dump_netclasses, "Inspect finalized CBaseNPC schema and ClassIDs; optional ServerClass name")
{
	g_CBaseNPCServerClassManager.DumpSchema(args.ArgC() > 1 ? args[1] : nullptr);
}
