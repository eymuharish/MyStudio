#include "telestudio/media/CaptureCatalogC.h"
#include "telestudio/media/DesktopCaptureSource.h"

#include <Windows.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <array>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <set>
#include <ranges>
#include <thread>
#include <atomic>
#include <chrono>
#include <string_view>
#include <utility>

#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
namespace telestudio::testing {
struct SyntheticCatalogEntry {
    CaptureItemInfo item;
    bool primary = false;
};
void ConfigureCaptureCatalog(std::vector<SyntheticCatalogEntry> entries,
                             bool supported, bool raiseSeh);
}
#endif

namespace {
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

#ifndef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
constexpr wchar_t kCatalogHarnessClass[] = L"EssaStudioCatalogHarnessClass";
constexpr wchar_t kCatalogHarnessTitle[] = L"Synthetic Capture Harness";

LRESULT CALLBACK CatalogHarnessProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}

int RunCatalogHarness() {
    WNDCLASSW cls{};
    cls.lpfnWndProc = CatalogHarnessProc;
    cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = kCatalogHarnessClass;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return 2;
    HWND window = CreateWindowExW(0, kCatalogHarnessClass, kCatalogHarnessTitle,
                                  WS_OVERLAPPEDWINDOW | WS_VISIBLE, 120, 120, 640, 360,
                                  nullptr, nullptr, cls.hInstance, nullptr);
    if (!window) return 3;
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return 0;
}

struct HarnessProcess {
    PROCESS_INFORMATION info{};
    HarnessProcess() = default;
    HarnessProcess(const HarnessProcess&) = delete;
    HarnessProcess& operator=(const HarnessProcess&) = delete;
    HarnessProcess(HarnessProcess&& other) noexcept : info(std::exchange(other.info, {})) {}
    ~HarnessProcess() { Close(); }
    void Close() noexcept {
        if (!info.hProcess) return;
        TerminateProcess(info.hProcess, 0);
        WaitForSingleObject(info.hProcess, 3000);
        CloseHandle(info.hThread);
        CloseHandle(info.hProcess);
        info = {};
    }
};

HarnessProcess StartCatalogHarness() {
    wchar_t executable[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, executable, MAX_PATH)) throw std::runtime_error("harness launch failed");
    std::wstring command = L"\"" + std::wstring(executable) + L"\" --catalog-harness";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    HarnessProcess child;
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &startup, &child.info))
        throw std::runtime_error("harness launch failed");
    return child;
}
#endif

#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
std::int32_t SyntheticStatus(std::vector<telestudio::testing::SyntheticCatalogEntry> entries) {
    telestudio::testing::ConfigureCaptureCatalog(std::move(entries), true, false);
    std::uint32_t count = 0, bytes = 0;
    std::uint64_t snapshot = 0;
    return EssaCaptureCatalogQueryV1(1, 0, nullptr, 0, nullptr, 0,
                                     &count, &bytes, &snapshot);
}

