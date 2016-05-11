#include "ElDorito.hpp"
#include "Console/GameConsole.hpp"
#include "DirectXHook.hpp"

#include <Windows.h>
#include <TlHelp32.h>

#include <codecvt>

#include "Utils/Utils.hpp"
#include "ElPatches.hpp"
#include "Patches/Network.hpp"
#include "ThirdParty/WebSockets.hpp"
#include "Server/ServerChat.hpp"
#include "Server/VariableSynchronization.hpp"
#include "Server/BanList.hpp"
#include "Patches/Core.hpp"
#include <fstream>
#include <detours.h>

#include "Blam\Cache\StringIdCache.hpp"

size_t ElDorito::MainThreadID = 0;

extern BOOL installMedalJunk();

void initMedals()
{
	// This is kind of a hack, but only install the medal system for now if
	// halo3.zip can be opened for reading
	std::ifstream halo3Zip("mods\\medals\\halo3.zip");
	if (!halo3Zip.is_open())
		return;
	halo3Zip.close();
	installMedalJunk();
}

ElDorito::ElDorito()
{
}

//bool(__cdecl * ElDorito::Video_InitD3D)(bool, bool) = (bool(__cdecl *) (bool, bool)) 0xA21B40;
//
//bool __cdecl ElDorito::hooked_Video_InitD3D(bool windowless, bool nullRefDevice) {
//	// TEMP: leave window enabled for now so async networkWndProc stuff still works
//	return (*Video_InitD3D)(false, true);
//}

void ElDorito::killProcessByName(const char *filename, int ourProcessID)
{
	HANDLE hSnapShot = CreateToolhelp32Snapshot(TH32CS_SNAPALL, NULL);
	PROCESSENTRY32 pEntry;
	pEntry.dwSize = sizeof(pEntry);
	BOOL hRes = Process32First(hSnapShot, &pEntry);
	while (hRes)
	{
		if (strcmp(pEntry.szExeFile, filename) == 0)
		{
			if (pEntry.th32ProcessID != ourProcessID)
			{
				HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, 0, (DWORD)pEntry.th32ProcessID);

				if (hProcess != NULL)
				{
					TerminateProcess(hProcess, 9);
					CloseHandle(hProcess);
				}
			}
		}
		hRes = Process32Next(hSnapShot, &pEntry);
	}
	CloseHandle(hSnapShot);
}

void ElDorito::Initialize()
{
	::CreateDirectoryA(GetDirectory().c_str(), NULL);

	// init our command modules
	Modules::ElModules::Instance();

	// load variables/commands from cfg file
	Modules::CommandMap::Instance().ExecuteCommand("Execute dewrito_prefs.cfg");
	Modules::CommandMap::Instance().ExecuteCommand("Execute autoexec.cfg"); // also execute autoexec, which is a user-made cfg guaranteed not to be overwritten by ElDew

	//This should be removed when we can save binds
	Modules::CommandMap::Instance().ExecuteCommand("Bind CAPITAL +VoIP.Talk");

	// Parse command-line commands
	int numArgs = 0;
	LPWSTR* szArgList = CommandLineToArgvW(GetCommandLineW(), &numArgs);
	bool usingLauncher = Modules::ModuleGame::Instance().VarSkipLauncher->ValueInt == 1;
	mapsFolder = "maps\\";

	if( szArgList && numArgs > 1 )
	{
		std::wstring_convert<std::codecvt_utf8<wchar_t>> converter;

		for( int i = 1; i < numArgs; i++ )
		{
			std::wstring arg = std::wstring(szArgList[i]);
			if( arg.compare(0, 1, L"-") != 0 ) // if it doesn't start with -
				continue;

#ifndef _DEBUG
			if (arg.compare(L"-launcher") == 0)
				usingLauncher = true;
#endif

			if (arg.compare(L"-dedicated") == 0)
			{
				isDedicated = true;
				usingLauncher = true;
			}

			if (arg.compare(L"-maps") == 0 && i < numArgs - 1)
			{
				mapsFolder = Utils::String::ThinString(szArgList[i + 1]);
				if (mapsFolder.length() > 0 && mapsFolder.back() != '\\' && mapsFolder.back() != '/')
					mapsFolder += "\\";
			}

			size_t pos = arg.find(L"=");
			if( pos == std::wstring::npos || arg.length() <= pos + 1 ) // if it doesn't contain an =, or there's nothing after the =
				continue;

			std::string argname = converter.to_bytes(arg.substr(1, pos - 1));
			std::string argvalue = converter.to_bytes(arg.substr(pos + 1));

			Modules::CommandMap::Instance().ExecuteCommand(argname + " \"" + argvalue + "\"", true);
		}
	}

	Patches::Core::SetMapsFolder(mapsFolder);

	if (isDedicated)
	{
		Patches::Network::ForceDedicated();

		//// Commenting this out for now because it makes testing difficult
		//DetourRestoreAfterWith();
		//DetourTransactionBegin();
		//DetourUpdateThread(GetCurrentThread());
		//DetourAttach((PVOID*)&Video_InitD3D, &hooked_Video_InitD3D);

		//if (DetourTransactionCommit() != NO_ERROR) {
		//return;
		//}

	}
	else
	{
		initMedals();
	}

	// Language patch
	Patch(0x2333FD, { (uint8_t)Modules::ModuleGame::Instance().VarLanguageID->ValueInt }).Apply();

	setWatermarkText("ElDewrito | Version: " + Utils::Version::GetVersionString() + " | Build Date: " __DATE__);

#ifndef _DEBUG
	if (!usingLauncher) // force release builds to use launcher, simple check so its easy to get around if needed
	{
		MessageBox(GetConsoleWindow(), "Please run Halo Online using the ElDewrito launcher.\nIt should be named DewritoUpdater.exe.", "ElDewrito", MB_OK | MB_ICONINFORMATION);
		TerminateProcess(GetCurrentProcess(), 0);
	}
#endif

	// Ensure a ban list file exists
	Server::SaveDefaultBanList(Server::LoadDefaultBanList());

	// Initialize server modules
	Server::Chat::Initialize();
	Server::VariableSynchronization::Initialize();
	CreateThread(0, 0, StartRconWebSocketServer, 0, 0, 0);

	if (!Blam::Cache::StringIdCache::Instance.Load("maps\\string_ids.dat"))
	{
		MessageBox(NULL, "Failed to load 'maps\\string_ids.dat'!", "", MB_OK);
	}
}

