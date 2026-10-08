
#include "cbasenpc_behavior.h"
#include "callbackbuffers.h"
#include "pluginentityfactory.h"
#include <set>
#include <vector>

#define CBPUSHCELL(cell) pCallback->PushCell((cell_t)(cell));
#define CBPUSHFLOAT(fl) pCallback->PushCell(sp_ftoc(fl));
#define CBPUSHENTITY(ent) CBPUSHCELL(gamehelpers->EntityToBCompatRef(ent))
#define CBPUSHSTRING(str) pCallback->PushString(str);
#define CBPUSHVECTOR(vec) \
	{ \
		const Vector vecBuffer = vec; \
		callbackBuffers.PushArray(pCallback, {sp_ftoc(vecBuffer[0]), sp_ftoc(vecBuffer[1]), sp_ftoc(vecBuffer[2])}); \
	}

#define BEGINACTIONCALLBACKEX(funcName, typeName, ...) \
ActionResult< INextBot > CBaseNPCPluginAction:: funcName (INextBot* me, ##__VA_ARGS__) { \
	CBaseNPCCallbackBuffers callbackBuffers; \
	ActionCallbackFrame callbackFrame(Continue(), m_actionCallback, m_returnedActionReason); \
	IPluginFunction* pCallback = m_pFactory->GetCallback( CBaseNPCPluginActionFactory::CallbackType::typeName ); \
	if (pCallback && pCallback->IsRunnable()) { \
		pCallback->PushCell(PtrToPawnAddress(this)); pCallback->PushCell(gamehelpers->EntityToBCompatRef(me->GetEntity()));

#define BEGINACTIONCALLBACK(funcName, ...) BEGINACTIONCALLBACKEX(funcName, funcName, ##__VA_ARGS__)

#define ENDACTIONCALLBACK() \
		pCallback->Execute(nullptr); \
	} \
	return callbackFrame.Finish(); \
}

#define BEGINQUERYCALLBACK(funcName, ...) \
QueryResultType CBaseNPCPluginAction:: funcName ( const INextBot *me, ##__VA_ARGS__) const {	\
	CBaseNPCCallbackBuffers callbackBuffers; \
	cell_t result = ANSWER_UNDEFINED; \
	IPluginFunction* pCallback = m_pFactory->GetQueryCallback( CBaseNPCPluginActionFactory::QueryCallbackType::funcName ); \
	if (pCallback && pCallback->IsRunnable()) { \
		CBPUSHCELL(PtrToPawnAddress(this)); CBPUSHCELL(PtrToPawnAddress(me));

#define ENDQUERYCALLBACK() \
		cell_t callbackResult = result; \
		if (pCallback->Execute(&callbackResult) == SP_ERROR_NONE) result = callbackResult; \
	}	\
	return (QueryResultType)result; \
}

#define BEGINEVENTCALLBACKEX(funcName, typeName, ...) \
EventDesiredResult< INextBot > CBaseNPCPluginAction:: funcName (INextBot* me, ##__VA_ARGS__) {	\
	CBaseNPCCallbackBuffers callbackBuffers; \
	EventCallbackFrame callbackFrame(TryContinue(RESULT_NONE), m_eventCallback, m_returnedEventReason); \
	IPluginFunction* pCallback = m_pFactory->GetEventCallback( CBaseNPCPluginActionFactory::EventResponderCallbackType::typeName ); \
	if (pCallback && pCallback->IsRunnable()) { \
		pCallback->PushCell(PtrToPawnAddress(this)); \
		pCallback->PushCell(gamehelpers->EntityToBCompatRef(me->GetEntity()));

#define BEGINEVENTCALLBACK(funcName, ...) BEGINEVENTCALLBACKEX(funcName, funcName, ##__VA_ARGS__)

#define EVENTPUSHCELL(cell) CBPUSHCELL(cell)
#define EVENTPUSHFLOAT(fl) CBPUSHFLOAT(fl)
#define EVENTPUSHENTITY(ent) CBPUSHENTITY(ent)
#define EVENTPUSHSTRING(str) CBPUSHSTRING(str)
#define EVENTPUSHVECTOR(vec) CBPUSHVECTOR(vec)

#define ENDEVENTCALLBACK() \
		pCallback->Execute(nullptr); \
	}	\
	return callbackFrame.Finish(); \
}

