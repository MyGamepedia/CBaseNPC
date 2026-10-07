#ifndef CBASENPC_NATIVE_ACTIVITY_H
#define CBASENPC_NATIVE_ACTIVITY_H

// Main-thread fence shared by all adapter contexts. A grant can make an idle
// owner's objects part of another consumer's active call. Cleanup itself also
// invokes forwards, so keep the fence raised throughout handle destruction.
class CBaseNPCNativeActivity
{
public:
  static bool IsActive() { return Depth() != 0; }
  static void Enter() { ++Depth(); }
  static void Leave() { --Depth(); }
  class Scope
  {
  public:
    Scope() { Enter(); }
    ~Scope() { Leave(); }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };
private:
  static unsigned& Depth() { static unsigned depth = 0; return depth; }
};
#endif
