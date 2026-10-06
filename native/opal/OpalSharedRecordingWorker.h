#pragma once
#include <telestudio/media/LectureRecorder.h>
#include <telestudio/media/VideoCompositor.h>
namespace telestudio {
class ISharedRecordingWorker:public IRecordingWorker {
public:
 virtual bool PushVideo(std::shared_ptr<const ComposedVideoFrame> pixels,int64_t timestamp100ns)noexcept=0;
};
// Consumes only already shared source pixels; never opens a capture source.
std::shared_ptr<ISharedRecordingWorker> CreateOpalSharedRecordingWorker(
 std::filesystem::path x264,std::filesystem::path output={},
 std::optional<media::ValidatedRecordingOutputDirectory> validatedOutput=std::nullopt,
 std::function<void(I420Frame)> observedEncoderInput={});
}
