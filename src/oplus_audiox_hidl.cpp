#include "rynx_audiox_dsp.h"

#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <type_traits>
#include <vector>

#define LOG_TAG "OplusAudioxHidlShim"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EFFECT_LIBRARY_API_VERSION 0x00030000
#define EFFECT_CONTROL_API_VERSION 0x00020000

#define EFFECT_FLAG_TYPE_INSERT         (0x0 << 0)
#define EFFECT_FLAG_INSERT_LAST         (0x2 << 3)
#define EFFECT_FLAG_VOLUME_IND          (0x2 << 6)
#define EFFECT_FLAG_DEVICE_IND          (0x1 << 9)
#define EFFECT_FLAG_AUDIO_MODE_IND      (0x1 << 14)

#define AUDIO_FORMAT_PCM_16_BIT         1
#define AUDIO_FORMAT_PCM_FLOAT          5

#define EFFECT_BUFFER_ACCESS_WRITE      0
#define EFFECT_BUFFER_ACCESS_READ       1
#define EFFECT_BUFFER_ACCESS_ACCUMULATE 2

enum effect_command_e {
    EFFECT_CMD_INIT = 0,
    EFFECT_CMD_SET_CONFIG = 1,
    EFFECT_CMD_RESET = 2,
    EFFECT_CMD_ENABLE = 3,
    EFFECT_CMD_DISABLE = 4,
    EFFECT_CMD_SET_PARAM = 5,
    EFFECT_CMD_SET_PARAM_DEFERRED = 6,
    EFFECT_CMD_SET_PARAM_COMMIT = 7,
    EFFECT_CMD_GET_PARAM = 8,
    EFFECT_CMD_SET_DEVICE = 9,
    EFFECT_CMD_SET_VOLUME = 10,
    EFFECT_CMD_SET_AUDIO_MODE = 11,
    EFFECT_CMD_SET_CONFIG_REVERSE = 12,
    EFFECT_CMD_SET_INPUT_DEVICE = 13,
    EFFECT_CMD_GET_CONFIG = 14,
    EFFECT_CMD_GET_CONFIG_REVERSE = 15,
    EFFECT_CMD_GET_FEATURE_SUPPORTED_CONFIGS = 16,
    EFFECT_CMD_GET_FEATURE_CONFIG = 17,
    EFFECT_CMD_SET_FEATURE_CONFIG = 18,
    EFFECT_CMD_SET_AUDIO_SOURCE = 19,
    EFFECT_CMD_OFFLOAD = 20,
};

typedef struct effect_uuid_s {
    uint32_t timeLow;
    uint16_t timeMid;
    uint16_t timeHiAndVersion;
    uint16_t clockSeq;
    uint8_t node[6];
} effect_uuid_t;

typedef struct effect_descriptor_s {
    effect_uuid_t type;
    effect_uuid_t uuid;
    uint32_t apiVersion;
    uint32_t flags;
    uint16_t cpuLoad;
    uint16_t memoryUsage;
    char name[64];
    char implementor[64];
} effect_descriptor_t;

typedef struct audio_buffer_s {
    size_t frameCount;
    union {
        void* raw;
        float* f32;
        int32_t* s32;
        int16_t* s16;
        uint8_t* u8;
    };
} audio_buffer_t;

typedef struct buffer_provider_s {
    int32_t (*getBuffer)(void* cookie, audio_buffer_t* buffer);
    void (*releaseBuffer)(void* cookie, audio_buffer_t* buffer);
    void* cookie;
} buffer_provider_t;

typedef struct buffer_config_s {
    audio_buffer_t buffer;
    uint32_t samplingRate;
    uint32_t channels;
    buffer_provider_t bufferProvider;
    uint8_t format;
    uint8_t accessMode;
    uint16_t mask;
} buffer_config_t;

typedef struct effect_config_s {
    buffer_config_t inputCfg;
    buffer_config_t outputCfg;
} effect_config_t;

typedef struct effect_param_s {
    int32_t status;
    uint32_t psize;
    uint32_t vsize;
    char data[];
} effect_param_t;

