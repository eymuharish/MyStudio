add_library(telestudio_media STATIC
    src/CodecPolicy.cpp
    src/AdaptiveQualityController.cpp
    src/VideoCompositor.cpp
    src/CameraCaptureSource.cpp
    src/ComposedVideoSource.cpp
    src/VideoSurfacePublisher.cpp
    src/ComposedVideoSender.cpp
)
add_executable(network_profiles_tests tests/NetworkProfilesTests.cpp)
target_compile_features(network_profiles_tests PRIVATE cxx_std_20)
target_include_directories(network_profiles_tests PRIVATE include)
add_test(NAME network_profiles_tests COMMAND network_profiles_tests)
target_compile_features(telestudio_media PUBLIC cxx_std_20)
target_include_directories(telestudio_media PUBLIC include)

add_executable(media_video_compositor_tests
    tests/VideoCompositorTests.cpp
)
target_compile_features(media_video_compositor_tests PRIVATE cxx_std_20)
target_link_libraries(media_video_compositor_tests PRIVATE telestudio_media)
add_test(NAME media_video_compositor_tests COMMAND media_video_compositor_tests)
add_executable(media_surface_publisher_tests tests/VideoSurfacePublisherTests.cpp)
target_compile_features(media_surface_publisher_tests PRIVATE cxx_std_20)
target_link_libraries(media_surface_publisher_tests PRIVATE telestudio_media d3d11 dxgi)
add_test(NAME media_surface_publisher_tests COMMAND media_surface_publisher_tests)
add_executable(media_composed_sender_tests tests/ComposedVideoSenderTests.cpp)
target_compile_features(media_composed_sender_tests PRIVATE cxx_std_20)
target_link_libraries(media_composed_sender_tests PRIVATE telestudio_media)
add_test(NAME media_composed_sender_tests COMMAND media_composed_sender_tests)

add_executable(media_composed_source_tests tests/ComposedVideoSourceTests.cpp)
target_compile_features(media_composed_source_tests PRIVATE cxx_std_20)
target_link_libraries(media_composed_source_tests PRIVATE telestudio_media)
add_test(NAME media_composed_source_tests COMMAND media_composed_source_tests)

add_executable(media_camera_source_tests tests/CameraCaptureSourceTests.cpp)
target_compile_features(media_camera_source_tests PRIVATE cxx_std_20)
target_link_libraries(media_camera_source_tests PRIVATE telestudio_media)
add_test(NAME media_camera_source_tests COMMAND media_camera_source_tests)
set_tests_properties(media_camera_source_tests media_composed_source_tests PROPERTIES TIMEOUT 20)

add_executable(media_codec_policy_tests
    tests/CodecPolicyTests.cpp
)
target_compile_features(media_codec_policy_tests PRIVATE cxx_std_20)
target_link_libraries(media_codec_policy_tests PRIVATE telestudio_media)
add_test(NAME media_codec_policy_tests COMMAND media_codec_policy_tests)

add_executable(media_adaptive_quality_tests
    tests/AdaptiveQualityControllerTests.cpp
)
target_compile_features(media_adaptive_quality_tests PRIVATE cxx_std_20)
target_link_libraries(media_adaptive_quality_tests PRIVATE telestudio_media)
add_test(NAME media_adaptive_quality_tests COMMAND media_adaptive_quality_tests)

set(TELESTUDIO_REPOSITORY_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "EssaStudio repository root used to resolve verified shared artifacts")
cmake_path(ABSOLUTE_PATH TELESTUDIO_REPOSITORY_ROOT
    BASE_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}" NORMALIZE
    OUTPUT_VARIABLE _TELESTUDIO_REPOSITORY_ROOT)

set(TELESTUDIO_VIDEO_CODECS_DIR
    "${_TELESTUDIO_REPOSITORY_ROOT}/work/build/video-codecs/install" CACHE PATH
    "Verified x264/x265/FFmpeg install root")
cmake_path(ABSOLUTE_PATH TELESTUDIO_VIDEO_CODECS_DIR
    BASE_DIRECTORY "${_TELESTUDIO_REPOSITORY_ROOT}" NORMALIZE
    OUTPUT_VARIABLE TELESTUDIO_VIDEO_CODECS_DIR)
