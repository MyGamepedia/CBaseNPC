#include "cbasenpcrecvtable.h"
#include "cbasenpcrecvproxy.h"
#include "client/cliententitymanager.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <limits>
#ifdef GetProp
#undef GetProp
#endif

// SDK out-of-line constructors are not part of tier1.lib.
RecvProp::RecvProp()
{
 m_pVarName = nullptr; m_RecvType = DPT_Int; m_Flags = 0;
 m_StringBufferSize = 0; m_bInsideArray = false; m_pExtraData = nullptr;
 m_pArrayProp = nullptr; m_ArrayLengthProxy = nullptr; m_ProxyFn = nullptr;
 m_DataTableProxyFn = nullptr; m_pDataTable = nullptr; m_Offset = 0;
 m_ElementStride = 0; m_nElements = 1; m_pParentArrayPropName = nullptr;
}
RecvTable::RecvTable() { Construct(nullptr, 0, nullptr); }
RecvTable::RecvTable(RecvProp* props, int count, const char* name) { Construct(props, count, name); }
RecvTable::~RecvTable() {}
void RecvTable::Construct(RecvProp* props, int count, const char* name)
{
 m_pProps = props; m_nProps = count; m_pNetTableName = name;
 m_pDecoder = nullptr; m_bInitialized = false; m_bInMainList = false;
}

namespace {
using Kind = CBaseNPCSendFieldKind;
// Function-local storage avoids static initialization order dependencies.
auto& Fields() { static std::unordered_map<const RecvProp*, const CBaseNPCRecvField*> fields; return fields; }
RecvProp DataTable(const char* name, RecvTable* table)
{
 RecvProp prop; prop.m_pVarName = name; prop.m_RecvType = DPT_DataTable;
 prop.SetDataTable(table); prop.SetDataTableProxyFn(CBaseNPCRecvProxy::StaticDataTable);
 return prop;
}
}

const char* CBaseNPCRecvTable::CopyString(const char* text)
{ m_Strings.emplace_back(text); return m_Strings.back().c_str(); }
CBaseNPCRecvTable::CBaseNPCRecvTable(const char* name, size_t fields, RecvTable* base, size_t inheritedSize)
 : m_Props(new RecvProp[fields + 1]), m_Size(inheritedSize)
{
 m_Props[0] = DataTable(CopyString("baseclass"), base);
 m_Table.Construct(m_Props.get(), int(fields + 1), CopyString(name));
}
CBaseNPCRecvTable::~CBaseNPCRecvTable()
{ for (auto prop : m_Registered) Fields().erase(prop); }
void CBaseNPCRecvTable::Register(RecvProp* prop, const CBaseNPCRecvField& field)
{
 m_Fields.push_back(field);
 const auto metadata = &m_Fields.back();
 prop->SetExtraData(metadata);
 Fields().emplace(prop, metadata);
 m_Registered.push_back(prop);
}
const CBaseNPCRecvField* CBaseNPCRecvTable::FindField(const RecvProp* prop)
{
 g_ClientEntityManager.RecordSlowRecvMetadataLookup();
 auto i = Fields().find(prop); return i == Fields().end() ? nullptr : i->second;
}
bool CBaseNPCRecvTable::IsCustom(const RecvProp* prop) { return FindField(prop) != nullptr; }

