#ifndef H_CBASENPC_CLIENT_ENTITY_PROPERTIES_
#define H_CBASENPC_CLIENT_ENTITY_PROPERTIES_

#include <datamap.h>
#include <dt_recv.h>
#include <cstdint>
#include <string>
#include <unordered_map>
struct CBaseNPCRecvField;
// DPT_Int alone cannot distinguish an integer from an entity handle.
bool CBaseNPC_IsEHandleRecvProp(const RecvProp* prop, RecvVarProxyFn stockProxy);

struct ClientDataMapInfo
{
  typedescription_t *prop = nullptr;
  int actualOffset = -1;
};
struct ClientRecvPropInfo
{
  RecvProp *prop = nullptr;
  int actualOffset = -1;
  const CBaseNPCRecvField* sidecar = nullptr;
};

// Owned by CClientEntityManager. Cache keys refer to metadata, never entities.
class CClientEntityProperties final
{
public:
  void ClearCaches();
  void ResetStats() { cacheHits_ = cacheMisses_ = 0; }
  bool FindDataMapInfo(datamap_t *map, const char *name, ClientDataMapInfo *result);
  bool FindRecvPropInfo(RecvTable *table, const char *name, ClientRecvPropInfo *result);
  uint64_t CacheHits() const { return cacheHits_; }
  uint64_t CacheMisses() const { return cacheMisses_; }
private:
  std::unordered_map<datamap_t *, std::unordered_map<std::string, ClientDataMapInfo>> dataMaps_;
  std::unordered_map<RecvTable *, std::unordered_map<std::string, ClientRecvPropInfo>> recvTables_;
  uint64_t cacheHits_ = 0;
  uint64_t cacheMisses_ = 0;
};
#endif
