#include "entityfactory.hpp"
#include "pluginentityfactory.h"
#include "sourcesdk/cbasenpcserverclass.h"

namespace natives::entityfactory {

inline CPluginEntityFactory* Get(IPluginContext* context, const cell_t param) {
	HandleSecurity security;
	security.pOwner = nullptr;
	security.pIdentity = myself->GetIdentity();
	Handle_t hndlObject = static_cast<Handle_t>(param);
	CPluginEntityFactory *factory = nullptr;
	READHANDLE(hndlObject, PluginEntityFactory, factory);
	return factory;
}

inline CPluginEntityFactory* Get(IPluginContext* context, const cell_t param, bool shouldBeInstalled) {
	auto factory = Get(context, param);
	if (!factory) {
		return nullptr;
	}

	if (factory->m_bInstalled != shouldBeInstalled) {
		if (shouldBeInstalled) {
			context->ThrowNativeError("Factory must be installed!");
		} else {
			context->ThrowNativeError("Factory must be uninstalled!");
		}
		return nullptr;
	}
	return factory;
}

inline CPluginEntityFactory* GetEditable(IPluginContext* context, cell_t handle)
{
	auto factory = Get(context, handle, false);
	if (factory && factory->IsNetworkLayoutFrozen())
	{
		context->ThrowNativeError("Factory layout is frozen by the network schema. Restart required to change it.");
		return nullptr;
	}
	return factory;
}

cell_t CPluginEntityFactory_Ctor(IPluginContext * context, const cell_t * params) {
	char* classname;
	context->LocalToString(params[1], &classname);

	if (!classname || !strlen(classname)) {
		return context->ThrowNativeError("Entity factory must have a classname");
	}

	IPlugin* plugin = plsys->FindPluginByContext(context);
	IPluginFunction *postConstructor = context->GetFunctionById(params[2]);
	IPluginFunction *onRemove = context->GetFunctionById(params[3]);

	if (params[0] >= 4 && params[4] != 0) {
		// This factory needs to be created for another plugin
		HandleError error = HandleError_None;
		plugin = plsys->PluginFromHandle(static_cast<Handle_t>(params[4]), &error);
		if (error != HandleError_None || plugin == nullptr) {
			return context->ThrowNativeError("Could not create entity factory with the given plugin handle %d (error %d)", params[4], error);
		}
	}

	CPluginEntityFactory* factory = new CPluginEntityFactory(plugin, classname, postConstructor, onRemove);
	return factory->m_Handle;
}

cell_t GetFactoryOfEntity(IPluginContext* context, const cell_t * params) {
	CBaseEntity* entity = gamehelpers->ReferenceToEntity(params[1]);
	if (!entity && params[1] != -1) {
		return context->ThrowNativeError("Entity %d (%d) is invalid", entity, params[1]);
	}
	
	CPluginEntityFactory* factory = g_pPluginEntityFactories->GetFactory(entity);
	if (!factory) {
		return BAD_HANDLE;
	}
	
	return factory->m_Handle;
}

cell_t GetNumInstalledFactories(IPluginContext* context, const cell_t* params) {
	return g_pPluginEntityFactories->GetInstalledFactoryHandles(nullptr, 0);
}

cell_t GetInstalledFactories(IPluginContext* context, const cell_t* params) {
	cell_t * addr;
	context->LocalToPhysAddr(params[1], &addr);
	Handle_t * pArray = reinterpret_cast<Handle_t *>(addr);

	int arraySize = params[2];

	return g_pPluginEntityFactories->GetInstalledFactoryHandles(pArray, arraySize);
}

cell_t DeriveFromBaseEntity(IPluginContext* context, const cell_t* params) {
	auto factory = GetEditable(context, params[1]);
	if (!factory) {
		return 0;
	}

	factory->DeriveFromBaseEntity(params[2] == 1);
	return 0;
}

cell_t DeriveFromNPC(IPluginContext* context, const cell_t* params) {
	auto factory = GetEditable(context, params[1]);
	if (!factory) {
		return 0;
	}

	factory->DeriveFromNPC();
	return 0;
}

cell_t SetInitialActionFactory(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1], false);
	if (!factory) {
		return 0;
	}

	auto hndlObject = static_cast<Handle_t>(params[2]);
	CBaseNPCPluginActionFactory* action = nullptr;
	if (hndlObject != BAD_HANDLE) {
		action = g_pBaseNPCPluginActionFactories->GetFactoryFromHandle(hndlObject);
		if (!action) {
			return context->ThrowNativeError("Invalid action factory");
		}
	}

	factory->SetBaseNPCInitialActionFactory(action);
	return 0;
}

