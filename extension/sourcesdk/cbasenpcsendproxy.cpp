#include "cbasenpcsendproxy.h"

#include "smsdk_ext.h"

#include <cstdint>
#include <cstdio>

#include <mathlib/mathlib.h>
#include <string_t.h>
#include <tier0/basetypes.h>
#include <vector.h>

namespace
{
template <typename T>
auto TryGetSendLocalDataTable(T* proxies, int) -> decltype(proxies->m_SendLocalDataTable)
{
	return proxies->m_SendLocalDataTable;
}

SendTableProxyFn TryGetSendLocalDataTable(CStandardSendProxies*, long)
{
	// Some Source SDK branches do not expose this optional standard proxy in
	// CStandardSendProxies. Do not infer a private engine object layout.
	return nullptr;
}
}

CBaseNPCSendProxy g_CBaseNPCSendProxy;

CBaseNPCSendProxy::CBaseNPCSendProxy()
	: m_pStandardSendProxies(nullptr),
	  m_pEHandleToInt(nullptr),
	  m_bInitialized(false)
{
}

bool CBaseNPCSendProxy::Init(char* error, size_t maxlength)
{
	if (m_bInitialized)
	{
		return true;
	}

	Shutdown();

	auto fail = [this, error, maxlength](const char* message) -> bool
	{
		if (error && maxlength > 0)
		{
			snprintf(error, maxlength, "%s", message);
		}

		Shutdown();
		return false;
	};

	if (!gamedll)
	{
		return fail("IServerGameDLL is not available");
	}

	if (!gamehelpers)
	{
		return fail("IGameHelpers is not available");
	}

	m_pStandardSendProxies = gamedll->GetStandardSendProxies();
	if (!m_pStandardSendProxies)
	{
		return fail("Failed to retrieve CStandardSendProxies");
	}

#define VALIDATE_STANDARD_PROXY(member) \
	if (!m_pStandardSendProxies->member) \
	{ \
		return fail("CStandardSendProxies is missing " #member); \
	}

	VALIDATE_STANDARD_PROXY(m_Int8ToInt32);
	VALIDATE_STANDARD_PROXY(m_Int16ToInt32);
	VALIDATE_STANDARD_PROXY(m_Int32ToInt32);
	VALIDATE_STANDARD_PROXY(m_UInt8ToInt32);
	VALIDATE_STANDARD_PROXY(m_UInt16ToInt32);
	VALIDATE_STANDARD_PROXY(m_UInt32ToInt32);
	VALIDATE_STANDARD_PROXY(m_FloatToFloat);
	VALIDATE_STANDARD_PROXY(m_VectorToVector);
	VALIDATE_STANDARD_PROXY(m_DataTableToDataTable);

#undef VALIDATE_STANDARD_PROXY

	SourceMod::sm_sendprop_info_t info = {};
	const char* propertyName = "m_hOwnerEntity";

	if (!gamehelpers->FindSendPropInfo("CBaseEntity", propertyName, &info))
	{
		propertyName = "m_hEffectEntity";
		if (!gamehelpers->FindSendPropInfo("CBaseEntity", propertyName, &info))
		{
			return fail(
				"Failed to find CBaseEntity.m_hOwnerEntity or "
				"CBaseEntity.m_hEffectEntity SendProp");
		}
	}

	SendProp* prop = info.prop;
	char validationError[256];

	if (!prop)
	{
		snprintf(validationError, sizeof(validationError),
			"CBaseEntity.%s has no SendProp", propertyName);
		return fail(validationError);
	}

	if (prop->GetType() != DPT_Int)
	{
		snprintf(validationError, sizeof(validationError),
			"CBaseEntity.%s is not DPT_Int", propertyName);
		return fail(validationError);
	}

	if (prop->IsSigned())
	{
		snprintf(validationError, sizeof(validationError),
			"CBaseEntity.%s is not unsigned", propertyName);
		return fail(validationError);
	}

	if (prop->m_nBits <= 0)
	{
		snprintf(validationError, sizeof(validationError),
			"CBaseEntity.%s has an invalid bit count", propertyName);
		return fail(validationError);
	}

	m_pEHandleToInt = prop->GetProxyFn();
	if (!m_pEHandleToInt)
	{
		snprintf(validationError, sizeof(validationError),
			"CBaseEntity.%s has no SendVarProxyFn", propertyName);
		return fail(validationError);
	}

	m_bInitialized = true;
	return true;
}

void CBaseNPCSendProxy::Shutdown()
{
	m_pStandardSendProxies = nullptr;
	m_pEHandleToInt = nullptr;
	m_bInitialized = false;
}

bool CBaseNPCSendProxy::IsInitialized() const
{
	return m_bInitialized;
}

CStandardSendProxies* CBaseNPCSendProxy::GetStandardSendProxies() const
{
	return m_pStandardSendProxies;
}

SendVarProxyFn CBaseNPCSendProxy::GetIntProxy(size_t size, bool isUnsigned) const
{
	if (!m_pStandardSendProxies)
	{
		return nullptr;
	}

	switch (size)
	{
	case 1:
		return isUnsigned
			? m_pStandardSendProxies->m_UInt8ToInt32
			: m_pStandardSendProxies->m_Int8ToInt32;
	case 2:
		return isUnsigned
			? m_pStandardSendProxies->m_UInt16ToInt32
			: m_pStandardSendProxies->m_Int16ToInt32;
	case 4:
		return isUnsigned
			? m_pStandardSendProxies->m_UInt32ToInt32
			: m_pStandardSendProxies->m_Int32ToInt32;
	default:
		return nullptr;
	}
}

SendVarProxyFn CBaseNPCSendProxy::GetFloatToFloat() const
{
	return m_pStandardSendProxies
		? m_pStandardSendProxies->m_FloatToFloat
		: nullptr;
}

SendVarProxyFn CBaseNPCSendProxy::GetVectorToVector() const
{
	return m_pStandardSendProxies
		? m_pStandardSendProxies->m_VectorToVector
		: nullptr;
}

SendTableProxyFn CBaseNPCSendProxy::GetDataTableToDataTable() const
{
	return m_pStandardSendProxies
		? m_pStandardSendProxies->m_DataTableToDataTable
		: nullptr;
}

SendTableProxyFn CBaseNPCSendProxy::GetSendLocalDataTable() const
{
	return m_pStandardSendProxies
		? TryGetSendLocalDataTable(m_pStandardSendProxies, 0)
		: nullptr;
}

SendVarProxyFn CBaseNPCSendProxy::GetEHandleToInt() const
{
	return m_pEHandleToInt;
}

void CBaseNPCSendProxy::VectorXYToVectorXY(
	const SendProp* pProp,
	const void* pStruct,
	const void* pData,
	DVariant* pOut,
	int iElement,
	int objectID)
{
	(void)pProp;
	(void)pStruct;
	(void)iElement;
	(void)objectID;

	const Vector* value = reinterpret_cast<const Vector*>(pData);
	pOut->m_Vector[0] = value->x;
	pOut->m_Vector[1] = value->y;
	pOut->m_Vector[2] = 0.0f;
}

void CBaseNPCSendProxy::AngleToFloat(
	const SendProp* pProp,
	const void* pStruct,
	const void* pData,
	DVariant* pOut,
	int iElement,
	int objectID)
{
	(void)pProp;
	(void)pStruct;
	(void)iElement;
	(void)objectID;

	const float angle = *reinterpret_cast<const float*>(pData);
	pOut->m_Float = anglemod(angle);
}

void CBaseNPCSendProxy::QAngles(
	const SendProp* pProp,
	const void* pStruct,
	const void* pData,
	DVariant* pOut,
	int iElement,
	int objectID)
{
	(void)pProp;
	(void)pStruct;
	(void)iElement;
	(void)objectID;

	const QAngle* angles = reinterpret_cast<const QAngle*>(pData);
	pOut->m_Vector[0] = anglemod(angles->x);
	pOut->m_Vector[1] = anglemod(angles->y);
	pOut->m_Vector[2] = anglemod(angles->z);
}

void CBaseNPCSendProxy::StringToString(
	const SendProp* pProp,
	const void* pStruct,
	const void* pData,
	DVariant* pOut,
	int iElement,
	int objectID)
{
	(void)pProp;
	(void)pStruct;
	(void)iElement;
	(void)objectID;

	static const char emptyString[] = "";
	pOut->m_pString = pData
		? reinterpret_cast<const char*>(pData)
		: emptyString;
}

void CBaseNPCSendProxy::StringTToString(
	const SendProp* pProp,
	const void* pStruct,
	const void* pData,
	DVariant* pOut,
	int iElement,
	int objectID)
{
	(void)pProp;
	(void)pStruct;
	(void)iElement;
	(void)objectID;

	static const char emptyString[] = "";
	if (!pData)
	{
		pOut->m_pString = emptyString;
		return;
	}

	const string_t& value = *reinterpret_cast<const string_t*>(pData);
	if (value == NULL_STRING)
	{
		pOut->m_pString = emptyString;
		return;
	}

	const char* stringValue = STRING(value);
	pOut->m_pString = stringValue ? stringValue : emptyString;
}

void CBaseNPCSendProxy::Color32ToInt(
	const SendProp* pProp,
	const void* pStruct,
	const void* pData,
	DVariant* pOut,
	int iElement,
	int objectID)
{
	(void)pProp;
	(void)pStruct;
	(void)iElement;
	(void)objectID;

	const color32* color = reinterpret_cast<const color32*>(pData);
	const uint32_t packed =
		(static_cast<uint32_t>(color->r) << 24) |
		(static_cast<uint32_t>(color->g) << 16) |
		(static_cast<uint32_t>(color->b) << 8) |
		static_cast<uint32_t>(color->a);

	pOut->m_Int = static_cast<int>(packed);
}
