cmake_minimum_required(VERSION 3.25)
project(OpalMediaProbe LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
set(media "${CMAKE_CURRENT_SOURCE_DIR}/../../native/media")
# The probe observes only the named test process. The production capture class
# excludes its process tree for screen sharing; compile a private included-tree
# copy here without modifying production source or capturing unrelated audio.
file(READ "${media}/src/SystemAudioCapture.cpp" capture)
string(REPLACE "ProcessLoopbackMode::ExcludeTargetProcessTree" "ProcessLoopbackMode::IncludeTargetProcessTree" capture "${capture}")
string(REPLACE "EXCLUDE_TARGET_PROCESS_TREE" "INCLUDE_TARGET_PROCESS_TREE" capture "${capture}")
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/IncludedProcessAudio.cpp" "${capture}")
add_executable(opal_media_probe MediaProbe.cpp "${CMAKE_CURRENT_BINARY_DIR}/IncludedProcessAudio.cpp")
target_include_directories(opal_media_probe PRIVATE "${media}/include")
target_compile_definitions(opal_media_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN NTDDI_VERSION=NTDDI_WIN10_FE)
target_link_libraries(opal_media_probe PRIVATE ole32 mmdevapi runtimeobject windowsapp mfplat mfreadwrite mfuuid)
add_executable(opal_debug_probe CrashProbe.cpp)
target_compile_definitions(opal_debug_probe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(opal_debug_probe PRIVATE dbghelp)
add_executable(opal_share_fixture ShareFixture.cpp)
target_compile_definitions(opal_share_fixture PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(opal_share_fixture PRIVATE user32 gdi32)
set(edition "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
add_executable(opal_bfcp_lifecycle_probe BfcpLifecycle.cpp)
target_compile_options(opal_bfcp_lifecycle_probe PRIVATE /Zi /FS)
target_link_options(opal_bfcp_lifecycle_probe PRIVATE /DEBUG)
# BFCP internals are private DLL symbols. Compile the pinned implementation
# directly for an isolated lifetime test with no endpoint calls.
file(GLOB_RECURSE bfcpSources "${edition}/dependencies/opal/src/sdp/BFCP/libbfcp/*.cpp" "${edition}/dependencies/opal/src/sdp/BFCP/libbfcp/*.c")
list(FILTER bfcpSources EXCLUDE REGEX "/samples/|/stdafx.cpp$|/BFCPapi.cpp$")
target_sources(opal_bfcp_lifecycle_probe PRIVATE ${bfcpSources})
set(bfcpCpp ${bfcpSources})
list(FILTER bfcpCpp INCLUDE REGEX "\\.cpp$")
set_source_files_properties(${bfcpCpp} PROPERTIES COMPILE_OPTIONS /FIptlib.h)
target_compile_definitions(opal_bfcp_lifecycle_probe PRIVATE P_DLL NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(opal_bfcp_lifecycle_probe PRIVATE
 "${edition}/dependencies/opal/include" "${edition}/dependencies/opal/lib/x64/include"
 "${edition}/dependencies/opal/src/sdp/BFCP/libbfcp"
 "${edition}/dependencies/ptlib/include" "${edition}/dependencies/ptlib/lib/x64/include")
target_link_libraries(opal_bfcp_lifecycle_probe PRIVATE
 "${edition}/dependencies/opal/lib/opal64.lib" "${edition}/dependencies/ptlib/lib/ptlib64.lib" ws2_32 winmm)