cell_t DeriveFromClass(IPluginContext* context, const cell_t* params) {
	auto factory = GetEditable(context, params[1]);
	if (!factory) {
		return 0;
	}

	char* classname;
	context->LocalToString(params[2], &classname);

	IEntityFactory* derivedFactory = g_pPluginEntityFactories->FindFactory(classname);
	if (!derivedFactory) {
		return context->ThrowNativeError("Cannot derive from uninstalled entity factory %s", classname);
	}
	
	factory->DeriveFromClass(classname);

	return 0;
}

cell_t DeriveFromFactory(IPluginContext* context, const cell_t* params) {
	auto factory = GetEditable(context, params[1]);
	if (!factory) {
		return 0;
	}

	CPluginEntityFactory* otherFactory = Get(context, params[2]);
	if (!otherFactory) {
		return 0;
	}

	if (otherFactory == factory) {
		return context->ThrowNativeError("Cannot derive from self");
	}

	factory->DeriveFromHandle( params[2] );

	return 0;
}

cell_t DeriveFromConf(IPluginContext* context, const cell_t* params) {
	auto factory = GetEditable(context, params[1]);
	if (!factory) {
		return 0;
	}
	
	size_t entitySize = params[2];
	IGameConfig* config = gameconfs->ReadHandle(params[3], context->GetIdentity(), nullptr);
	if (!config) {
		return context->ThrowNativeError("Invalid gameconfig handle");
	}
	
	int type = params[4];
	char* entry;
	context->LocalToString(params[5], &entry);

	if (!factory->DeriveFromConf(entitySize, config, type, entry))
	{
		context->ThrowNativeError("Failed to obtain constructor function from gamedata entry %s", entry);
	}
	return 0;
}

cell_t Install(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1], false);
	if (!factory) {
		return 0;
	}
	
	if (!factory->IsAbstract()) {
		const char* classname = factory->m_iClassname.c_str();
		if (g_pPluginEntityFactories->FindPluginFactory(classname) != nullptr) {
			return context->ThrowNativeError("Entity factory already exists with the same classname");
		}
	}

	if (factory->DoesNotDerive()) {
		return context->ThrowNativeError("Entity factory must derive from an existing class or classname");
	}

	if (!factory->Install()) {
		return context->ThrowNativeError("Failed to install; make sure base factory is installed first");
	}
	
	return 0;
}

cell_t Uninstall(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1], true);
	if (!factory) {
		return 0;
	}

	factory->Uninstall();
	return 0;
}

cell_t GetIsInstalled(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1]);
	if (!factory) {
		return 0;
	}

	return factory->m_bInstalled;
}

cell_t GetIsAbstract(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1]);
	if (!factory) {
		return 0;
	}

	return factory->IsAbstract();
}

cell_t SetIsAbstract(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1], false);
	if (!factory) {
		return 0;
	}

	factory->SetAbstract(params[2] == 1);
	return 0;
}

cell_t GetClassname(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1]);
	if (!factory) {
		return 0;
	}

	size_t bufferSize = params[3];
	context->StringToLocal(params[2], bufferSize, factory->m_iClassname.c_str());
	return 0;
}

