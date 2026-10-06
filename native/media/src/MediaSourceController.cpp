#include "telestudio/media/MediaSourceController.h"

#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace telestudio {

struct MediaSourceController::State {
    mutable std::mutex routeMutex;
    DesktopSourceFactory factory;
    FrameCallback output;
    VideoSource active = VideoSource::Camera;
    uint64_t generation = 0;
    std::shared_ptr<IDesktopCaptureSource> candidate;
    std::shared_ptr<IDesktopCaptureSource> desktop;
    std::vector<std::shared_ptr<IDesktopCaptureSource>> failedSources;
    std::string lastError;
};

namespace {

std::shared_ptr<IDesktopCaptureSource> DefaultDesktopSourceFactory() {
    return std::make_shared<DesktopCaptureSource>();
}

void StopSources(std::vector<std::shared_ptr<IDesktopCaptureSource>> sources) noexcept {
    for (const auto& source : sources) {
        if (!source) {
            continue;
        }
        try {
            source->Stop();
        } catch (...) {
        }
    }
}

} // namespace

MediaSourceController::MediaSourceController(DesktopSourceFactory factory)
    : state_(std::make_shared<State>()) {
    state_->factory = factory ? std::move(factory) : DefaultDesktopSourceFactory;
}

MediaSourceController::~MediaSourceController() {
    std::vector<std::shared_ptr<IDesktopCaptureSource>> sources;
    {
        std::lock_guard lock(state_->routeMutex);
        ++state_->generation;
        if (state_->candidate) {
            sources.push_back(std::move(state_->candidate));
        }
        if (state_->desktop) {
            sources.push_back(std::move(state_->desktop));
        }
        for (auto& source : state_->failedSources) {
            sources.push_back(std::move(source));
        }
        state_->failedSources.clear();
    }
    StopSources(std::move(sources));
}

void MediaSourceController::SetCameraSource(FrameCallback attach) {
    if (!attach) {
        throw std::invalid_argument("Camera frame callback is required");
    }

    std::vector<std::shared_ptr<IDesktopCaptureSource>> sources;
    {
        std::lock_guard lock(state_->routeMutex);
        ++state_->generation;
        state_->active = VideoSource::Camera;
        state_->output = std::move(attach);
        state_->lastError.clear();
        if (state_->candidate) {
            sources.push_back(std::move(state_->candidate));
        }
        if (state_->desktop) {
            sources.push_back(std::move(state_->desktop));
        }
        for (auto& source : state_->failedSources) {
            sources.push_back(std::move(source));
        }
        state_->failedSources.clear();
    }
    StopSources(std::move(sources));
}

void MediaSourceController::SetDesktopSource(std::string displayId) {
    if (displayId.empty()) {
        throw std::invalid_argument("Desktop display ID is required");
    }

    std::shared_ptr<IDesktopCaptureSource> oldCandidate;
    std::shared_ptr<IDesktopCaptureSource> candidate;
    uint64_t requestGeneration = 0;
    {
        std::lock_guard lock(state_->routeMutex);
        if (!state_->output) {
            throw std::logic_error("SetCameraSource must install the output route first");
        }
        requestGeneration = ++state_->generation;
        state_->lastError.clear();
        oldCandidate = std::move(state_->candidate);
        candidate = state_->factory();
        if (!candidate) {
            throw std::runtime_error("Desktop source factory returned no source");
        }
        state_->candidate = candidate;
    }
    if (oldCandidate) {
        oldCandidate->Stop();
    }

    const std::weak_ptr<State> weakState = state_;
    const std::weak_ptr<IDesktopCaptureSource> weakCandidate = candidate;
    try {
        candidate->Start(
            displayId, 30,
            [weakState, weakCandidate, requestGeneration](I420Frame frame) {
                const auto state = weakState.lock();
                const auto source = weakCandidate.lock();
                if (!state || !source) {
                    return;
                }
                std::shared_ptr<IDesktopCaptureSource> oldDesktop;
                {
                    std::lock_guard lock(state->routeMutex);
                    const bool committedDesktop = state->active == VideoSource::Desktop &&
                                                  state->desktop == source;
                    if (!committedDesktop) {
                        if (state->generation != requestGeneration || state->candidate != source) {
                            return;
                        }
                        oldDesktop = std::move(state->desktop);
                        state->candidate.reset();
                        state->desktop = source;
                        state->active = VideoSource::Desktop;
                    }
                    state->output(frame);
                }
                if (oldDesktop && oldDesktop != source) {
                    oldDesktop->Stop();
                }
            },
            [weakState, weakCandidate, requestGeneration](const std::string& error) {
                const auto state = weakState.lock();
                const auto source = weakCandidate.lock();
                if (!state || !source) {
                    return;
                }
                std::lock_guard lock(state->routeMutex);
                if (state->generation == requestGeneration && state->candidate == source) {
                    state->lastError = error;
                    state->failedSources.push_back(state->candidate);
                    state->candidate.reset();
                } else if (state->desktop == source) {
                    state->lastError = error;
                    state->failedSources.push_back(state->desktop);
                    state->desktop.reset();
                    state->active = VideoSource::Camera;
                }
            });
    } catch (const std::exception& error) {
        std::lock_guard lock(state_->routeMutex);
        if (state_->generation == requestGeneration && state_->candidate == candidate) {
            state_->lastError = error.what();
            state_->failedSources.push_back(state_->candidate);
            state_->candidate.reset();
        }
    }
}

void MediaSourceController::DeliverCameraFrame(I420Frame frame) {
    std::lock_guard lock(state_->routeMutex);
    if (state_->active == VideoSource::Camera && state_->output) {
        state_->output(frame);
    }
}

VideoSource MediaSourceController::ActiveSource() const {
    std::lock_guard lock(state_->routeMutex);
    return state_->active;
}

std::string MediaSourceController::LastError() const {
    std::lock_guard lock(state_->routeMutex);
    return state_->lastError;
}

} // namespace telestudio
