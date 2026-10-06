#include "telestudio/media/VideoCompositor.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

struct SyntheticFrame {
    int width;
    int height;
    int strideY;
    int strideU;
    int strideV;
    std::vector<uint8_t> y;
    std::vector<uint8_t> u;
    std::vector<uint8_t> v;

    SyntheticFrame(int frameWidth, int frameHeight, uint8_t yValue, uint8_t uValue, uint8_t vValue,
                   int extraStride = 0)
        : width(frameWidth), height(frameHeight), strideY(frameWidth + extraStride),
          strideU((frameWidth + 1) / 2 + extraStride), strideV((frameWidth + 1) / 2 + extraStride),
          y(static_cast<size_t>(strideY) * height, yValue),
          u(static_cast<size_t>(strideU) * ((height + 1) / 2), uValue),
          v(static_cast<size_t>(strideV) * ((height + 1) / 2), vValue) {}

    telestudio::I420Frame View() const {
        return {y.data(), u.data(), v.data(), strideY, strideU, strideV, width, height, 11};
    }
};

bool HasValue(const std::vector<uint8_t>& values, uint8_t value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void RequireRejected(const telestudio::I420Frame* desktop, const telestudio::I420Frame* camera,
                     const telestudio::VideoCompositionSettings& settings, const char* message) {
    bool rejected = false;
    try { (void)telestudio::ComposeVideoFrame(desktop, camera, settings, 0); }
    catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, message);
}

} // namespace