set(TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR "${TELESTUDIO_VIDEO_CODECS_DIR}/include")
set(TELESTUDIO_VIDEO_CODECS_BIN_DIR "${TELESTUDIO_VIDEO_CODECS_DIR}/bin")

if(NOT EXISTS "${TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR}/x264.h" OR
   NOT EXISTS "${TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR}/x265.h")
    message(FATAL_ERROR "Pinned video codec headers are required under ${TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR}")
endif()

add_library(telestudio_media_encoders STATIC
    src/DynamicLibrary.cpp
    src/X264Encoder.cpp
    src/X265Encoder.cpp
)
target_compile_features(telestudio_media_encoders PUBLIC cxx_std_20)
target_include_directories(telestudio_media_encoders
    PUBLIC include
    PRIVATE "${TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR}"
)

add_executable(media_encoder_tests
    tests/EncoderTests.cpp
)
target_compile_features(media_encoder_tests PRIVATE cxx_std_20)
target_compile_definitions(media_encoder_tests PRIVATE
    TELESTUDIO_CODEC_BIN_DIR="${TELESTUDIO_VIDEO_CODECS_BIN_DIR}"
)
target_include_directories(media_encoder_tests PRIVATE "${TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR}")
target_link_libraries(media_encoder_tests PRIVATE telestudio_media_encoders)
add_test(NAME media_encoder_tests COMMAND media_encoder_tests)
add_executable(media_encoder_rollback_tests tests/EncoderRollbackTests.cpp)
target_compile_features(media_encoder_rollback_tests PRIVATE cxx_std_20)
target_compile_definitions(media_encoder_rollback_tests PRIVATE TELESTUDIO_CODEC_BIN_DIR="${TELESTUDIO_VIDEO_CODECS_BIN_DIR}")
target_link_libraries(media_encoder_rollback_tests PRIVATE telestudio_media_encoders)
add_test(NAME media_encoder_rollback_tests COMMAND media_encoder_rollback_tests)
set_tests_properties(media_encoder_tests PROPERTIES
    WORKING_DIRECTORY "${_TELESTUDIO_REPOSITORY_ROOT}"
)

add_executable(media_rtp_packetizer_tests
    tests/RtpPacketizerTests.cpp
    src/RtpPacketizer.cpp
)
target_compile_features(media_rtp_packetizer_tests PRIVATE cxx_std_20)
target_include_directories(media_rtp_packetizer_tests PRIVATE include)
add_test(NAME media_rtp_packetizer_tests COMMAND media_rtp_packetizer_tests)

add_executable(media_ffmpeg_decoder_tests
    tests/FfmpegVideoDecoderTests.cpp
    src/FfmpegVideoDecoder.cpp
)
target_compile_features(media_ffmpeg_decoder_tests PRIVATE cxx_std_20)
target_compile_definitions(media_ffmpeg_decoder_tests PRIVATE
    TELESTUDIO_CODEC_BIN_DIR="${TELESTUDIO_VIDEO_CODECS_BIN_DIR}"
    TELESTUDIO_DEFAULT_CODEC_BIN_DIR=L"${TELESTUDIO_VIDEO_CODECS_BIN_DIR}"
)
target_include_directories(media_ffmpeg_decoder_tests PRIVATE include "${TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR}")
target_link_libraries(media_ffmpeg_decoder_tests PRIVATE d3d11 dxgi)
add_test(NAME media_ffmpeg_decoder_tests COMMAND media_ffmpeg_decoder_tests)
set_tests_properties(media_ffmpeg_decoder_tests PROPERTIES
    WORKING_DIRECTORY "${_TELESTUDIO_REPOSITORY_ROOT}")

add_executable(media_decoder_state_tests
    tests/DecoderStateMachineTests.cpp
    src/FfmpegVideoDecoder.cpp
)
target_compile_features(media_decoder_state_tests PRIVATE cxx_std_20)
target_compile_definitions(media_decoder_state_tests PRIVATE
    TELESTUDIO_DEFAULT_CODEC_BIN_DIR=L"${TELESTUDIO_VIDEO_CODECS_BIN_DIR}"
)
target_include_directories(media_decoder_state_tests PRIVATE include "${TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR}")
target_link_libraries(media_decoder_state_tests PRIVATE d3d11 dxgi)
add_test(NAME media_decoder_state_tests COMMAND media_decoder_state_tests)

