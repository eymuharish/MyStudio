#include "../OpalSharedRecordingWorker.h"
#include <Windows.h>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <thread>
#include <iostream>
#include <array>

void RecordingDiagnosticFailureProof(const std::filesystem::path& directory) {
    using namespace telestudio;
    const auto blocked = directory / L"selected-output-is-a-file";
    std::ofstream(blocked) << "owned test boundary";
    const auto log = directory / (L"opal-recording-" + std::to_wstring(GetCurrentProcessId()) + L".log");
    auto failAtSelectedDirectory = [&] {
        auto worker = CreateOpalSharedRecordingWorker({}, blocked);
        if (worker->Start({"explicit-owned-source", false, false}, 17, 0) != RecordingError::None)
            throw std::runtime_error("Diagnostic fixture was not accepted");
        if (worker->WaitQuiesced(std::chrono::seconds(5)) != RecordingWaitOutcome::Quiesced)
            throw std::runtime_error("Selected-directory failure did not quiesce");
        const auto update = worker->Poll();
        if (!update || update->kind != RecordingWorkerUpdateKind::Failed || update->error != RecordingError::SinkFailed)
            throw std::runtime_error("Diagnostic fixture changed the real failure result");
    };
    _wputenv_s(L"ESSASTUDIO_OPAL_RECORDING_DIAGNOSTIC_DIRECTORY", L"");
    failAtSelectedDirectory();
    if (std::filesystem::exists(log)) throw std::runtime_error("Inactive recording diagnostics wrote a file");
    _wputenv_s(L"ESSASTUDIO_OPAL_RECORDING_DIAGNOSTIC_DIRECTORY", directory.c_str());
    failAtSelectedDirectory();
    std::ifstream input(log);
    const std::string detail((std::istreambuf_iterator<char>(input)), {});
    if (detail.find("phase=configure-path") == std::string::npos || detail.find("error=7") == std::string::npos)
        throw std::runtime_error("Selected-directory failure did not identify its actual diagnostic boundary");
    if (detail.find("selected-output-is-a-file") != std::string::npos || detail.find("owned test boundary") != std::string::npos)
        throw std::runtime_error("Recording diagnostics logged file content or a private path");
    auto badEncoder=CreateOpalSharedRecordingWorker(directory/L"absent-x264.dll",directory);
    if(badEncoder->Start({"explicit-owned-source",false,false},18,0)!=RecordingError::None ||
       badEncoder->WaitQuiesced(std::chrono::seconds(5))!=RecordingWaitOutcome::Quiesced)
        throw std::runtime_error("Unavailable encoder failure did not quiesce");
    const auto encoderFailure=badEncoder->Poll();
    if(!encoderFailure || encoderFailure->kind!=RecordingWorkerUpdateKind::Failed || encoderFailure->error!=RecordingError::EncoderFailed)
        throw std::runtime_error("Recording recovery hid a genuine encoder failure");
    _wputenv_s(L"ESSASTUDIO_OPAL_RECORDING_DIAGNOSTIC_DIRECTORY", L"");
}

void OpalLateAudioProof(const std::filesystem::path& directory) {
    using namespace telestudio;
    constexpr std::int64_t epoch = 1'000'000'000;
    constexpr std::uint64_t generation = 19;
    _wputenv_s(L"ESSASTUDIO_OPAL_RECORDING_DIAGNOSTIC_DIRECTORY", directory.c_str());
    wchar_t executable[32768];
    GetModuleFileNameW(nullptr, executable, 32768);
    auto worker = CreateOpalSharedRecordingWorker(std::filesystem::path(executable).parent_path() / L"libx264-165.dll", directory);
    if (worker->Start({"explicit-owned-pixels", true, true}, generation, epoch) != RecordingError::None)
        throw std::runtime_error("Late-tap fixture could not start");
    std::array<std::uint8_t,256> y;std::array<std::uint8_t,64> u,v;
    y.fill(200);u.fill(128);v.fill(128);
    I420Frame input{y.data(),u.data(),v.data(),16,8,8,16,16,epoch};
    auto pixels = std::make_shared<ComposedVideoFrame>(ComposeVideoFrame(&input,nullptr,{1920,1080},epoch));
    if (!worker->PushVideo(pixels, epoch)) throw std::runtime_error("Owned bootstrap frame rejected");
    auto poll = [&](RecordingWorkerUpdateKind wanted) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            if (auto update = worker->Poll()) {
                if (update->kind == wanted) return *update;
                if (update->kind == RecordingWorkerUpdateKind::Failed)
                    throw std::runtime_error("Late-tap recording failed with error=" + std::to_string(int(update->error)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("Late-tap fixture update timed out");
    };
    poll(RecordingWorkerUpdateKind::Ready);
    auto block = [&](media::AudioTapKind source, std::int64_t relative) {
        media::OwnedAudioBlock audio;
        audio.source=source;audio.callGeneration=generation;audio.sampleRateHz=48000;audio.channels=1;
        audio.duration100ns=200'000;audio.timestamp100ns=epoch+relative;audio.samples.assign(960,source==media::AudioTapKind::Microphone?1200:2400);
        return audio;
    };
    auto push = [&](media::AudioTapKind source, int slot) {
        if (!worker->Push(block(source,slot*200'000ll))) throw std::runtime_error("Current valid tap was rejected");
    };
    for(int slot=0;slot<35;++slot) {
        push(media::AudioTapKind::Microphone,slot);push(media::AudioTapKind::Room,slot);
        if(slot>0&&slot%5==0)worker->PushVideo(pixels,epoch+slot*200'000ll);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // One tap temporarily leads. The bounded mixer commits room-only slots35..37.
    for(int slot=35;slot<62;++slot) {
        push(media::AudioTapKind::Room,slot);
        if(slot%5==0)worker->PushVideo(pixels,epoch+slot*200'000ll);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const bool staleAccepted = worker->Push(block(media::AudioTapKind::Microphone,7'000'000));
    if(staleAccepted) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(2);
        while(std::chrono::steady_clock::now()<deadline) {
            if(auto update=worker->Poll();update&&update->kind==RecordingWorkerUpdateKind::Failed) {
                worker->WaitQuiesced(std::chrono::seconds(2));
                throw std::runtime_error("Microphone7000000 behind committed7600000 was accepted; actual recording error="+std::to_string(int(update->error)));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        worker->Stop();worker->WaitQuiesced(std::chrono::seconds(5));
        throw std::runtime_error("Already committed late microphone block was accepted");
    }
    push(media::AudioTapKind::Microphone,62);push(media::AudioTapKind::Room,62);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    if(!worker->SetPaused(true,epoch+14'000'000)||!worker->SetPaused(false,epoch+16'000'000))
        throw std::runtime_error("Late-tap recording did not remain pausable/resumable");
    push(media::AudioTapKind::Microphone,81);push(media::AudioTapKind::Room,81);
    worker->PushVideo(pixels,epoch+16'200'000);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));worker->Stop();
    if(worker->WaitQuiesced(std::chrono::seconds(5))!=RecordingWaitOutcome::Quiesced)
        throw std::runtime_error("Late-tap recording failed to finalize");
    const auto completed=poll(RecordingWorkerUpdateKind::Completed);
    if(completed.fileName.empty()||std::filesystem::file_size(directory/completed.fileName)<5300)
        throw std::runtime_error("Late-tap recording did not produce MP4");
    _wputenv_s(L"ESSASTUDIO_OPAL_RECORDING_DIAGNOSTIC_DIRECTORY",L"");
    std::cout<<"Late-tap recording pause/resume/finalize PASS MP4="<<(directory/completed.fileName).string()<<'\n';
}
