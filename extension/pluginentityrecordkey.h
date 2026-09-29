#ifndef CBASENPC_PLUGIN_ENTITY_RECORD_KEY_H
#define CBASENPC_PLUGIN_ENTITY_RECORD_KEY_H

class CBaseEntity;

// Internal record identity is a native pointer, never a 32-bit SourcePawn cell.
// Keeping this tiny declaration independent of the 32-bit Source SDK lets the
// pointer-width contract receive real x64 compile coverage.
using PluginEntityRecordKey = CBaseEntity *;
static_assert(sizeof(PluginEntityRecordKey) == sizeof(void *),
              "Entity record keys must preserve the native pointer width");

#endif