add_executable(media_remote_surface_tests
    tests/RemoteVideoSurfaceTests.cpp
    src/RemoteVideoSurface.cpp
)
target_compile_features(media_remote_surface_tests PRIVATE cxx_std_20)
target_include_directories(media_remote_surface_tests PRIVATE include)
target_link_libraries(media_remote_surface_tests PRIVATE d3d11 dxgi)
add_test(NAME media_remote_surface_tests COMMAND media_remote_surface_tests)

add_executable(media_remote_surface_state_tests
    tests/RemoteVideoSurfaceStateTests.cpp
    src/RemoteVideoSurface.cpp
)
target_compile_features(media_remote_surface_state_tests PRIVATE cxx_std_20)
target_include_directories(media_remote_surface_state_tests PRIVATE include)
target_link_libraries(media_remote_surface_state_tests PRIVATE d3d11 dxgi)
add_test(NAME media_remote_surface_state_tests COMMAND media_remote_surface_state_tests)

target_sources(telestudio_media PRIVATE
    src/RtpPacketizer.cpp
    src/FfmpegVideoDecoder.cpp
    src/RemoteVideoSurface.cpp
)
target_include_directories(telestudio_media PRIVATE "${TELESTUDIO_VIDEO_CODECS_INCLUDE_DIR}")
target_compile_definitions(telestudio_media PRIVATE
    TELESTUDIO_DEFAULT_CODEC_BIN_DIR=L"${TELESTUDIO_VIDEO_CODECS_BIN_DIR}"
)

set(LIBYUV_INCLUDE_DIR "" CACHE PATH "Explicit libyuv include directory")
set(LIBYUV_LIBRARY "" CACHE FILEPATH "Explicit libyuv static library")
if(NOT LIBYUV_INCLUDE_DIR OR NOT EXISTS "${LIBYUV_INCLUDE_DIR}/libyuv/convert.h")
    message(FATAL_ERROR "Set LIBYUV_INCLUDE_DIR to a verified standalone libyuv include directory")
endif()
if(NOT LIBYUV_LIBRARY OR NOT EXISTS "${LIBYUV_LIBRARY}")
    message(FATAL_ERROR "Set LIBYUV_LIBRARY to a verified standalone libyuv static library")
endif()
cmake_path(ABSOLUTE_PATH LIBYUV_INCLUDE_DIR NORMALIZE OUTPUT_VARIABLE _TELESTUDIO_LIBYUV_INCLUDE_ABSOLUTE)
cmake_path(ABSOLUTE_PATH LIBYUV_LIBRARY NORMALIZE OUTPUT_VARIABLE _TELESTUDIO_LIBYUV_LIBRARY_ABSOLUTE)
target_sources(telestudio_media PRIVATE
    src/FrameConverter.cpp
    src/DesktopCaptureSource.cpp
    src/MediaSourceController.cpp
)
target_include_directories(telestudio_media PRIVATE "${_TELESTUDIO_LIBYUV_INCLUDE_ABSOLUTE}")
target_link_libraries(telestudio_media PRIVATE "${_TELESTUDIO_LIBYUV_LIBRARY_ABSOLUTE}" d3d11 dxgi dwmapi windowsapp mfplat mfreadwrite mfuuid mf ole32)

add_executable(media_frame_converter_tests tests/FrameConverterTests.cpp)
target_compile_features(media_frame_converter_tests PRIVATE cxx_std_20)
target_link_libraries(media_frame_converter_tests PRIVATE telestudio_media)
add_test(NAME media_frame_converter_tests COMMAND media_frame_converter_tests)

add_executable(media_source_controller_tests tests/MediaSourceControllerTests.cpp)
target_compile_features(media_source_controller_tests PRIVATE cxx_std_20)
target_link_libraries(media_source_controller_tests PRIVATE telestudio_media)
add_test(NAME media_source_controller_tests COMMAND media_source_controller_tests)

