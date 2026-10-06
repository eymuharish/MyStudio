#pragma once

// Register before loading mystudio_opal_aac.dll, including PProcess startup.
bool RegisterMyStudioOpalAacHandler();

// Register after PProcess/plugin loading, before constructing H.323 endpoints.
bool RegisterMyStudioOpalAacCapabilities();