int RunSyntheticContractTests() {
    using telestudio::CaptureItemInfo;
    using telestudio::CaptureItemKind;
    using telestudio::testing::SyntheticCatalogEntry;
    std::uint32_t count = 9, bytes = 9;
    std::uint64_t snapshot = 9;
    telestudio::testing::ConfigureCaptureCatalog({}, false, false);
    Require(EssaCaptureCatalogQueryV1(1, 0, nullptr, 0, nullptr, 0,
                &count, &bytes, &snapshot) == ESSA_CAPTURE_NOT_SUPPORTED &&
                count == 0 && bytes == 0 && snapshot == 0,
            "unsupported WGC must fail with no catalog publication");
    telestudio::testing::ConfigureCaptureCatalog({}, true, false);
    Require(EssaCaptureCatalogQueryV1(1, 0, nullptr, 0, nullptr, 0,
                &count, &bytes, &snapshot) == ESSA_CAPTURE_OK && snapshot != 0 &&
                count == 0 && bytes == 0,
            "an empty supported catalog must have a stable nonzero snapshot");

    std::string longName;
    for (int index = 0; index != 600; ++index) longName += "\xF0\x9F\x8E\xA5";
    std::vector<SyntheticCatalogEntry> ordered = {
        {{"window-b", CaptureItemKind::Window, "beta", 640, 360}, false},
        {{"monitor-b", CaptureItemKind::Monitor, "secondary", 1280, 720}, false},
        {{"window-a", CaptureItemKind::Window, "Alpha", 640, 360}, false},
        {{"monitor-a", CaptureItemKind::Monitor, "primary", 1920, 1080}, true},
        {{"window-long", CaptureItemKind::Window, longName, 640, 360}, false},
    };
    telestudio::testing::ConfigureCaptureCatalog(ordered, true, false);
    count = bytes = 0; snapshot = 0;
    Require(EssaCaptureCatalogQueryV1(1, 0, nullptr, 0, nullptr, 0,
                &count, &bytes, &snapshot) == ESSA_CAPTURE_BUFFER_TOO_SMALL && count == ordered.size(),
            "synthetic valid catalog must size exactly");
    std::vector<EssaCaptureCatalogItemV1> items(count);
    std::vector<std::uint8_t> utf8(bytes);
    Require(EssaCaptureCatalogQueryV1(1, snapshot, items.data(), count, utf8.data(), bytes,
                &count, &bytes, &snapshot) == ESSA_CAPTURE_OK,
            "synthetic valid catalog must fill");
    auto id = [&](std::size_t index) {
        return std::string(reinterpret_cast<const char*>(utf8.data() + items[index].idOffset),
                           items[index].idLength);
    };
    Require(id(0) == "monitor-a" && id(1) == "monitor-b" && id(2) == "window-a" &&
                id(3) == "window-b",
            "catalog ordering must be primary, stable monitors, then ordinal case-insensitive windows");
    const auto longRecord = std::find_if(items.begin(), items.end(), [&](const auto& item) {
        return std::string(reinterpret_cast<const char*>(utf8.data() + item.idOffset), item.idLength) ==
               "window-long";
    });
    Require(longRecord != items.end() && longRecord->nameLength <= 2048,
            "overlong multibyte names must truncate inside the byte limit");
    const auto* truncated = utf8.data() + longRecord->nameOffset;
    Require(longRecord->nameLength >= 3 && truncated[longRecord->nameLength - 3] == 0xe2 &&
                truncated[longRecord->nameLength - 2] == 0x80 &&
                truncated[longRecord->nameLength - 1] == 0xa6,
            "truncated names must end with one Unicode ellipsis");

    Require(SyntheticStatus({{{std::string(1024, 'i'), CaptureItemKind::Window,
                               std::string(512, 'n'), 16384, 16384}, false}}) ==
                ESSA_CAPTURE_BUFFER_TOO_SMALL,
            "exact ID/name/scalar/dimension maxima must remain accepted");
    std::vector<SyntheticCatalogEntry> exactItems;
    exactItems.reserve(1024);
    for (int index = 0; index != 1024; ++index)
        exactItems.push_back({{"item-" + std::to_string(index), CaptureItemKind::Window,
                               "name", 1, 1}, false});
    Require(SyntheticStatus(std::move(exactItems)) == ESSA_CAPTURE_BUFFER_TOO_SMALL,
            "exactly 1024 catalog items must remain accepted");

    std::vector<SyntheticCatalogEntry> tooMany(1025,
        {{"id", CaptureItemKind::Window, "name", 1, 1}, false});
    for (std::size_t index = 0; index < tooMany.size(); ++index)
        tooMany[index].item.id = "id-" + std::to_string(index);
    Require(SyntheticStatus(std::move(tooMany)) == ESSA_CAPTURE_LIMIT_EXCEEDED,
            "1025 catalog items must fail closed");
    Require(SyntheticStatus({{{std::string(1025, 'x'), CaptureItemKind::Window, "name", 1, 1}, false}}) ==
                ESSA_CAPTURE_LIMIT_EXCEEDED,
            "an ID beyond 1024 bytes must fail closed");
    Require(SyntheticStatus({{{std::string("\xC0\x80", 2), CaptureItemKind::Window, "name", 1, 1}, false}}) ==
                ESSA_CAPTURE_LIMIT_EXCEEDED,
            "malformed UTF-8 must fail closed");
    Require(SyntheticStatus({{{std::string("x\0y", 3), CaptureItemKind::Window, "name", 1, 1}, false}}) ==
                ESSA_CAPTURE_LIMIT_EXCEEDED,
            "embedded NUL must fail closed");
    Require(SyntheticStatus({{{"same", CaptureItemKind::Window, "a", 1, 1}, false},
                              {{"same", CaptureItemKind::Window, "b", 1, 1}, false}}) ==
                ESSA_CAPTURE_LIMIT_EXCEEDED,
            "duplicate IDs must fail closed");
    Require(SyntheticStatus({{{"bad-size", CaptureItemKind::Window, "name", 16385, 1}, false}}) ==
                ESSA_CAPTURE_LIMIT_EXCEEDED,
            "dimensions above 16384 must fail closed");

    telestudio::testing::ConfigureCaptureCatalog({}, true, true);
    std::array<std::uint8_t, 64> canary{};
    canary.fill(0x6d);
    count = bytes = 0; snapshot = 0;
    Require(EssaCaptureCatalogQueryV1(1, 0,
                reinterpret_cast<EssaCaptureCatalogItemV1*>(canary.data()), 1,
                canary.data() + 32, 32, &count, &bytes, &snapshot) == ESSA_CAPTURE_INTERNAL_FAILURE &&
                std::all_of(canary.begin(), canary.end(), [](auto value) { return value == 0x6d; }),
            "SEH must translate at the C boundary without partial buffer writes");
    std::cout << "capture_catalog_c_contract_tests: PASS\n";
    return EXIT_SUCCESS;
}
#endif
}

