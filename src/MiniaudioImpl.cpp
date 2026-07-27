// Single translation unit for the vendored audio stack. miniaudio picks up
// stb_vorbis for .ogg when its declarations precede the implementation and
// the implementation follows it (documented miniaudio pattern).
#pragma warning(push)
#pragma warning(disable : 4100 4189 4244 4245 4267 4456 4457 4701)
#define STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_ENABLE_ONLY_SPECIFIC_BACKENDS
#define MA_ENABLE_WASAPI
#include "miniaudio/miniaudio.h"

// libopus decoding backend (spec Q5). The vendored miniaudio 0.11.21 ships
// extras/miniaudio_libopus.h as a DATA SOURCE only; it does NOT bundle a
// ma_decoding_backend_vtable (the g_ma_decoding_backend_vtable_libopus symbol
// belongs to a later restructuring of miniaudio's extras that is not
// API-compatible with 0.11.21). So we compile the data source here - its
// implementation is gated on MINIAUDIO_IMPLEMENTATION, defined just above -
// and hand-write the small backend vtable glue the resource manager's
// ppCustomDecodingBackendVTables expects, exactly as miniaudio's
// custom_decoders example prescribes. libopus/libopusfile are linked in via
// vcpkg (see CMakeLists.txt); <opusfile.h> resolves through Opus::opus's
// interface include dir.
#include "miniaudio/ma_libopus.h"

static ma_result sh_libopus_backend_init(
    void* pUserData, ma_read_proc onRead, ma_seek_proc onSeek,
    ma_tell_proc onTell, void* pReadSeekTellUserData,
    const ma_decoding_backend_config* pConfig,
    const ma_allocation_callbacks* pAllocationCallbacks,
    ma_data_source** ppBackend) {
    (void)pUserData;
    ma_libopus* pOpus =
        static_cast<ma_libopus*>(ma_malloc(sizeof(*pOpus), pAllocationCallbacks));
    if (pOpus == nullptr) return MA_OUT_OF_MEMORY;
    ma_result result = ma_libopus_init(onRead, onSeek, onTell,
                                       pReadSeekTellUserData, pConfig,
                                       pAllocationCallbacks, pOpus);
    if (result != MA_SUCCESS) {
        ma_free(pOpus, pAllocationCallbacks);
        return result;
    }
    *ppBackend = pOpus;
    return MA_SUCCESS;
}

static ma_result sh_libopus_backend_init_file(
    void* pUserData, const char* pFilePath,
    const ma_decoding_backend_config* pConfig,
    const ma_allocation_callbacks* pAllocationCallbacks,
    ma_data_source** ppBackend) {
    (void)pUserData;
    ma_libopus* pOpus =
        static_cast<ma_libopus*>(ma_malloc(sizeof(*pOpus), pAllocationCallbacks));
    if (pOpus == nullptr) return MA_OUT_OF_MEMORY;
    ma_result result =
        ma_libopus_init_file(pFilePath, pConfig, pAllocationCallbacks, pOpus);
    if (result != MA_SUCCESS) {
        ma_free(pOpus, pAllocationCallbacks);
        return result;
    }
    *ppBackend = pOpus;
    return MA_SUCCESS;
}

static void sh_libopus_backend_uninit(
    void* pUserData, ma_data_source* pBackend,
    const ma_allocation_callbacks* pAllocationCallbacks) {
    (void)pUserData;
    ma_libopus* pOpus = static_cast<ma_libopus*>(static_cast<void*>(pBackend));
    ma_libopus_uninit(pOpus, pAllocationCallbacks);
    ma_free(pOpus, pAllocationCallbacks);
}

// External symbol consumed by AudioEngine.cpp's resource-manager wiring. Same
// (C++) linkage there via a matching extern declaration.
ma_decoding_backend_vtable g_ma_decoding_backend_vtable_libopus = {
    sh_libopus_backend_init,
    sh_libopus_backend_init_file,
    nullptr,  // onInitFileW  - optional; RM falls back to its generic path
    nullptr,  // onInitMemory - optional
    sh_libopus_backend_uninit,
};

#undef STB_VORBIS_HEADER_ONLY
#include "stb/stb_vorbis.c"
#pragma warning(pop)