#define ENDEVENTCALLBACK_NOEXECUTE() \
	}	\
	return callbackFrame.Finish(); \
}

// https://github.com/alliedmodders/sourcemod/blob/6928d21bcf746920b0f2f54e2c28b34097a66be2/core/smn_keyvalues.h#L42
struct KeyValueStack
{
	KeyValues *pBase;
	SourceHook::CStack<KeyValues *> pCurRoot;
	bool m_bDeleteOnDestroy = true;
};

extern IdentityToken_t * g_pCoreIdent;
extern HandleType_t g_KeyValueType;

HandleType_t g_BaseNPCPluginActionFactoryHandle;

CBaseNPCPluginActionFactories* g_pBaseNPCPluginActionFactories = new CBaseNPCPluginActionFactories();

CBaseNPCPluginAction::CBaseNPCPluginAction(CBaseNPCPluginActionFactory* pFactory) : 
	Action< INextBot >(),
	m_pFactory(pFactory)
{
	size_t dataSize = pFactory->GetActionDataSize();
	m_pData = (dataSize > 0) ? calloc(1, dataSize) : nullptr;

	pFactory->OnActionCreated(this);
}

CBaseNPCPluginAction::~CBaseNPCPluginAction()
{
	if (m_pData)
		free(m_pData);
	
	m_pFactory->OnActionRemoved(this);
}

datamap_t* CBaseNPCPluginAction::GetDataDescMap() const
{
	return m_pFactory->GetDataDescMap();
}

const char* CBaseNPCPluginAction::GetName() const 
{
	return m_pFactory->GetName();
}

void CBaseNPCPluginAction::PluginContinue()
{
	m_actionCallback->result = Continue();
	m_actionCallback->reason.Keep(nullptr);
}

void CBaseNPCPluginAction::PluginChangeTo( Action< INextBot > *action, const char *reason )
{
	m_actionCallback->result = ChangeTo(action, m_actionCallback->reason.Keep(reason));
}

void CBaseNPCPluginAction::PluginSuspendFor( Action< INextBot > *action, const char *reason )
{
	m_actionCallback->result = SuspendFor(action, m_actionCallback->reason.Keep(reason));
}

void CBaseNPCPluginAction::PluginDone( const char *reason )
{
	m_actionCallback->result = Done(m_actionCallback->reason.Keep(reason));
}

void CBaseNPCPluginAction::PluginTryContinue( EventResultPriorityType priority ) 
{ 
	m_eventCallback->result = TryContinue(priority);
	m_eventCallback->reason.Keep(nullptr);
}

void CBaseNPCPluginAction::PluginTryChangeTo( Action< INextBot > *action, EventResultPriorityType priority, const char *reason ) 
{ 
	m_eventCallback->result = TryChangeTo(action, priority, m_eventCallback->reason.Keep(reason));
}

void CBaseNPCPluginAction::PluginTrySuspendFor( Action< INextBot > *action, EventResultPriorityType priority, const char *reason ) 
{ 
	m_eventCallback->result = TrySuspendFor(action, priority, m_eventCallback->reason.Keep(reason));
}

void CBaseNPCPluginAction::PluginTryDone( EventResultPriorityType priority, const char *reason ) 
{ 
	m_eventCallback->result = TryDone(priority, m_eventCallback->reason.Keep(reason));
}

void CBaseNPCPluginAction::PluginTryToSustain( EventResultPriorityType priority, const char *reason ) 
{ 
	m_eventCallback->result = TryToSustain(priority, m_eventCallback->reason.Keep(reason));
}

// Actions

BEGINACTIONCALLBACK(OnStart, Action< INextBot > *prevAction)
	CBPUSHCELL(PtrToPawnAddress(prevAction))
ENDACTIONCALLBACK()

BEGINACTIONCALLBACK(Update, float interval)
	CBPUSHFLOAT(interval)
ENDACTIONCALLBACK()

BEGINACTIONCALLBACK(OnSuspend, Action< INextBot > *interruptingAction)
	CBPUSHCELL(PtrToPawnAddress(interruptingAction))
ENDACTIONCALLBACK()

BEGINACTIONCALLBACK(OnResume, Action< INextBot > *interruptingAction)
	CBPUSHCELL(PtrToPawnAddress(interruptingAction))
