#include "telestudio/media/RemoteVideoSurface.h"

#include <algorithm>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void Require(bool condition, const std::string& message) { if (!condition) throw std::runtime_error(message); }

telestudio::HostRegistration Registration() {
    telestudio::HostRegistration registration;
    registration.desktopPid = 4242;
    for (size_t i = 0; i < registration.sessionToken.size(); ++i) registration.sessionToken[i] = static_cast<std::byte>(i);
    return registration;
}

telestudio::DecodedBgraFrame Frame(int width, int height, uint8_t value = 1) {
    return {width, height, width * 4, 1, std::vector<uint8_t>(static_cast<size_t>(width) * height * 4, value)};
}

struct PlatformState {
    enum class UploadMode { Success, Timeout, ErrorAfterAcquire };
    bool alive = true;
    uintptr_t processHandle = 0x424200;
    uint32_t openedPid = 0;
    int liveSlots = 0;
    int destroyedSlots = 0;
    int nativeLeases = 0;
    uint64_t nextHostHandle = 1000;
    std::vector<uintptr_t> duplicateTargets;
    std::vector<std::string> mutexEvents;
    std::deque<UploadMode> uploadModes;
};

class FakeSlot final : public telestudio::detail::IRemoteSurfaceSlot {
public:
    FakeSlot(std::shared_ptr<PlatformState> state, uint32_t width, uint32_t height)
        : state_(std::move(state)), width_(width), height_(height) { ++state_->liveSlots; }
    ~FakeSlot() override { --state_->liveSlots; ++state_->destroyedSlots; }
    uint32_t Width() const noexcept override { return width_; }
    uint32_t Height() const noexcept override { return height_; }
    bool Upload(const telestudio::DecodedBgraFrame&) override {
        auto mode = PlatformState::UploadMode::Success;
        if (!state_->uploadModes.empty()) { mode = state_->uploadModes.front(); state_->uploadModes.pop_front(); }
        state_->mutexEvents.push_back("acquire-0");
        if (mode == PlatformState::UploadMode::Timeout) return false;
        ++state_->nativeLeases;
        state_->mutexEvents.push_back(mode == PlatformState::UploadMode::Success ? "upload-bgra" : "upload-error");
        state_->mutexEvents.push_back("release-1");
        --state_->nativeLeases;
        return mode == PlatformState::UploadMode::Success;
    }
    uint64_t DuplicateToProcess(uintptr_t processHandle) override {
        state_->duplicateTargets.push_back(processHandle);
        return state_->nextHostHandle++;
    }
    bool HasNativeMutexLease() const noexcept override { return state_->nativeLeases != 0; }
private:
    std::shared_ptr<PlatformState> state_;
    uint32_t width_;
    uint32_t height_;
};

class FakePlatform final : public telestudio::detail::IRemoteSurfacePlatform {
public:
    explicit FakePlatform(std::shared_ptr<PlatformState> state) : state_(std::move(state)) {}
    uintptr_t OpenProcess(uint32_t pid) override { state_->openedPid = pid; return pid == 4242 ? state_->processHandle : 0; }
    void CloseProcess(uintptr_t) noexcept override {}
    bool IsProcessAlive(uintptr_t handle) const noexcept override { return state_->alive && handle == state_->processHandle; }
    std::unique_ptr<telestudio::detail::IRemoteSurfaceSlot> CreateSlot(uint32_t width, uint32_t height) override {
        return std::make_unique<FakeSlot>(state_, width, height);
    }
private:
    std::shared_ptr<PlatformState> state_;
};

class FakeScheduler final : public telestudio::detail::IRemoteSurfaceScheduler {
public:
    telestudio::detail::RemoteSurfaceTimePoint Now() const noexcept override { return now_; }
    void Schedule(telestudio::detail::RemoteSurfaceTimePoint deadline, std::function<void()> callback) override {
        tasks_.push_back({deadline, std::move(callback)});
    }
    void CancelAndWait() noexcept override { tasks_.clear(); }
    void Advance(std::chrono::milliseconds duration) {
        now_ += duration;
        for (;;) {
            auto found = std::find_if(tasks_.begin(), tasks_.end(), [&](const auto& task) { return task.first <= now_; });
            if (found == tasks_.end()) break;
            auto callback = std::move(found->second); tasks_.erase(found); callback();
        }
    }
private:
    telestudio::detail::RemoteSurfaceTimePoint now_{};
    std::vector<std::pair<telestudio::detail::RemoteSurfaceTimePoint, std::function<void()>>> tasks_;
};

