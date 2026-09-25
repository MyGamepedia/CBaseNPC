#if defined _CBASENPC_ENTITYFACTORY_INC_
 #endinput
#endif
#define _CBASENPC_ENTITYFACTORY_INC_

#define CBASENPC_SENDPROP_HIGH_DEFAULT -121121.121121
#define CBASENPC_SENDPROP_MAX_STRING_LENGTH 512

// BMS dt_common.h user-facing flags. Internal table/array flags are not exposed.
enum
{
	CBaseNPCSendProp_None = 0,
	CBaseNPCSendProp_Unsigned = (1 << 0),
	CBaseNPCSendProp_Coord = (1 << 1),
	CBaseNPCSendProp_NoScale = (1 << 2),
	CBaseNPCSendProp_RoundDown = (1 << 3),
	CBaseNPCSendProp_RoundUp = (1 << 4),
	CBaseNPCSendProp_Normal = (1 << 5),
	CBaseNPCSendProp_VarInt = (1 << 5),
	CBaseNPCSendProp_XYZE = (1 << 7),
	CBaseNPCSendProp_ChangesOften = (1 << 10),
	CBaseNPCSendProp_CoordMP = (1 << 13),
	CBaseNPCSendProp_CoordMPLowPrecision = (1 << 14),
	CBaseNPCSendProp_CoordMPIntegral = (1 << 15)
};

typeset InputFuncCallback
{
	// Void
	function void (int entity, int activator, int caller);
	function void (CBaseEntity entity, CBaseEntity activator, CBaseEntity caller);

	// Integer
	function void (int entity, int activator, int caller, int value);
	function void (CBaseEntity entity, CBaseEntity activator, CBaseEntity caller, int value);

	// Float
	function void (int entity, int activator, int caller, float value);
	function void (CBaseEntity entity, CBaseEntity activator, CBaseEntity caller, float value);

	// Color32
	function void (int entity, int activator, int caller, const int value[4]);
	function void (CBaseEntity entity, CBaseEntity activator, CBaseEntity caller, const int value[4]);

	// Vector
	function void (int entity, int activator, int caller, const float value[3]);
	function void (CBaseEntity entity, CBaseEntity activator, CBaseEntity caller, const float value[3]);

	// String
	function void (int entity, int activator, int caller, const char[] value);
	function void (CBaseEntity entity, CBaseEntity activator, CBaseEntity caller, const char[] value);

	// Integer/Float/Unknown
	function void (int entity, int activator, int caller, any value);
	function void (CBaseEntity entity, CBaseEntity activator, CBaseEntity caller, any value);
}

enum InputFuncValueType
{
	InputFuncValueType_Void = 0,
	InputFuncValueType_String,
	InputFuncValueType_Boolean,
	InputFuncValueType_Color32,
	InputFuncValueType_Float,
	InputFuncValueType_Integer,
	InputFuncValueType_Vector,
	InputFuncValueType_Max
}

typeset CEntityFactoryPostConstructor
{
	function void (int entity);
	function void (CBaseEntity entity);
}

typeset CEntityFactoryOnRemoveCallback
{
	function void (int entity);
	function void (CBaseEntity entity);
}

typeset CEntityFactoryNextBot
{
	function INextBot (Address link);
};