typedef struct effect_offload_param_s {
    bool isOffload;
    int32_t ioHandle;
} effect_offload_param_t;

struct effect_interface_s;
typedef struct effect_interface_s** effect_handle_t;

struct effect_interface_s {
    int32_t (*process)(effect_handle_t self, audio_buffer_t* inBuffer, audio_buffer_t* outBuffer);
    int32_t (*command)(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize, void* pCmdData,
                       uint32_t* replySize, void* pReplyData);
    int32_t (*get_descriptor)(effect_handle_t self, effect_descriptor_t* pDescriptor);
    int32_t (*process_reverse)(effect_handle_t self, audio_buffer_t* inBuffer, audio_buffer_t* outBuffer);
};

typedef struct audio_effect_library_s {
    uint32_t tag;
    uint32_t version;
    const char* name;
    const char* implementor;
    int32_t (*create_effect)(const effect_uuid_t* uuid, int32_t sessionId, int32_t ioId,
                             effect_handle_t* pHandle);
    int32_t (*release_effect)(effect_handle_t handle);
    int32_t (*get_descriptor)(const effect_uuid_t* uuid, effect_descriptor_t* pDescriptor);
} audio_effect_library_t;

static_assert(sizeof(buffer_config_t) == 0x38, "buffer_config_t size must match 64-bit AOSP ABI");
static_assert(sizeof(effect_config_t) == 0x70, "effect_config_t size must match 64-bit AOSP ABI");

static const effect_uuid_t kAudioxUuid = {
    0x41f6c0f4, 0x5d8f, 0x11ec, 0xbf63, {0x02, 0x42, 0xac, 0x13, 0x00, 0x02}
};

static const effect_descriptor_t kAudioxDescriptor = {
    kAudioxUuid,
    kAudioxUuid,
    EFFECT_CONTROL_API_VERSION,
    EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_LAST | EFFECT_FLAG_VOLUME_IND |
        EFFECT_FLAG_DEVICE_IND | EFFECT_FLAG_AUDIO_MODE_IND,
    0,
    1,
    "OplusAudiox",
    "OPLUS"
};

namespace {

constexpr size_t kOplusAudioxEffectSize = 0xde38;

using CtorFn      = void (*)(void* self, size_t statusDepth, const void* common);
using DtorFn      = void (*)(void* self);
using VoidIntFn   = int (*)(void* self);
using SetConfigFn = int (*)(void* self, effect_config_t* cfg);
using GetConfigFn = int (*)(void* self, effect_config_t* cfg);
using SetParamFn  = int (*)(void* self, int paramId, unsigned int valueSize, void* value);
using GetParamFn  = int (*)(void* self, int paramId, unsigned int* valueSize, void* value);
using SetDeviceFn = int (*)(void* self, uint32_t device);
using SetVolumeFn = int (*)(void* self, int numChannels, const uint32_t* volumes);
using OffloadFn   = int (*)(void* self, bool isOffload, int ioHandle);
using ProcessFn   = int (*)(void* self, audio_buffer_t* inBuf, audio_buffer_t* outBuf);

struct AidlSyms {
    void* handle = nullptr;
    void* algoHandle = nullptr;
    CtorFn ctor = nullptr;
    DtorFn dtor = nullptr;
    VoidIntFn init = nullptr;
    VoidIntFn reset = nullptr;
    VoidIntFn enable = nullptr;
    VoidIntFn disable = nullptr;
    VoidIntFn release = nullptr;
    SetConfigFn setConfig = nullptr;
    GetConfigFn getConfig = nullptr;
    SetParamFn setParam = nullptr;
    GetParamFn getParam = nullptr;
    SetDeviceFn setDevice = nullptr;
    SetVolumeFn setVolume = nullptr;
    OffloadFn offload = nullptr;
    ProcessFn process = nullptr;
    rynx::SpatGetEnFn spatGetEn = nullptr;
    rynx::SpatSetEnFn spatSetEn = nullptr;
    bool loaded = false;
};

AidlSyms gSyms;

bool loadAidlLib() {
    if (gSyms.loaded) return true;
    const char* paths[] = {
        "/vendor/lib64/soundfx/libOplusAudioxAidl.so",
        "libOplusAudioxAidl.so",
    };
    for (const char* p : paths) {
        gSyms.handle = dlopen(p, RTLD_NOW | RTLD_LOCAL);
        if (gSyms.handle) {
            ALOGI("Loaded %s", p);
            break;
        }
        ALOGE("dlopen(%s) failed: %s", p, dlerror());
    }
    if (!gSyms.handle) return false;

    auto resolve = [&](auto& fn, const char* sym) -> bool {
        fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(gSyms.handle, sym));
        if (!fn) {
            ALOGE("dlsym(%s) failed: %s", sym, dlerror());
            return false;
        }
        return true;
    };

