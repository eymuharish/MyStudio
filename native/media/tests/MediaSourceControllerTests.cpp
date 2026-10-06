#include "telestudio/media/FrameConverter.h"
#include "telestudio/media/MediaSourceController.h"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void Require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

telestudio::I420Buffer Frame(int64_t timestamp) {
    const std::array<uint8_t, 16> bgra = {
        0, 0, 0, 255, 0, 0, 0, 255,
        0, 0, 0, 255, 0, 0, 0, 255,
    };
    return telestudio::ConvertBgraToI420(bgra, 2, 2, 8, timestamp);
}

class FakeDesktopSource final : public telestudio::IDesktopCaptureSource {
public:
    void Start(std::string_view, int, telestudio::FrameCallback callback,
               telestudio::CaptureErrorCallback errorCallback) override {
        callback_ = std::move(callback);
        errorCallback_ = std::move(errorCallback);
        started = true;
    }

    void Stop() override {
        stopped = true;
    }

    void Emit(const telestudio::I420Buffer& buffer) {
        if (callback_) {
            callback_(buffer.View());
        }
    }

    void Fail(const std::string& message) {
        if (errorCallback_) {
            errorCallback_(message);
        }
    }

    bool started = false;
    bool stopped = false;

private:
    telestudio::FrameCallback callback_;
    telestudio::CaptureErrorCallback errorCallback_;
};

struct FakeFactory {
    std::vector<std::shared_ptr<FakeDesktopSource>> sources;

    std::shared_ptr<telestudio::IDesktopCaptureSource> Create() {
        auto source = std::make_shared<FakeDesktopSource>();
        sources.push_back(source);
        return source;
    }
};

} // namespace