cell_t AttachNextBot(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1], false);
	if (!factory) {
		return 0;
	}

	// TO-DO: 2.0.0
	bool old = ((params[0] < 2) || context->GetFunctionById(params[2]) == nullptr);
	factory->AttachNextBot((old) ? (IPluginFunction*)0x1 : context->GetFunctionById(params[2]));
	return 0;
}

cell_t BeginDataMapDesc(IPluginContext* context, const cell_t* params) {
	auto factory = GetEditable(context, params[1]);
	if (!factory) {
		return 0;
	}

	char* dataClassName;
	context->LocalToStringNULL(params[2], &dataClassName);
	bool startedDesc;
	if (!dataClassName) {
		startedDesc = factory->BeginDataDesc(factory->m_iClassname.c_str());
	} else {
		startedDesc = factory->BeginDataDesc(dataClassName);
	}

	if (!startedDesc) {
		return context->ThrowNativeError("Base factory was not installed before datamap definition");
	}

	return params[1];
}

cell_t EndDataMapDesc(IPluginContext* context, const cell_t* params) {
	auto factory = GetEditable(context, params[1]);
	if (!factory) {
		return 0;
	}

	factory->EndDataDesc();
	return 0;
}

// Optional arguments are read only when present, preserving already compiled SMX files.
inline cell_t Optional(const cell_t* params, int index, cell_t value = 0)
{
	return params[0] >= index ? params[index] : value;
}

cell_t DefineServerClass(IPluginContext* context, const cell_t* params)
{
	auto factory = Get(context, params[1]);
	if (!factory) return 0;
	char *name, *table, *base;
	context->LocalToString(params[2], &name);
	context->LocalToString(params[3], &table);
	context->LocalToString(params[4], &base);
	std::string error;
	if (!factory->DefineServerClass(name, table, base, error))
		return context->ThrowNativeError("%s: %s", factory->m_iClassname.c_str(), error.c_str());
	return params[1];
}

cell_t DefineField(IPluginContext* context, const cell_t* params, fieldtype_t fieldType, CBaseNPCSendFieldKind kind)
{
	auto factory = GetEditable(context, params[1]);
	if (!factory) return 0;
	char* name;
	context->LocalToString(params[2], &name);
	if (!name || !*name) return context->ThrowNativeError("Field name cannot be empty");
	int count = params[3];
	if (count < 1 || count > 65535) return context->ThrowNativeError("%s: element count must be in 1..65535", name);
	bool entity = kind == CBaseNPCSendFieldKind::EHandle;
	char* key = nullptr;
	if (!entity) context->LocalToStringNULL(params[4], &key);
	if (key && (!*key || count > 1)) return context->ThrowNativeError("%s: key name must be nonempty and key fields cannot be arrays", name);
	bool send = Optional(params, entity ? 4 : 5) != 0;
	CBaseNPCSendFieldDesc desc;
	desc.kind = kind;
	if (send)
	{
		if (!g_CBaseNPCServerClassManager.IsRegistrationOpen())
			return context->ThrowNativeError("%s", g_CBaseNPCServerClassManager.RegistrationError());
		if (!factory->IsDefiningDataDesc())
			return context->ThrowNativeError("%s: network fields require BeginDataMapDesc/EndDataMapDesc", name);
		if (count > MAX_ARRAY_ELEMENTS)
			return context->ThrowNativeError("%s: array exceeds MAX_ARRAY_ELEMENTS (%d)", name, MAX_ARRAY_ELEMENTS);
		using Kind = CBaseNPCSendFieldKind;
		switch (kind)
		{
		case Kind::Int: case Kind::Short: case Kind::Char:
			desc.bits = Optional(params, 6, -1); desc.flags = Optional(params, 7); break;
		case Kind::Float: case Kind::Vector: case Kind::VectorXY:
			desc.bits = Optional(params, 6, 32);
			desc.flags = Optional(params, 7, kind == Kind::Float ? 0 : SPROP_NOSCALE);
			if (params[0] >= 8) desc.lowValue = sp_ctof(params[8]);
			if (params[0] >= 9) desc.highValue = sp_ctof(params[9]);
			break;
		case Kind::Angle: case Kind::QAngle:
			desc.bits = Optional(params, 6, 32); desc.flags = Optional(params, 7); break;
		case Kind::StringT:
			desc.stringMaxLength = Optional(params, 6, DT_MAX_STRING_BUFFERSIZE);
			desc.flags = Optional(params, 7); break;
		case Kind::EHandle: desc.flags = Optional(params, 5); break;
		default: desc.flags = Optional(params, 6); break;
		}
		std::string error;
		if (!CBaseNPC_ValidateSendFieldOptions(desc, error))
			return context->ThrowNativeError("%s: %s", name, error.c_str());
	}
	desc.dataDescIndex = factory->DefineFieldAndGetIndex(name, fieldType, static_cast<unsigned short>(count),
		FTYPEDESC_SAVE | (key ? FTYPEDESC_KEY : 0), key, 0.0f);
	if (send) factory->AddSendField(desc);
	return params[1];
}

