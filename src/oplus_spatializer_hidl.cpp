#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <type_traits>

#define LOG_TAG "OplusSpatializerHidlShim"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define EFFECT_LIBRARY_API_VERSION 0x00030000
#define EFFECT_CONTROL_API_VERSION 0x00020000

#define EFFECT_FLAG_TYPE_INSERT         (0x0 << 0)
#define EFFECT_FLAG_INSERT_LAST         (0x2 << 3)
#define EFFECT_FLAG_VOLUME_IND          (0x2 << 6)
#define EFFECT_FLAG_DEVICE_IND          (0x1 << 9)
#define EFFECT_FLAG_AUDIO_MODE_IND      (0x1 << 14)

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

static const effect_uuid_t kSpatializerTypeUuid = {
    0xccd4cf09, 0xa79d, 0x46c2, 0x9aae, {0x06, 0xa1, 0x69, 0x8d, 0x6c, 0x8f}
};

static const effect_uuid_t kSpatializerImplUuid = {
    0xde4d6aaf, 0x4e9d, 0x473d, 0x8168, {0x4c, 0xe9, 0x2d, 0x50, 0xa6, 0x37}
};

static const effect_descriptor_t kSpatializerDescriptor = {
    kSpatializerTypeUuid,
    kSpatializerImplUuid,
    EFFECT_CONTROL_API_VERSION,
    EFFECT_FLAG_TYPE_INSERT | EFFECT_FLAG_INSERT_LAST | EFFECT_FLAG_VOLUME_IND |
        EFFECT_FLAG_DEVICE_IND | EFFECT_FLAG_AUDIO_MODE_IND,
    0,
    1,
    "OplusSpatializer",
    "OPLUS"
};

namespace {

constexpr size_t kOplusSpatializerEffectSize = 0x50f10;

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
    bool loaded = false;
};

AidlSyms gSyms;

