#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum EssaCaptureCatalogStatusV1 {
    ESSA_CAPTURE_OK = 0,
    ESSA_CAPTURE_BUFFER_TOO_SMALL = 1,
    ESSA_CAPTURE_CATALOG_CHANGED = 2,
    ESSA_CAPTURE_ABI_MISMATCH = 3,
    ESSA_CAPTURE_INVALID_ARGUMENT = 4,
    ESSA_CAPTURE_NOT_SUPPORTED = 5,
    ESSA_CAPTURE_ENUMERATION_FAILED = 6,
    ESSA_CAPTURE_LIMIT_EXCEEDED = 7,
    ESSA_CAPTURE_WRONG_APARTMENT = 8,
    ESSA_CAPTURE_INTERNAL_FAILURE = 9
} EssaCaptureCatalogStatusV1;

typedef enum EssaCaptureItemKindV1 {
    ESSA_CAPTURE_MONITOR = 1,
    ESSA_CAPTURE_WINDOW = 2
} EssaCaptureItemKindV1;

#pragma pack(push, 4)
typedef struct EssaCaptureCatalogItemV1 {
    uint32_t structSize;
    uint32_t kind;
    uint32_t width;
    uint32_t height;
    uint32_t idOffset;
    uint32_t idLength;
    uint32_t nameOffset;
    uint32_t nameLength;
} EssaCaptureCatalogItemV1;
#pragma pack(pop)

__declspec(dllexport) uint32_t __cdecl EssaCaptureCatalogAbiVersion(void);
// Separate camera enumeration: same record layout, kind=3, unknown width/height=0.
// No device activation. Missing export on older DLLs means cameras unavailable.
__declspec(dllexport) int32_t __cdecl EssaCameraCatalogQueryV1(
    uint32_t abiVersion, uint64_t expectedSnapshot, EssaCaptureCatalogItemV1* items,
    uint32_t itemCapacity, uint8_t* utf8, uint32_t utf8Capacity,
    uint32_t* outRequiredItems, uint32_t* outRequiredUtf8, uint64_t* outSnapshot);
__declspec(dllexport) int32_t __cdecl EssaCaptureCatalogQueryV1(
    uint32_t abiVersion,
    uint64_t expectedSnapshot,
    EssaCaptureCatalogItemV1* items,
    uint32_t itemCapacity,
    uint8_t* utf8,
    uint32_t utf8Capacity,
    uint32_t* outRequiredItems,
    uint32_t* outRequiredUtf8,
    uint64_t* outSnapshot);

#ifdef __cplusplus
}
#endif
