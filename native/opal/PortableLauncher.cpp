#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>

// Statically linked launcher: no .NET, C++ runtime installer or PATH lookup.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    std::vector<wchar_t> module(32768);
    const auto size = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
    if (!size || size >= module.size()) return ERROR_BAD_PATHNAME;
    const std::wstring executable(module.data(), size);
    const auto separator = executable.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return ERROR_BAD_PATHNAME;
    const auto directory = executable.substr(0, separator) + L"\\appfiles";
    const auto application = directory + L"\\MyStudio.exe";
    std::wstring command = L"\"" + application + L"\"";
    if (arguments && *arguments) command += std::wstring(L" ") + arguments;
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE,
                        0, nullptr, directory.c_str(), &startup, &child)) {
        const DWORD error = GetLastError();
        const auto message = L"MyStudio could not start (Windows error " + std::to_wstring(error) +
            L").\nExtract the complete package and keep appfiles beside MyStudio.exe.";
        MessageBoxW(nullptr, message.c_str(), L"MyStudio", MB_OK | MB_ICONERROR);
        return static_cast<int>(error);
    }
    CloseHandle(child.hThread);
    const auto wait = WaitForSingleObject(child.hProcess, INFINITE);
    DWORD result = ERROR_PROCESS_ABORTED;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(child.hProcess, &result);
    CloseHandle(child.hProcess);
    return static_cast<int>(result);
}