ENDACTIONCALLBACK()

void CBaseNPCPluginAction::OnEnd( INextBot * me, Action< INextBot > *nextAction ) {
	IPluginFunction* pCallback = m_pFactory->GetCallback( CBaseNPCPluginActionFactory::CallbackType::OnEnd );
	if (pCallback && pCallback->IsRunnable()) {
		CBPUSHCELL(PtrToPawnAddress(this))
		CBPUSHENTITY(me->GetEntity())
		CBPUSHCELL(PtrToPawnAddress(nextAction))
		pCallback->Execute(nullptr);
	}
}

Action< INextBot >* CBaseNPCPluginAction::InitialContainedAction( INextBot * me )
{
	cell_t result = 0;

	IPluginFunction* pCallback = m_pFactory->GetCallback( CBaseNPCPluginActionFactory::CallbackType::InitialContainedAction );
	if (pCallback && pCallback->IsRunnable()) 
	{
		CBPUSHCELL(PtrToPawnAddress(this))
		CBPUSHENTITY(me->GetEntity())

		pCallback->Execute(&result);
	}

	return (Action< INextBot >*)PawnAddressToPtr(result);
}

bool CBaseNPCPluginAction::IsAbleToBlockMovementOf( const INextBot *botInMotion ) const
{
	cell_t result = Action< INextBot >::IsAbleToBlockMovementOf( botInMotion );

	IPluginFunction* pCallback = m_pFactory->GetCallback( CBaseNPCPluginActionFactory::CallbackType::IsAbleToBlockMovementOf );
	if (pCallback && pCallback->IsRunnable()) 
	{
		CBPUSHCELL(PtrToPawnAddress(this))
		CBPUSHCELL(PtrToPawnAddress(botInMotion))

		cell_t callbackResult = result;
		if (pCallback->Execute(&callbackResult) == SP_ERROR_NONE) result = callbackResult;
	}

	return !!result;
}

// Queries

BEGINQUERYCALLBACK(ShouldPickUp, CBaseEntity *item ) 
	CBPUSHENTITY(item);
ENDQUERYCALLBACK()

BEGINQUERYCALLBACK(ShouldHurry)
ENDQUERYCALLBACK()

BEGINQUERYCALLBACK(ShouldRetreat)
ENDQUERYCALLBACK()

BEGINQUERYCALLBACK(ShouldAttack, const CKnownEntity *them)
	CBPUSHCELL(PtrToPawnAddress(them))
ENDQUERYCALLBACK()

BEGINQUERYCALLBACK(IsHindrance, CBaseEntity* blocker)
	CBPUSHENTITY(blocker == IS_ANY_HINDRANCE_POSSIBLE ? nullptr : blocker)
ENDQUERYCALLBACK()

Vector CBaseNPCPluginAction::SelectTargetPoint( const INextBot* me, const CBaseCombatCharacter* subject ) const
{
	Vector result = vec3_origin;

	IPluginFunction* pCallback = m_pFactory->GetQueryCallback( CBaseNPCPluginActionFactory::QueryCallbackType::SelectTargetPoint );
	if (pCallback && pCallback->IsRunnable()) 
	{ 
		cell_t buffer[3]; 
		buffer[0] = sp_ftoc(result[0]);
		buffer[1] = sp_ftoc(result[1]);
		buffer[2] = sp_ftoc(result[2]);

		CBPUSHCELL(PtrToPawnAddress(this))
		CBPUSHCELL(PtrToPawnAddress(me))
		CBPUSHENTITY((CBaseCombatCharacter*)subject)
		pCallback->PushArray(buffer, 3, SM_PARAM_COPYBACK);
		pCallback->Execute(nullptr);

		result[0] = sp_ctof(buffer[0]);
		result[1] = sp_ctof(buffer[1]);
		result[2] = sp_ctof(buffer[2]);
	}

	return result;
}

BEGINQUERYCALLBACK(IsPositionAllowed, const Vector &pos)
	CBPUSHVECTOR(pos)
ENDQUERYCALLBACK()

