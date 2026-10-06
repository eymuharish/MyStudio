#include <windows.h>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace telestudio::detail {
struct LoadedLibrary;
using LibraryHandle = LoadedLibrary*;
LibraryHandle OpenExplicitLibrary(const std::filesystem::path&, const std::vector<std::wstring>&);
void CloseLibrary(LibraryHandle) noexcept;
void* RequireExport(LibraryHandle, const char*);
}
namespace fs = std::filesystem;
void Require(bool value, const char* detail) { if (!value) throw std::runtime_error(detail); }
struct FixtureFolder {
    fs::path previous = fs::current_path(), temporary = fs::absolute(fs::temp_directory_path());
    fs::path root = temporary / ("MyStudio-loader-security-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    FixtureFolder() { Require(fs::create_directory(root), "Fixture folder already exists"); }
    ~FixtureFolder() {
        std::error_code ignored; fs::current_path(previous, ignored);
        if (root != temporary && root.parent_path() == temporary && root.filename().string().starts_with("MyStudio-loader-security-")) fs::remove_all(root, ignored);
    }
};
int wmain(int argc, wchar_t** argv) {
    using namespace telestudio::detail;
    try {
        Require(argc == 3, "Expected consumer and dependency fixture paths");
        const auto consumerSource = fs::absolute(argv[1]), dependencySource = fs::absolute(argv[2]);
        FixtureFolder folder;
        const auto codecs = folder.root / "codecs", unrelated = folder.root / "unrelated-working-directory";
        fs::create_directory(codecs); fs::create_directory(unrelated);
        const auto consumer = codecs / consumerSource.filename();
        fs::copy_file(consumerSource, consumer); fs::copy_file(dependencySource, unrelated / dependencySource.filename());
        fs::current_path(unrelated);
        bool rejected = false;
        try { auto library = OpenExplicitLibrary(consumer, {}); CloseLibrary(library); }
        catch (const std::runtime_error&) { rejected = true; }
        Require(rejected, "Codec dependency was loaded from the unrelated current working directory");
        fs::copy_file(dependencySource, codecs / dependencySource.filename());
        auto library = OpenExplicitLibrary(consumer, {});
        int result = 0;
        try { result = reinterpret_cast<int(*)()>(RequireExport(library, "MyStudioSecurityProbe"))(); }
        catch (...) { CloseLibrary(library); throw; }
        CloseLibrary(library);
        Require(result == 42, "A legitimate dependency beside the codec did not load");
        std::cout << "Explicit codec loading rejects CWD dependencies and accepts sibling dependencies PASS\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
