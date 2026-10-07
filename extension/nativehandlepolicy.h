#ifndef CBASENPC_NATIVE_HANDLE_POLICY_H
#define CBASENPC_NATIVE_HANDLE_POLICY_H
#include <IShareSys.h>
#include <map>
#include <set>

// Authorization follows the object, not the numeric handle: cloning a handle
// does not silently grant permission to change another owner's factory.
class CBaseNPCNativeHandlePolicy
{
public:
  using Identity = SourceMod::IdentityToken_t*;
  // Creation on behalf of a plugin does not make the creating consumer its
  // lifetime owner. Only a scope belonging to the actual owner may revoke it.
  void Register(void* object, Identity owner, Identity creator, void* scope = nullptr) {
    objects_[object] = {owner, owner && owner == creator ? scope : nullptr, {}};
  }
  void Forget(void* object) { objects_.erase(object); }
  bool Owns(void* object, Identity owner) const {
    auto found = objects_.find(object);
    return owner && found != objects_.end() && found->second.owner == owner;
  }
  bool CanMutate(void* object, Identity caller, void* scope) const {
    auto found = objects_.find(object);
    return caller && found != objects_.end() &&
      (found->second.owner == caller || (scope && found->second.grantees.count(scope)));
  }
  bool Grant(void* object, Identity owner, void* recipient) {
    if (!recipient || !Owns(object, owner)) return false;
    objects_.find(object)->second.grantees.insert(recipient);
    return true;
  }
  bool Revoke(void* object, Identity owner, void* recipient) {
    if (!Owns(object, owner)) return false;
    objects_.find(object)->second.grantees.erase(recipient);
    return true;
  }
  void RemoveIdentity(Identity identity) {
    for (auto& object : objects_) {
      if (object.second.owner == identity) {
        object.second.owner = nullptr;
        object.second.grantees.clear();
      }
    }
  }
  void RemoveScope(void* scope) {
    if (!scope) return;
    for (auto& object : objects_) {
      object.second.grantees.erase(scope);
      if (object.second.scope == scope) {
        object.second.owner = nullptr;
        object.second.grantees.clear();
      }
    }
  }
private:
  struct Access { Identity owner; void* scope; std::set<void*> grantees; };
  std::map<void*, Access> objects_;
};
#endif
