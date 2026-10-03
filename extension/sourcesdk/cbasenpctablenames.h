#ifndef H_CBASENPC_TABLE_NAMES_
#define H_CBASENPC_TABLE_NAMES_

#include <map>
#include <string>
#include <stdexcept>

inline std::string CBaseNPCArrayTableName(const char* root, const char* field)
{
  return std::string(root) + "__" + field;
}

// Null entries reserve declared roots until their objects are built. Shared
// inheritance is legal; two different objects with one name are not.
template <typename Table, typename Compare>
void CBaseNPCRegisterTableName(Table* table, std::map<std::string, Table*, Compare>& names)
{
  if (!table || !table->GetName() || !*table->GetName())
    throw std::runtime_error("unnamed network table");
  auto inserted = names.emplace(table->GetName(), table);
  if (!inserted.second) {
    if (!inserted.first->second) inserted.first->second = table;
    else if (inserted.first->second != table)
      throw std::runtime_error(std::string("duplicate network table name: ") + table->GetName());
  }
}

#endif