#define DEFINE_FIELD_NATIVE(name, type, kind) \
cell_t name(IPluginContext* context, const cell_t* params) \
{ return DefineField(context, params, type, CBaseNPCSendFieldKind::kind); }
DEFINE_FIELD_NATIVE(DefineIntField, FIELD_INTEGER, Int)
DEFINE_FIELD_NATIVE(DefineShortField, FIELD_SHORT, Short)
DEFINE_FIELD_NATIVE(DefineCharField, FIELD_CHARACTER, Char)
DEFINE_FIELD_NATIVE(DefineBoolField, FIELD_BOOLEAN, Bool)
DEFINE_FIELD_NATIVE(DefineFloatField, FIELD_FLOAT, Float)
DEFINE_FIELD_NATIVE(DefineVectorField, FIELD_VECTOR, Vector)
DEFINE_FIELD_NATIVE(DefineVectorXYField, FIELD_VECTOR, VectorXY)
DEFINE_FIELD_NATIVE(DefineStringField, FIELD_STRING, StringT)
DEFINE_FIELD_NATIVE(DefineColorField, FIELD_COLOR32, Color32)
DEFINE_FIELD_NATIVE(DefineEntityField, FIELD_EHANDLE, EHandle)
DEFINE_FIELD_NATIVE(DefineTimeField, FIELD_TIME, Time)
DEFINE_FIELD_NATIVE(DefineAngleField, FIELD_FLOAT, Angle)
DEFINE_FIELD_NATIVE(DefineQAngleField, FIELD_VECTOR, QAngle)
DEFINE_FIELD_NATIVE(DefineModelIndexField, FIELD_MODELINDEX, ModelIndex)
#undef DEFINE_FIELD_NATIVE

cell_t DefineInputFunc(IPluginContext* context, const cell_t* params) {
	auto factory = GetEditable(context, params[1]);
	if (!factory) {
		return 0;
	}

	char* keyName;
	context->LocalToString(params[2], &keyName);
	if (!keyName || (keyName && !keyName[0])) {
		return context->ThrowNativeError("Input name cannot be NULL or empty");
	}

	cell_t handlerType = params[3];
	IPluginFunction *handlerFunc = context->GetFunctionById(params[4]);

	fieldtype_t fieldType;
	switch (handlerType) {
		case 0: fieldType = FIELD_VOID; break;
		case 1: fieldType = FIELD_STRING; break;
		case 2: fieldType = FIELD_BOOLEAN; break;
		case 3: fieldType = FIELD_COLOR32; break;
		case 4: fieldType = FIELD_FLOAT; break;
		case 5:	fieldType = FIELD_INTEGER; break;
		case 6: fieldType = FIELD_VECTOR; break;
		default: fieldType = FIELD_CUSTOM; break;
	}

	int fieldNameSize = strlen(keyName) + 7;
	char* fieldName = new char[fieldNameSize];
	snprintf(fieldName, fieldNameSize, "Input%s", keyName);

	factory->DefineInputFunc(fieldName, fieldType, keyName, factory->CreateInputFuncDelegate(handlerFunc, fieldType) );

	delete[] fieldName;
	return params[1];
}

