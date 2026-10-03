#if defined _cbasenpc_entityfactoryclient_included
 #endinput
#endif
#define _cbasenpc_entityfactoryclient_included

/** Called after one stock client construction and sidecar/hook attachment.
 * Network values have NOT yet been decoded. Use OnEntityCreatedClient at
 * FRAME_NET_UPDATE_END to inspect the initial replicated fields/classname.
 */
typedef PostConstructorClient = function void(int entity);
/** Called once, before sidecar cleanup. Receives the usual client entity ref. */
typedef OnRemoveClient = function void(int entity);

/** Declare during initial OnPluginStart/OnAllPluginsLoaded; changes need a restart.
 * Extra receive fields are generated from the matching SERVER factory's Send
 * fields. No physical client C++ members/datamap fields are allocated.
 * On dedicated servers these declarations are accepted but have no runtime.
 */
methodmap CEntityFactoryClient < Handle
{
 public native CEntityFactoryClient(const char[] classname,
     PostConstructorClient postConstructor = INVALID_FUNCTION,
     OnRemoveClient onRemove = INVALID_FUNCTION);
 public native bool DefineClientClass(const char[] networkName,
     const char[] rootRecvTableName, const char[] baseNetworkClass);
 /** Physical construction base must name a STOCK ClientClass with a create fn. */
 public native bool DeriveFromNetworkClass(const char[] stockNetworkClass);
 public native bool Install();
};
