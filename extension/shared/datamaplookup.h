#ifndef H_CBASENPC_DATAMAP_LOOKUP_
#define H_CBASENPC_DATAMAP_LOOKUP_
#include <datamap.h>
#include <climits>
#include <cstddef>
#include <cstring>

// Both server factory datamaps and client stock datamaps use this lookup.
namespace CBaseNPCDataMapLookup
{
inline typedescription_t *FindRecursive(datamap_t *map, const char *name, int offset,
  int *actualOffset, datamap_t **path, size_t depth, size_t &remaining)
{
  if (!map || depth >= 64 || !remaining) return nullptr;
  for (size_t i = 0; i < depth; ++i)
    if (path[i] == map) return nullptr;
  path[depth] = map;
  if (map->dataNumFields < 0 || map->dataNumFields > 65536 ||
      (map->dataNumFields && !map->dataDesc)) return nullptr;
  for (int i = 0; i < map->dataNumFields && remaining; ++i)
  {
    --remaining;
    auto *field = &map->dataDesc[i];
    const int local = field->fieldOffset[TD_OFFSET_NORMAL];
    if (!field->fieldName || local < 0 || offset > INT_MAX - local) continue;
    if (!std::strcmp(name, field->fieldName))
    {
      if (actualOffset) *actualOffset = offset + local;
      return field;
    }
    if (auto *found = FindRecursive(field->td, name, offset + local,
                                    actualOffset, path, depth + 1, remaining))
      return found;
  }
  return FindRecursive(map->baseMap, name, offset, actualOffset, path, depth + 1, remaining);
}
inline typedescription_t *Find(datamap_t *map, const char *name, int *actualOffset)
{
  if (actualOffset) *actualOffset = -1;
  if (!name || !*name) return nullptr;
  datamap_t *path[64];
  size_t remaining = 65536;
  return FindRecursive(map, name, 0, actualOffset, path, 0, remaining);
}
}
#endif
