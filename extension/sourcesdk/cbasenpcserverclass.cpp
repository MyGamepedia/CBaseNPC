#include "cbasenpcserverclass.h"
#include "cbasenpctablenames.h"
#include "cbasenpcnetworkschema.h"
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

struct DTPropBitsPatch
{
	std::string tableName;
	std::string propName;
	int bits;
};

struct PreparedDTPropBitsPatch
{
	SendTable* table;
	std::string tableName;
	std::string propName;
	int oldBits;
	int newBits;
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

void CollectStockTableNames(SendTable* table, std::set<SendTable*>& visited,
	std::map<std::string, SendTable*, CaseInsensitiveCompare>& names)
{
	if (!table || !visited.insert(table).second) return;
	CBaseNPCCollectStockTableName(table, names);
	for (int i = 0; i < table->GetNumProps(); ++i)
		if (table->GetProp(i)->GetType() == DPT_DataTable)
			CollectStockTableNames(table->GetProp(i)->GetDataTable(), visited, names);
}

// A custom table owns its root and every non-baseclass child table created for
// its fields. Its inherited base graph was already collected and validated.
// Keep names created by CBaseNPC strict even though stock nested aliases are
// legal, so custom schema lookup remains deterministic.
void CollectOwnedTableNames(SendTable* table, std::set<SendTable*>& visited,
	std::set<SendTable*>& all,
	std::map<std::string, SendTable*, CaseInsensitiveCompare>& names)
{
	if (!table || !visited.insert(table).second) return;
	if (!all.insert(table).second) return;
	if (!table->GetName() || !*table->GetName())
		throw std::runtime_error("unnamed custom SendTable");
	if (!names.emplace(table->GetName(), table).second)
		throw std::runtime_error(std::string("duplicate custom SendTable name: ") + table->GetName());
	for (int i = 0; i < table->GetNumProps(); ++i)
	{
		auto prop = table->GetProp(i);
		if (prop->GetType() == DPT_DataTable && Q_stricmp(prop->GetName(), "baseclass"))
			CollectOwnedTableNames(prop->GetDataTable(), visited, all, names);
	}
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

SendProp* FindDirectProp(SendTable* table, const char* name)
{
	if (!table || !name) return nullptr;
	for (int i = 0; i < table->GetNumProps(); ++i)
	{
		auto prop = table->GetProp(i);
		if (prop && prop->GetName() && !Q_stricmp(prop->GetName(), name)) return prop;
	}
	return nullptr;
}

void ValidateBitPatchTarget(SendTable* table, const DTPropBitsPatch& patch,
	PreparedDTPropBitsPatch& prepared)
{
	auto prop = FindDirectProp(table, patch.propName.c_str());
	if (!prop)
		throw std::runtime_error(patch.tableName + "." + patch.propName +
			" is not a direct SendProp of the named SendTable");
	if (prop->GetType() != DPT_Int)
		throw std::runtime_error(patch.tableName + "." + patch.propName +
			" is type " + std::to_string(int(prop->GetType())) +
			"; SetDTPropBits currently supports only DPT_Int");
	if (prop->GetFlags() & SPROP_VARINT)
		throw std::runtime_error(patch.tableName + "." + patch.propName +
			" uses SPROP_VARINT; its fixed bit count cannot be patched");
	prepared = {table, patch.tableName, patch.propName, prop->m_nBits, patch.bits};
}
}

struct CBaseNPCServerClassManager::State
{
	FCall<bool, SendTable**, int> init;
	FCall<void> term;
	std::vector<ServerClass*> stock;
	std::vector<std::unique_ptr<FinalClass>> classes;
	std::vector<ServerClass*> combined;
	std::vector<SendTable*> roots;
	std::map<CPluginEntityFactory*, FinalClass*> built;
	std::vector<DTPropBitsPatch> bitPatches;
	std::vector<PreparedDTPropBitsPatch> preparedBitPatches;
	bool prepared = false;
	ServerClass* head = nullptr;
	int hook = 0;
	bool available = false;
	bool attempted = false;
	bool finalized = false;
	bool failed = false;
	bool published = false;
	bool stopped = false;
	bool registrationBlocked = false;
	std::string failure;
	std::string registrationFailure;
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
	if (m_State->published)
	{
		m_State->stopped = true;
		g_pSM->LogError(myself, "CBaseNPC unload after network publication is unsupported; retained metadata/code do not make hooks safe. Restart the process immediately.");
		return;
	}
	if (m_State->hook) SH_REMOVE_HOOK_ID(m_State->hook);
	m_State->hook = 0;
	m_State.reset();
}

bool CBaseNPCServerClassManager::IsRegistrationOpen() const
{
	return m_State && m_State->available && !m_State->attempted && !m_State->stopped && !m_State->registrationBlocked;
}
const char* CBaseNPCServerClassManager::RegistrationError() const
{
	if (m_State && m_State->registrationBlocked) return m_State->registrationFailure.c_str();
	if (!m_State || !m_State->available) return "Dynamic networking is available only in the BMS build with SendTable_Init/Term gamedata";
	return "CBaseNPC network tables are finalized or failed. Register during initial plugin startup; restart the game/server after installing this plugin.";
}
void CBaseNPCServerClassManager::BlockRegistrationForLateLoad()
{
	if (!m_State || m_State->attempted) return;
	m_State->registrationBlocked = true;
	m_State->registrationFailure = "Dynamic ServerClass/ClientClass registration is unavailable after a late extension load with existing entities; restart the game/server with CBaseNPC and its plugins loaded before map startup";
}
bool CBaseNPCServerClassManager::RegisterDTPropBitsPatch(const char* tableName,
	const char* propName, int bits, char* error, size_t maxlength)
{
	if (!IsRegistrationOpen()) return SetError(error, maxlength, RegistrationError());
	if (!tableName || !*tableName) return SetError(error, maxlength, "SendTable name must not be empty");
	if (!propName || !*propName) return SetError(error, maxlength, "SendProp name must not be empty");
	if (bits < 1 || bits > 32) return SetError(error, maxlength, "SendProp bit count must be in the range 1..32");
	for (const auto& patch : m_State->bitPatches)
	{
		if (Q_stricmp(patch.tableName.c_str(), tableName) ||
			Q_stricmp(patch.propName.c_str(), propName)) continue;
		if (patch.bits == bits) return true;
		return SetError(error, maxlength, "Conflicting DT bit patch for " +
			patch.tableName + "." + patch.propName + ": " +
			std::to_string(patch.bits) + " bits already requested, cannot request " +
			std::to_string(bits));
	}
	m_State->bitPatches.push_back({tableName, propName, bits});
	return true;
}
bool CBaseNPCServerClassManager::HasInstalledNetworkDeclarations() const
{
	if (m_State && !m_State->bitPatches.empty()) return true;
	for (int i = 0; g_pPluginEntityFactories && i < g_pPluginEntityFactories->m_Factories.Count(); ++i)
	{
		auto factory = g_pPluginEntityFactories->m_Factories[i];
		if (factory && factory->m_bInstalled &&
			(factory->HasServerClassDeclaration() || !factory->m_SendFields.empty())) return true;
	}
	return false;
}
bool CBaseNPCServerClassManager::IsFinalized() const { return m_State && m_State->finalized && !m_State->failed && !m_State->stopped; }
bool CBaseNPCServerClassManager::HasFailed() const { return m_State && m_State->failed; }
bool CBaseNPCServerClassManager::IsPublished() const { return m_State && m_State->published; }
ServerClass* CBaseNPCServerClassManager::GetCombinedHead() const { return m_State && !m_State->combined.empty() ? m_State->combined.front() : nullptr; }
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

bool CBaseNPCServerClassManager::Prepare(bool forceRebuild, char* error, size_t maxlength)
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
		std::set<std::string, CaseInsensitiveCompare> classNames;
		std::map<std::string, SendTable*, CaseInsensitiveCompare> tableNames;
		std::set<std::string, CaseInsensitiveCompare> declaredTableNames;
		std::set<SendTable*> stockTables;
		for (auto sc : state.stock)
		{
			if (!classNames.insert(sc->m_pNetworkName).second) throw std::runtime_error("duplicate stock ServerClass name");
			CollectStockTableNames(sc->m_pTable, stockTables, tableNames);
		}
		for (int i = 0; i < g_pPluginEntityFactories->m_Factories.Count(); ++i)
		{
			auto factory = g_pPluginEntityFactories->m_Factories[i];
			if (!factory || !factory->m_bInstalled) continue;
			if (!factory->m_SendFields.empty() && !factory->HasServerClassDeclaration())
				throw std::runtime_error(factory->m_iClassname + ": network fields require DefineServerClass()");
			if (!factory->HasServerClassDeclaration()) continue;
			if (!classNames.insert(factory->m_NetworkName).second)
				throw std::runtime_error(factory->m_iClassname + ": duplicate ServerClass network name " + factory->m_NetworkName);
			if (tableNames.count(factory->m_SendTableName) ||
				!declaredTableNames.insert(factory->m_SendTableName).second)
				throw std::runtime_error(factory->m_iClassname + ": duplicate SendTable name " + factory->m_SendTableName);
			declarations.emplace(factory->m_NetworkName, factory);
		}
		if (declarations.empty() && state.bitPatches.empty() && !forceRebuild)
		{
			state.prepared = true; // Dedicated stock-only: no mutation needed.
			return true;
		}
		if (!state.available || !g_CBaseNPCSendProxy.IsInitialized()) throw std::runtime_error("networking dependencies are unavailable");
		if (engine->GetEntityCount() > 0) throw std::runtime_error("too late to register network classes: edict pool already exists; restart required");
		if (state.stock.size() + declarations.size() > MAX_SERVER_CLASSES) throw std::runtime_error("too many ServerClasses (MAX_SERVER_CLASSES)");