int main(int argc, char** argv) {
    try {
#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
        return RunSyntheticContractTests();
#else
        if (argc == 2 && std::string_view(argv[1]) == "--catalog-harness")
            return RunCatalogHarness();
        Require(sizeof(EssaCaptureCatalogItemV1) == 32, "ABI item must be 32 bytes");
        Require(offsetof(EssaCaptureCatalogItemV1, structSize) == 0 &&
                    offsetof(EssaCaptureCatalogItemV1, kind) == 4 &&
                    offsetof(EssaCaptureCatalogItemV1, width) == 8 &&
                    offsetof(EssaCaptureCatalogItemV1, height) == 12 &&
                    offsetof(EssaCaptureCatalogItemV1, idOffset) == 16 &&
                    offsetof(EssaCaptureCatalogItemV1, idLength) == 20 &&
                    offsetof(EssaCaptureCatalogItemV1, nameOffset) == 24 &&
                    offsetof(EssaCaptureCatalogItemV1, nameLength) == 28,
                "ABI offsets must be frozen");
        Require(EssaCaptureCatalogAbiVersion() == 1, "ABI version must be one");
        std::uint32_t count = 0, bytes = 0;
        std::uint64_t snapshot = 0;
        const auto sizing = EssaCaptureCatalogQueryV1(1, 0, nullptr, 0, nullptr, 0,
                                                       &count, &bytes, &snapshot);
        Require(snapshot != 0 && (sizing == ESSA_CAPTURE_BUFFER_TOO_SMALL || sizing == ESSA_CAPTURE_OK),
                "sizing must return a snapshot and exact requirements");
        std::vector<EssaCaptureCatalogItemV1> items(count);
        std::vector<std::uint8_t> utf8(bytes);
        std::array<std::uint8_t, 64> canary{};
        canary.fill(0xa5);
        std::uint32_t changedCount = 1, changedBytes = 1;
        std::uint64_t changedSnapshot = 1;
        const auto mismatch = EssaCaptureCatalogQueryV1(2, snapshot,
            reinterpret_cast<EssaCaptureCatalogItemV1*>(canary.data()), 1,
            canary.data() + 32, 32, &changedCount, &changedBytes, &changedSnapshot);
        Require(mismatch == ESSA_CAPTURE_ABI_MISMATCH &&
                    std::all_of(canary.begin(), canary.end(), [](auto value) { return value == 0xa5; }),
                "ABI mismatch must not partially write caller buffers");
        const auto snapshotFailure = EssaCaptureCatalogQueryV1(1, snapshot ^ 0x55,
            reinterpret_cast<EssaCaptureCatalogItemV1*>(canary.data()), 1,
            canary.data() + 32, 32, &changedCount, &changedBytes, &changedSnapshot);
        Require(snapshotFailure == ESSA_CAPTURE_CATALOG_CHANGED,
                "changed snapshot must return CATALOG_CHANGED");
        Require(std::all_of(canary.begin(), canary.end(), [](auto value) { return value == 0xa5; }),
                "changed snapshot must not partially write caller buffers");
        const auto fill = EssaCaptureCatalogQueryV1(1, snapshot, items.data(), count,
                                                     utf8.data(), bytes, &count, &bytes, &snapshot);
        Require(fill == ESSA_CAPTURE_OK && count > 0, "fill must return the real catalog");
        std::vector<EssaCaptureCatalogItemV1> shortItems(count);
        std::vector<std::uint8_t> shortUtf8(bytes, 0xa5);
        std::memset(shortItems.data(), 0xa5, shortItems.size() * sizeof(EssaCaptureCatalogItemV1));
        changedCount = changedBytes = 0;
        changedSnapshot = 0;
        const auto shortBuffer = EssaCaptureCatalogQueryV1(1, snapshot,
            shortItems.data(), count, shortUtf8.data(), bytes - 1,
            &changedCount, &changedBytes, &changedSnapshot);
        Require(shortBuffer == ESSA_CAPTURE_BUFFER_TOO_SMALL && changedCount == count &&
                    changedBytes == bytes && changedSnapshot == snapshot &&
                    std::all_of(reinterpret_cast<const std::uint8_t*>(shortItems.data()),
                                reinterpret_cast<const std::uint8_t*>(shortItems.data()) +
                                    shortItems.size() * sizeof(EssaCaptureCatalogItemV1),
                                [](auto value) { return value == 0xa5; }) &&
                    std::all_of(shortUtf8.begin(), shortUtf8.end(), [](auto value) { return value == 0xa5; }),
                "insufficient buffers must return exact requirements with no partial write");
        Require(EssaCaptureCatalogQueryV1(1, 0, nullptr, 1, nullptr, 0,
                    &changedCount, &changedBytes, &changedSnapshot) == ESSA_CAPTURE_INVALID_ARGUMENT,
                "null buffer with nonzero capacity must be rejected");
        std::int32_t apartmentStatus = 0;
        std::array<std::uint8_t, 64> apartmentCanary{};
        apartmentCanary.fill(0x5a);
        std::thread sta([&] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            std::uint32_t requiredItems = 0, requiredBytes = 0;
            std::uint64_t token = 0;
            apartmentStatus = EssaCaptureCatalogQueryV1(1, 0,
                reinterpret_cast<EssaCaptureCatalogItemV1*>(apartmentCanary.data()), 1,
                apartmentCanary.data() + 32, 32, &requiredItems, &requiredBytes, &token);
            CoUninitialize();
        });
        sta.join();
        Require(apartmentStatus == ESSA_CAPTURE_WRONG_APARTMENT &&
                    std::all_of(apartmentCanary.begin(), apartmentCanary.end(),
                                [](auto value) { return value == 0x5a; }),
                "STA query must return WRONG_APARTMENT without partial data");
        std::set<std::string> unique;
        for (const auto& item : items) {
            Require(item.structSize == 32 && item.width > 0 && item.height > 0,
                    "ABI records must be bounded and versioned");
            Require(item.idOffset + item.idLength <= utf8.size() &&
                        item.nameOffset + item.nameLength <= utf8.size(),
                    "ABI offsets must stay within the blob");
            Require(item.kind == ESSA_CAPTURE_MONITOR || item.kind == ESSA_CAPTURE_WINDOW,
                    "item kind must be frozen");
            std::string id(reinterpret_cast<const char*>(utf8.data() + item.idOffset), item.idLength);
            Require(unique.insert(id).second, "ABI IDs must be unique");
        }
        HMODULE module = GetModuleHandleW(L"EssaStudio.CaptureCatalog.dll");
        Require(module != nullptr, "catalog DLL must be loaded with its exact filename");
        const auto base = reinterpret_cast<const std::uint8_t*>(module);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        Require(nt->FileHeader.Machine == IMAGE_FILE_MACHINE_AMD64, "catalog DLL must be AMD64");
        const auto exportRva = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
        const auto exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + exportRva);
        Require(exports->NumberOfNames == 3, "catalog DLL must have exactly three exports");
        const auto names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        std::set<std::string> exportNames;
        for (DWORD index = 0; index < exports->NumberOfNames; ++index)
            exportNames.emplace(reinterpret_cast<const char*>(base + names[index]));
        Require(exportNames == std::set<std::string>{"EssaCaptureCatalogAbiVersion", "EssaCaptureCatalogQueryV1", "EssaCameraCatalogQueryV1"},
                "catalog exports must be exact and undecorated");

        auto harness = StartCatalogHarness();
        std::string portableWindowId;
        for (int retry = 0; retry != 50 && portableWindowId.empty(); ++retry) {
            std::uint32_t liveCount = 0, liveBytes = 0;
            std::uint64_t liveSnapshot = 0;
            const auto liveSizing = EssaCaptureCatalogQueryV1(1, 0, nullptr, 0, nullptr, 0,
                                                               &liveCount, &liveBytes, &liveSnapshot);
            if (liveSizing == ESSA_CAPTURE_BUFFER_TOO_SMALL || liveSizing == ESSA_CAPTURE_OK) {
                std::vector<EssaCaptureCatalogItemV1> liveItems(liveCount);
                std::vector<std::uint8_t> liveUtf8(liveBytes);
                if (EssaCaptureCatalogQueryV1(1, liveSnapshot, liveItems.data(), liveCount,
                                               liveUtf8.data(), liveBytes, &liveCount, &liveBytes,
                                               &liveSnapshot) == ESSA_CAPTURE_OK) {
                    for (const auto& item : liveItems) {
                        if (item.kind != ESSA_CAPTURE_WINDOW) continue;
                        const std::string name(
                            reinterpret_cast<const char*>(liveUtf8.data() + item.nameOffset),
                            item.nameLength);
                        if (name == "Synthetic Capture Harness") {
                            portableWindowId.assign(
                                reinterpret_cast<const char*>(liveUtf8.data() + item.idOffset),
                                item.idLength);
                            break;
                        }
                    }
                }
            }
            if (portableWindowId.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        Require(!portableWindowId.empty() &&
                    telestudio::DesktopCaptureSource::ValidateCaptureItem(portableWindowId),
                "DLL catalog bytes must round-trip to the engine for a cross-process window");
        harness.Close();
        for (int retry = 0; retry != 20 &&
             telestudio::DesktopCaptureSource::ValidateCaptureItem(portableWindowId); ++retry)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        Require(!telestudio::DesktopCaptureSource::ValidateCaptureItem(portableWindowId),
                "closed cross-process window identity must become stale");
        std::atomic_bool callback{false};
        bool staleRejected = false;
        try {
            telestudio::DesktopCaptureSource stale;
            stale.StartOwned(portableWindowId, 30, 320, 180,
                             [&](telestudio::OwnedCaptureFrame) { callback = true; });
        } catch (const std::exception&) { staleRejected = true; }
        Require(staleRejected && !callback.load(),
                "stale window start must fail without monitor/window fallback or a frame callback");
        std::cout << "capture_catalog_c_abi_tests: PASS\n";
        return EXIT_SUCCESS;
#endif
    } catch (const std::exception& error) {
        std::cerr << "capture_catalog_c_abi_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