const CKnownEntity * CBaseNPCPluginAction::SelectMoreDangerousThreat( const INextBot *me, 
	const CBaseCombatCharacter *subject,
	const CKnownEntity *threat1, 
	const CKnownEntity *threat2 ) const
{
	cell_t result = 0;

	IPluginFunction* pCallback = m_pFactory->GetQueryCallback( CBaseNPCPluginActionFactory::QueryCallbackType::SelectMoreDangerousThreat );
	if (pCallback && pCallback->IsRunnable()) 
	{
		CBPUSHCELL(PtrToPawnAddress(this))
		CBPUSHCELL(PtrToPawnAddress(me))
		CBPUSHENTITY((CBaseCombatCharacter*)subject)
		CBPUSHCELL(PtrToPawnAddress(threat1))
		CBPUSHCELL(PtrToPawnAddress(threat2))
		pCallback->Execute(&result);
	}

	return (const CKnownEntity*)PawnAddressToPtr(result);
}

// Events

BEGINEVENTCALLBACK(OnLeaveGround, CBaseEntity* ground)
	EVENTPUSHENTITY(ground)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnLandOnGround, CBaseEntity* ground)
	EVENTPUSHENTITY(ground)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnContact, CBaseEntity* other, CGameTrace* traceResult)
	EVENTPUSHENTITY(other)
	EVENTPUSHCELL(PtrToPawnAddress(traceResult))
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnMoveToSuccess, const Path *path)
	EVENTPUSHCELL(PtrToPawnAddress(path))
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnMoveToFailure, const Path *path, MoveToFailureType reason)
	EVENTPUSHCELL(PtrToPawnAddress(path))
	EVENTPUSHCELL(reason)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnStuck)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnUnStuck)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnPostureChanged)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnAnimationActivityComplete, int activity)
	EVENTPUSHCELL(activity)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnAnimationActivityInterrupted, int activity)
	EVENTPUSHCELL(activity)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnAnimationEvent, animevent_t *event)
	EVENTPUSHCELL(event->event)
	EVENTPUSHSTRING(event->options)
	EVENTPUSHFLOAT(event->cycle)
	EVENTPUSHFLOAT(event->eventtime)
	EVENTPUSHCELL(event->type)
	EVENTPUSHENTITY((CBaseAnimating*)event->pSource)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnIgnite)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnInjured, const CTakeDamageInfo &info)
	EVENTPUSHENTITY(info.GetAttacker())
	EVENTPUSHENTITY(info.GetInflictor())
	EVENTPUSHFLOAT(info.GetDamage())
	EVENTPUSHCELL(info.GetDamageType())
	EVENTPUSHENTITY(info.GetWeapon())
	EVENTPUSHVECTOR(info.GetDamageForce())
	EVENTPUSHVECTOR(info.GetDamagePosition())
	EVENTPUSHCELL(info.GetDamageCustom())
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnKilled, const CTakeDamageInfo &info)
	EVENTPUSHENTITY(info.GetAttacker())
	EVENTPUSHENTITY(info.GetInflictor())
	EVENTPUSHFLOAT(info.GetDamage())
	EVENTPUSHCELL(info.GetDamageType())
	EVENTPUSHENTITY(info.GetWeapon())
	EVENTPUSHVECTOR(info.GetDamageForce())
	EVENTPUSHVECTOR(info.GetDamagePosition())
	EVENTPUSHCELL(info.GetDamageCustom())
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnOtherKilled, CBaseCombatCharacter *victim, const CTakeDamageInfo &info)
	EVENTPUSHENTITY(victim)
	EVENTPUSHENTITY(info.GetAttacker())
	EVENTPUSHENTITY(info.GetInflictor())
	EVENTPUSHFLOAT(info.GetDamage())
	EVENTPUSHCELL(info.GetDamageType())
	EVENTPUSHENTITY(info.GetWeapon())
	EVENTPUSHVECTOR(info.GetDamageForce())
	EVENTPUSHVECTOR(info.GetDamagePosition())
	EVENTPUSHCELL(info.GetDamageCustom())
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnSight, CBaseEntity *subject)
	EVENTPUSHENTITY(subject)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnLostSight, CBaseEntity *subject)
	EVENTPUSHENTITY(subject)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnSound, CBaseEntity *source, const Vector &pos, KeyValues *keys)
	EVENTPUSHENTITY(source)
	EVENTPUSHVECTOR(pos)

	KeyValueStack *pStk = new KeyValueStack;
	pStk->pBase = keys;
	pStk->pCurRoot.push(pStk->pBase);
	pStk->m_bDeleteOnDestroy = false;

	Handle_t hndl = handlesys->CreateHandle(g_KeyValueType, pStk, g_pCoreIdent, g_pCoreIdent, nullptr);
	EVENTPUSHCELL(hndl)

	pCallback->Execute(nullptr);

	HandleSecurity sec(g_pCoreIdent, g_pCoreIdent);

	// Deletes pStk
	handlesys->FreeHandle(hndl, &sec);

