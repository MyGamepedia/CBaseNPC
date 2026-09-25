// Construction semantics adapted from the Source SDK public/dt_send.cpp.
// Deliberately does not define a second standard SendProxy registry.
#include "cbasenpcsendtable.h"
#include "cbasenpcsendproxy.h"
#include <const.h>
#include <string_t.h>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <climits>

// These small out-of-line SDK methods are not supplied by our link libraries.
// Initialize every member, including fields added in the BMS SDK headers.
SendProp::SendProp()
{
	m_pMatchingRecvProp = nullptr;
	m_Type = DPT_Int;
	m_nBits = 0;
	m_fLowValue = m_fHighValue = m_fHighLowMul = 0.0f;
	m_pArrayProp = nullptr;
	m_ArrayLengthProxy = nullptr;
	m_nElements = 1;
	m_ElementStride = -1;
	m_pExcludeDTName = m_pParentArrayPropName = m_pVarName = nullptr;
	m_Flags = 0;
	m_ProxyFn = nullptr;
	m_DataTableProxyFn = nullptr;
	m_pDataTable = nullptr;
	m_Offset = 0;
	m_pExtraData = nullptr;
#ifdef SENDPROP_DEFAULT_PRIORITY
	m_priority = SENDPROP_DEFAULT_PRIORITY;
#endif
}
SendProp::~SendProp() {}
SendTable::SendTable() { Construct(nullptr, 0, nullptr); }
SendTable::SendTable(SendProp* props, int count, const char* name) { Construct(props, count, name); }
SendTable::~SendTable() {}
void SendTable::Construct(SendProp* props, int count, const char* name)
{
	m_pProps = props;
	m_nProps = count;
	m_pNetTableName = name;
	m_pPrecalc = nullptr;
	m_bInitialized = false;
	m_bHasBeenWritten = false;
	m_bHasPropsEncodedAgainstCurrentTickCount = false;
}

