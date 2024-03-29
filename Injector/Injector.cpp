#include <Windows.h>
#include <WtsApi32.h>
#include <atlstr.h>

#include <iostream>
#include <filesystem>
#include <fstream>
#include <string>
#include <sstream>

#include "spdlog\spdlog.h"
#include "spdlog\sinks\stdout_color_sinks.h"

#include "nlohmann\json.hpp"

#ifdef _M_AMD64
	#define DLL_NAME "InjectorDLL_x64.dll"
#else
	#define DLL_NAME "InjectorDLL_x86.dll"
#endif

namespace filesystem = std::experimental::filesystem;

void handle();

std::string randomString(size_t length) {
	auto randchar = []() -> char {
		const char charset[] =
			"0123456789"
			"ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz";
		const size_t max_index = (sizeof(charset) - 1);
		return charset[rand() % max_index];
	};

	std::string str(length, 0);
	std::generate_n(str.begin(), length, randchar);

	return str;
}

int main() {
	SetConsoleTitleA(randomString(128).c_str());

	handle();

	std::cin.get();
    return 0;
}

struct param_enum
{
	unsigned long ulPID;
	HWND hWnd_out;
};

BOOL CALLBACK EnumWindowsCallback(HWND handle, LPARAM lParam) {
	param_enum& param_data = *(param_enum*)lParam;
	unsigned long process_id = 0;

	GetWindowThreadProcessId(handle, &process_id);

	if (param_data.ulPID != process_id) {
		return TRUE;
	}

	param_data.hWnd_out = handle;

	return FALSE;
}

HWND FindProcessWindow(unsigned long process_id) {
	param_enum param_data;
	param_data.ulPID = process_id;
	param_data.hWnd_out = 0;
	EnumWindows(EnumWindowsCallback, (LPARAM)&param_data);

	return param_data.hWnd_out;
}

void handle() {
	auto logger = spdlog::stderr_color_mt("Injector");

	#ifdef _DEBUG
		spdlog::set_level(spdlog::level::debug);
	#else 
		spdlog::set_level(spdlog::level::info);
	#endif

	logger->info("Searching for configuration file...");

	filesystem::path configPath = filesystem::current_path() / "injector_config.json";

	std::ifstream configStream(configPath);
	if (!configStream) {
		logger->error("Configuration not found in directory: " + configPath.parent_path().string());
		return;
	}

	filesystem::path tempConfigPath = filesystem::temp_directory_path() / "injector_config.json";
	bool configCopied = filesystem::copy_file(configPath, tempConfigPath, filesystem::copy_options::overwrite_existing);

	if (!configCopied) {
		logger->error("Failed to copy configuration!");
		return;
	}

	nlohmann::json configData;
	configStream >> configData;

	std::string jarPath = configData["jar_path"];
	std::string className = configData["class_name"];
	std::string methodName = configData["method_name"];

	logger->debug("JAR Path: " + jarPath);
	logger->debug("Class Name: " + className);
	logger->debug("Method Name: " + methodName);

	logger->info("Loaded config!");
	logger->info("Searching for Minecraft process...");

	DWORD gameProcessID = -1;

	WTS_PROCESS_INFO* pWPIs = NULL;
	DWORD dwProcCount = 0;
	if (WTSEnumerateProcesses(WTS_CURRENT_SERVER_HANDLE, NULL, 1, &pWPIs, &dwProcCount)) {
		for (DWORD i = 0; i < dwProcCount; i++) {
			WTS_PROCESS_INFO processInfo = pWPIs[i];

			std::string processName = std::string(CW2A(processInfo.pProcessName));
			DWORD processID = processInfo.ProcessId;

			if (processName != "java.exe" && processName != "javaw.exe") {
				continue;
			}

			HWND processWindow = FindProcessWindow(processID);

			char windowName[256];
			GetWindowTextA(processWindow, windowName, 256);

			std::ostringstream processDebugMessage;
			processDebugMessage << "Java process (" << processName << ") with PID " << processID << " and title \"" << windowName << "\"";

			logger->debug(processDebugMessage.str());

			std::string windowNameString(windowName);
			if (windowNameString.find("Minecraft") != std::string::npos) {
				logger->debug("Process is candidate!");

				gameProcessID = processID;
			}
		}
	}

	if (pWPIs) {
		WTSFreeMemory(pWPIs);
		pWPIs = NULL;
	}

	if (gameProcessID == -1) {
		logger->error("Failed to find game process!");
		return;
	}

	std::ostringstream foundMessage;
	foundMessage << "Found game process with ID: " << gameProcessID;
	logger->info(foundMessage.str());

	filesystem::path injectDllPath = filesystem::current_path() / DLL_NAME;

	std::ifstream dllInputStream(injectDllPath);
	if (!dllInputStream) {
		logger->error("Failed to find DLL at path: " + injectDllPath.string());
		return;
	}

	logger->debug("Using DLL: " + injectDllPath.string());

	HANDLE gameProcess = OpenProcess(PROCESS_ALL_ACCESS, false, gameProcessID);
	if (!gameProcess) {
		logger->error("Failed to open game process!");
		return;
	}

	USHORT pProcessMachine = 0;
	IsWow64Process2(gameProcess, &pProcessMachine, nullptr);

	USHORT pProcessMachineOwn = 0;
	IsWow64Process2(GetCurrentProcess(), &pProcessMachineOwn, nullptr);

	if (pProcessMachine != pProcessMachineOwn) {
		logger->error("Architecture mismatch! Use the other executable!");
		return;
	}

	std::string dllPath = injectDllPath.string();

	long dllPathLength = dllPath.length() + 1;

	LPVOID allocatedMemory = VirtualAllocEx(gameProcess, NULL, dllPathLength, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (allocatedMemory == nullptr) {
		logger->error("Failed to allocate memory in the game process!");
		return;
	}

	int memoryWriteResult = WriteProcessMemory(gameProcess, allocatedMemory, dllPath.c_str(), dllPathLength, 0);
	if (memoryWriteResult == 0) {
		logger->error("Failed to write process memory!");
	}

	DWORD dWord;
	LPTHREAD_START_ROUTINE loadLibraryAddress = (LPTHREAD_START_ROUTINE)GetProcAddress(LoadLibraryA("kernel32"), "LoadLibraryA");
	HANDLE remoteThread = CreateRemoteThread(gameProcess, NULL, 0, loadLibraryAddress, allocatedMemory, 0, &dWord);
	if (remoteThread == NULL) {
		logger->error("Failed to create remote thread!");
		return;
	}

	if ((gameProcess != NULL) && (allocatedMemory != NULL) && (memoryWriteResult != ERROR_INVALID_HANDLE) && (remoteThread != NULL)) {
		logger->info("Injection complete! You can now close this window.");
	} else {
		logger->error("Injection failed!");
	}
}