bool CBaseNPCRecvTable::BuildField(size_t index, const typedescription_t& td,
 const CBaseNPCSendFieldDesc& desc, std::string& error)
{
 if (!CBaseNPC_ValidateSendFieldOptions(desc, error)) return false;
 if (index + 1 >= size_t(m_Table.GetNumProps()) || !td.fieldName || !*td.fieldName || !strcmp(td.fieldName, "baseclass") ||
     !strcmp(td.fieldName, "m_szCBaseNPCServerClassname") || !td.fieldSize || td.fieldSize > MAX_ARRAY_ELEMENTS)
 { error = "invalid receive field descriptor"; return false; }
 size_t size = 4, alignment = 4;
 SendPropType type = DPT_Int;
 switch (desc.kind) {
 case Kind::Short: size = alignment = 2; break;
 case Kind::Char: case Kind::Bool: size = alignment = 1; break;
 case Kind::Float: case Kind::Time: case Kind::Angle: type = DPT_Float; break;
 case Kind::Vector: case Kind::QAngle: size = 12; type = DPT_Vector; break;
 case Kind::VectorXY: size = 8; type = DPT_VectorXY; break;
 case Kind::StringT: size = desc.stringMaxLength; alignment = 1; type = DPT_String; break;
 default: break;
 }
 size_t offset = (m_Size + alignment - 1) & ~(alignment - 1);
 // Keep pathological plugin declarations from allocating unbounded entity storage.
 if (offset > 16 * 1024 * 1024 || size * td.fieldSize > 16 * 1024 * 1024 - offset)
 { error = "client sidecar exceeds 16 MiB"; return false; }
 CBaseNPCRecvField field{desc.kind, offset, size, td.fieldSize, size,
   type == DPT_String ? desc.stringMaxLength : 0,
   (desc.flags & SPROP_UNSIGNED) != 0 || desc.kind == Kind::Bool || desc.kind == Kind::Color32};
 RecvProp scalar;
 scalar.m_pVarName = CopyString(td.fieldName); scalar.m_RecvType = type;
 scalar.m_StringBufferSize = field.stringBufferSize;
 scalar.m_Flags = field.isUnsigned ? SPROP_UNSIGNED : 0;
 scalar.SetProxyFn(Receive);
 // Every physical offset is zero. The proxy obtains the entity from pStruct.
 if (td.fieldSize == 1) {
   m_Props[index + 1] = scalar; Register(&m_Props[index + 1], field);
 } else {
   std::unique_ptr<RecvProp[]> props(new RecvProp[td.fieldSize]);
   for (int i = 0; i < td.fieldSize; ++i) {
     char name[16]; snprintf(name, sizeof(name), "%03d", i);
     props[i] = scalar; props[i].m_pVarName = CopyString(name);
     props[i].SetParentArrayPropName(scalar.m_pVarName);
     auto element = field; element.offset += i * size; element.elementCount = 1;
     Register(&props[i], element);
   }
   std::unique_ptr<RecvTable> table(new RecvTable(props.get(), td.fieldSize, scalar.m_pVarName));
   m_Props[index + 1] = DataTable(scalar.m_pVarName, table.get());
   Register(&m_Props[index + 1], field);
   m_ArrayProps.push_back(std::move(props)); m_ArrayTables.push_back(std::move(table));
 }
 m_Size = offset + size * td.fieldSize;
 if (desc.kind == Kind::EHandle) m_EHandleInitRanges.push_back({offset, td.fieldSize, size});
 return true;
}

void CBaseNPCRecvTable::InitializeStorage(RecvTable* table, unsigned char* bytes, size_t size)
{
 for (int i = 0; table && i < table->GetNumProps(); ++i) {
   auto prop = table->GetProp(i);
   auto field = FindField(prop);
   if (field && field->kind == Kind::EHandle && prop->GetType() != DPT_DataTable &&
       field->offset <= size && sizeof(CBaseHandle) <= size - field->offset)
     reinterpret_cast<CBaseHandle*>(bytes + field->offset)->Term();
   if (prop->GetType() == DPT_DataTable) InitializeStorage(prop->GetDataTable(), bytes, size);
 }
}

void CBaseNPCRecvTable::Receive(const CRecvProxyData* data, void* object, void*)
{
 if (!data) return;
 // This proxy is attached only to CBaseNPC-owned leaf RecvProps. Their stable
 // metadata is installed in m_pExtraData while the schema is built, avoiding
 // a global RecvProp hash lookup on every network update.
 auto field = data->m_pRecvProp
   ? static_cast<const CBaseNPCRecvField*>(data->m_pRecvProp->GetExtraData()) : nullptr;
 if (!field) return;
 auto out = g_ClientEntityManager.GetNetworkSidecarAddress(
   data->m_ObjectID, object, field->offset, field->elementSize);
 if (!out) return;
 switch (field->kind) {
 case Kind::Bool: *out = data->m_Value.m_Int != 0; break;
 case Kind::Char: *out = static_cast<unsigned char>(data->m_Value.m_Int); break;
 case Kind::Short: { auto v = static_cast<short>(data->m_Value.m_Int); memcpy(out, &v, 2); break; }
 case Kind::Float: case Kind::Time: case Kind::Angle: memcpy(out, &data->m_Value.m_Float, 4); break;
 case Kind::Vector: case Kind::QAngle: memcpy(out, data->m_Value.m_Vector, 12); break;
 case Kind::VectorXY: memcpy(out, data->m_Value.m_Vector, 8); break;
 case Kind::StringT: {
   const char* text = data->m_Value.m_pString ? data->m_Value.m_pString : "";
   size_t n = 0; while (n + 1 < field->elementSize && text[n]) ++n;
   memcpy(out, text, n); out[n] = 0; break;
 }
 case Kind::Color32: {
   uint32_t color = LittleDWord(static_cast<uint32_t>(data->m_Value.m_Int));
   memcpy(out, &color, sizeof(color)); break;
 }
 case Kind::EHandle:
   // BMS packs the network handle using MAX_EDICT_BITS; the in-memory
   // CBaseHandle uses NUM_ENT_ENTRY_BITS. Do not memcpy the wire integer.
   if (static_cast<unsigned>(data->m_Value.m_Int) == INVALID_NETWORKED_EHANDLE_VALUE)
     reinterpret_cast<CBaseHandle*>(out)->Term();
   else reinterpret_cast<CBaseHandle*>(out)->Init(data->m_Value.m_Int & ((1 << MAX_EDICT_BITS) - 1),
     static_cast<unsigned>(data->m_Value.m_Int) >> MAX_EDICT_BITS);
   break;
 default: memcpy(out, &data->m_Value.m_Int, 4); break;
 }
}