add_executable(capture_smoke tools/capture_smoke.cpp)
target_compile_features(capture_smoke PRIVATE cxx_std_20)
target_link_libraries(capture_smoke PRIVATE telestudio_media)
set_target_properties(capture_smoke PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/$<CONFIG>"
)

set(TELESTUDIO_AUDIO_DSP_INSTALL
    "${_TELESTUDIO_REPOSITORY_ROOT}/work/build/audio-dsp/install" CACHE PATH
    "Verified private SpeexDSP install root")
cmake_path(ABSOLUTE_PATH TELESTUDIO_AUDIO_DSP_INSTALL
    BASE_DIRECTORY "${_TELESTUDIO_REPOSITORY_ROOT}"
    NORMALIZE OUTPUT_VARIABLE _TELESTUDIO_AUDIO_DSP_INSTALL)
set(_TELESTUDIO_AUDIO_DSP_MANIFEST "${_TELESTUDIO_AUDIO_DSP_INSTALL}/audio-dsp-build.json")
set(_TELESTUDIO_AUDIO_DSP_LICENSE "${_TELESTUDIO_AUDIO_DSP_INSTALL}/licenses/speexdsp-COPYING")
set(_TELESTUDIO_AUDIO_DSP_LIBRARY "${_TELESTUDIO_AUDIO_DSP_INSTALL}/lib/speexdsp-private.lib")
foreach(_required IN ITEMS
    "${_TELESTUDIO_AUDIO_DSP_MANIFEST}"
    "${_TELESTUDIO_AUDIO_DSP_LICENSE}"
    "${_TELESTUDIO_AUDIO_DSP_LIBRARY}"
    "${_TELESTUDIO_AUDIO_DSP_INSTALL}/include/speex/speex_preprocess.h"
    "${_TELESTUDIO_AUDIO_DSP_INSTALL}/include/speex/speex_resampler.h")
    if(NOT EXISTS "${_required}")
        message(FATAL_ERROR "Verified private SpeexDSP artifact is missing: ${_required}")
    endif()
endforeach()
file(READ "${_TELESTUDIO_AUDIO_DSP_MANIFEST}" _audio_dsp_json)
string(JSON _audio_dsp_tag_object GET "${_audio_dsp_json}" tagObject)
string(JSON _audio_dsp_commit GET "${_audio_dsp_json}" peeledCommit)
string(JSON _audio_dsp_arch GET "${_audio_dsp_json}" architecture)
string(JSON _audio_dsp_config GET "${_audio_dsp_json}" configuration)
string(JSON _audio_dsp_language GET "${_audio_dsp_json}" language)
string(JSON _audio_dsp_runtime GET "${_audio_dsp_json}" runtime)
string(JSON _audio_dsp_classification GET "${_audio_dsp_json}" library classification)
if(NOT _audio_dsp_tag_object STREQUAL "e762196693c487249d7061081915c8eaf7511f4f" OR
   NOT _audio_dsp_commit STREQUAL "1b28a0f61bc31162979e1f26f3981fc3637095c8" OR
   NOT _audio_dsp_arch STREQUAL "x64" OR NOT _audio_dsp_config STREQUAL "Release" OR
   NOT _audio_dsp_language STREQUAL "C" OR NOT _audio_dsp_runtime STREQUAL "/MD" OR
   NOT _audio_dsp_classification STREQUAL "static-archive")
    message(FATAL_ERROR "Private SpeexDSP build manifest policy mismatch")
endif()
string(JSON _audio_dsp_library_hash GET "${_audio_dsp_json}" library sha256)
string(JSON _audio_dsp_license_hash GET "${_audio_dsp_json}" license stagedSha256)
file(SHA256 "${_TELESTUDIO_AUDIO_DSP_LIBRARY}" _audio_dsp_actual_library_hash)
file(SHA256 "${_TELESTUDIO_AUDIO_DSP_LICENSE}" _audio_dsp_actual_license_hash)
string(TOUPPER "${_audio_dsp_actual_library_hash}" _audio_dsp_actual_library_hash)
string(TOUPPER "${_audio_dsp_actual_license_hash}" _audio_dsp_actual_license_hash)
if(NOT _audio_dsp_actual_library_hash STREQUAL _audio_dsp_library_hash OR
   NOT _audio_dsp_actual_license_hash STREQUAL _audio_dsp_license_hash)
    message(FATAL_ERROR "Private SpeexDSP staged artifact hash mismatch")
