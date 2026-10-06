#include <windows.h>

#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace telestudio::detail {

struct LoadedLibrary {
    HMODULE module;
    std::filesystem::path path;
};

using LibraryHandle = LoadedLibrary*;

LibraryHandle OpenExplicitLibrary(const std::filesystem::path& supplied,
                                  const std::vector<std::wstring>& directoryNames) {
    std::vector<std::filesystem::path> candidates;
    if (std::filesystem::is_directory(supplied)) {
        for (const auto& name : directoryNames) {
            candidates.push_back(std::filesystem::absolute(supplied / name));
        }
    } else {
        candidates.push_back(std::filesystem::absolute(supplied));
    }

    std::ostringstream attempted;
    for (const auto& candidate : candidates) {
        if (!attempted.str().empty()) {
            attempted << "; ";
        }
        attempted << candidate.string();
        const HMODULE module = LoadLibraryExW(candidate.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module != nullptr) {
            return new LoadedLibrary { module, candidate };
        }
    }
    throw std::runtime_error("Could not load codec DLL; attempted: " + attempted.str());
}

void CloseLibrary(LibraryHandle library) noexcept {
    if (library != nullptr) {
        FreeLibrary(library->module);
        delete library;
    }
}

void* RequireExport(LibraryHandle library, const char* name) {
    const FARPROC symbol = GetProcAddress(library->module, name);
    if (symbol == nullptr) {
        throw std::runtime_error("Codec DLL " + library->path.string() +
                                 " is missing required export: " + name);
    }
    return reinterpret_cast<void*>(symbol);
}

} // namespace telestudio::detail