class FakeHostChannel {
public:
    explicit FakeHostChannel(std::shared_ptr<PlatformState> state) : state_(std::move(state)) {}
    void Send(telestudio::PublishedSurface surface) { messages.push_back(surface); }
    void OpenConsumeAndRelease(telestudio::RemoteVideoSurface& producer) {
        Require(!messages.empty(), "Host channel has a published surface");
        state_->mutexEvents.push_back("host-acquire-1");
        producer.Opened(messages.back().epoch);
        state_->mutexEvents.push_back("host-release-0");
    }
    std::vector<telestudio::PublishedSurface> messages;
private:
    std::shared_ptr<PlatformState> state_;
};

struct Fixture {
    telestudio::HostRegistration registration = Registration();
    std::shared_ptr<PlatformState> state = std::make_shared<PlatformState>();
    std::shared_ptr<FakePlatform> platform = std::make_shared<FakePlatform>(state);
    std::shared_ptr<FakeScheduler> scheduler = std::make_shared<FakeScheduler>();
};

void VerifyOpenedTimeoutAndAuthenticatedDuplicate() {
    Fixture fixture;
    telestudio::RemoteVideoSurface surface(fixture.registration, fixture.platform, fixture.scheduler);
    auto wrong = fixture.registration; wrong.sessionToken[0] ^= std::byte{1};
    Require(!surface.RegisterHost(wrong) && surface.RegisterHost(fixture.registration), "Only exact immutable host session registers");
    Require(surface.UpdateOnRenderThread(Frame(2,2)), "Initial fake upload succeeds");
    const auto published = surface.PublishForHost();
    Require(published.sequence == 1 && published.epoch == 1 && published.hostHandleValue == 1000 &&
            fixture.state->duplicateTargets == std::vector<uintptr_t>{fixture.state->processHandle},
            "Publish duplicates into the exact registered process with sequence/epoch 1");
    fixture.scheduler->Advance(std::chrono::milliseconds(1999));
    Require(surface.CurrentEpoch() == 1, "Opened timeout does not fire early");
    fixture.scheduler->Advance(std::chrono::milliseconds(1));
    Require(surface.CurrentEpoch() == 0 && fixture.state->liveSlots == 0,
            "2-second PublishedAwaitOpened timeout releases local resource without another surface call");
    surface.Opened(1);
    Require(surface.CurrentEpoch() == 0, "Late Opened after timeout is a no-op");
}

void VerifyAckRetirementAndTimeouts() {
    Fixture fixture;
    telestudio::RemoteVideoSurface surface(fixture.registration, fixture.platform, fixture.scheduler);
    Require(surface.RegisterHost(fixture.registration), "Host registers");
    Require(surface.UpdateOnRenderThread(Frame(2,2)), "Initial upload succeeds");
    const auto first = surface.PublishForHost();
    surface.Opened(0); surface.Opened(999); surface.Opened(first.epoch); surface.Opened(first.epoch);
    fixture.scheduler->Advance(std::chrono::seconds(2));
    Require(surface.CurrentEpoch() == first.epoch, "One valid current Opened transitions to Displayed; duplicates/future are no-op");
    surface.Released(first.epoch); surface.Removed(first.epoch);
    Require(surface.CurrentEpoch() == first.epoch, "Current Displayed Released/Removed are wrong-state no-ops");

    fixture.state->uploadModes.push_back(PlatformState::UploadMode::Timeout);
    Require(!surface.UpdateOnRenderThread(Frame(2,2,2)) && surface.CurrentEpoch() == first.epoch,
            "AcquireSync timeout drops new frame and preserves old epoch");
    fixture.state->uploadModes.push_back(PlatformState::UploadMode::ErrorAfterAcquire);
    Require(!surface.UpdateOnRenderThread(Frame(2,2,3)) && fixture.state->nativeLeases == 0,
            "Upload error balances the native mutex release");

    Require(surface.UpdateOnRenderThread(Frame(4,2)), "Resize fills replacement before retirement");
    const auto second = surface.PublishForHost();
    Require(second.sequence == 2 && second.epoch == 2 && fixture.state->liveSlots == 2,
            "Resize retires old epoch and publishes monotonic replacement");
    surface.Released(first.epoch);
    Require(fixture.state->liveSlots == 1 && surface.CurrentEpoch() == second.epoch,
            "Matching retired Released closes only retired epoch");

    Require(surface.UpdateOnRenderThread(Frame(6,2)), "Second resize succeeds");
    const auto third = surface.PublishForHost();
    surface.Opened(third.epoch);
    fixture.scheduler->Advance(std::chrono::milliseconds(4999));
    Require(fixture.state->liveSlots == 2, "Retired release timeout does not fire early");
    fixture.scheduler->Advance(std::chrono::milliseconds(1));
    Require(fixture.state->liveSlots == 1 && surface.CurrentEpoch() == third.epoch,
            "5-second RetiredAwaitReleased timeout releases local retired resource without polling");
    surface.Released(second.epoch);
    Require(fixture.state->liveSlots == 1, "Late Released after timeout is a no-op");

    surface.BeginRemoval();
    Require(surface.CurrentEpoch() == 0 && fixture.state->nativeLeases == 0 && fixture.state->liveSlots == 1,
            "Shutdown stops updates and enters RemovalPending without a native mutex lease");
    fixture.scheduler->Advance(std::chrono::milliseconds(4999));
    Require(fixture.state->liveSlots == 1, "Removal timeout does not fire early");
    fixture.scheduler->Advance(std::chrono::milliseconds(1));
    Require(fixture.state->liveSlots == 0, "5-second RemovalPending timeout releases local resource without polling");
    surface.Removed(third.epoch);
    Require(fixture.state->liveSlots == 0, "Late Removed after timeout is a no-op");
}

