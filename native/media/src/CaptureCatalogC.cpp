#include "telestudio/media/CaptureCatalogC.h"
#include "telestudio/media/DesktopCaptureSource.h"
#include "telestudio/media/CameraCaptureSource.h"

#include <Windows.h>
#include <roapi.h>
#include <bcrypt.h>
#include <windows.graphics.capture.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
struct CatalogSourceEntry {
    telestudio::CaptureItemInfo item;
    bool primary = false;
};

#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
struct SyntheticCatalogState {
    std::vector<CatalogSourceEntry> entries;
    bool configured = false;
    bool supported = true;
    bool raiseSeh = false;
};
thread_local SyntheticCatalogState g_syntheticCatalog;
#endif

struct CatalogBlob {
    std::vector<EssaCaptureCatalogItemV1> items;
    std::vector<std::uint8_t> utf8;
    std::uint64_t snapshot = 0;
};

bool StrictUtf8(std::string_view value, std::size_t* scalarCount = nullptr) noexcept {
    std::size_t count = 0;
    for (std::size_t index = 0; index < value.size();) {
        const auto first = static_cast<unsigned char>(value[index]);
        std::size_t length = 0;
        std::uint32_t scalar = 0;
        if (first < 0x80) { length = 1; scalar = first; }
        else if ((first & 0xe0) == 0xc0) { length = 2; scalar = first & 0x1f; }
        else if ((first & 0xf0) == 0xe0) { length = 3; scalar = first & 0x0f; }
        else if ((first & 0xf8) == 0xf0) { length = 4; scalar = first & 7; }
        else return false;
        if (index + length > value.size()) return false;
        for (std::size_t offset = 1; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(value[index + offset]);
            if ((continuation & 0xc0) != 0x80) return false;
            scalar = (scalar << 6) | (continuation & 0x3f);
        }
        if ((length == 2 && scalar < 0x80) || (length == 3 && scalar < 0x800) ||
            (length == 4 && scalar < 0x10000) || scalar > 0x10ffff ||
            (scalar >= 0xd800 && scalar <= 0xdfff) || scalar == 0) return false;
        ++count;
        index += length;
    }
    if (scalarCount) *scalarCount = count;
    return true;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                          static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) throw std::length_error("invalid catalog UTF-8");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), count) != count)
        throw std::length_error("invalid catalog UTF-8");
    return result;
}

std::string NormalizeName(std::string name) {
    std::size_t scalarCount = 0;
    if (!StrictUtf8(name, &scalarCount)) throw std::length_error("catalog item limit");
    if (scalarCount <= 512 && name.size() <= 2048) return name;
    std::size_t index = 0;
    std::size_t scalars = 0;
    while (index < name.size() && scalars < 511 && index < 2045) {
        const unsigned char first = static_cast<unsigned char>(name[index]);
        const std::size_t length = first < 0x80 ? 1 : (first & 0xe0) == 0xc0 ? 2 :
                                   (first & 0xf0) == 0xe0 ? 3 : 4;
        if (index + length + 3 > 2048) break;
        index += length;
        ++scalars;
    }
    name.resize(index);
    name += "\xE2\x80\xA6";
    return name;
}

std::vector<CatalogSourceEntry> LoadCatalogSource() {
#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
    if (g_syntheticCatalog.configured) return g_syntheticCatalog.entries;
#endif
    const auto native = telestudio::DesktopCaptureSource::EnumerateCaptureItems();
    std::vector<CatalogSourceEntry> result;
    result.reserve(native.size());
    bool firstMonitor = true;
    for (const auto& item : native) {
        const bool primary = item.kind == telestudio::CaptureItemKind::Monitor && firstMonitor;
        if (item.kind == telestudio::CaptureItemKind::Monitor) firstMonitor = false;
        result.push_back({item, primary});
    }
    return result;
}

