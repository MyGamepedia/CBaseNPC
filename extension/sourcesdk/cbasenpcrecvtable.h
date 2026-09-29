#ifndef CBASENPC_RECVTABLE_H
#define CBASENPC_RECVTABLE_H

#include "cbasenpcsendtable.h"
#include <dt_recv.h>
#include <unordered_map>

// Offsets in this descriptor are NEVER offsets in a C_BaseEntity.
struct CBaseNPCRecvField
{
 CBaseNPCSendFieldKind kind;
 size_t offset, elementSize, elementCount, stride;
 int stringBufferSize;
 bool isUnsigned;
};

class CBaseNPCRecvTable
{
public:
 CBaseNPCRecvTable(const char* name, size_t fields, RecvTable* base, size_t inheritedSize);
 ~CBaseNPCRecvTable();
 bool BuildField(size_t index, const typedescription_t& td, const CBaseNPCSendFieldDesc& desc, std::string& error);
 RecvTable* GetTable() { return &m_Table; }
 size_t GetSidecarSize() const { return m_Size; }
 const char* CopyString(const char* text);
 static const CBaseNPCRecvField* FindField(const RecvProp* prop);
 // Includes array containers. Used to reject raw object-offset queries.
 static bool IsCustom(const RecvProp* prop);
 static void InitializeStorage(RecvTable* table, unsigned char* bytes, size_t size);
 static void Receive(const CRecvProxyData* data, void* object, void* ignored);
private:
 void Register(RecvProp* prop, const CBaseNPCRecvField& field);
 std::deque<std::string> m_Strings;
 std::unique_ptr<RecvProp[]> m_Props;
 std::vector<std::unique_ptr<RecvProp[]>> m_ArrayProps;
 std::vector<std::unique_ptr<RecvTable>> m_ArrayTables;
 std::vector<const RecvProp*> m_Registered;
 RecvTable m_Table;
 size_t m_Size;
};
#endif
