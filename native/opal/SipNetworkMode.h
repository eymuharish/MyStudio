#pragma once

namespace telestudio::sip {

enum class SipNetworkMode { Direct, External };

struct SipAudioJitterPolicy {
  bool overrideDefaults = false;
  int initialMs = -1;
  int minimumPrefetchMs = -1;
  int maximumPrefetchMs = -1;
  int maximumMs = -1;
};

constexpr SipAudioJitterPolicy BuildSipAudioJitterPolicy(
    SipNetworkMode mode) noexcept {
  if (mode == SipNetworkMode::External) return {true, 100, 100, 600, 600};
  return {};
}

}  // namespace telestudio::sip