CatalogBlob BuildCatalog(bool cameras = false) {
    std::vector<CatalogSourceEntry> source;
    if (!cameras) source = LoadCatalogSource();
    else {
#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
        source = g_syntheticCatalog.entries;
#else
        for (const auto& camera : telestudio::CameraCaptureSource::Enumerate())
            source.push_back({{camera.id, telestudio::CaptureItemKind::Monitor, camera.name, 1, 1}, false});
#endif
    }
    if (source.size() > 1024) throw std::length_error("catalog limit");
    for (auto& entry : source) entry.item.name = NormalizeName(std::move(entry.item.name));
    std::sort(source.begin(), source.end(), [](const auto& left, const auto& right) {
        if (left.item.kind != right.item.kind)
            return left.item.kind == telestudio::CaptureItemKind::Monitor;
        if (left.item.kind == telestudio::CaptureItemKind::Monitor) {
            if (left.primary != right.primary) return left.primary;
            return left.item.id < right.item.id;
        }
        const auto leftName = Utf8ToWide(left.item.name);
        const auto rightName = Utf8ToWide(right.item.name);
        const int comparison = CompareStringOrdinal(leftName.data(), static_cast<int>(leftName.size()),
                                                     rightName.data(), static_cast<int>(rightName.size()), TRUE);
        if (comparison == 0) throw std::runtime_error("catalog ordering failed");
        return comparison == CSTR_EQUAL ? left.item.id < right.item.id : comparison == CSTR_LESS_THAN;
    });
    CatalogBlob result;
    result.items.reserve(source.size());
    std::vector<std::string> ids;
    ids.reserve(source.size());
    for (const auto& entry : source) {
        const auto& item = entry.item;
        std::string name = item.name;
        std::size_t nameScalars = 0;
        if (!StrictUtf8(name, &nameScalars)) throw std::length_error("catalog item limit");
        if (item.id.empty() || item.id.size() > (cameras ? 2048u : 1024u) || !StrictUtf8(item.id) ||
            name.empty() || name.size() > 2048 || nameScalars > 512 || item.width <= 0 || item.height <= 0 || item.width > 16384 ||
            item.height > 16384 || std::find(ids.begin(), ids.end(), item.id) != ids.end()) {
            throw std::length_error("catalog item limit");
        }
        if (result.utf8.size() + item.id.size() + name.size() > 4u * 1024u * 1024u) {
            throw std::length_error("catalog blob limit");
        }
        const auto idOffset = static_cast<std::uint32_t>(result.utf8.size());
        result.utf8.insert(result.utf8.end(), item.id.begin(), item.id.end());
        const auto nameOffset = static_cast<std::uint32_t>(result.utf8.size());
        result.utf8.insert(result.utf8.end(), name.begin(), name.end());
        result.items.push_back({32,
            cameras ? 3u : static_cast<std::uint32_t>(item.kind == telestudio::CaptureItemKind::Monitor
                ? ESSA_CAPTURE_MONITOR : ESSA_CAPTURE_WINDOW),
            cameras ? 0u : static_cast<std::uint32_t>(item.width), cameras ? 0u : static_cast<std::uint32_t>(item.height),
            idOffset, static_cast<std::uint32_t>(item.id.size()), nameOffset,
            static_cast<std::uint32_t>(name.size())});
        ids.push_back(item.id);
    }
    std::vector<std::uint8_t> fingerprintInput;
    if (!result.items.empty()) {
        const auto* records = reinterpret_cast<const std::uint8_t*>(result.items.data());
        fingerprintInput.insert(fingerprintInput.end(), records,
            records + result.items.size() * sizeof(EssaCaptureCatalogItemV1));
    }
    fingerprintInput.insert(fingerprintInput.end(), result.utf8.begin(), result.utf8.end());
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("snapshot fingerprint unavailable");
    }
    DWORD objectLength = 0;
    DWORD copied = 0;
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength),
                          &copied, 0) < 0 || copied != sizeof(objectLength) || objectLength == 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("snapshot fingerprint unavailable");
    }
    std::vector<std::uint8_t> hashObject(objectLength);
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength,
                         nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw std::runtime_error("snapshot fingerprint unavailable");
    }
    const auto inputLength = static_cast<ULONG>(fingerprintInput.size());
    const NTSTATUS hashStatus = fingerprintInput.empty()
        ? 0
        : BCryptHashData(hash, fingerprintInput.data(), inputLength, 0);
    std::array<std::uint8_t, 32> digest{};
    const NTSTATUS finishStatus = hashStatus < 0
        ? hashStatus
        : BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (finishStatus < 0) throw std::runtime_error("snapshot fingerprint failed");
    std::uint64_t token = 0;
    std::memcpy(&token, digest.data(), sizeof(token));
    result.snapshot = token == 0 ? 1 : token;
    return result;
}

bool IsCaptureSupported() {
#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
    if (g_syntheticCatalog.configured) return g_syntheticCatalog.supported;
#endif
    HSTRING_HEADER header{};
    HSTRING className = nullptr;
    constexpr wchar_t runtimeClass[] =
        L"Windows.Graphics.Capture.GraphicsCaptureSession";
    winrt::check_hresult(WindowsCreateStringReference(
        runtimeClass, static_cast<UINT32>(std::size(runtimeClass) - 1), &header, &className));
    winrt::com_ptr<ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics> statics;
    winrt::check_hresult(RoGetActivationFactory(
        className,
        __uuidof(ABI::Windows::Graphics::Capture::IGraphicsCaptureSessionStatics),
        statics.put_void()));
    boolean supported = FALSE;
    winrt::check_hresult(statics->IsSupported(&supported));
    return supported != FALSE;
}
}

#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
namespace telestudio::testing {
struct SyntheticCatalogEntry {
    CaptureItemInfo item;
    bool primary = false;
};

void ConfigureCaptureCatalog(std::vector<SyntheticCatalogEntry> entries,
                             bool supported, bool raiseSeh) {
    g_syntheticCatalog.entries.clear();
    g_syntheticCatalog.entries.reserve(entries.size());
    for (auto& entry : entries)
        g_syntheticCatalog.entries.push_back({std::move(entry.item), entry.primary});
    g_syntheticCatalog.supported = supported;
    g_syntheticCatalog.raiseSeh = raiseSeh;
    g_syntheticCatalog.configured = true;
}
}
#endif