bool loadAidlLib() {
    if (gSyms.loaded) return true;
    const char* paths[] = {
        "/vendor/lib64/soundfx/libOplusSpatializerAidl.so",
        "libOplusSpatializerAidl.so",
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
    ok &= resolve(gSyms.ctor, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffectC1EmRKNS3_9Parameter6CommonE");
    ok &= resolve(gSyms.dtor, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffectD1Ev");
    ok &= resolve(gSyms.init, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect4initEv");
    ok &= resolve(gSyms.reset, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect5resetEv");
    ok &= resolve(gSyms.enable, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect6enableEv");
    ok &= resolve(gSyms.disable, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect7disableEv");
    ok &= resolve(gSyms.release, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect7releaseEv");
    ok &= resolve(gSyms.setConfig, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect9setConfigEP15effect_config_s");
    ok &= resolve(gSyms.getConfig, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect9getConfigEP15effect_config_s");
    ok &= resolve(gSyms.setParam, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect8setParamEijPv");
    ok &= resolve(gSyms.getParam, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect8getParamEiPjPv");
    ok &= resolve(gSyms.setDevice, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect9setDeviceE15audio_devices_t");
    ok &= resolve(gSyms.setVolume, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect9setVolumeEiPKj");
    ok &= resolve(gSyms.offload, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect7offloadEbi");
    ok &= resolve(gSyms.process, "_ZN4aidl7android8hardware5audio6effect22OplusSpatializerEffect7processEP14audio_buffer_sS6_");

    gSyms.loaded = ok;
    return ok;
}

void buildDefaultCommon(uint8_t buf[512], int32_t sessionId, int32_t ioId) {
    memset(buf, 0, 512);
    auto write32 = [&](size_t off, uint32_t v) { memcpy(buf + off, &v, 4); };
    auto write64 = [&](size_t off, uint64_t v) { memcpy(buf + off, &v, 8); };

    write32(0x00, static_cast<uint32_t>(sessionId));
    write32(0x04, static_cast<uint32_t>(ioId));

    write32(0x08, 48000);
    write32(0x0c, 0x3);
    write32(0x10, 3);
    buf[0x18] = 1;
    buf[0x19] = 4;
    write64(0x98, 256);

    write32(0xa0, 48000);
    write32(0xa4, 0x3);
    write32(0xa8, 3);
    buf[0xb0] = 1;
    buf[0xb1] = 4;
    write64(0x130, 256);
}

struct SpatializerContext {
    const struct effect_interface_s* itfe;
    void* impl;
    uint8_t level;
    uint8_t headTrackingMode;
    uint32_t inMask;
    uint32_t outMask;
    uint32_t processCount;
};

int32_t spatializerProcess(effect_handle_t self, audio_buffer_t* inBuffer, audio_buffer_t* outBuffer) {
    auto* ctx = reinterpret_cast<SpatializerContext*>(self);
    if (!ctx || !ctx->impl || !inBuffer || !outBuffer) return -EINVAL;
    int32_t ret = gSyms.process(ctx->impl, inBuffer, outBuffer);
    uint32_t cnt = ++ctx->processCount;
    if (cnt == 1 || (cnt % 500) == 0) {
        ALOGI("OplusSpatializer process #%u ret=%d frames=%zu inMask=0x%x outMask=0x%x",
              cnt, ret, inBuffer->frameCount, ctx->inMask, ctx->outMask);
    }
    return ret;
}

int32_t spatializerCommand(effect_handle_t self, uint32_t cmdCode, uint32_t cmdSize, void* pCmdData,
                           uint32_t* replySize, void* pReplyData) {
    auto* ctx = reinterpret_cast<SpatializerContext*>(self);
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
            ctx->inMask = cfg->inputCfg.channels;
            ctx->outMask = cfg->outputCfg.channels;
            // Sync HIDL channel masks into OplusSpatializerEffect's AIDL Common struct
            auto* base = reinterpret_cast<uint8_t*>(ctx->impl);
            uint32_t tagLayout = 3;
            memcpy(base + 0x3c, &ctx->inMask, 4);
            memcpy(base + 0x40, &tagLayout, 4);
            memcpy(base + 0xd4, &ctx->outMask, 4);
            memcpy(base + 0xd8, &tagLayout, 4);
            memcpy(base + 0x50ecc, &ctx->inMask, 4);
            memcpy(base + 0x50ed0, &tagLayout, 4);
            int ret = gSyms.setConfig(ctx->impl, cfg);
            ALOGI("OplusSpatializer setConfig rate=%u inMask=0x%x outMask=0x%x fmt=%u ret=%d",
                  cfg->inputCfg.samplingRate, ctx->inMask, ctx->outMask, cfg->inputCfg.format, ret);
            setReplyStatus(ret);
            return 0;
        }
        case EFFECT_CMD_GET_CONFIG: {
            if (!pReplyData || !replySize || *replySize < sizeof(effect_config_t)) return -EINVAL;
            int ret = gSyms.getConfig(ctx->impl, reinterpret_cast<effect_config_t*>(pReplyData));
            *replySize = sizeof(effect_config_t);
            return ret;
        }
        case EFFECT_CMD_RESET: {
            gSyms.reset(ctx->impl);
            return 0;
        }
        case EFFECT_CMD_ENABLE: {
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
            if (paramId == 1 && p->vsize >= 1) {
                ctx->level = *reinterpret_cast<uint8_t*>(val);
                gSyms.setParam(ctx->impl, paramId, p->vsize, val);
                setReplyStatus(0);
                return 0;
            }
            if (paramId == 3 && p->vsize >= 1) {
                ctx->headTrackingMode = *reinterpret_cast<uint8_t*>(val);
                gSyms.setParam(ctx->impl, paramId, p->vsize, val);
                setReplyStatus(0);
                return 0;
            }
            int ret = gSyms.setParam(ctx->impl, paramId, p->vsize, val);
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
            unsigned int maxVsize = *replySize - static_cast<uint32_t>(sizeof(effect_param_t) + voffset);
            int paramId = *reinterpret_cast<int32_t*>(pOut->data);
            uint8_t* outVal = reinterpret_cast<uint8_t*>(pOut->data + voffset);

            auto replyBytes = [&](const void* src, uint32_t len) -> int32_t {
                if (maxVsize < len) {
                    pOut->status = -EINVAL;
                    pOut->vsize = 0;
                } else {
                    memcpy(outVal, src, len);
                    pOut->status = 0;
                    pOut->vsize = len;
                }
                *replySize = static_cast<uint32_t>(sizeof(effect_param_t) + voffset + pOut->vsize);
                return 0;
            };

            switch (paramId) {
                case 0: {
                    const uint8_t levels[3] = {2, 0, 1};
                    return replyBytes(levels, sizeof(levels));
                }
                case 1:
                    return replyBytes(&ctx->level, 1);
                case 2: {
                    const uint8_t ht = 0;
                    return replyBytes(&ht, 1);
                }
                case 3:
                    return replyBytes(&ctx->headTrackingMode, 1);
                case 4: {
                    const uint32_t masks[5] = {4, 0x3f, 0xc003f, 0x63f, 0x2d63f};
                    return replyBytes(masks, sizeof(masks));
                }
                case 5: {
                    const uint8_t modes[3] = {2, 0, 1};
                    return replyBytes(modes, sizeof(modes));
                }
                case 13: {
                    const uint32_t masks[5] = {4, 0x3f, 0xc003f, 0x63f, 0x2d63f};
                    return replyBytes(masks, sizeof(masks));
                }
                default:
                    break;
            }

            unsigned int vsize = maxVsize;
            int ret = gSyms.getParam(ctx->impl, paramId, &vsize, outVal);
            pOut->status = ret;
            pOut->vsize = (ret == 0) ? vsize : 0;
            *replySize = static_cast<uint32_t>(sizeof(effect_param_t) + voffset + pOut->vsize);
            return 0;
        }
        case EFFECT_CMD_SET_DEVICE: {
            if (!pCmdData || cmdSize < sizeof(uint32_t)) return -EINVAL;
            return gSyms.setDevice(ctx->impl, *reinterpret_cast<uint32_t*>(pCmdData));
        }
        case EFFECT_CMD_SET_VOLUME: {
            if (!pCmdData || cmdSize < sizeof(uint32_t)) return -EINVAL;
            int numCh = static_cast<int>(cmdSize / sizeof(uint32_t));
            int ret = gSyms.setVolume(ctx->impl, numCh, reinterpret_cast<const uint32_t*>(pCmdData));
            if (pReplyData && replySize && *replySize >= cmdSize) {
                memcpy(pReplyData, pCmdData, cmdSize);
                *replySize = cmdSize;
            }
            return ret;
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

int32_t spatializerGetDescriptor(effect_handle_t self, effect_descriptor_t* pDescriptor) {
    if (!self || !pDescriptor) return -EINVAL;
    *pDescriptor = kSpatializerDescriptor;
    return 0;
}

const struct effect_interface_s kSpatializerInterface = {
    spatializerProcess,
    spatializerCommand,
    spatializerGetDescriptor,
    nullptr,
};

int32_t libCreateEffect(const effect_uuid_t* uuid, int32_t sessionId, int32_t ioId,
                        effect_handle_t* pHandle) {
    if (!uuid || !pHandle) return -EINVAL;
    if (memcmp(uuid, &kSpatializerImplUuid, sizeof(effect_uuid_t)) != 0) return -ENOENT;
    if (!loadAidlLib()) return -ENOSYS;

    void* mem = nullptr;
    if (posix_memalign(&mem, 16, kOplusSpatializerEffectSize) != 0 || !mem) return -ENOMEM;
    memset(mem, 0, kOplusSpatializerEffectSize);

    alignas(16) uint8_t commonBuf[512];
    buildDefaultCommon(commonBuf, sessionId, ioId);
    gSyms.ctor(mem, 1, commonBuf);

    auto* ctx = new SpatializerContext{&kSpatializerInterface, mem, 0, 0, 0x3, 0x3, 0};
    *pHandle = reinterpret_cast<effect_handle_t>(ctx);
    ALOGI("Created OplusSpatializer HIDL-bridged effect (session=%d io=%d)", sessionId, ioId);
    return 0;
}

int32_t libReleaseEffect(effect_handle_t handle) {
    auto* ctx = reinterpret_cast<SpatializerContext*>(handle);
    if (!ctx) return -EINVAL;
    if (ctx->impl) {
        gSyms.release(ctx->impl);
        gSyms.dtor(ctx->impl);
        free(ctx->impl);
    }
    delete ctx;
    ALOGI("Released OplusSpatializer HIDL-bridged effect");
    return 0;
}

int32_t libGetDescriptor(const effect_uuid_t* uuid, effect_descriptor_t* pDescriptor) {
    if (!uuid || !pDescriptor) return -EINVAL;
    if (memcmp(uuid, &kSpatializerImplUuid, sizeof(effect_uuid_t)) != 0) return -ENOENT;
    *pDescriptor = kSpatializerDescriptor;
    return 0;
}

} // namespace

#define AUDIO_EFFECT_LIBRARY_INFO_SYM AELI
extern "C" __attribute__((visibility("default")))
audio_effect_library_t AUDIO_EFFECT_LIBRARY_INFO_SYM = {
    .tag = 0x41454c54,
    .version = EFFECT_LIBRARY_API_VERSION,
    .name = "OplusSpatializer HIDL Shim",
    .implementor = "OPLUS",
    .create_effect = libCreateEffect,
    .release_effect = libReleaseEffect,
    .get_descriptor = libGetDescriptor,
};
