#ifndef CBASENPC_ACTION_REASONS_H
#define CBASENPC_ACTION_REASONS_H
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// IActionResult carries a borrowed pointer and is copied by the behavior code.
// Each callback/result slot owns just its current reason, not a history of all
// reasons. Pending event slots live outside Action to preserve its engine ABI.
// Like the behavior engine, these slots are used on the game thread only.
class CBaseNPCActionReasons
{
public:
  using Lease = std::shared_ptr<const std::string>;
  const char* Keep(const char* reason) {
    if (!reason) reason_.reset();
    else if (!reason_ || *reason_ != reason) reason_ = std::make_shared<const std::string>(reason);
    return Get();
  }
  const char* Get() const { return reason_ ? reason_->c_str() : nullptr; }
  Lease Pin() const { return reason_; }
  size_t Count() const { return reason_ ? 1 : 0; }

  // ProcessPendingEvents returns a borrowed pointer before ApplyResult can
  // copy it. Keep that handoff alive until the enclosing Behavior::Update
  // finishes, including nested updates and destruction of the source action.
  class Scope {
  public:
    Scope() : previous_(ActiveScope()) { ActiveScope() = this; }
    ~Scope() { ActiveScope() = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  private:
    friend class CBaseNPCActionReasons;
    Scope* previous_;
    std::vector<Lease> consumed_;
  };
  static const char* StorePending(const void* action, Lease reason) {
    if (!reason) { ClearPending(action); return nullptr; }
    auto& slot = Pending()[action];
    slot = std::move(reason);
    return slot->c_str();
  }
  static void ClearPending(const void* action) { Pending().erase(action); }
  static void ConsumePending(const void* action) {
    auto found = Pending().find(action);
    if (found == Pending().end()) return;
    if (auto scope = ActiveScope()) {
      scope->consumed_.push_back(std::move(found->second));
      Pending().erase(found);
    }
    // The SDK's consumption path always runs inside Update's scope. Retain
    // the single slot if used outside it, rather than returning a dangling ptr.
  }
  static size_t PendingCount() { return Pending().size(); }
private:
  static Scope*& ActiveScope() { static thread_local Scope* scope = nullptr; return scope; }
  static std::map<const void*, Lease>& Pending() { static std::map<const void*, Lease> pending; return pending; }
  Lease reason_;
};

// Nested callbacks have independent results and ownership. Finishing a nested
// callback must neither overwrite the outer result nor free its borrowed text.
template<typename Result>
class CBaseNPCActionResultFrame
{
public:
  CBaseNPCActionResultFrame(Result initial, CBaseNPCActionResultFrame*& active,
                           CBaseNPCActionReasons& returned)
    : result(initial), active_(active), previous_(active), returned_(returned) { active_ = this; }
  ~CBaseNPCActionResultFrame() { active_ = previous_; }
  CBaseNPCActionResultFrame(const CBaseNPCActionResultFrame&) = delete;
  CBaseNPCActionResultFrame& operator=(const CBaseNPCActionResultFrame&) = delete;
  Result Finish() { returned_ = reason; return result; }
  Result result;
  CBaseNPCActionReasons reason;
private:
  CBaseNPCActionResultFrame*& active_;
  CBaseNPCActionResultFrame* previous_;
  CBaseNPCActionReasons& returned_;
};
#endif