ENDEVENTCALLBACK_NOEXECUTE()

BEGINEVENTCALLBACK(OnSpokeConcept, CBaseCombatCharacter* who, AIConcept_t concept, AI_Response *response)
	EVENTPUSHENTITY(who)
	EVENTPUSHCELL(concept)
	EVENTPUSHCELL(PtrToPawnAddress(response))
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnWeaponFired, CBaseCombatCharacter* whoFired, CBaseEntity* weapon )
	EVENTPUSHENTITY(whoFired)
	EVENTPUSHENTITY(weapon)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnNavAreaChanged, CNavArea *newArea, CNavArea *oldArea)
	EVENTPUSHCELL(PtrToPawnAddress(newArea))
	EVENTPUSHCELL(PtrToPawnAddress(oldArea))
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnModelChanged)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnPickUp, CBaseEntity* item, CBaseCombatCharacter* giver)
	EVENTPUSHENTITY(item)
	EVENTPUSHENTITY(giver)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnDrop, CBaseEntity* item)
	EVENTPUSHENTITY(item)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnActorEmoted, CBaseCombatCharacter* emoter, int emote)
	EVENTPUSHENTITY(emoter)
	EVENTPUSHCELL(emote)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnCommandAttack, CBaseEntity *victim)
	EVENTPUSHENTITY(victim)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnCommandApproach, const Vector &pos, float range )
	EVENTPUSHVECTOR(pos)
	EVENTPUSHFLOAT(range)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACKEX(OnCommandApproach, OnCommandApproachEntity, CBaseEntity* goal)
	EVENTPUSHENTITY(goal)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnCommandRetreat, CBaseEntity *threat, float range)
	EVENTPUSHENTITY(threat)
	EVENTPUSHFLOAT(range)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnCommandPause, float duration)
	EVENTPUSHFLOAT(duration)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnCommandResume)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnCommandString, const char *command)
	EVENTPUSHSTRING(command)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnShoved, CBaseEntity *pusher)
	EVENTPUSHENTITY(pusher)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnBlinded, CBaseEntity *blinder)
	EVENTPUSHENTITY(blinder)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnTerritoryContested, int territoryID)
	EVENTPUSHCELL(territoryID)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnTerritoryCaptured, int territoryID)
	EVENTPUSHCELL(territoryID)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnTerritoryLost, int territoryID)
	EVENTPUSHCELL(territoryID)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnWin)
ENDEVENTCALLBACK()

BEGINEVENTCALLBACK(OnLose)
ENDEVENTCALLBACK()

CBaseNPCIntention::CBaseNPCIntention( INextBot * bot, CBaseNPCPluginActionFactory* initialActionFactory ) 
	: IIntention( bot ), m_pInitialActionFactory(initialActionFactory)
{
	m_pBehavior = nullptr;
	g_pBaseNPCPluginActionFactories->OnIntentionCreated(this);
	InitBehavior();
}

CBaseNPCIntention::~CBaseNPCIntention()
{
	g_pBaseNPCPluginActionFactories->OnIntentionDestroyed(this);
	DestroyBehavior();
}

void CBaseNPCIntention::Reset()
{ 
	if (m_bResetting) return;
	m_bResetting = true;
	struct Guard { bool& resetting; ~Guard() { resetting = false; } } guard{m_bResetting};
	DestroyBehavior();
	InitBehavior();
}

void CBaseNPCIntention::InitBehavior()
{
	if (m_pInitialActionFactory)
	{
		Action< INextBot > * pAction = m_pInitialActionFactory->Create();
		if (!pAction) return; // A factory being retired cannot create new actions.
		m_pInitialActionFactory->OnCreateInitialAction( pAction );
		m_pBehavior = new Behavior< INextBot >( pAction );
	}
	else 
	{
		m_pBehavior = nullptr;
	}
}

