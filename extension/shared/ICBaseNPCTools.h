#ifndef CBASENPC_TOOLS_H
#define CBASENPC_TOOLS_H

// Keep the established binary identity used by existing dependent extensions.
// Renaming the C++ wrapper must not create an incompatible ShareSys interface.
#define SMINTERFACE_CBASENPC_TOOLS_NAME		"NPCTools"
#define SMINTERFACE_CBASENPC_TOOLS_VERSION	2
#define MAX_NPCS						200
#define INVALID_NPC_ID					-1

class CExtNPC;
class INextBot;
class IBody;
class ILocomotion;
class CBaseEntity;
class NextBotGroundLocomotion;
class INextBotEventResponder_Hook;
class ILocomotion_Hook;
class NextBotGroundLocomotion_Hook;
class IBody_Hook;

#include <IShareSys.h>

class ICBaseNPCTools : public SourceMod::SMInterface
{
public:
	virtual const char* GetInterfaceName() = 0;
	virtual unsigned int GetInterfaceVersion() = 0;
public:
	/**
	 * @brief Grants an index to an npc entity, this index will be used by plugins to store information
	 *
	 * @return			NPC index, -1 otherwise
	 */
	virtual int GrantID(CBaseEntity* ent, CExtNPC* npc) = 0;

	/**
	 * @brief Deletes an npc object from the array of registered npcs
	 *
	 * @return			Deleted npc
	 */
	virtual CExtNPC* DeleteNPC(CExtNPC* npc) = 0;

	/**
	 * @brief Deletes an npc object from the array of registered npcs
	 *
	 * @return			Deleted npc
	 */
	virtual CExtNPC* DeleteNPCByEntIndex(int index) = 0;

	/**
	 * @brief Retrieves the nextbot interface from a given entity;
	 *
	 * @return			Returns nextbot interface, NULL otherwise
	 */
	virtual INextBot* GetNextBotOfEntity(CBaseEntity* pEntity) = 0;
};

extern ICBaseNPCTools* g_pCBaseNPCTools;

#endif // CBASENPC_TOOLS_H
