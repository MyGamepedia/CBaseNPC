#ifndef CBASENPC_ACTION_REASONS_H
#define CBASENPC_ACTION_REASONS_H
#include <set>
#include <string>

// IActionResult carries a borrowed pointer and is copied by the behavior code.
// Keep reasons stable (including across nested OnEnd/event calls) only for the
// lifetime of the action that generated them, never for a retired consumer.
class CBaseNPCActionReasons
{
public:
  const char* Keep(const char* reason) {
    return reason ? reasons_.emplace(reason).first->c_str() : nullptr;
  }
  size_t Count() const { return reasons_.size(); }
private:
  std::set<std::string> reasons_;
};
#endif