void CBaseNPCIntention::DestroyBehavior()
{
	if ( !m_pBehavior )
		return;

	auto behavior = m_pBehavior;
	m_pBehavior = nullptr;
	delete behavior;
}

bool CBaseNPCIntention::UsesActionFactory(const CBaseNPCPluginActionFactory* factory) const
{
	return m_pInitialActionFactory == factory || factory->IsUsedBy(m_pBehavior);
}

void CBaseNPCIntention::Update()
{
	if (m_pBehavior)
	{
		m_pBehavior->Update( GetBot(), GetUpdateInterval() );
	}
}

CBaseNPCPluginActionFactories::CBaseNPCPluginActionFactories()
{
}

bool CBaseNPCPluginActionFactories::Init( IGameConfig* config, char* error, size_t maxlength )
{
	m_FactoryType = g_BaseNPCPluginActionFactoryHandle = handlesys->CreateType( "BaseNPCPluginActionFactory", this, 0, nullptr, nullptr, myself->GetIdentity(), nullptr );
	if ( !m_FactoryType )
	{
		snprintf( error, maxlength, "Failed to register BaseNPCPluginActionFactory handle type" );
		return false;
	}

	return true;
}

void CBaseNPCPluginActionFactories::OnCoreMapEnd()
{
}

void CBaseNPCPluginActionFactories::SDK_OnUnload()
{
	handlesys->RemoveType( m_FactoryType, myself->GetIdentity() );
}

void CBaseNPCPluginActionFactories::OnHandleDestroy( HandleType_t type, void * object )
{
	CBaseNPCForgetNativeHandleObject(object);
	CBaseNPCPluginActionFactory* factory = (CBaseNPCPluginActionFactory*)object;
	delete factory;
}

CBaseNPCPluginActionFactory* CBaseNPCPluginActionFactories::GetFactoryFromHandle( Handle_t handle, HandleError *err )
{
	CBaseNPCPluginActionFactory* pFactory;
	HandleError _err;
	HandleSecurity security( nullptr, myself->GetIdentity() );
	if ( ( _err = handlesys->ReadHandle(handle, m_FactoryType, &security, (void **)&pFactory) ) != HandleError_None )
	{
		pFactory = nullptr;
	}

	if (err)
		*err = _err;

	return pFactory;
}

void CBaseNPCPluginActionFactories::OnFactoryCreated( CBaseNPCPluginActionFactory* pFactory )
{
	m_Factories.AddToTail( pFactory );
}

void CBaseNPCPluginActionFactories::OnFactoryDestroyed( CBaseNPCPluginActionFactory* pFactory )
{
	m_Factories.FindAndRemove( pFactory );
}

void CBaseNPCPluginActionFactories::OnIntentionCreated(CBaseNPCIntention* intention)
{
	m_Intentions.insert(intention);
}

void CBaseNPCPluginActionFactories::OnIntentionDestroyed(CBaseNPCIntention* intention)
{
	m_Intentions.erase(intention);
}

void CBaseNPCPluginActionFactories::DetachPendingAction(Action<INextBot>* action)
{
	for (int i = 0; i < m_Factories.Count(); ++i)
		for (int j = 0; j < m_Factories[i]->m_Actions.Count(); ++j)
			m_Factories[i]->m_Actions[j]->DetachPendingAction(action);
}

void CBaseNPCPluginActionFactories::ResetIntentionsUsingFactory(CBaseNPCPluginActionFactory* factory)
{
	std::vector<CBaseNPCIntention*> affected;
	for (auto intention : m_Intentions)
	{
		if (!intention->UsesActionFactory(factory)) continue;
		affected.push_back(intention);
		if (intention->m_pInitialActionFactory == factory)
			intention->m_pInitialActionFactory = nullptr;
	}
	for (auto intention : affected)
	{
		// Reset may destroy another entity/intention. Never dereference a stale
		// snapshot entry, and never downcast a game's stock IIntention object.
		if (m_Intentions.count(intention)) intention->Reset();
	}
}

