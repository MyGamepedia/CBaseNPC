#if defined _cbasenpc_cliententity_included
  #endinput
#endif
#define _cbasenpc_cliententity_included

/**
 * Black Mesa Win32 client entities. Check CClientEntityManager.IsAvailable()
 * before use: dedicated servers expose the API but have no client subsystem.
 *
 * These values are CLIENT references, not server CBaseEntity addresses.
 * Networked BCompat references are entindices (world 0 is valid); client-only
 * references contain a serialized client CBaseHandle. Invalid is -1, NOT 0.
 * Use GetHandleRef() for storage across frames: it includes the serial and
 * cannot resolve a new occupant after slot reuse. Do not test refs with > 0.
 *
 * Address methods use SourceMod's real Address ABI, not CBaseEntity's
 * pseudo-address representation. Raw offsets/addresses must refer to valid
 * client memory; the manager cannot determine an object's allocation size.
 *
 * Prop_Data searches client datamaps; Prop_Send searches client RecvTables.
 * Writes affect local client memory only and may be overwritten by networking.
 * Get/SetPropEntity and Get/SetDataEntity use client refs; -1 clears a handle.
 * Strings of type string_t are allocated in the CLIENT string pool.
 */
methodmap CClientEntity
{
  public CClientEntity(int clientRef) { return view_as<CClientEntity>(clientRef); }
  public int GetRef() { return view_as<int>(this); }
  public native int GetHandleRef();

  public native Address GetAddress();
  public native bool IsValid();
  public native int GetEntIndex(bool warn = true);
  public native bool GetClassname(char[] buffer, int maxlength);
  public native bool GetClassnameDiagnostics(
    int &entityIndex,
    char[] networkName, int networkNameLength,
    char[] mapClassname, int mapClassnameLength,
    char[] clientClassname, int clientClassnameLength,
    char[] serverClassname, int serverClassnameLength,
    char[] effectiveClassname, int effectiveClassnameLength);
  public native int GetData(int offset, int size = 4);
  public native void SetData(int offset, any value, int size = 4);
  public native float GetDataFloat(int offset);
  public native void SetDataFloat(int offset, float value);
  public native int GetDataEntity(int offset);
  public native void SetDataEntity(int offset, int other);
  public native void GetDataVector(int offset, float vec[3]);
  public native void SetDataVector(int offset, const float vec[3]);
  public native int GetDataString(int offset, char[] buffer, int maxlen);
  public native int SetDataString(int offset, const char[] buffer, int maxlen);
  public native void GetDataArray(int offset, any[] array, int arraySize, int dataSize = 4);
  public native void SetDataArray(int offset, const any[] array, int arraySize, int dataSize = 4);
  public native int GetProp(PropType type, const char[] prop, int size = 4, int element = 0);
  public native void SetProp(PropType type, const char[] prop, any value, int size = 4, int element = 0);
  public native float GetPropFloat(PropType type, const char[] prop, int element = 0);
  public native void SetPropFloat(PropType type, const char[] prop, float value, int element = 0);
  public native int GetPropEntity(PropType type, const char[] prop, int element = 0);
  public native void SetPropEntity(PropType type, const char[] prop, int other, int element = 0);
  public native void GetPropVector(PropType type, const char[] prop, float vec[3], int element = 0);
  public native void SetPropVector(PropType type, const char[] prop, const float vec[3], int element = 0);
  public native int GetPropString(PropType type, const char[] prop, char[] buffer, int maxlen, int element = 0);
  public native int SetPropString(PropType type, const char[] prop, const char[] buffer, int element = 0);
  public native int GetPropArraySize(PropType type, const char[] prop);
  public native bool HasProp(PropType type, const char[] prop);
  // actual=false: local property offset; true: accumulated table offset.
  // Returns -1 if absent. Does not execute DataTable pointer-redirection proxies.
  public native int GetRecvPropOffset(const char[] prop, bool actual = false);
  public native void StoreHandle(Address address);
  public native bool GetNetworkClass(char[] clsname, int maxlength);
  public native bool IsNetworkable();
  // SUB_Remove is restricted to client-only entities. Networked ones are kept.
  public native void Remove();
};

/**
 * FromEntIndex returns a serial-validated proper reference. Enumeration,
 * FromAddress, FromHandleAddress and FindByClassname return BCompat refs.
 * FindByClassname starts after startRef (-1 begins), supports a trailing '*'
 * and is case-insensitive. Classnames currently prefer the matching server
 * classname, then the client's IClientTools name, then C_BaseEntity.
 */
methodmap CClientEntityManager
{
  public static native bool IsAvailable();
  public static native CClientEntity FromAddress(Address clientAddress);
  public static native int GetCount();
  public static native CClientEntity GetByOrdinal(int ordinal);
  public static native CClientEntity FromHandleAddress(Address address);
  public static native CClientEntity FromEntIndex(int entityIndex);
  public static native CClientEntity FindByClassname(int startEnt, const char[] classname);
};

// Pre-existing entities are seeded silently. During destruction the record
// remains available until callbacks return. Do not retain callback addresses.
forward void OnEntityCreatedClient(int clientEntRef,
                                   const char[] classname, Address clientAddress);
forward void OnEntityDestroyedClient(int clientEntRef);