    bool ok = true;
    ok &= resolve(gSyms.ctor, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffectC1EmRKNS3_9Parameter6CommonE");
    ok &= resolve(gSyms.dtor, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffectD1Ev");
    ok &= resolve(gSyms.init, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect4initEv");
    ok &= resolve(gSyms.reset, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect5resetEv");
    ok &= resolve(gSyms.enable, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect6enableEv");
    ok &= resolve(gSyms.disable, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect7disableEv");
    ok &= resolve(gSyms.release, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect7releaseEv");
    ok &= resolve(gSyms.setConfig, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9setConfigEP15effect_config_s");
    ok &= resolve(gSyms.getConfig, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9getConfigEP15effect_config_s");
    ok &= resolve(gSyms.setParam, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect8setParamEijPv");
    ok &= resolve(gSyms.getParam, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect8getParamEiPjPv");
    ok &= resolve(gSyms.setDevice, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9setDeviceE15audio_devices_t");
    ok &= resolve(gSyms.setVolume, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9setVolumeEiPKj");
    ok &= resolve(gSyms.offload, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect7offloadEbi");
    ok &= resolve(gSyms.process, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect7processEP14audio_buffer_sS6_");

    gSyms.algoHandle = dlopen("/vendor/lib64/liboplus_audiox.so", RTLD_NOW | RTLD_NOLOAD);
    if (!gSyms.algoHandle) {
        gSyms.algoHandle = dlopen("/vendor/lib64/liboplus_audiox.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (gSyms.algoHandle) {
        gSyms.spatGetEn = reinterpret_cast<rynx::SpatGetEnFn>(dlsym(gSyms.algoHandle, "oplus_algo_spat_get_enable_ID"));
        gSyms.spatSetEn = reinterpret_cast<rynx::SpatSetEnFn>(dlsym(gSyms.algoHandle, "oplus_algo_spat_set_enable_ID"));
    }

    gSyms.loaded = ok;
    return ok;
}

void buildDefaultCommon(uint8_t buf[512], int32_t sessionId, int32_t ioId) {
    memset(buf, 0, 512);
    auto write32 = [&](size_t off, uint32_t v) { memcpy(buf + off, &v, 4); };
    auto write64 = [&](size_t off, uint64_t v) { memcpy(buf + off, &v, 8); };

    write32(0x00, static_cast<uint32_t>(sessionId));
    write32(0x04, static_cast<uint32_t>(ioId));

    write32(0x08, rynx::kAudioxTargetRate);
    write32(0x0c, 0x3);
    write32(0x10, 3);
    buf[0x18] = 1;
    buf[0x19] = 4;
    write64(0x98, 256);

    write32(0xa0, rynx::kAudioxTargetRate);
    write32(0xa4, 0x3);
    write32(0xa8, 3);
    buf[0xb0] = 1;
    buf[0xb1] = 4;
    write64(0x130, 256);
}

struct AudioxContext {
    const struct effect_interface_s* itfe = nullptr;
    void* impl = nullptr;
    bool resampleActive = false;
    uint8_t hostFormat = AUDIO_FORMAT_PCM_FLOAT;
    uint8_t outAccessMode = EFFECT_BUFFER_ACCESS_WRITE;
    float prevOut48[2] = {0.0f, 0.0f};
    rynx::RynxDspState dsp;
    std::vector<float> bufIn48;
    std::vector<float> bufOut48;
};

int32_t processResampledFloat(AudioxContext* ctx, audio_buffer_t* inBuffer, audio_buffer_t* outBuffer) {
    const size_t inFrames = inBuffer->frameCount;
    const size_t outFrames = outBuffer->frameCount;
    if (inFrames == 0 || outFrames == 0) return 0;

    const size_t frames48 = (inFrames * rynx::kAudioxTargetRate + ctx->dsp.hostRate / 2) / ctx->dsp.hostRate;
    if (frames48 == 0) return 0;

    ctx->bufIn48.resize(frames48 * 2);
    ctx->bufOut48.resize(frames48 * 2);

    const float* src = inBuffer->f32;
    float* dst48 = ctx->bufIn48.data();

    if (ctx->dsp.hostRate == 96000 && inFrames == frames48 * 2) {
        for (size_t i = 0; i < frames48; ++i) {
            dst48[2 * i]     = 0.5f * (src[4 * i]     + src[4 * i + 2]);
            dst48[2 * i + 1] = 0.5f * (src[4 * i + 1] + src[4 * i + 3]);
        }
    } else {
        const double step = static_cast<double>(inFrames) / static_cast<double>(frames48);
        for (size_t i = 0; i < frames48; ++i) {
            double pos = i * step;
            size_t idx = static_cast<size_t>(pos);
            float frac = static_cast<float>(pos - idx);
            size_t idx2 = (idx + 1 < inFrames) ? (idx + 1) : (inFrames - 1);
            dst48[2 * i]     = src[2 * idx]     * (1.0f - frac) + src[2 * idx2]     * frac;
            dst48[2 * i + 1] = src[2 * idx + 1] * (1.0f - frac) + src[2 * idx2 + 1] * frac;
        }
    }

    audio_buffer_t bIn48{frames48, {ctx->bufIn48.data()}};
    audio_buffer_t bOut48{frames48, {ctx->bufOut48.data()}};
    memset(ctx->bufOut48.data(), 0, frames48 * 2 * sizeof(float));

    int32_t ret = gSyms.process(ctx->impl, &bIn48, &bOut48);
    if (ret != 0) return ret;

    const float* p48 = ctx->bufOut48.data();
    float* dst = outBuffer->f32;
    const bool accum = (ctx->outAccessMode == EFFECT_BUFFER_ACCESS_ACCUMULATE);

    if (ctx->dsp.hostRate == 96000 && outFrames == frames48 * 2) {
        float prevL = ctx->prevOut48[0];
        float prevR = ctx->prevOut48[1];
        for (size_t i = 0; i < frames48; ++i) {
            float curL = p48[2 * i];
            float curR = p48[2 * i + 1];
            float midL = 0.5f * (prevL + curL);
            float midR = 0.5f * (prevR + curR);
            if (accum) {
                dst[4 * i]     += midL;
                dst[4 * i + 1] += midR;
                dst[4 * i + 2] += curL;
                dst[4 * i + 3] += curR;
            } else {
                dst[4 * i]     = midL;
                dst[4 * i + 1] = midR;
                dst[4 * i + 2] = curL;
                dst[4 * i + 3] = curR;
            }
            prevL = curL;
            prevR = curR;
        }
        ctx->prevOut48[0] = prevL;
        ctx->prevOut48[1] = prevR;
    } else {
        const double step = static_cast<double>(frames48) / static_cast<double>(outFrames);
        for (size_t i = 0; i < outFrames; ++i) {
            double pos = i * step;
            size_t idx = static_cast<size_t>(pos);
            float frac = static_cast<float>(pos - idx);
            size_t idx2 = (idx + 1 < frames48) ? (idx + 1) : (frames48 - 1);
            float l = p48[2 * idx]     * (1.0f - frac) + p48[2 * idx2]     * frac;
            float r = p48[2 * idx + 1] * (1.0f - frac) + p48[2 * idx2 + 1] * frac;
            if (accum) {
                dst[2 * i]     += l;
                dst[2 * i + 1] += r;
            } else {
                dst[2 * i]     = l;
                dst[2 * i + 1] = r;
            }
        }
        ctx->prevOut48[0] = p48[2 * (frames48 - 1)];
        ctx->prevOut48[1] = p48[2 * (frames48 - 1) + 1];
    }
    return 0;
}

int32_t audioxProcess(effect_handle_t self, audio_buffer_t* inBuffer, audio_buffer_t* outBuffer) {
    auto* ctx = reinterpret_cast<AudioxContext*>(self);
    if (!ctx || !ctx->impl || !inBuffer || !outBuffer) return -EINVAL;

    rynx::ensureSpeakerImmersiveBrir(ctx->impl, ctx->dsp, gSyms.spatGetEn, gSyms.spatSetEn, LOG_TAG);

    int32_t ret = 0;
    if (ctx->resampleActive && ctx->hostFormat == AUDIO_FORMAT_PCM_FLOAT) {
        ret = processResampledFloat(ctx, inBuffer, outBuffer);
    } else {
        ret = gSyms.process(ctx->impl, inBuffer, outBuffer);
    }
    if (ret == 0 && ctx->hostFormat == AUDIO_FORMAT_PCM_FLOAT) {
        rynx::applyModeDsp(ctx->dsp, outBuffer->f32, outBuffer->frameCount);
        rynx::applyVolumeLeveler(ctx->dsp, outBuffer->f32, outBuffer->frameCount, LOG_TAG);
    }
    return ret;
}

int32_t audioxCommand(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize, void* pCmdData,
                      uint32_t* replySize, void* pReplyData) {
    auto* ctx = reinterpret_cast<AudioxContext*>(self);
    if (!ctx || !ctx->impl) return -EINVAL;

    auto setReplyStatus = [&](int32_t status) {
        if (replySize && pReplyData && *replySize >= sizeof(int32_t)) {
            *reinterpret_cast<int32_t*>(pReplyData) = status;
            *replySize = sizeof(int32_t);
        }
    };

    switch (cmdCode) {
        case EFFECT_CMD_INIT: {
            int ret = gSyms.init(ctx->impl);
            setReplyStatus(ret);
            return 0;
        }
        case EFFECT_CMD_SET_CONFIG: {
            if (!pCmdData || cmdSize < sizeof(effect_config_t)) return -EINVAL;
            auto* cfg = reinterpret_cast<effect_config_t*>(pCmdData);
            ctx->dsp.hostRate = cfg->inputCfg.samplingRate;
            ctx->hostFormat = cfg->inputCfg.format;
            ctx->outAccessMode = cfg->outputCfg.accessMode;
            ctx->dsp.refreshProps();
            ctx->resampleActive = false;
            ctx->prevOut48[0] = 0.0f;
            ctx->prevOut48[1] = 0.0f;

            if (cfg->inputCfg.samplingRate != rynx::kAudioxTargetRate &&
                cfg->inputCfg.samplingRate == cfg->outputCfg.samplingRate &&
                cfg->inputCfg.format == AUDIO_FORMAT_PCM_FLOAT &&
                rynx::isDownsampleEnabled()) {
                effect_config_t cfg48 = *cfg;
                cfg48.inputCfg.samplingRate = rynx::kAudioxTargetRate;
                cfg48.outputCfg.samplingRate = rynx::kAudioxTargetRate;
                cfg48.outputCfg.accessMode = EFFECT_BUFFER_ACCESS_WRITE;
                int ret = gSyms.setConfig(ctx->impl, &cfg48);
                if (ret == 0) {
                    ctx->resampleActive = true;
                    ALOGI("Downsampling enabled (%s=true): %u Hz <-> 48000 Hz",
                          rynx::kDownsampleProp, ctx->dsp.hostRate);
                }
                setReplyStatus(ret);
                return 0;
            }

            int ret = gSyms.setConfig(ctx->impl, cfg);
            setReplyStatus(ret);
            return 0;
        }
        case EFFECT_CMD_GET_CONFIG: {
            if (!pReplyData || !replySize || *replySize < sizeof(effect_config_t)) return -EINVAL;
            auto* cfg = reinterpret_cast<effect_config_t*>(pReplyData);
            int ret = gSyms.getConfig(ctx->impl, cfg);
            if (ret == 0 && ctx->resampleActive) {
                cfg->inputCfg.samplingRate = ctx->dsp.hostRate;
                cfg->outputCfg.samplingRate = ctx->dsp.hostRate;
                cfg->outputCfg.accessMode = ctx->outAccessMode;
            }
            *replySize = sizeof(effect_config_t);
            return ret;
        }
        case EFFECT_CMD_RESET: {
            ctx->prevOut48[0] = 0.0f;
            ctx->prevOut48[1] = 0.0f;
            ctx->dsp.modeDsp.reset();
            gSyms.reset(ctx->impl);
            return 0;
        }
        case EFFECT_CMD_ENABLE: {
            ctx->dsp.refreshProps();
            int ret = gSyms.enable(ctx->impl);
            setReplyStatus(ret);
            return 0;
        }
        case EFFECT_CMD_DISABLE: {
            int ret = gSyms.disable(ctx->impl);
            setReplyStatus(ret);
            return 0;
        }
        case EFFECT_CMD_SET_PARAM:
        case EFFECT_CMD_SET_PARAM_DEFERRED: {
            if (!pCmdData || cmdSize < sizeof(effect_param_t) + sizeof(int32_t)) return -EINVAL;
            auto* p = reinterpret_cast<effect_param_t*>(pCmdData);
            uint32_t voffset = (p->psize + 3) & ~3u;
            if (cmdSize < sizeof(effect_param_t) + voffset + p->vsize) return -EINVAL;
            int paramId = *reinterpret_cast<int32_t*>(p->data);
            void* val = p->data + voffset;
            int32_t volIdxOverride = 0;
            void* forwardVal = rynx::trackAudioxParam(ctx->dsp, paramId, p->vsize, val, volIdxOverride, LOG_TAG);
            int ret = gSyms.setParam(ctx->impl, paramId, p->vsize, forwardVal);
            setReplyStatus(ret);
            return 0;
        }
        case EFFECT_CMD_SET_PARAM_COMMIT: {
            setReplyStatus(0);
            return 0;
        }
        case EFFECT_CMD_GET_PARAM: {
            if (!pCmdData || !pReplyData || !replySize || cmdSize < sizeof(effect_param_t) + sizeof(int32_t)) {
                return -EINVAL;
            }
            auto* pIn = reinterpret_cast<effect_param_t*>(pCmdData);
            auto* pOut = reinterpret_cast<effect_param_t*>(pReplyData);
            uint32_t voffset = (pIn->psize + 3) & ~3u;
            if (*replySize < sizeof(effect_param_t) + voffset) return -EINVAL;
            memcpy(pOut, pIn, sizeof(effect_param_t) + pIn->psize);
            unsigned int vsize = *replySize - static_cast<uint32_t>(sizeof(effect_param_t) + voffset);
            int paramId = *reinterpret_cast<int32_t*>(pOut->data);
            int ret = gSyms.getParam(ctx->impl, paramId, &vsize, pOut->data + voffset);
            pOut->status = ret;
            pOut->vsize = (ret == 0) ? vsize : 0;
            *replySize = static_cast<uint32_t>(sizeof(effect_param_t) + voffset + pOut->vsize);
            return 0;
        }
        case EFFECT_CMD_SET_DEVICE: {
            if (!pCmdData || cmdSize < sizeof(uint32_t)) return -EINVAL;
            ctx->dsp.currentDevice = *reinterpret_cast<uint32_t*>(pCmdData);
            return gSyms.setDevice(ctx->impl, ctx->dsp.currentDevice);
        }
        case EFFECT_CMD_SET_VOLUME: {
            if (!pCmdData || cmdSize < sizeof(uint32_t)) return -EINVAL;
            int numCh = static_cast<int>(cmdSize / sizeof(uint32_t));
            const auto* vols = reinterpret_cast<const uint32_t*>(pCmdData);
            uint32_t maxV = 0;
            for (int i = 0; i < numCh; ++i) {
                if (vols[i] > maxV) maxV = vols[i];
            }
            rynx::updateLinearVolume(ctx->dsp, static_cast<float>(maxV) * 5.9604645e-08f);
            gSyms.setVolume(ctx->impl, numCh, vols);
            if (pReplyData && replySize && *replySize >= cmdSize) {
                memcpy(pReplyData, pCmdData, cmdSize);
                *replySize = cmdSize;
            }
            return 0;
        }
        case EFFECT_CMD_OFFLOAD: {
            if (!pCmdData || cmdSize < sizeof(effect_offload_param_t)) return -EINVAL;
            auto* op = reinterpret_cast<effect_offload_param_t*>(pCmdData);
            int ret = gSyms.offload(ctx->impl, op->isOffload, op->ioHandle);
            setReplyStatus(ret);
            return 0;
        }
        case EFFECT_CMD_SET_AUDIO_MODE:
        case EFFECT_CMD_SET_INPUT_DEVICE:
        case EFFECT_CMD_SET_AUDIO_SOURCE:
            return 0;
        default:
            return -EINVAL;
    }
}

int32_t audioxGetDescriptor(effect_handle_t self, effect_descriptor_t* pDescriptor) {
    if (!self || !pDescriptor) return -EINVAL;
    *pDescriptor = kAudioxDescriptor;
    return 0;
}

const struct effect_interface_s kAudioxInterface = {
    audioxProcess,
    audioxCommand,
    audioxGetDescriptor,
    nullptr,
};

int32_t libCreateEffect(const effect_uuid_t* uuid, int32_t sessionId, int32_t ioId,
                        effect_handle_t* pHandle) {
    if (!uuid || !pHandle) return -EINVAL;
    if (memcmp(uuid, &kAudioxUuid, sizeof(effect_uuid_t)) != 0) return -ENOENT;
    if (!loadAidlLib()) return -ENOSYS;

    void* mem = nullptr;
    if (posix_memalign(&mem, 16, kOplusAudioxEffectSize) != 0 || !mem) return -ENOMEM;
    memset(mem, 0, kOplusAudioxEffectSize);

    alignas(16) uint8_t commonBuf[512];
    buildDefaultCommon(commonBuf, sessionId, ioId);
    gSyms.ctor(mem, 1, commonBuf);

    auto* ctx = new AudioxContext{};
    ctx->itfe = &kAudioxInterface;
    ctx->impl = mem;
    ctx->dsp.refreshProps();
    *pHandle = reinterpret_cast<effect_handle_t>(ctx);
    ALOGI("Created OplusAudiox HIDL-bridged effect (session=%d io=%d)", sessionId, ioId);
    return 0;
}

int32_t libReleaseEffect(effect_handle_t handle) {
    auto* ctx = reinterpret_cast<AudioxContext*>(handle);
    if (!ctx) return -EINVAL;
    if (ctx->impl) {
        gSyms.release(ctx->impl);
        gSyms.dtor(ctx->impl);
        free(ctx->impl);
    }
    delete ctx;
    ALOGI("Released OplusAudiox HIDL-bridged effect");
    return 0;
}

int32_t libGetDescriptor(const effect_uuid_t* uuid, effect_descriptor_t* pDescriptor) {
    if (!uuid || !pDescriptor) return -EINVAL;
    if (memcmp(uuid, &kAudioxUuid, sizeof(effect_uuid_t)) != 0) return -ENOENT;
    *pDescriptor = kAudioxDescriptor;
    return 0;
}

} // namespace

#define AUDIO_EFFECT_LIBRARY_INFO_SYM AELI
extern "C" __attribute__((visibility("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
    .tag = 0x41454c54,
    .version = EFFECT_LIBRARY_API_VERSION,
    .name = "OplusAudiox HIDL Shim",
    .implementor = "OPLUS",
    .create_effect = libCreateEffect,
    .release_effect = libReleaseEffect,
    .get_descriptor = libGetDescriptor,
};