cell_t DefineOutput(IPluginContext* context, const cell_t* params) {
	auto factory = Get(context, params[1]);
	if (!factory) return 0;
	if (factory->IsNetworkLayoutFrozen()) return context->ThrowNativeError("Factory layout is frozen by the network schema; restart required");
	
	char* keyName;
	context->LocalToString(params[2], &keyName);
	if (!keyName || (keyName && !keyName[0])) {
		return context->ThrowNativeError("Output name cannot be NULL or empty.");
	}

	int fieldNameSize = strlen(keyName) + 7;
	char* fieldName = new char[fieldNameSize];
	snprintf(fieldName, fieldNameSize, "m_%s", keyName);

	factory->DefineOutput(fieldName, keyName);

	delete[] fieldName;
	return params[1];
}

void setup(std::vector<sp_nativeinfo_t>& natives) {
	sp_nativeinfo_t list[] = {
		{"CEntityFactory.CEntityFactory", CPluginEntityFactory_Ctor},
		{"CEntityFactory.DeriveFromBaseEntity", DeriveFromBaseEntity},
		{"CEntityFactory.DeriveFromClass", DeriveFromClass},
		{"CEntityFactory.DeriveFromNPC", DeriveFromNPC},
		{"CEntityFactory.DeriveFromFactory", DeriveFromFactory},
		{"CEntityFactory.DeriveFromConf", DeriveFromConf},
		{"CEntityFactory.SetInitialActionFactory", SetInitialActionFactory},
		{"CEntityFactory.Install", Install},
		{"CEntityFactory.Uninstall", Uninstall},
		{"CEntityFactory.IsInstalled.get", GetIsInstalled},
		{"CEntityFactory.IsAbstract.get", GetIsAbstract},
		{"CEntityFactory.IsAbstract.set", SetIsAbstract},
		{"CEntityFactory.GetClassname", GetClassname},
		{"CEntityFactory.GetFactoryOfEntity", GetFactoryOfEntity},
		{"CEntityFactory.GetNumInstalledFactories", GetNumInstalledFactories},
		{"CEntityFactory.GetInstalledFactories", GetInstalledFactories},
		{"CEntityFactory.AttachNextBot", AttachNextBot},
		{"CEntityFactory.BeginDataMapDesc", BeginDataMapDesc},
		{"CEntityFactory.DefineIntField", DefineIntField},
		{"CEntityFactory.DefineServerClass", DefineServerClass},
		{"CEntityFactory.DefineShortField", DefineShortField},
		{"CEntityFactory.DefineTimeField", DefineTimeField},
		{"CEntityFactory.DefineAngleField", DefineAngleField},
		{"CEntityFactory.DefineQAngleField", DefineQAngleField},
		{"CEntityFactory.DefineModelIndexField", DefineModelIndexField},
		{"CEntityFactory.DefineVectorXYField", DefineVectorXYField},
		{"CEntityFactory.DefineFloatField", DefineFloatField},
		{"CEntityFactory.DefineCharField", DefineCharField},
		{"CEntityFactory.DefineBoolField", DefineBoolField},
		{"CEntityFactory.DefineVectorField", DefineVectorField},
		{"CEntityFactory.DefineStringField", DefineStringField},
		{"CEntityFactory.DefineColorField", DefineColorField},
		{"CEntityFactory.DefineEntityField", DefineEntityField},
		{"CEntityFactory.DefineInputFunc", DefineInputFunc},
		{"CEntityFactory.DefineOutput", DefineOutput},
		{"CEntityFactory.EndDataMapDesc", EndDataMapDesc},
	};

	natives.insert(natives.end(), std::begin(list), std::end(list));
}

}