void ElDorito::Tick(const std::chrono::duration<double>& DeltaTime)
{
	Server::VariableSynchronization::Tick();
	Server::Chat::Tick();
	Patches::Tick();

	// TODO: refactor this elsewhere
	Modules::ModuleCamera::Instance().UpdatePosition();

	Utils::AntiSpeedHack::OnTickCheck();

	if (executeCommandQueue)
	{
		Modules::CommandMap::Instance().ExecuteQueue();
		executeCommandQueue = false;
	}
}

namespace
{
	static void HandleFinder()
	{
	};
}

std::string ElDorito::GetDirectory()
{
	char Path[MAX_PATH];
	HMODULE hMod;
	if( !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&::HandleFinder, &hMod) )
	{
		int Error = GetLastError();
		OutputDebugString(std::string("Unable to resolve current directory, error code: ").append(std::to_string(Error)).c_str());
	}
	GetModuleFileNameA(hMod, Path, sizeof(Path));
	std::string Dir(Path);
	Dir = Dir.substr(0, std::string(Dir).find_last_of('\\') + 1);
	return Dir;
}

void ElDorito::OnMainMenuShown()
{
	executeCommandQueue = true;
	if (!isDedicated)
	{
		DirectXHook::hookDirectX();
		GameConsole::Instance();
	}
}

bool ElDorito::IsHostPlayer()
{
	return Patches::Network::IsInfoSocketOpen(); // TODO: find a way of using an ingame variable instead
}

// This is for the watermark in the bottom right corner (hidden by default)
void ElDorito::setWatermarkText(const std::string& Message)
{
	static wchar_t msgBuf[256];
	wmemset(msgBuf, 0, 256);

	std::wstring msgUnicode = Utils::String::WidenString(Message);
	wcscpy_s(msgBuf, 256, msgUnicode.c_str());

	Pointer::Base(0x2E5338).Write<uint8_t>(0x68);
	Pointer::Base(0x2E5339).Write(&msgBuf);
	Pointer::Base(0x2E533D).Write<uint8_t>(0x90);
	Pointer::Base(0x2E533E).Write<uint8_t>(0x90);
}

void* _mainTLS;
Pointer ElDorito::GetMainTls(size_t tlsOffset)
{
	// cache the result allowing future cross-thread calls to succeed
	if (_mainTLS == nullptr)
	{
		_asm
		{
			mov     eax, dword ptr fs:[2Ch]
			mov     eax, dword ptr ds:[eax]
			mov		_mainTLS, eax
		}
	}

	return Pointer(_mainTLS)(tlsOffset);
}