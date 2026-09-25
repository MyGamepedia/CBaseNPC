#ifndef H_CBASENPC_SERVER_CLASS_
#define H_CBASENPC_SERVER_CLASS_

#include <IGameConfigs.h>
#include <memory>
#include <cstddef>

class ServerClass;

class CBaseNPCServerClassManager final
{
public:
	CBaseNPCServerClassManager();
	~CBaseNPCServerClassManager();
	bool Init(SourceMod::IGameConfig* config, char* error, size_t maxlength);
	void Shutdown();
	bool Finalize(char* error, size_t maxlength);
	bool IsFinalized() const;
	bool HasFailed() const;
	bool IsRegistrationOpen() const;
	const char* RegistrationError() const;
	ServerClass* GetCombinedHead() const;
	ServerClass* FindStockOrCustomClass(const char* name) const;
	ServerClass* Hook_GetAllServerClasses();
	void DumpSchema(const char* name = nullptr) const;
private:
	struct State;
	std::unique_ptr<State> m_State;
};

extern CBaseNPCServerClassManager g_CBaseNPCServerClassManager;
#endif