namespace
{
constexpr int kCoords = SPROP_COORD | SPROP_COORD_MP | SPROP_COORD_MP_LOWPRECISION | SPROP_COORD_MP_INTEGRAL;
// The SDK clears m_nBits for these codecs, but not for XYZE.
constexpr int kSpecial = kCoords | SPROP_NOSCALE | SPROP_NORMAL;
constexpr int kModes = kSpecial | SPROP_XYZE;
constexpr int kRound = SPROP_ROUNDDOWN | SPROP_ROUNDUP;
using Kind = CBaseNPCSendFieldKind;

bool Fail(std::string& error, const char* reason) { error = reason; return false; }
bool MultipleBits(int flags) { return flags && (flags & (flags - 1)); }

SendProp MakeDataTable(const char* name, int offset, SendTable* table, bool collapsible)
{
	SendProp prop;
	prop.m_Type = DPT_DataTable;
	prop.m_pVarName = name;
	prop.SetOffset(offset);
	prop.SetDataTable(table);
	prop.SetDataTableProxyFn(g_CBaseNPCSendProxy.GetDataTableToDataTable());
	prop.SetFlags(SPROP_PROXY_ALWAYS_YES | (collapsible ? SPROP_COLLAPSIBLE : 0));
	return prop;
}

bool MakeScalar(const typedescription_t& td, const CBaseNPCSendFieldDesc& desc,
	SendProp& prop, std::string& error)
{
	if (!CBaseNPC_ValidateSendFieldOptions(desc, error))
		return false;
	int size = td.fieldSizeInBytes / td.fieldSize;
	int expectedSize = 4;
	fieldtype_t expectedType = FIELD_INTEGER;
	int bits = desc.bits;
	int flags = desc.flags;
	float low = desc.lowValue, high = desc.highValue;
	SendVarProxyFn proxy = nullptr;
	bool floating = false, angle = false;
	prop.m_Type = DPT_Int;
	switch (desc.kind)
	{
	case Kind::Int: break;
	case Kind::Short: expectedSize = 2; expectedType = FIELD_SHORT; break;
	case Kind::Char: expectedSize = 1; expectedType = FIELD_CHARACTER; break;
	case Kind::Bool:
		expectedSize = 1; expectedType = FIELD_BOOLEAN; bits = 1; flags |= SPROP_UNSIGNED; break;
	case Kind::Color32:
		expectedType = FIELD_COLOR32; bits = 32; flags |= SPROP_UNSIGNED;
		proxy = &CBaseNPCSendProxy::Color32ToInt; break;
	case Kind::EHandle:
		expectedType = FIELD_EHANDLE;
		bits = g_CBaseNPCSendProxy.GetEHandleBits();
		flags |= g_CBaseNPCSendProxy.GetEHandleFlags();
		proxy = g_CBaseNPCSendProxy.GetEHandleToInt(); break;
	case Kind::ModelIndex: expectedType = FIELD_MODELINDEX; bits = SP_MODEL_INDEX_BITS; break;
	case Kind::Float: case Kind::Time: case Kind::Angle:
		expectedType = desc.kind == Kind::Time ? FIELD_TIME : FIELD_FLOAT;
		prop.m_Type = DPT_Float; floating = true;
		angle = desc.kind == Kind::Angle;
		proxy = angle ? &CBaseNPCSendProxy::AngleToFloat : g_CBaseNPCSendProxy.GetFloatToFloat();
		if (desc.kind == Kind::Time) { bits = 32; flags |= SPROP_NOSCALE; }
		break;
	case Kind::Vector: case Kind::VectorXY: case Kind::QAngle:
		expectedType = FIELD_VECTOR; expectedSize = 12; floating = true;
		angle = desc.kind == Kind::QAngle;
		prop.m_Type = desc.kind == Kind::VectorXY ? DPT_VectorXY : DPT_Vector;
		proxy = angle ? &CBaseNPCSendProxy::QAngles :
			(desc.kind == Kind::VectorXY ? &CBaseNPCSendProxy::VectorXYToVectorXY : g_CBaseNPCSendProxy.GetVectorToVector());
		break;
	case Kind::StringT:
		expectedType = FIELD_STRING; expectedSize = sizeof(string_t);
		prop.m_Type = DPT_String; bits = 0;
		proxy = &CBaseNPCSendProxy::StringTToString;
		// Like SDK SendPropString, bufferLen is a validation argument, not a
		// SendProp member or a wire-schema length. The engine caps strings.
		break;
	default: return Fail(error, "unsupported send field kind");
	}
	if (td.fieldType != expectedType || size != expectedSize)
		return Fail(error, "datamap type/element size does not match the SendProp kind");
	if (prop.m_Type == DPT_Int)
	{
		if (bits <= 0) bits = size * 8;
		if (bits < 1 || bits > 32) return Fail(error, "invalid integer bit count");
		if (!proxy) proxy = g_CBaseNPCSendProxy.GetIntProxy(size, (flags & SPROP_UNSIGNED) != 0);
	}
	if (floating)
	{
		if (angle)
		{
			if (bits <= 0) bits = 32;
			if (bits == 32) flags |= SPROP_NOSCALE;
			low = 0.0f; high = 360.0f;
			// Angle/QAngles retain their bit count, as in the SDK helpers.
		}
		else if (bits <= 0 || bits == 32)
		{
			// Explicit coordinate/normal modes must not acquire a competing
			// NOSCALE flag. In the ordinary full-resolution case use NOSCALE.
			if (!(flags & kModes)) flags |= SPROP_NOSCALE;
			if ((flags & SPROP_XYZE) && bits <= 0) bits = 32;
			low = high = 0.0f;
		}
		else
		{
			const float levels = std::ldexp(1.0f, bits);
			if (high == HIGH_DEFAULT) high = levels;
			if (flags & SPROP_ROUNDDOWN) high -= (high - low) / levels;
			else if (flags & SPROP_ROUNDUP) low += (high - low) / levels;
		}
		if (!(flags & kModes) && (!std::isfinite(low) || !std::isfinite(high) || high <= low))
			return Fail(error, "quantized range must be finite and high must exceed low after rounding");
		prop.m_fLowValue = low;
		prop.m_fHighValue = high;
		prop.m_fHighLowMul = CBaseNPC_AssignRangeMultiplier(bits > 0 ? bits : 32, double(high) - low);
		if (!std::isfinite(prop.m_fHighLowMul)) return Fail(error, "range multiplier overflows");
		if (!angle && (flags & kSpecial)) bits = 0;
	}
	if (!proxy) return Fail(error, "required SendProxy is unavailable");
	prop.m_nBits = bits;
	prop.SetFlags(flags);
	prop.SetProxyFn(proxy);
#ifdef SENDPROP_CHANGES_OFTEN_PRIORITY
	if (flags & SPROP_CHANGES_OFTEN) prop.SetPriority(SENDPROP_CHANGES_OFTEN_PRIORITY);
#endif
	return true;
}
}

bool CBaseNPC_ValidateSendFieldOptions(const CBaseNPCSendFieldDesc& desc, std::string& error)
{
	int allowed = SPROP_CHANGES_OFTEN;
	bool hasBits = false, hasRange = false;
	switch (desc.kind)
	{
	case Kind::Int: case Kind::Short: case Kind::Char:
		allowed |= SPROP_UNSIGNED | SPROP_VARINT; hasBits = true; break;
	case Kind::Float:
		allowed |= kCoords | SPROP_NOSCALE | kRound; hasBits = hasRange = true; break;
	case Kind::Vector:
		allowed |= SPROP_NORMAL | SPROP_XYZE;
		// fall through
	case Kind::VectorXY:
		allowed |= kCoords | SPROP_NOSCALE | kRound; hasBits = hasRange = true; break;
	case Kind::Angle: case Kind::QAngle:
		allowed |= SPROP_NOSCALE; hasBits = true; break;
	case Kind::StringT:
		if (desc.stringMaxLength < 1 || desc.stringMaxLength > DT_MAX_STRING_BUFFERSIZE)
			return Fail(error, "string length must be in 1..DT_MAX_STRING_BUFFERSIZE");
		break;
	case Kind::Bool: case Kind::Time: case Kind::Color32: case Kind::EHandle: case Kind::ModelIndex: break;
	default: return Fail(error, "unsupported send field kind");
	}
	if (desc.flags & ~allowed)
	{
		char message[128];
		snprintf(message, sizeof(message), "invalid SendProp flags 0x%x (allowed 0x%x)", desc.flags, allowed);
		return Fail(error, message);
	}
	if (hasBits && (desc.bits < -1 || desc.bits > 32))
		return Fail(error, "bit count must be -1, 0 (full resolution), or 1..32");
	if (hasRange)
	{
		if (MultipleBits(desc.flags & (kSpecial | SPROP_XYZE)) || MultipleBits(desc.flags & kRound))
			return Fail(error, "conflicting SendProp encoding/rounding flags");
		if ((desc.flags & kRound) && ((desc.flags & (kSpecial | SPROP_XYZE)) || desc.bits <= 0 || desc.bits == 32))
			return Fail(error, "rounding requires a quantized range");
		if (!std::isfinite(desc.lowValue) || !std::isfinite(desc.highValue))
			return Fail(error, "range endpoints must be finite");
		if (!(desc.flags & (kSpecial | SPROP_XYZE)) && desc.bits > 0 && desc.bits < 32 &&
			(desc.highValue == HIGH_DEFAULT ? std::ldexp(1.0f, desc.bits) : desc.highValue) <= desc.lowValue)
			return Fail(error, "quantized range high must exceed low");
	}
	return true;
}