endif()
string(JSON _audio_dsp_header_count LENGTH "${_audio_dsp_json}" headers)
if(_audio_dsp_header_count EQUAL 0)
    message(FATAL_ERROR "Private SpeexDSP build manifest has no staged headers")
endif()
math(EXPR _audio_dsp_header_last "${_audio_dsp_header_count} - 1")
foreach(_header_index RANGE 0 ${_audio_dsp_header_last})
    string(JSON _header_path GET "${_audio_dsp_json}" headers ${_header_index} path)
    string(JSON _header_hash GET "${_audio_dsp_json}" headers ${_header_index} sha256)
    set(_header_file "${_TELESTUDIO_AUDIO_DSP_INSTALL}/${_header_path}")
    if(NOT EXISTS "${_header_file}")
        message(FATAL_ERROR "Private SpeexDSP staged header is missing: ${_header_path}")
    endif()
    file(SHA256 "${_header_file}" _header_actual_hash)
    string(TOUPPER "${_header_actual_hash}" _header_actual_hash)
    if(NOT _header_actual_hash STREQUAL _header_hash)
        message(FATAL_ERROR "Private SpeexDSP staged header hash mismatch: ${_header_path}")
    endif()
endforeach()

add_library(telestudio_speexdsp STATIC IMPORTED GLOBAL)
set_target_properties(telestudio_speexdsp PROPERTIES
    IMPORTED_LOCATION "${_TELESTUDIO_AUDIO_DSP_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${_TELESTUDIO_AUDIO_DSP_INSTALL}/include")

add_library(telestudio_audio STATIC
    src/MonotonicClock.cpp
    src/AudioBlockFramer.cpp
    src/AudioControlProcessor.cpp
    src/RoomPlaybackProcessor.cpp
    src/OutboundAudioMixer.cpp
    src/ProtocolAudioRateBridge.cpp
    src/SystemAudioCapture.cpp
)
target_compile_features(telestudio_audio PUBLIC cxx_std_20)
target_include_directories(telestudio_audio PUBLIC include)
target_compile_definitions(telestudio_audio PRIVATE NTDDI_VERSION=NTDDI_WIN10_FE)
target_link_libraries(telestudio_audio PRIVATE telestudio_speexdsp mmdevapi ole32)

add_executable(monotonic_clock_tests tests/MonotonicClockTests.cpp)
target_compile_features(monotonic_clock_tests PRIVATE cxx_std_20)
target_link_libraries(monotonic_clock_tests PRIVATE telestudio_audio)
add_test(NAME monotonic_clock_tests COMMAND monotonic_clock_tests)

add_executable(audio_block_framer_tests tests/AudioBlockFramerTests.cpp)
target_compile_features(audio_block_framer_tests PRIVATE cxx_std_20)
target_link_libraries(audio_block_framer_tests PRIVATE telestudio_audio)
add_test(NAME audio_block_framer_tests COMMAND audio_block_framer_tests)

add_executable(audio_control_processor_tests tests/AudioControlProcessorTests.cpp)
target_compile_features(audio_control_processor_tests PRIVATE cxx_std_20)
target_link_libraries(audio_control_processor_tests PRIVATE telestudio_audio)
add_test(NAME audio_control_processor_tests COMMAND audio_control_processor_tests)

add_executable(room_playback_processor_tests tests/RoomPlaybackProcessorTests.cpp)
target_compile_features(room_playback_processor_tests PRIVATE cxx_std_20)
target_link_libraries(room_playback_processor_tests PRIVATE telestudio_audio)
add_test(NAME room_playback_processor_tests COMMAND room_playback_processor_tests)

add_executable(outbound_audio_mixer_tests tests/OutboundAudioMixerTests.cpp)
target_compile_features(outbound_audio_mixer_tests PRIVATE cxx_std_20)
target_link_libraries(outbound_audio_mixer_tests PRIVATE telestudio_audio)
add_test(NAME outbound_audio_mixer COMMAND outbound_audio_mixer_tests)

