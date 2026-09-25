#ifndef H_CBASENPC_SEND_PROXY_
#define H_CBASENPC_SEND_PROXY_

#ifdef _WIN32
#pragma once
#endif

#include <cstddef>

#include <dt_send.h>

class CBaseNPCSendProxy final
{
public:
	CBaseNPCSendProxy();

	bool Init(char* error, size_t maxlength);
	void Shutdown();

	bool IsInitialized() const;

	CStandardSendProxies* GetStandardSendProxies() const;

	SendVarProxyFn GetIntProxy(size_t size, bool isUnsigned) const;
	SendVarProxyFn GetFloatToFloat() const;
	SendVarProxyFn GetVectorToVector() const;

	SendTableProxyFn GetDataTableToDataTable() const;
	SendTableProxyFn GetSendLocalDataTable() const;

	SendVarProxyFn GetEHandleToInt() const;

public:
	static void VectorXYToVectorXY(
		const SendProp* pProp,
		const void* pStruct,
		const void* pData,
		DVariant* pOut,
		int iElement,
		int objectID);

	static void AngleToFloat(
		const SendProp* pProp,
		const void* pStruct,
		const void* pData,
		DVariant* pOut,
		int iElement,
		int objectID);

	static void QAngles(
		const SendProp* pProp,
		const void* pStruct,
		const void* pData,
		DVariant* pOut,
		int iElement,
		int objectID);

	static void StringToString(
		const SendProp* pProp,
		const void* pStruct,
		const void* pData,
		DVariant* pOut,
		int iElement,
		int objectID);

	static void StringTToString(
		const SendProp* pProp,
		const void* pStruct,
		const void* pData,
		DVariant* pOut,
		int iElement,
		int objectID);

	static void Color32ToInt(
		const SendProp* pProp,
		const void* pStruct,
		const void* pData,
		DVariant* pOut,
		int iElement,
		int objectID);

private:
	CStandardSendProxies* m_pStandardSendProxies;
	SendVarProxyFn m_pEHandleToInt;
	bool m_bInitialized;
};

extern CBaseNPCSendProxy g_CBaseNPCSendProxy;

#endif