extern "C" std::uint32_t __cdecl EssaCaptureCatalogAbiVersion() { return 1; }

namespace {
std::int32_t CaptureCatalogQueryImpl(
    std::uint32_t abiVersion, std::uint64_t expectedSnapshot,
    EssaCaptureCatalogItemV1* items, std::uint32_t itemCapacity,
    std::uint8_t* utf8, std::uint32_t utf8Capacity,
    std::uint32_t* outRequiredItems, std::uint32_t* outRequiredUtf8,
    std::uint64_t* outSnapshot, bool cameras = false) {
    if (!outRequiredItems || !outRequiredUtf8 || !outSnapshot ||
        ((items == nullptr) != (itemCapacity == 0)) || ((utf8 == nullptr) != (utf8Capacity == 0))) {
        return ESSA_CAPTURE_INVALID_ARGUMENT;
    }
    *outRequiredItems = 0;
    *outRequiredUtf8 = 0;
    *outSnapshot = 0;
    if (abiVersion != 1) return ESSA_CAPTURE_ABI_MISMATCH;
#ifdef ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS
    if (g_syntheticCatalog.raiseSeh) RaiseException(EXCEPTION_ACCESS_VIOLATION, 0, 0, nullptr);
#endif
    try {
        const HRESULT apartment = RoInitialize(RO_INIT_MULTITHREADED);
        if (apartment == RPC_E_CHANGED_MODE) return ESSA_CAPTURE_WRONG_APARTMENT;
        if (FAILED(apartment)) return ESSA_CAPTURE_ENUMERATION_FAILED;
        struct RoGuard { ~RoGuard() { RoUninitialize(); } } guard;
        if (!cameras && !IsCaptureSupported()) {
            return ESSA_CAPTURE_NOT_SUPPORTED;
        }
        const auto catalog = BuildCatalog(cameras);
        *outRequiredItems = static_cast<std::uint32_t>(catalog.items.size());
        *outRequiredUtf8 = static_cast<std::uint32_t>(catalog.utf8.size());
        *outSnapshot = catalog.snapshot;
        const bool sizing = items == nullptr && utf8 == nullptr;
        if (expectedSnapshot != 0 && expectedSnapshot != catalog.snapshot) {
            return ESSA_CAPTURE_CATALOG_CHANGED;
        }
        if (sizing || itemCapacity < catalog.items.size() || utf8Capacity < catalog.utf8.size()) {
            return catalog.items.empty() && catalog.utf8.empty() ? ESSA_CAPTURE_OK
                                                                 : ESSA_CAPTURE_BUFFER_TOO_SMALL;
        }
        if (!catalog.items.empty()) std::memcpy(items, catalog.items.data(), catalog.items.size() * sizeof(*items));
        if (!catalog.utf8.empty()) std::memcpy(utf8, catalog.utf8.data(), catalog.utf8.size());
        return ESSA_CAPTURE_OK;
    } catch (const winrt::hresult_wrong_thread&) {
        return ESSA_CAPTURE_WRONG_APARTMENT;
    } catch (const std::length_error&) {
        return ESSA_CAPTURE_LIMIT_EXCEEDED;
    } catch (const winrt::hresult_error&) {
        return ESSA_CAPTURE_ENUMERATION_FAILED;
    } catch (...) {
        return ESSA_CAPTURE_INTERNAL_FAILURE;
    }
}
}

extern "C" std::int32_t __cdecl EssaCaptureCatalogQueryV1(
    std::uint32_t abiVersion, std::uint64_t expectedSnapshot,
    EssaCaptureCatalogItemV1* items, std::uint32_t itemCapacity,
    std::uint8_t* utf8, std::uint32_t utf8Capacity,
    std::uint32_t* outRequiredItems, std::uint32_t* outRequiredUtf8,
    std::uint64_t* outSnapshot) {
    __try {
        return CaptureCatalogQueryImpl(abiVersion, expectedSnapshot, items, itemCapacity,
                                       utf8, utf8Capacity, outRequiredItems, outRequiredUtf8,
                                       outSnapshot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ESSA_CAPTURE_INTERNAL_FAILURE;
    }
}

extern "C" std::int32_t __cdecl EssaCameraCatalogQueryV1(
    std::uint32_t abiVersion, std::uint64_t expectedSnapshot,
    EssaCaptureCatalogItemV1* items, std::uint32_t itemCapacity,
    std::uint8_t* utf8, std::uint32_t utf8Capacity,
    std::uint32_t* outRequiredItems, std::uint32_t* outRequiredUtf8,
    std::uint64_t* outSnapshot) {
    __try {
        return CaptureCatalogQueryImpl(abiVersion, expectedSnapshot, items, itemCapacity,
            utf8, utf8Capacity, outRequiredItems, outRequiredUtf8, outSnapshot, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return ESSA_CAPTURE_INTERNAL_FAILURE; }
}