add_executable(protocol_audio_rate_bridge_tests tests/ProtocolAudioRateBridgeTests.cpp)
target_compile_features(protocol_audio_rate_bridge_tests PRIVATE cxx_std_20)
target_link_libraries(protocol_audio_rate_bridge_tests PRIVATE telestudio_audio)
add_test(NAME protocol_audio_rate_bridge COMMAND protocol_audio_rate_bridge_tests)

set(TELESTUDIO_AAC_LD_INSTALL
    "${_TELESTUDIO_REPOSITORY_ROOT}/work/build/aac-ld/install" CACHE PATH
    "Verified private FDK-AAC install root")
set(_TELESTUDIO_AAC_LD_INCLUDE "${TELESTUDIO_AAC_LD_INSTALL}/include/fdk-aac")
set(_TELESTUDIO_AAC_LD_LIBRARY "${TELESTUDIO_AAC_LD_INSTALL}/lib/fdk-aac.lib")
if(NOT EXISTS "${_TELESTUDIO_AAC_LD_INCLUDE}/aacenc_lib.h" OR
   NOT EXISTS "${_TELESTUDIO_AAC_LD_INCLUDE}/aacdecoder_lib.h" OR
   NOT EXISTS "${_TELESTUDIO_AAC_LD_LIBRARY}")
    message(FATAL_ERROR "Pinned FDK-AAC artifacts are required under ${TELESTUDIO_AAC_LD_INSTALL}")
endif()

add_library(telestudio_aac_ld STATIC
  src/AacLdCodec.cpp
  src/AacLdPayload.cpp)
target_compile_features(telestudio_aac_ld PUBLIC cxx_std_20)
target_include_directories(telestudio_aac_ld
    PUBLIC include
    PRIVATE "${_TELESTUDIO_AAC_LD_INCLUDE}")
target_link_libraries(telestudio_aac_ld PRIVATE "${_TELESTUDIO_AAC_LD_LIBRARY}")

add_executable(aac_ld_codec_tests tests/AacLdCodecTests.cpp)
target_compile_features(aac_ld_codec_tests PRIVATE cxx_std_20)
target_link_libraries(aac_ld_codec_tests PRIVATE telestudio_aac_ld)
add_test(NAME aac_ld_codec COMMAND aac_ld_codec_tests)

add_executable(aac_ld_payload_tests
    tests/AacLdPayloadTests.cpp)
target_link_libraries(aac_ld_payload_tests PRIVATE telestudio_aac_ld)
target_compile_features(aac_ld_payload_tests PRIVATE cxx_std_20)
target_include_directories(aac_ld_payload_tests PRIVATE include)
add_test(NAME aac_ld_payload COMMAND aac_ld_payload_tests)

add_executable(system_audio_capture_tests tests/SystemAudioCaptureTests.cpp)
target_compile_features(system_audio_capture_tests PRIVATE cxx_std_20)
target_link_libraries(system_audio_capture_tests PRIVATE telestudio_audio winmm)
add_test(NAME system_audio_capture COMMAND system_audio_capture_tests)

add_library(telestudio_recording STATIC
    src/RecordingAudioMixer.cpp
    src/RecordingOutputOverride.cpp
    src/MediaFoundationMp4Sink.cpp
    src/LectureRecorder.cpp
)
target_compile_features(telestudio_recording PUBLIC cxx_std_20)
target_include_directories(telestudio_recording PUBLIC include)
target_compile_definitions(telestudio_recording PRIVATE
    TELESTUDIO_CODEC_BIN_DIR=L"${TELESTUDIO_VIDEO_CODECS_BIN_DIR}"
)
target_link_libraries(telestudio_recording
    PUBLIC telestudio_audio telestudio_media telestudio_media_encoders
    PRIVATE telestudio_speexdsp mfplat mfreadwrite mfuuid ole32 shell32)

add_library(EssaStudio_CaptureCatalog SHARED
    src/CaptureCatalogC.cpp
    src/CaptureCatalogC.def
)
target_compile_features(EssaStudio_CaptureCatalog PRIVATE cxx_std_20)
target_include_directories(EssaStudio_CaptureCatalog PUBLIC include)
target_link_libraries(EssaStudio_CaptureCatalog PRIVATE telestudio_media runtimeobject bcrypt)
target_compile_options(EssaStudio_CaptureCatalog PRIVATE /EHa)
set_target_properties(EssaStudio_CaptureCatalog PROPERTIES
    OUTPUT_NAME "EssaStudio.CaptureCatalog"
    WINDOWS_EXPORT_ALL_SYMBOLS OFF)

