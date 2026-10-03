#include "cbasenpcrecvproxy.h"
#include "client/cliententitymanager.h"
#include "smsdk_ext.h"

#include <cdll_int.h>
#include <icliententitylist.h>
#include <cstdio>
#include <cstring>

CBaseNPCRecvProxy g_CBaseNPCRecvProxy;

CBaseNPCRecvProxy::CBaseNPCRecvProxy()
	: m_pStandardRecvProxies(nullptr), m_pEHandleProxy(nullptr),
	  m_pColor32Proxy(nullptr), m_bInitialized(false)
{
}

bool CBaseNPCRecvProxy::Init(char* error, size_t maxlength)
{
	if (m_bInitialized) return true;
	Shutdown();
	auto fail = [this, error, maxlength](const char* message) -> bool
	{
		if (error && maxlength) snprintf(error, maxlength, "%s", message);
		Shutdown();
		return false;
	};
	if (!g_ClientEntityManager.IsAvailable())
		return fail("CClientEntityManager must initialize before CBaseNPCRecvProxy");
	m_pStandardRecvProxies = g_ClientEntityManager.GetStandardRecvProxies();
	if (!m_pStandardRecvProxies)
		return fail(CLIENT_DLL_INTERFACE_VERSION "::GetStandardRecvProxies returned null");

#define VALIDATE_STANDARD_PROXY(member) \
	if (!m_pStandardRecvProxies->member) \
		return fail("CStandardRecvProxies is missing " #member)
	VALIDATE_STANDARD_PROXY(m_Int32ToInt8);
	VALIDATE_STANDARD_PROXY(m_Int32ToInt16);
	VALIDATE_STANDARD_PROXY(m_Int32ToInt32);
	VALIDATE_STANDARD_PROXY(m_FloatToFloat);
	VALIDATE_STANDARD_PROXY(m_VectorToVector);
#undef VALIDATE_STANDARD_PROXY

	const char* handleNames[] = {"m_hOwnerEntity", "m_hEffectEntity"};
	const char* handleSource = nullptr;
	const char* failures[2] = {};
	for (size_t i = 0; i < 2; ++i)
	{
		RecvProp* prop = g_ClientEntityManager.FindRecvProp("CBaseEntity", handleNames[i]);
		if (!prop) failures[i] = "RecvProp not found";
		else if (prop->GetType() != DPT_Int) failures[i] = "not DPT_Int";
		else if (!prop->GetProxyFn()) failures[i] = "no RecvVarProxyFn";
		else
		{
			m_pEHandleProxy = prop->GetProxyFn();
			handleSource = handleNames[i];
			break;
		}
	}
	if (!m_pEHandleProxy)
	{
		char message[256];
		snprintf(message, sizeof(message), "Failed to resolve EHandle RecvProxy: CBaseEntity.%s (%s); CBaseEntity.%s (%s)",
			handleNames[0], failures[0], handleNames[1], failures[1]);
		return fail(message);
	}
	RecvProp* color = g_ClientEntityManager.FindRecvProp("CBaseEntity", "m_clrRender");
	if (color && color->GetType() == DPT_Int && color->GetProxyFn())
		m_pColor32Proxy = color->GetProxyFn();
	m_bInitialized = true;
	g_pSM->LogMessage(myself,
		"[CBASENPC] Client networking runtime initialized: interface=%s; entity list=%s; ClientClasses=%u; StandardRecvProxies=OK; EHandle=CBaseEntity.%s; Color=%s",
		CLIENT_DLL_INTERFACE_VERSION, VCLIENTENTITYLIST_INTERFACE_VERSION,
		static_cast<unsigned int>(g_ClientEntityManager.GetClassCount()), handleSource,
		m_pColor32Proxy ? "CBaseEntity.m_clrRender" : "unavailable (stock DPT_Int RecvProp/proxy missing)");
	return true;
}

void CBaseNPCRecvProxy::Shutdown()
{
	// All retained objects/functions are owned by client.dll.
	m_pStandardRecvProxies = nullptr;
	m_pEHandleProxy = nullptr;
	m_pColor32Proxy = nullptr;
	m_bInitialized = false;
}

bool CBaseNPCRecvProxy::IsInitialized() const { return m_bInitialized; }
CStandardRecvProxies* CBaseNPCRecvProxy::GetStandardRecvProxies() const { return m_pStandardRecvProxies; }
RecvVarProxyFn CBaseNPCRecvProxy::GetIntProxy(size_t size) const
{
	if (!m_pStandardRecvProxies) return nullptr;
	switch (size)
	{
	case 1: return m_pStandardRecvProxies->m_Int32ToInt8;
	case 2: return m_pStandardRecvProxies->m_Int32ToInt16;
	case 4: return m_pStandardRecvProxies->m_Int32ToInt32;
	default: return nullptr;
	}
}
RecvVarProxyFn CBaseNPCRecvProxy::GetFloatToFloat() const
{
	return m_pStandardRecvProxies ? m_pStandardRecvProxies->m_FloatToFloat : nullptr;
}
RecvVarProxyFn CBaseNPCRecvProxy::GetVectorToVector() const
{
	return m_pStandardRecvProxies ? m_pStandardRecvProxies->m_VectorToVector : nullptr;
}
RecvVarProxyFn CBaseNPCRecvProxy::GetEHandleProxy() const { return m_pEHandleProxy; }
RecvVarProxyFn CBaseNPCRecvProxy::GetColor32Proxy() const { return m_pColor32Proxy; }

void CBaseNPCRecvProxy::VectorXYToVectorXY(const CRecvProxyData* pData, void* pStruct, void* pOut)
{
	(void)pStruct;
	if (!pData || !pOut) return;
	auto out = static_cast<float*>(pOut);
	out[0] = pData->m_Value.m_Vector[0];
	out[1] = pData->m_Value.m_Vector[1];
}

void CBaseNPCRecvProxy::StringToString(const CRecvProxyData* pData, void* pStruct, void* pOut)
{
	(void)pStruct;
	if (!pData || !pData->m_pRecvProp || !pOut) return;
	const int size = pData->m_pRecvProp->m_StringBufferSize;
	if (size <= 0) return;
	auto out = static_cast<char*>(pOut);
	const char* source = pData->m_Value.m_pString;
	std::strncpy(out, source ? source : "", static_cast<size_t>(size - 1));
	out[size - 1] = '\0';
}

void CBaseNPCRecvProxy::StaticDataTable(const RecvProp* pProp, void** pOut, void* pData, int objectID)
{
	(void)pProp;
	(void)objectID;
	if (pOut) *pOut = pData;
}