		std::map<CPluginEntityFactory*, int> visit;
		auto& built = state.built;
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
		// Register only tables owned by each custom class. Inherited stock/custom
		// base graphs were already visited; stock is allowed to contain distinct
		// nested objects with the same local table name.
		for (auto& sc : state.classes)
		{
			std::set<SendTable*> owned;
			CollectOwnedTableNames(sc->table->GetTable(), owned, stockTables, tableNames);
		}
		if (stockTables.size() > MAX_DATATABLES) throw std::runtime_error("too many SendTables (MAX_DATATABLES)");
		state.preparedBitPatches.clear();
		state.preparedBitPatches.reserve(state.bitPatches.size());
		for (const auto& patch : state.bitPatches)
		{
			auto found = tableNames.find(patch.tableName);
			if (found == tableNames.end())
				throw std::runtime_error("SendTable " + patch.tableName + " does not exist");
			if (!found->second)
				throw std::runtime_error("SendTable name " + patch.tableName +
					" is ambiguous because multiple stock tables use it");
			PreparedDTPropBitsPatch prepared;
			ValidateBitPatchTarget(found->second, patch, prepared);
			state.preparedBitPatches.push_back(std::move(prepared));
		}

		auto& combined = state.combined;
		combined.reserve(state.stock.size() + state.classes.size());
		combined = state.stock;
		for (auto& sc : state.classes) combined.push_back(sc->Get());
		std::sort(combined.begin(), combined.end(), [](ServerClass* a, ServerClass* b) { return Q_stricmp(a->m_pNetworkName, b->m_pNetworkName) < 0; });
		auto& roots = state.roots;
		roots.reserve(combined.size());
		for (auto sc : combined) {
			std::set<SendTable*> path;
			// Reserve the synthetic DT_BaseEntity classname leaf before any
			// mutation. Conservatively reserve it for non-entity tables too.
			if (CountProps(sc->m_pTable, path) >= MAX_DATATABLE_PROPS)
				throw std::runtime_error(std::string(sc->m_pNetworkName) + ": no flattened property capacity for classname bridge");
			roots.push_back(sc->m_pTable); // Intentionally NOT deduplicated.
		}
		state.prepared = true;
		return true;
	}
	catch (const std::exception& ex)
	{
		state.failed = true;
		state.failure = ex.what();
		state.built.clear(); state.classes.clear(); state.roots.clear(); state.combined.clear();
		state.preparedBitPatches.clear();
		return SetError(error, maxlength, state.failure);
	}
}