add_executable(media_desktop_capture_source_tests tests/DesktopCaptureSourceTests.cpp)
target_compile_features(media_desktop_capture_source_tests PRIVATE cxx_std_20)
target_link_libraries(media_desktop_capture_source_tests PRIVATE telestudio_media)
add_test(NAME media_desktop_capture_source_tests COMMAND media_desktop_capture_source_tests)

add_executable(recording_audio_mixer_tests tests/RecordingAudioMixerTests.cpp)
target_compile_features(recording_audio_mixer_tests PRIVATE cxx_std_20)
target_link_libraries(recording_audio_mixer_tests PRIVATE telestudio_recording)
add_test(NAME recording_audio_mixer_tests COMMAND recording_audio_mixer_tests)

add_executable(media_foundation_mp4_sink_tests tests/MediaFoundationMp4SinkTests.cpp)
target_compile_features(media_foundation_mp4_sink_tests PRIVATE cxx_std_20)
target_link_libraries(media_foundation_mp4_sink_tests PRIVATE telestudio_recording)
add_test(NAME media_foundation_mp4_sink_tests COMMAND media_foundation_mp4_sink_tests)

add_executable(lecture_recorder_tests tests/LectureRecorderTests.cpp)
target_compile_features(lecture_recorder_tests PRIVATE cxx_std_20)
target_link_libraries(lecture_recorder_tests PRIVATE telestudio_recording)
target_link_libraries(lecture_recorder_tests PRIVATE winmm bcrypt mfplat mfreadwrite mfuuid ole32)
add_test(NAME lecture_recorder_tests COMMAND lecture_recorder_tests)

add_executable(capture_catalog_c_abi_tests tests/CaptureCatalogCAbiTests.cpp)
target_compile_features(capture_catalog_c_abi_tests PRIVATE cxx_std_20)
target_link_libraries(capture_catalog_c_abi_tests PRIVATE EssaStudio_CaptureCatalog telestudio_media)
add_test(NAME capture_catalog_c_abi_tests COMMAND capture_catalog_c_abi_tests)

add_executable(capture_catalog_c_contract_tests
    tests/CaptureCatalogCAbiTests.cpp
    src/CaptureCatalogC.cpp
)
target_compile_features(capture_catalog_c_contract_tests PRIVATE cxx_std_20)
target_include_directories(capture_catalog_c_contract_tests PRIVATE include)
target_compile_definitions(capture_catalog_c_contract_tests PRIVATE
    ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS=1)
target_compile_options(capture_catalog_c_contract_tests PRIVATE /EHa)
target_link_libraries(capture_catalog_c_contract_tests PRIVATE telestudio_media runtimeobject bcrypt)
add_test(NAME capture_catalog_c_contract_tests COMMAND capture_catalog_c_contract_tests)

add_executable(camera_catalog_c_contract_tests tests/CameraCatalogCAbiTests.cpp src/CaptureCatalogC.cpp)
target_compile_features(camera_catalog_c_contract_tests PRIVATE cxx_std_20)
target_include_directories(camera_catalog_c_contract_tests PRIVATE include)
target_compile_definitions(camera_catalog_c_contract_tests PRIVATE ESSA_CAPTURE_CATALOG_SYNTHETIC_TESTS=1)
target_compile_options(camera_catalog_c_contract_tests PRIVATE /EHa)
target_link_libraries(camera_catalog_c_contract_tests PRIVATE telestudio_media runtimeobject bcrypt)
add_test(NAME camera_catalog_c_contract_tests COMMAND camera_catalog_c_contract_tests)

add_executable(console_event_writer_tests
    tests/ConsoleEventWriterTests.cpp
    src/ConsoleEventWriter.cpp
)
target_compile_features(console_event_writer_tests PRIVATE cxx_std_20)
target_include_directories(console_event_writer_tests PRIVATE include)
add_test(NAME console_event_writer_tests COMMAND console_event_writer_tests)
