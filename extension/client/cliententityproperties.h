#ifndef H_CBASENPC_CLIENT_ENTITY_PROPERTIES_
#define H_CBASENPC_CLIENT_ENTITY_PROPERTIES_

#include <datamap.h>
#include <dt_recv.h>
#include <string>
#include <unordered_map>
struct CBaseNPCRecvField;

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
  bool FindDataMapInfo(datamap_t *map, const char *name, ClientDataMapInfo *result);
  bool FindRecvPropInfo(RecvTable *table, const char *name, ClientRecvPropInfo *result);
private:
  std::unordered_map<datamap_t *, std::unordered_map<std::string, ClientDataMapInfo>> dataMaps_;
  std::unordered_map<RecvTable *, std::unordered_map<std::string, ClientRecvPropInfo>> recvTables_;
};
#endif