int main() {
    try {
        using namespace telestudio;

        {
            FakeFactory factory;
            std::vector<int64_t> delivered;
            MediaSourceController controller([&factory] { return factory.Create(); });
            controller.SetCameraSource([&](I420Frame frame) { delivered.push_back(frame.timestamp100ns); });
            auto camera1 = Frame(1);
            controller.DeliverCameraFrame(camera1.View());
            controller.SetDesktopSource("display-1");
            Require(factory.sources.size() == 1 && factory.sources[0]->started,
                    "Desktop request must start one unpublished candidate");
            auto camera2 = Frame(2);
            controller.DeliverCameraFrame(camera2.View());
            Require(controller.ActiveSource() == VideoSource::Camera && delivered == std::vector<int64_t>({1, 2}),
                    "Camera frames must continue until the desktop candidate is ready");

            auto desktop3 = Frame(3);
            factory.sources[0]->Emit(desktop3);
            Require(controller.ActiveSource() == VideoSource::Desktop &&
                        delivered == std::vector<int64_t>({1, 2, 3}),
                    "The first valid desktop frame must commit and cross the route boundary exactly once");
            auto camera4 = Frame(4);
            auto desktop5 = Frame(5);
            controller.DeliverCameraFrame(camera4.View());
            factory.sources[0]->Emit(desktop5);
            Require(delivered == std::vector<int64_t>({1, 2, 3, 5}),
                    "Camera frames must not pass after desktop commits");
        }

        {
            FakeFactory factory;
            std::vector<int64_t> delivered;
            MediaSourceController controller([&factory] { return factory.Create(); });
            controller.SetCameraSource([&](I420Frame frame) { delivered.push_back(frame.timestamp100ns); });
            controller.SetDesktopSource("stale");
            auto stale = factory.sources.front();
            controller.SetCameraSource([&](I420Frame frame) { delivered.push_back(frame.timestamp100ns); });
            Require(stale->stopped, "A camera request must stop the unpublished desktop candidate");
            auto staleFrame = Frame(10);
            stale->Emit(staleFrame);
            Require(controller.ActiveSource() == VideoSource::Camera && delivered.empty(),
                    "A stale candidate frame must be discarded after a camera request");

            controller.SetDesktopSource("failed");
            factory.sources.back()->Fail("display disappeared");
            auto camera = Frame(11);
            controller.DeliverCameraFrame(camera.View());
            Require(controller.ActiveSource() == VideoSource::Camera && delivered == std::vector<int64_t>({11}),
                    "A failed desktop candidate must leave the camera route intact");
        }

        {
            FakeFactory factory;
            std::vector<int64_t> delivered;
            MediaSourceController controller([&factory] { return factory.Create(); });
            controller.SetCameraSource([&](I420Frame frame) { delivered.push_back(frame.timestamp100ns); });
            controller.SetDesktopSource("desktop-a");
            const auto desktopA = factory.sources.at(0);
            auto a30 = Frame(30);
            desktopA->Emit(a30);

            controller.SetDesktopSource("desktop-b");
            const auto desktopB = factory.sources.at(1);
            auto a31 = Frame(31);
            desktopA->Emit(a31);
            Require(delivered == std::vector<int64_t>({30, 31}),
                    "Committed desktop A must keep delivering while desktop B is not ready");

            auto b32 = Frame(32);
            desktopB->Emit(b32);
            Require(desktopA->stopped && controller.ActiveSource() == VideoSource::Desktop,
                    "Desktop B first frame must commit before desktop A stops");
            auto a33 = Frame(33);
            auto b34 = Frame(34);
            desktopA->Emit(a33);
            desktopB->Emit(b34);
            Require(delivered == std::vector<int64_t>({30, 31, 32, 34}),
                    "Desktop A must stop routing only after desktop B commits");
        }

        {
            FakeFactory factory;
            std::vector<int64_t> delivered;
            MediaSourceController controller([&factory] { return factory.Create(); });
            controller.SetCameraSource([&](I420Frame frame) { delivered.push_back(frame.timestamp100ns); });
            controller.SetDesktopSource("desktop-a");
            const auto desktopA = factory.sources.at(0);
            auto a40 = Frame(40);
            desktopA->Emit(a40);

            controller.SetDesktopSource("desktop-b-fails");
            const auto desktopB = factory.sources.at(1);
            auto a41 = Frame(41);
            desktopA->Emit(a41);
            desktopB->Fail("desktop B disappeared");
            auto a42 = Frame(42);
            auto a43 = Frame(43);
            desktopA->Emit(a42);
            desktopA->Emit(a43);
            Require(controller.ActiveSource() == VideoSource::Desktop &&
                        delivered == std::vector<int64_t>({40, 41, 42, 43}),
                    "A failed desktop B candidate must leave committed desktop A fully operational");
        }

        {
            using namespace std::chrono_literals;
            FakeFactory factory;
            std::mutex gateMutex;
            std::condition_variable gate;
            bool callbackEntered = false;
            bool releaseCallback = false;
            std::atomic<bool> switchReturned = false;
            std::vector<int64_t> delivered;
            MediaSourceController controller([&factory] { return factory.Create(); });
            controller.SetCameraSource([&](I420Frame frame) {
                std::unique_lock lock(gateMutex);
                delivered.push_back(frame.timestamp100ns);
                callbackEntered = true;
                gate.notify_all();
                gate.wait(lock, [&] { return releaseCallback; });
            });

            auto camera = Frame(20);
            std::thread producer([&] { controller.DeliverCameraFrame(camera.View()); });
            {
                std::unique_lock lock(gateMutex);
                Require(gate.wait_for(lock, 2s, [&] { return callbackEntered; }),
                        "Blocking callback did not start");
            }
            std::thread switcher([&] {
                controller.SetDesktopSource("serialized");
                switchReturned = true;
            });
            std::this_thread::sleep_for(50ms);
            Require(!switchReturned.load(), "A source switch must not overlap an in-progress callback");
            {
                std::lock_guard lock(gateMutex);
                releaseCallback = true;
            }
            gate.notify_all();
            producer.join();
            switcher.join();
            Require(switchReturned.load() && factory.sources.size() == 1,
                    "The serialized switch must proceed after the callback returns");
            auto desktop = Frame(21);
            factory.sources.front()->Emit(desktop);
            Require(delivered == std::vector<int64_t>({20, 21}),
                    "A blocked callback and switch must retain deterministic frame order");
        }

        std::cout << "media_source_controller_tests: PASS\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "media_source_controller_tests: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
