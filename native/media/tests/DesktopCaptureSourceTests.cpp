#include "telestudio/media/DesktopCaptureSource.h"

#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unordered_set>
#include <condition_variable>
#include <mutex>
#include <chrono>

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
}

int main() {
    try {
        const auto legacy = telestudio::DesktopCaptureSource::Enumerate();
        const auto catalog = telestudio::DesktopCaptureSource::EnumerateCaptureItems();
        Require(!legacy.empty(), "a real monitor is required");
        Require(!catalog.empty(), "capture catalog is required");
        Require(catalog.front().kind == telestudio::CaptureItemKind::Monitor,
                "the primary monitor must sort first");
        std::unordered_set<std::string> ids;
        for (const auto& item : catalog) {
            Require(!item.id.empty() && !item.name.empty(), "catalog strings must be nonempty");
            Require(item.width > 0 && item.height > 0, "catalog dimensions must be positive");
            Require(ids.insert(item.id).second, "catalog IDs must be unique");
        }
        for (const auto& display : legacy) {
            bool found = false;
            for (const auto& item : catalog) {
                found |= item.kind == telestudio::CaptureItemKind::Monitor && item.id == display.id;
            }
            Require(found, "legacy monitor IDs must remain byte-for-byte stable");
        }
        Require(!telestudio::DesktopCaptureSource::ValidateCaptureItem(""),
                "empty capture ID must fail closed");
        Require(!telestudio::DesktopCaptureSource::ValidateCaptureItem("esw1.invalid"),
                "malformed window identity must fail closed");
        std::mutex mutex;
        std::condition_variable changed;
        telestudio::OwnedCaptureFrame retained;
        telestudio::DesktopCaptureSource source;
        source.StartOwned(catalog.front().id, 30, 320, 180,
            [&](telestudio::OwnedCaptureFrame frame) {
                std::lock_guard lock(mutex);
                if (!retained.pixels) retained = std::move(frame);
                changed.notify_all();
            });
        {
            std::unique_lock lock(mutex);
            Require(changed.wait_for(lock, std::chrono::seconds(3), [&] { return retained.pixels != nullptr; }),
                    "real WGC must deliver one owned frame");
        }
        source.Stop();
        Require(retained.pixels->Width() == 320 && retained.pixels->Height() == 180 &&
                    !retained.pixels->Y().empty(),
                "owned pixels must survive callback return, unmap, and source Stop");
        std::cout << "media_desktop_capture_source_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "media_desktop_capture_source_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