void VerifyRemovalAckAndExhaustion() {
    Fixture fixture;
    telestudio::RemoteVideoSurface surface(fixture.registration, fixture.platform, fixture.scheduler);
    Require(surface.RegisterHost(fixture.registration) && surface.UpdateOnRenderThread(Frame(2,2)), "Fixture opens");
    const auto published = surface.PublishForHost();
    surface.Opened(published.epoch);
    surface.BeginRemoval();
    surface.Removed(published.epoch);
    Require(fixture.state->liveSlots == 0, "Matching Removed closes only RemovalPending epoch");

    Fixture exhausted;
    const uint64_t maximum = static_cast<uint64_t>((std::numeric_limits<int64_t>::max)());
    telestudio::RemoteVideoSurface limited(exhausted.registration, exhausted.platform, exhausted.scheduler,
                                           {maximum, maximum});
    Require(limited.RegisterHost(exhausted.registration) && limited.UpdateOnRenderThread(Frame(2,2)), "Exhaustion fixture opens");
    const auto last = limited.PublishForHost();
    Require(last.sequence == maximum && last.epoch == maximum, "Last legal native integer values publish exactly once");
    Require(limited.UpdateOnRenderThread(Frame(4,2)), "Final replacement uploads");
    bool rejected = false;
    try { (void)limited.PublishForHost(); } catch (const std::overflow_error&) { rejected = true; }
    Require(rejected, "Sequence/epoch exhaustion is terminal and never wraps");
}

void VerifyChannelMutexAndPlatformErrors() {
    Fixture fixture;
    telestudio::RemoteVideoSurface surface(fixture.registration, fixture.platform, fixture.scheduler);
    Require(surface.RegisterHost(fixture.registration) && surface.UpdateOnRenderThread(Frame(2,2)),
            "Channel fixture opens and uploads");
    FakeHostChannel channel(fixture.state);
    channel.Send(surface.PublishForHost());
    channel.OpenConsumeAndRelease(surface);
    Require(fixture.state->mutexEvents == std::vector<std::string>({
                "acquire-0", "upload-bgra", "release-1", "host-acquire-1", "host-release-0"}),
            "Fake producer, channel, and host enforce keyed-mutex 0-to-1-to-0 ordering");
    fixture.state->alive = false;
    bool rejected = false;
    try { (void)surface.PublishForHost(); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "Publishing rejects an exited registered process");

    Fixture dead;
    dead.state->alive = false;
    telestudio::RemoteVideoSurface deadSurface(dead.registration, dead.platform, dead.scheduler);
    Require(!deadSurface.RegisterHost(dead.registration), "Registration rejects a process that is not alive");

    Fixture duplicateFailure;
    duplicateFailure.state->nextHostHandle = 0;
    telestudio::RemoteVideoSurface retryable(duplicateFailure.registration, duplicateFailure.platform,
                                             duplicateFailure.scheduler);
    Require(retryable.RegisterHost(duplicateFailure.registration) &&
            retryable.UpdateOnRenderThread(Frame(2,2)), "Duplicate failure fixture opens");
    rejected = false;
    try { (void)retryable.PublishForHost(); } catch (const std::runtime_error&) { rejected = true; }
    Require(rejected && retryable.CurrentEpoch() == 0,
            "Null duplicated handle fails without consuming an epoch");
    duplicateFailure.state->nextHostHandle = 2000;
    const auto recovered = retryable.PublishForHost();
    Require(recovered.epoch == 1 && recovered.sequence == 1 && recovered.hostHandleValue == 2000,
            "Retry after DuplicateHandle failure starts at epoch and sequence 1");
}

} // namespace

int main() {
    try {
        VerifyOpenedTimeoutAndAuthenticatedDuplicate();
        VerifyAckRetirementAndTimeouts();
        VerifyRemovalAckAndExhaustion();
        VerifyChannelMutexAndPlatformErrors();
        std::cout << "media_remote_surface_state_tests: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "media_remote_surface_state_tests: " << error.what() << '\n';
        return 1;
    }
}
