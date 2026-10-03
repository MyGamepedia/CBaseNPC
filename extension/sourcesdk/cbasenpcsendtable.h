#ifndef H_CBASENPC_SEND_TABLE_
#define H_CBASENPC_SEND_TABLE_

#include <dt_send.h>
#include <datamap.h>
#include <deque>
#include <memory>
#include <string>
#include <vector>

enum class CBaseNPCSendFieldKind
{
	Int, Short, Char, Bool, Float, Time, Angle, Vector, VectorXY, QAngle,
	StringT, Color32, EHandle, ModelIndex
};

struct CBaseNPCSendFieldDesc
{
	int dataDescIndex = -1;
	CBaseNPCSendFieldKind kind = CBaseNPCSendFieldKind::Int;
	int bits = -1;
	int flags = 0;
	float lowValue = 0.0f;
	float highValue = HIGH_DEFAULT;
	int stringMaxLength = DT_MAX_STRING_BUFFERSIZE;
};

bool CBaseNPC_ValidateSendFieldOptions(const CBaseNPCSendFieldDesc& desc, std::string& error);
float CBaseNPC_AssignRangeMultiplier(int bits, double range);

// All engine-facing objects/strings are separately allocated or in a deque.
// The owner itself is never moved after construction. No pointers into the
// factory's datamap vector or SourcePawn memory survive finalization.
class CBaseNPCSendTable final
{
public:
	CBaseNPCSendTable(const char* name, size_t fields, SendTable* base);
	bool BuildField(size_t index, const typedescription_t& td,
		const CBaseNPCSendFieldDesc& desc, std::string& error);
	SendTable* GetTable() { return &m_Table; }
	const char* CopyString(const char* value);
	CBaseNPCSendTable(const CBaseNPCSendTable&) = delete;
	CBaseNPCSendTable& operator=(const CBaseNPCSendTable&) = delete;

private:
	std::deque<std::string> m_Strings;
	std::unique_ptr<SendProp[]> m_Props;
	std::vector<std::unique_ptr<SendProp[]>> m_ArrayProps;
	std::vector<std::unique_ptr<SendProp>> m_ArrayTemplates;
	std::vector<std::unique_ptr<SendTable>> m_ArrayTables;
	SendTable m_Table;
};

#endif