methodmap CEntityFactory < Handle
{
	/**
	 * A class that facilites the creation of entities of a specified class.
	 * Factories created this way must derive from an existing installed factory
	 * via the `DeriveFrom*()` functions.
	 *
	 * Once a CEntityFactory is installed via Install(), the game will utilize
	 * the factory to create entities under its classname, and can also be
	 * utilized by maps.
	 *
	 * Note: Be cautious when using KillTimer() in the `onRemove` callback on timers flagged
	 * with TIMER_FLAG_NO_MAPCHANGE. When a map changes, timers with this flag are removed 
	 * first before this is called, so consider letting the timers end on their own instead 
	 * (i.e. returning Plugin_Stop).
	 *
	 * @param classname         The entity's classname. This cannot match the
	 *                          classname of an already installed factory.
	 * @param postConstructor   Called when the factory finishes creating the entity.
	 *                          This is the best place to initialize your entity.
	 * @param onRemove          Called when an entity created by this factory is removed.
	 *                          Perform any needed cleanup for your entity here.
	 * @param error             Invalid handle, classname is NULL or empty, or out of
	 *                          memory.
	 * @param plugin            Optional Handle to another plugin to mark as the owner.
	 *                          If no owner is passed, the owner is the calling plugin.
	 */
	public native CEntityFactory(const char[] classname, CEntityFactoryPostConstructor postConstructor=INVALID_FUNCTION, CEntityFactoryOnRemoveCallback onRemove=INVALID_FUNCTION, Handle plugin = INVALID_HANDLE);
	
	/**
	 * Instructs the factory to use CBaseNPC (NextBot) as the base class.
	 */
	public native void DeriveFromNPC();

	/**
	 * If this is a CBaseNPC entity factory, then this will set the action
	 * factory to use to instantiate the NPC's main action.
	 *
	 * This will override the action factory set by any base factories.
	 *
	 * @param factory      The action factory used to initialize the main action
	 * @error              Invalid handle.
	 */
	public native void SetInitialActionFactory(NextBotActionFactory factory);

	/**
	 * Instructs the factory to use CBaseEntity as the base class.
	 *
	 * @param bServerOnly       If true, entity will be server side only and not
	 *                          networked to clients.
	 * @error                   Invalid handle.
	 */
	public native void DeriveFromBaseEntity(bool bServerOnly=false);

	/**
	 * Instructs the factory to use an existing installed factory as the base factory.
	 *
	 * You can specify a base factory that was created via CEntityFactory(), even
	 * if the base factory was created by another plugin. However, if the base 
	 * factory is uninstalled, this factory will be uninstalled first.
	 *
	 * @param classname     The classname of the installed factory.
	 * @error               Invalid handle.
	 */
	public native void DeriveFromClass(const char[] classname);

	/**
	 * Instructs the factory to use the specified factory as a base.
	 *
	 * @param baseFactory     The base factory
	 * @error                 Invalid handle.
	 */
	public native void DeriveFromFactory(CEntityFactory baseFactory);

	/**
	 * Instructs the factory to initialize the entity using the given
	 * entity size and constructor function signature/address. Only use this
	 * if you are absolutely sure of what you're doing! Most will not!
	 * 
	 * The constructor function must be parameterless.
	 * 
	 * @param entitySize      The size of the entity, in bytes
	 * @param config          Gamedata to read signature from
	 * @param source          Offsets, Signatures, or Addresses
	 * @param name            Name of entry in gamedata.
	 * @error                 Invalid handle or function not found.
	 */
	public native void DeriveFromConf(int entitySize, GameData config, SDKFuncConfSource source, const char[] name);

	/**
	 * Installs the factory. This marks the factory available for use.
	 * For non-abstract factories, the factory will be added to the global
	 * entity factory dictionary.
	 *
	 * @error   Invalid handle, An installed factory already has classname, or
	 *          base factory/class doesn't exist.
	 */
	public native void Install();

	/**
	 * Uninstalls the factory. If any CEntityFactory's are using this factory
	 * as a base, then those factories will be uninstalled first.
	 *
	 * Upon uninstalling a factory, all entities that were directly created
	 * by this factory (not including derived factories) will be removed.
	 *
	 * @error   Invalid handle.
	 */
	public native void Uninstall();

	/**
	 * Tells if the factory is installed or not.
	 *
	 * @return     Installed
	 * @error      Invalid handle.
	 */
	property bool IsInstalled
	{
		public native get();
	}

	/**
	 * Tells if the factory is abstract or not.
	 * 
	 * An abstract factory is never added to the entity factory dictionary,
	 * and is used only as a base for other factories. This is synonymous with
	 * abstract classes in C++.
	 *
	 * You are still required to install abstract factories in order for them
	 * to be used by other factories.
	 *
	 * To use an abstract factory as a base, you need to use DeriveFromFactory().
	 */
	property bool IsAbstract
	{
		public native get();
		public native set(bool bIsAbstract);
	}

	/**
	 * The entity classname the factory was created with.
	 *
	 * @param sBuffer        Buffer
	 * @param iBufferLen     Size of buffer
	 * @error                Invalid handle.
	 */
	public native void GetClassname(char[] sBuffer, int iBufferLen);

	/**
	 * The CEntityFactory that created the entity.
	 *
	 * @param entity     Entity index/reference
	 * @return           CEntityFactory, or null if not created by a CEntityFactory.
	 * @error            Invalid entity.
	 */
	public static native CEntityFactory GetFactoryOfEntity(int entity);

	/**
	 * Number of installed CEntityFactory's.
	 *
	 * @return     Number of installed CEntityFactory's
	 */
	public static native int GetNumInstalledFactories();

	/**
	 * Gets list of installed CEntityFactory's.
	 *
	 * @param array         Buffer
	 * @param arraySize     Buffer size
	 * @return              Number of installed CEntityFactory's
	 */
	public static native int GetInstalledFactories(CEntityFactory[] array, int arraySize);

	/**
	 * Attaches a NextBot factory to the entity factory.
	 * Note: No NextBot interface will be created, if the entity already has a NextBot interface
	 * or doesn't derive from the class CBaseCombatCharacter.
	 * 
	 * @error                Invalid handle.
	 */
	public native void AttachNextBot(CEntityFactoryNextBot factory = INVALID_FUNCTION);

	// Datamap

	/**
	 * Declares a server-side network identity, independent of the entity classname.
	 * The base is a stock OR custom ServerClass network name, not an entity classname
	 * or DT_ name. No temporary entity is constructed to infer the base.
	 * May be called before/after fields, in initial OnPluginStart/OnAllPluginsLoaded.
	 * Zero custom SendProps is valid. All send=true fields require a declaration.
	 * A custom base containing fields must also be an allocation ancestor (via
	 * DeriveFromFactory/DeriveFromClass). Choose a stock base compatible with the
	 * actual entity's C++ memory layout; network inheritance cannot add C++ fields.
	 * Tables are finalized once AFTER every plugin's OnAllPluginsLoaded returns.
	 * Creating network entities before that boundary fails. Late registration and
	 * changing a finalized layout require a process restart. Plugin removal leaves
	 * the schema alive; runtime extension unload/reload is unsupported.
	 * This is BMS server-side only: a matching client ClientClass/RecvTable is still
	 * required for clients to use these entities.
	 * @return This factory, for chaining.
	 */
	public native CEntityFactory DefineServerClass(const char[] sNetworkName, const char[] sSendTableName, const char[] sBaseNetworkName);

	// SendProp options shared by Define*Field below:
	// bIsSendProp=false preserves the original datamap-only behavior.
	// true uses the SAME memory/offset as the datamap, without extra storage.
	// iSendBits=-1/0 selects full storage width for integers, full resolution for
	// floats; positive bits are 1..32, per component for vectors/angles.
	// iSendFlags is a combination of CBaseNPCSendProp_* values valid for the type.
	// Coord/CoordMP*/NoScale/Normal/XYZE are mutually exclusive modes. Normal and
	// XYZE apply to full Vector only; rounding applies to quantized Float/Vector.
	// Bool, Color, EHandle, Time and ModelIndex accept only ChangesOften.
	// Float/Vector ranges use flSendLow/flSendHigh; HIGH_DEFAULT selects 2^bits.
	// Counts > 1 produce fixed Array3-style nested datatables (up to 2048 elements).
	// Character arrays are integer arrays, NOT strings. String fields are string_t.
	// iSendMaxLength validates SDK SendPropString's bufferLen (1..512); it is not
	// a per-property runtime truncation limit in Source's SendProp layout.
	// Direct datamap writes need the existing CBaseEntity.NetworkStateChanged()
	// mechanism after initialization. For string_t, prefer Prop_Data setters plus
	// notification: SourceMod's Prop_Send setter guesses storage from proxy output,
	// which is not reliable for initially NULL_STRING fields across DLL boundaries.

	/**
	 * Begin defining the datamap structure. The factory uses this datamap to
	 * create and map out custom, user-defined properties on the entity. These 
	 * properties can be accessed using Get/SetEntProp. Use EndDataMapDesc()
	 * to finish defining the datamap.
	 *
	 * If deriving from another CEntityFactory (whether by factory or classname), 
	 * you must specify and install the base factory before calling 
	 * BeginDataMapDesc() on this factory, as offsets are calculated based on 
	 * the entity size of the base factory. You also must not change the base
	 * factory's datamap during/after defining this factory's datamap, as this
	 * will lead to undefined behavior.
	 *
	 * About key fields: If sKeyName is given, this will declare the property
	 * as a key field, which can be accessed by the map and the DispatchKeyValue
	 * function using sKeyName. Key fields cannot be declared as an array,
	 * meaning iNumElements must be set to 1.
	 *
	 * @param sDataClass      Name of the datamap. If null or empty, will use
	 *                        the entity's classname instead.
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle, or beginning datamap definition
	 *                        before base factory is installed
	 */
	public native CEntityFactory BeginDataMapDesc(const char[] sDataClass=NULL_STRING);

	/**
	 * Defines an integer field in the datamap.
	 *
	 * @param sFieldName      Name of the field
	 * @param iNumElements    Number of elements. If > 1, will declare the field
	 *                        as an array.
	 * @param sKeyName        Key field name. See BeginDataMapDesc's documentation
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle, or tried to declare key field as
	 *                        array.
	 */
	public native CEntityFactory DefineIntField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendBits=-1, int iSendFlags=0);

	/**
	 * Defines a float field in the datamap.
	 *
	 * @param sFieldName      Name of the field
	 * @param iNumElements    Number of elements. If > 1, will declare the field
	 *                        as an array.
	 * @param sKeyName        Key field name. See BeginDataMapDesc's documentation
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle, or tried to declare key field as
	 *                        array.
	 */
	public native CEntityFactory DefineFloatField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendBits=32, int iSendFlags=0, float flSendLow=0.0, float flSendHigh=CBASENPC_SENDPROP_HIGH_DEFAULT);

	/**
	 * Defines a character field in the datamap.
	 *
	 * @param sFieldName      Name of the field
	 * @param iNumElements    Number of elements. If > 1, will declare the field
	 *                        as an array.
	 * @param sKeyName        Key field name. See BeginDataMapDesc's documentation
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle, or tried to declare key field as
	 *                        array.
	 */
	public native CEntityFactory DefineCharField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendBits=-1, int iSendFlags=0);

	/**
	 * Defines a boolean field in the datamap.
	 *
	 * @param sFieldName      Name of the field
	 * @param iNumElements    Number of elements. If > 1, will declare the field
	 *                        as an array.
	 * @param sKeyName        Key field name. See BeginDataMapDesc's documentation
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle, or tried to declare key field as
	 *                        array.
	 */
	public native CEntityFactory DefineBoolField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendFlags=0);

	/**
	 * Defines a Vector field in the datamap.
	 *
	 * @param sFieldName      Name of the field
	 * @param iNumElements    Number of elements. If > 1, will declare the field
	 *                        as an array.
	 * @param sKeyName        Key field name. See BeginDataMapDesc's documentation
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle, or tried to declare key field as
	 *                        array.
	 */
	public native CEntityFactory DefineVectorField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendBits=32, int iSendFlags=CBaseNPCSendProp_NoScale, float flSendLow=0.0, float flSendHigh=CBASENPC_SENDPROP_HIGH_DEFAULT);

	/**
	 * Defines a string field in the datamap.
	 *
	 * @param sFieldName      Name of the field
	 * @param iNumElements    Number of elements. If > 1, will declare the field
	 *                        as an array.
	 * @param sKeyName        Key field name. See BeginDataMapDesc's documentation
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle, or tried to declare key field as
	 *                        array.
	 */
	public native CEntityFactory DefineStringField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendMaxLength=CBASENPC_SENDPROP_MAX_STRING_LENGTH, int iSendFlags=0);

	/**
	 * Defines a color field in the datamap.
	 *
	 * @param sFieldName      Name of the field
	 * @param iNumElements    Number of elements. If > 1, will declare the field
	 *                        as an array.
	 * @param sKeyName        Key field name. See BeginDataMapDesc's documentation
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle, or tried to declare key field as
	 *                        array.
	 */
	public native CEntityFactory DefineColorField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendFlags=0);

	/**
	 * Defines an entity field in the datamap.
	 *
	 * @param sFieldName      Name of the field
	 * @param iNumElements    Number of elements. If > 1, will declare the field
	 *                        as an array.
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle.
	 */
	public native CEntityFactory DefineEntityField(const char[] sFieldName, int iNumElements=1, bool bIsSendProp=false, int iSendFlags=0);

	/**
	 * Defines an input function. Inputs can be invoked using AcceptEntityInput,
	 * ent_fire, or from outputs.
	 *
	 * @param sInputName      Name of the input
	 * @param valueType       Type of value that the input function callback
	 *                        should handle. The engine will attempt to convert
	 *                        the given value to this type and will pass it to
	 *                        the callback.
	 * @param handler         Callback to handle the input.
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle.
	 */
	public native CEntityFactory DefineInputFunc(const char[] sInputName, InputFuncValueType valueType, InputFuncCallback handler);

	/** Signed/unsigned 16-bit storage; transmitted as an integer. */
	public native CEntityFactory DefineShortField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendBits=-1, int iSendFlags=0);
	/** Three-float storage, only X/Y transmitted. */
	public native CEntityFactory DefineVectorXYField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendBits=32, int iSendFlags=CBaseNPCSendProp_NoScale, float flSendLow=0.0, float flSendHigh=CBASENPC_SENDPROP_HIGH_DEFAULT);
	/** FIELD_TIME, transmitted as a generic unscaled float, not simulation time. */
	public native CEntityFactory DefineTimeField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendFlags=0);
	/** Float angle, normalized to 0..360 for transmission. */
	public native CEntityFactory DefineAngleField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendBits=32, int iSendFlags=0);
	/** Three normalized angles in FIELD_VECTOR storage. */
	public native CEntityFactory DefineQAngleField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendBits=32, int iSendFlags=0);
	/** FIELD_MODELINDEX, engine-defined network bit width. */
	public native CEntityFactory DefineModelIndexField(const char[] sFieldName, int iNumElements=1, const char[] sKeyName=NULL_STRING, bool bIsSendProp=false, int iSendFlags=0);

	/**
	 * Defines an output. Outputs can be fired using FireEntityOutput.
	 *
	 * @param sOutputName     Name of the output
	 * @return                The CEntityFactory to chain calls
	 * @error                 Invalid handle.
	 */
	public native CEntityFactory DefineOutput(const char[] sOutputName);

	/**
	 * Finishes the datamap definition.
	 *
	 * @error                 Invalid handle.
	 */
	public native void EndDataMapDesc();
}

/**
 * Called after CEntityFactory was installed.
 *
 * @param classname     The classname of the factory
 * @param factory       Factory that was installed
 */
forward void CEntityFactory_OnInstalled(const char[] classname, CEntityFactory factory);

/**
 * Called after CEntityFactory was uninstalled.
 *
 * @param classname     The classname of the factory
 * @param factory       Factory that was uninstalled
 */
forward void CEntityFactory_OnUninstalled(const char[] classname, CEntityFactory factory);