CBaseNPCPluginActionFactory::CBaseNPCPluginActionFactory( IPlugin* plugin, const char* actionName, IdentityToken_t* owner ) : 
	IDataMapContainer(),
	m_bDestroying(false),
	m_iActionName(actionName)
{
	SetDefLessFunc(m_Callbacks);
	SetDefLessFunc(m_QueryCallbacks);
	SetDefLessFunc(m_EventCallbacks);

	m_Handle = handlesys->CreateHandle( g_pBaseNPCPluginActionFactories->GetFactoryType(), this, plugin ? plugin->GetIdentity() : owner, myself->GetIdentity(), nullptr );

	g_pBaseNPCPluginActionFactories->OnFactoryCreated( this );
}

CBaseNPCPluginActionFactory::~CBaseNPCPluginActionFactory()
{
	m_bDestroying = true;
	g_pPluginEntityFactories->DetachActionFactory(this);
	g_pBaseNPCPluginActionFactories->ResetIntentionsUsingFactory(this);
	// Behaviors own attached actions (even before OnStart); the resets above
	// destroy those first. Drain any standalone Create() results afterwards.
	// Action destruction can also delete children/pending actions and shrink
	// this vector, so do not iterate an index/snapshot across those deletions.
	while (m_Actions.Count() > 0) delete m_Actions.Tail();

	DestroyDataDesc();

	g_pBaseNPCPluginActionFactories->OnFactoryDestroyed( this );
}

bool CBaseNPCPluginActionFactory::IsUsedBy(const Behavior<INextBot>* behavior) const
{
	if (!behavior) return false;
	for (int i = 0; i < m_Actions.Count(); ++i)
		if (behavior->ContainsAction(m_Actions[i])) return true;
	return false;
}

IPluginFunction* CBaseNPCPluginActionFactory::GetCallback(CallbackType cbType)
{
	unsigned short index = m_Callbacks.Find(cbType);
	if (!m_Callbacks.IsValidIndex(index))
		return nullptr;
	
	return m_Callbacks[index];
}

void CBaseNPCPluginActionFactory::SetCallback(CallbackType cbType, IPluginFunction* pCallback)
{
	m_Callbacks.Insert(cbType, pCallback);
}

IPluginFunction* CBaseNPCPluginActionFactory::GetQueryCallback(QueryCallbackType cbType)
{
	unsigned short index = m_QueryCallbacks.Find(cbType);
	if (!m_QueryCallbacks.IsValidIndex(index))
		return nullptr;
	
	return m_QueryCallbacks[index];
}

void CBaseNPCPluginActionFactory::SetQueryCallback(QueryCallbackType cbType, IPluginFunction* pCallback)
{
	m_QueryCallbacks.Insert(cbType, pCallback);
}

IPluginFunction* CBaseNPCPluginActionFactory::GetEventCallback(EventResponderCallbackType eventType)
{
	unsigned short index = m_EventCallbacks.Find(eventType);
	if (!m_EventCallbacks.IsValidIndex(index))
		return nullptr;
	
	return m_EventCallbacks[index];
}

void CBaseNPCPluginActionFactory::SetEventCallback(EventResponderCallbackType eventType, IPluginFunction* pCallback)
{
	m_EventCallbacks.Insert(eventType, pCallback);
}

Action <INextBot>* CBaseNPCPluginActionFactory::Create()
{
	if (m_bDestroying)
		return nullptr;

	if (HasDataDesc() && !GetDataDescMap())
		CreateDataDescMap(nullptr);

	CBaseNPCPluginAction * action = new CBaseNPCPluginAction( this );
	return action;
}

void CBaseNPCPluginActionFactory::OnActionCreated(Action <INextBot>* pAction)
{
	m_Actions.AddToTail(pAction);
}

void CBaseNPCPluginActionFactory::OnActionRemoved(Action <INextBot>* pAction)
{
	m_Actions.FindAndRemove(pAction);
	if (m_bDestroying) g_pBaseNPCPluginActionFactories->DetachPendingAction(pAction);
}

void CBaseNPCPluginActionFactory::OnCreateInitialAction(Action <INextBot>* pAction)
{
	IPluginFunction * pCallback = GetCallback( CreateInitialAction );
	if (pCallback && pCallback->IsRunnable())
	{
		pCallback->PushCell(PtrToPawnAddress(pAction));
		pCallback->Execute(nullptr);
	}
}
