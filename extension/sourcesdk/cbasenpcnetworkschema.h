#ifndef CBASENPC_NETWORK_SCHEMA_H
#define CBASENPC_NETWORK_SCHEMA_H
#include <cstddef>

constexpr const char* CBASENPC_CLASSNAME_PROP = "m_szCBaseNPCServerClassname";
// Protocol ABI, including NUL. The Source string encoder truncates to 511 bytes;
// keep the bridge independent of per-entity storage and plugin buffer sizes.
constexpr size_t CBASENPC_NETWORK_CLASSNAME_LENGTH = 512;
class CBaseNPCNetworkSchemaManager
{
public:
 void ConfigureLoad(bool lateLoad, bool entitiesExist);
 bool Finalize(char* error, size_t maxlength);
 bool IsFinalized() const { return finalized_ && !failed_; }
 bool IsPublished() const { return published_; }
 bool IsClassnameBridgeEnabled() const { return classnameBridgeEnabled_; }
 void StopAfterUnload() { failed_ = true; }
private:
 bool attempted_ = false, finalized_ = false, failed_ = false, published_ = false;
 bool lateLoad_ = false;
 bool classnameBridgeEnabled_ = false;
};
extern CBaseNPCNetworkSchemaManager g_CBaseNPCNetworkSchemaManager;
extern bool g_CBaseNPCNetworkDebugEnabled;
inline bool CBaseNPCNetworkDebugEnabled() { return g_CBaseNPCNetworkDebugEnabled; }
#endif