float CBaseNPC_AssignRangeMultiplier(int bits, double range)
{
	// uint32_t (not unsigned long) keeps the Source 32-bit semantics on LP64.
	const uint32_t high = bits == 32 ? UINT32_C(0xfffffffe) : (UINT32_C(1) << bits) - 1;
	if (std::fabs(range) < 0.001) return static_cast<float>(high);
	float mul = static_cast<float>(high / range);
	if (double(mul) * range > high)
	{
		const float corrections[] = {0.9999f, 0.99f, 0.9f, 0.8f, 0.7f};
		for (float correction : corrections)
		{
			mul = static_cast<float>(high / range) * correction;
			if (double(mul) * range <= high) break;
		}
	}
	return mul;
}

const char* CBaseNPCSendTable::CopyString(const char* value)
{
	m_Strings.emplace_back(value);
	return m_Strings.back().c_str();
}

CBaseNPCSendTable::CBaseNPCSendTable(const char* name, size_t fields, SendTable* base)
	: m_Props(new SendProp[fields + 1])
{
	m_Props[0] = MakeDataTable(CopyString("baseclass"), 0, base, true);
	m_Table.Construct(m_Props.get(), static_cast<int>(fields + 1), CopyString(name));
}

bool CBaseNPCSendTable::BuildField(size_t index, const typedescription_t& td,
	const CBaseNPCSendFieldDesc& desc, std::string& error)
{
	if (index + 1 >= static_cast<size_t>(m_Table.GetNumProps())) return Fail(error, "invalid property index");
	if (!td.fieldName || !td.fieldName[0] || !std::strcmp(td.fieldName, "baseclass"))
		return Fail(error, "empty or reserved field name");
	if (!td.fieldSize || td.fieldSize > MAX_ARRAY_ELEMENTS || td.fieldSizeInBytes <= 0 || td.fieldSizeInBytes % td.fieldSize)
		return Fail(error, "invalid field count/size or array exceeds MAX_ARRAY_ELEMENTS");
	const int offset = td.fieldOffset[TD_OFFSET_NORMAL];
#ifdef SENDPROP_OFFSET_MASK
	const int maxOffset = SENDPROP_OFFSET_MASK;
#else
	const int maxOffset = INT_MAX;
#endif
	if (offset < 0 || offset > maxOffset || td.fieldSizeInBytes - 1 > maxOffset - offset)
		return Fail(error, "field exceeds the SendProp offset range");
	SendProp scalar;
	if (!MakeScalar(td, desc, scalar, error)) return false;
	scalar.m_pVarName = CopyString(td.fieldName);
	scalar.SetOffset(offset);
	if (td.fieldSize == 1)
	{
		m_Props[index + 1] = scalar;
		return true;
	}
	const int stride = td.fieldSizeInBytes / td.fieldSize;
	scalar.SetOffset(0);
	std::unique_ptr<SendProp> element(new SendProp(scalar));
	std::unique_ptr<SendProp[]> props(new SendProp[td.fieldSize]);
	for (int i = 0; i < td.fieldSize; ++i)
	{
		char name[16];
		snprintf(name, sizeof(name), "%03d", i);
		props[i] = scalar;
		props[i].m_pVarName = CopyString(name);
		props[i].m_pParentArrayPropName = scalar.m_pVarName;
		props[i].SetOffset(i * stride);
	}
	std::unique_ptr<SendTable> table(new SendTable(props.get(), td.fieldSize, scalar.m_pVarName));
	SendProp outer = MakeDataTable(scalar.m_pVarName, offset, table.get(), false);
	outer.SetArrayProp(element.get());
	m_Props[index + 1] = outer;
	m_ArrayProps.push_back(std::move(props));
	m_ArrayTemplates.push_back(std::move(element));
	m_ArrayTables.push_back(std::move(table));
	return true;
}