bool CBaseNPCServerClassManager::Publish(char* error, size_t maxlength)
{
	if (!m_State || !m_State->prepared || m_State->failed) return SetError(error, maxlength, "server schema was not prepared");
	auto& state = *m_State;
	if (state.combined.empty() || state.published) return true;
	try {
		if (!RetainCodeModule()) throw std::runtime_error("cannot retain network proxy module for process lifetime");
		auto& combined = state.combined;
		for (size_t i = 0; i < combined.size(); ++i) combined[i]->m_pNext = i + 1 < combined.size() ? combined[i + 1] : nullptr;
		state.head = combined.front();
		state.published = true;
		return true;
	} catch (const std::exception& ex) { return SetError(error, maxlength, ex.what()); }
}

bool CBaseNPCServerClassManager::Commit(char* error, size_t maxlength)
{
	if (!m_State || !m_State->prepared || m_State->failed) return SetError(error, maxlength, "server schema was not prepared");
	auto& state = *m_State;
	if (state.finalized) return true;
	if (state.combined.empty()) { state.finalized = true; return true; }
	if (!state.published) return SetError(error, maxlength, "server schema was not published");
	try {
		auto& roots = state.roots;
		auto& built = state.built;
		auto& combined = state.combined;
		state.term();
		for (const auto& patch : state.preparedBitPatches)
		{
			auto prop = FindDirectProp(patch.table, patch.propName.c_str());
			if (!prop)
				throw std::runtime_error(patch.tableName + "." + patch.propName +
					" disappeared after SendTable_Term; PROCESS RESTART REQUIRED");
			if (prop->GetType() != DPT_Int || (prop->GetFlags() & SPROP_VARINT) ||
				prop->m_nBits != patch.oldBits)
				throw std::runtime_error(patch.tableName + "." + patch.propName +
					" changed after preparation; PROCESS RESTART REQUIRED");
			prop->m_nBits = patch.newBits;
			if (CBaseNPCNetworkDebugEnabled())
				g_pSM->LogMessage(myself, "[CBASENPC] Patched SendProp %s.%s bits: %d -> %d",
					patch.tableName.c_str(), patch.propName.c_str(), patch.oldBits, patch.newBits);
		}
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
			if (CBaseNPCNetworkDebugEnabled()) {
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
		g_pSM->LogMessage(myself, "Finalized %u custom ServerClasses with %u custom SendProps and %u stock SendProp bit patches; %u stock classes, %u ordered root SendTables.", unsigned(built.size()), unsigned(fields), unsigned(state.preparedBitPatches.size()), unsigned(state.stock.size()), unsigned(roots.size()));
		state.built.clear(); // Never retain plugin-owned factory pointers after publication.
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
