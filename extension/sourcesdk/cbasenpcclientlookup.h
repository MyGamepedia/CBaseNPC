#ifndef H_CBASENPC_CLIENT_LOOKUP_
#define H_CBASENPC_CLIENT_LOOKUP_

#include <client_class.h>
#include <tier1/strtools.h>
#include <climits>
#include <cstddef>
#include <cstring>

// Internal, read-only metadata helpers, also exercised without a running game.
namespace CBaseNPCClientLookup
{
constexpr size_t MaxClasses = 4096;
constexpr size_t MaxTableDepth = 64;
constexpr size_t MaxPropertyVisits = 65536;

inline bool ValidateClasses(ClientClass* head, size_t& count)
{
	count = 0;
	if (!head) return false;
	for (ClientClass* cc = head; cc; cc = cc->m_pNext)
	{
		if (++count > MaxClasses || !cc->m_pNetworkName || !*cc->m_pNetworkName ||
			!cc->m_pRecvTable || !cc->m_pRecvTable->GetName())
			return false;
		// Event/special classes may legitimately lack m_pCreateFn.
	}
	return true;
}

inline ClientClass* FindClass(ClientClass* head, const char* name)
{
	if (!name || !*name) return nullptr;
	size_t count = 0;
	for (ClientClass* cc = head; cc && count++ < MaxClasses; cc = cc->m_pNext)
	{
		if (cc->m_pNetworkName && !Q_stricmp(cc->m_pNetworkName, name))
			return cc;
	}
	return nullptr;
}

inline RecvProp* FindPropRecursive(RecvTable* table, const char* name, int offset,
	int* actualOffset, RecvTable** path, size_t depth, size_t& remaining)
{
	if (!table || depth >= MaxTableDepth) return nullptr;
	for (size_t i = 0; i < depth; ++i)
		if (path[i] == table) return nullptr;
	path[depth] = table;
	const int count = table->GetNumProps();
	if (count < 0 || static_cast<size_t>(count) > MaxPropertyVisits ||
		(count && !table->m_pProps)) return nullptr;
	for (int i = 0; i < count && remaining; ++i)
	{
		--remaining;
		RecvProp* prop = table->GetProp(i);
		const int local = prop->GetOffset();
		if (local < 0 || offset > INT_MAX - local) continue;
		const int total = offset + local;
		if (prop->GetName() && !std::strcmp(prop->GetName(), name))
		{
			if (actualOffset) *actualOffset = total;
			return prop;
		}
		if (prop->GetType() == DPT_DataTable)
		{
			RecvProp* found = FindPropRecursive(prop->GetDataTable(), name, total,
				actualOffset, path, depth + 1, remaining);
			if (found) return found;
		}
	}
	return nullptr;
}

inline RecvProp* FindProp(RecvTable* table, const char* name, int* actualOffset)
{
	if (actualOffset) *actualOffset = -1;
	if (!name || !*name) return nullptr;
	RecvTable* path[MaxTableDepth];
	size_t remaining = MaxPropertyVisits;
	return FindPropRecursive(table, name, 0, actualOffset, path, 0, remaining);
}
}

#endif
