#pragma once

#include "telestudio/media/MediaTypes.h"

namespace telestudio {

[[nodiscard]] MediaFormat ResolveMediaFormat(EndpointModel endpoint,
                                              Protocol protocol,
                                              VideoSource source,
                                              QualityMode quality,
                                              const RemoteCapabilities& capabilities,
                                              const VideoPreferences& preferences = {});

} // namespace telestudio