int main() {
    try {
        using namespace telestudio;
        const VideoCompositionSettings canvas{8, 6};

        const auto black = ComposeVideoFrame(nullptr, nullptr, canvas, 123);
        Require(black.width == 8 && black.height == 6 && black.timestamp100ns == 123,
                "empty composition must preserve canvas dimensions and timestamp");
        Require(std::all_of(black.y.begin(), black.y.end(), [](uint8_t value) { return value == 16; }) &&
                    std::all_of(black.u.begin(), black.u.end(), [](uint8_t value) { return value == 128; }) &&
                    std::all_of(black.v.begin(), black.v.end(), [](uint8_t value) { return value == 128; }),
                "empty composition must be video-range black");

        SyntheticFrame desktop(4, 2, 40, 90, 140, 2);
        const auto desktopFrame = desktop.View();
        const auto desktopBefore = desktop.y;
        const auto desktopUBefore = desktop.u;
        const auto desktopVBefore = desktop.v;
        const auto desktopOnly = ComposeVideoFrame(&desktopFrame, nullptr, canvas, 124);
        Require(HasValue(desktopOnly.y, 40) && HasValue(desktopOnly.u, 90) && HasValue(desktopOnly.v, 140),
                "desktop-only composition must copy all source planes");
        Require(HasValue(desktopOnly.y, 16), "desktop-only composition must preserve aspect with black padding");
        Require(desktop.y == desktopBefore && desktop.u == desktopUBefore && desktop.v == desktopVBefore,
                "desktop composition must not mutate caller bytes");

        SyntheticFrame camera(2, 4, 200, 30, 220);
        const auto cameraFrame = camera.View();
        const auto cameraOnly = ComposeVideoFrame(nullptr, &cameraFrame, canvas, 125);
        Require(HasValue(cameraOnly.y, 200) && HasValue(cameraOnly.u, 30) && HasValue(cameraOnly.v, 220) &&
                    HasValue(cameraOnly.y, 16),
                "camera-only composition must preserve aspect and black-pad");

        for (const auto size : {CameraOverlaySize::Small, CameraOverlaySize::Medium, CameraOverlaySize::Large}) {
            for (const auto position : {CameraOverlayPosition::TopLeft, CameraOverlayPosition::TopRight,
                                        CameraOverlayPosition::BottomLeft, CameraOverlayPosition::BottomRight}) {
                VideoCompositionSettings settings{100, 100, size, position};
                const auto both = ComposeVideoFrame(&desktopFrame, &cameraFrame, settings, 126);
                Require(HasValue(both.y, 40) && HasValue(both.y, 200) && HasValue(both.u, 90) &&
                            HasValue(both.u, 30) && HasValue(both.v, 140) && HasValue(both.v, 220),
                        "both sources must retain desktop and overlay camera values for every size and corner");
                const int percent = size == CameraOverlaySize::Small ? 20 :
                                    size == CameraOverlaySize::Medium ? 28 : 36;
                const int box = (100 * percent / 100) & ~1;
                const int fittedWidth = box;
                const int cameraX = (position == CameraOverlayPosition::TopLeft ||
                                     position == CameraOverlayPosition::BottomLeft)
                                        ? 2 : 100 - 2 - fittedWidth;
                const int cameraY = (position == CameraOverlayPosition::TopLeft ||
                                     position == CameraOverlayPosition::TopRight) ? 2 : 100 - 2 - box * 2;
                Require(both.y[static_cast<size_t>(cameraY) * 100 + cameraX] == 200,
                        "camera overlay must be placed in the requested corner with a two-percent margin");
            }
        }

        SyntheticFrame wideDesktop(160, 90, 40, 90, 140);
        SyntheticFrame standardCamera(40, 30, 200, 30, 220);
        const auto wideView = wideDesktop.View();
        const auto standardView = standardCamera.View();
        struct OverlayCase { CameraOverlaySize size; int width; int height; };
        for (const auto expected : {OverlayCase{CameraOverlaySize::Small, 320, 240},
                                  OverlayCase{CameraOverlaySize::Medium, 448, 336},
                                  OverlayCase{CameraOverlaySize::Large, 576, 432}}) {
            const auto result = ComposeVideoFrame(&wideView, &standardView,
                {1600,900,expected.size,CameraOverlayPosition::TopLeft}, 1);
            Require(result.y[18 * 1600 + 32] == 200 &&
                    result.y[(18 + expected.height - 1) * 1600 + 32 + expected.width - 1] == 200 &&
                    result.y[18 * 1600 + 32 + expected.width] == 40 &&
                    result.y[(18 + expected.height) * 1600 + 32] == 40,
                    "4:3 camera must occupy literal 20/28/36 percent width with its own aspect");
        }
        SyntheticFrame portraitCamera(10, 100, 200, 30, 220);
        const auto portraitView = portraitCamera.View();
        const auto portrait = ComposeVideoFrame(&wideView, &portraitView,
            {1600,900,CameraOverlaySize::Large,CameraOverlayPosition::BottomRight}, 1);
        Require(portrait.y[18 * 1600 + 1482] == 200 &&
                portrait.y[881 * 1600 + 1567] == 200 &&
                portrait.y[18 * 1600 + 1481] == 40,
                "portrait overlay clamps to available 864px height and 86px width with margins");

        const auto owned = ComposeVideoFrame(&desktopFrame, &cameraFrame, canvas, 127);
        desktop.y.assign(desktop.y.size(), 1);
        camera.y.assign(camera.y.size(), 2);
        Require(HasValue(owned.y, 40) && HasValue(owned.y, 200),
                "composition output must own bytes independently of caller frames");
        const auto ownedView = owned.View();
        Require(ownedView.y == owned.y.data() && ownedView.strideY == owned.width &&
                    ownedView.strideU == owned.width / 2 && ownedView.timestamp100ns == 127,
                "owned composition view must expose tightly packed I420 planes");

        SyntheticFrame oddSource(3, 3, 71, 81, 91);
        const auto oddFrame = oddSource.View();
        const auto odd = ComposeVideoFrame(&oddFrame, nullptr, canvas, 128);
        Require(HasValue(odd.y, 71) && HasValue(odd.u, 81) && HasValue(odd.v, 91),
                "odd source dimensions must use ceil chroma dimensions safely");

        RequireRejected(nullptr, nullptr, {0, 6}, "zero canvas width must be rejected");
        RequireRejected(nullptr, nullptr, {1, 2}, "canvas width below two must be rejected");
        RequireRejected(nullptr, nullptr, {3, 6}, "odd canvas width must be rejected");
        RequireRejected(nullptr, nullptr, {8, 5}, "odd canvas height must be rejected");
        RequireRejected(nullptr, nullptr, {4098, 6}, "canvas width above limit must be rejected");
        RequireRejected(nullptr, nullptr, {8, 2162}, "canvas height above limit must be rejected");
        RequireRejected(nullptr, nullptr, {8, 6, static_cast<CameraOverlaySize>(99)},
                        "invalid overlay size must be rejected");
        RequireRejected(nullptr, nullptr, {8, 6, CameraOverlaySize::Small,
                                           static_cast<CameraOverlayPosition>(99)},
                        "invalid overlay position must be rejected");
        auto invalid = desktop.View();
        invalid.y = nullptr;
        RequireRejected(&invalid, nullptr, canvas, "null source plane must be rejected");
        invalid = desktop.View();
        invalid.strideU = 1;
        RequireRejected(&invalid, nullptr, canvas, "short chroma stride must be rejected before reading");
        invalid = desktop.View();
        invalid.strideY = 1;
        RequireRejected(&invalid, nullptr, canvas, "short luma stride must be rejected before reading");
        invalid = desktop.View();
        invalid.v = nullptr;
        RequireRejected(&invalid, nullptr, canvas, "null chroma plane must be rejected");
        invalid = desktop.View();
        invalid.strideV = -1;
        RequireRejected(&invalid, nullptr, canvas, "negative chroma stride must be rejected before reading");

        std::cout << "media_video_compositor_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "media_video_compositor_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
