#ifndef H_CBASENPC_RECV_PROXY_
#define H_CBASENPC_RECV_PROXY_

#include <cstddef>
#include <dt_recv.h>

class CBaseNPCRecvProxy final
{
public:
	CBaseNPCRecvProxy();
	bool Init(char* error, size_t maxlength);
	void Shutdown();
	bool IsInitialized() const;
	CStandardRecvProxies* GetStandardRecvProxies() const;
	RecvVarProxyFn GetIntProxy(size_t size) const;
	RecvVarProxyFn GetFloatToFloat() const;
	RecvVarProxyFn GetVectorToVector() const;
	RecvVarProxyFn GetEHandleProxy() const;
	RecvVarProxyFn GetColor32Proxy() const;

	// Normal pOut semantics: future sidecar layout is supplied by the manager,
	// never inferred from m_ObjectID or from server datamap offsets.
	static void VectorXYToVectorXY(const CRecvProxyData* pData, void* pStruct, void* pOut);
	static void StringToString(const CRecvProxyData* pData, void* pStruct, void* pOut);
	static void StaticDataTable(const RecvProp* pProp, void** pOut, void* pData, int objectID);

private:
	CStandardRecvProxies* m_pStandardRecvProxies;
	RecvVarProxyFn m_pEHandleProxy;
	RecvVarProxyFn m_pColor32Proxy;
	bool m_bInitialized;
};

extern CBaseNPCRecvProxy g_CBaseNPCRecvProxy;

#endif
