#include "rynx_audiox_dsp.h"

#include <dlfcn.h>
#include <math.h>
#include <mutex>
#include <stdint.h>
#include <stdlib.h>
#include <string>
#include <string.h>
#include <unordered_map>

#define LOG_TAG "OplusAudioxAidlShim"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace {

struct AidlEffectStatus {
    int32_t status;
    int32_t fmqConsumed;
    int32_t fmqProduced;
};

static_assert(sizeof(AidlEffectStatus) == 12, "IEffect::Status must be 12 bytes");

using QueryEffectFn    = int32_t (*)(const void* implUuid, void* desc);
using CreateEffectFn   = int32_t (*)(const void* implUuid, void* instanceSpp);
using DestroyEffectFn  = int32_t (*)(const void* instanceSp);
using CtorFn           = void (*)(void* self, size_t statusDepth, const void* common);
using DtorFn           = void (*)(void* self);
using VoidIntFn        = int (*)(void* self);
using SetCommonFn      = int (*)(void* self, const void* common);
using SetDeviceFn      = int (*)(void* self, uint32_t device);
using SetVolumeStFn    = int (*)(void* self, const float* volStereo);
using SetParamFn       = int (*)(void* self, int paramId, unsigned int valueSize, void* value);
using ProcessAidlFn    = AidlEffectStatus (*)(void* self, float* in, float* out, int samples);

struct RealSyms {
    void* aidlHandle = nullptr;
    void* algoHandle = nullptr;
    QueryEffectFn queryEffect = nullptr;
    CreateEffectFn createEffect = nullptr;
    DestroyEffectFn destroyEffect = nullptr;
    CtorFn ctor = nullptr;
    DtorFn dtor = nullptr;
    VoidIntFn release = nullptr;
    VoidIntFn enable = nullptr;
    VoidIntFn reset = nullptr;
    SetCommonFn setCommon = nullptr;
    SetDeviceFn setDevice = nullptr;
    SetVolumeStFn setVolumeStereo = nullptr;
    SetParamFn setParam = nullptr;
    ProcessAidlFn processforAidl = nullptr;
    rynx::SpatGetEnFn spatGetEn = nullptr;
    rynx::SpatSetEnFn spatSetEn = nullptr;
    std::once_flag once;
};

RealSyms gReal;
std::mutex gStateMu;
std::unordered_map<void*, rynx::RynxDspState> gStates;

void* resolveRealSym(const char* name) {
    void* p = nullptr;
    if (gReal.aidlHandle) {
        p = dlsym(gReal.aidlHandle, name);
    }
    if (!p) {
        p = dlsym(RTLD_NEXT, name);
    }
    if (!p) {
        ALOGE("Failed to resolve real symbol %s: %s", name, dlerror());
    }
    return p;
}

void initRealSyms() {
    std::call_once(gReal.once, []() {
        // Resolve sibling libOplusAudioxAidl.so next to this shim first (works in /vendor, /odm, or test dirs)
        Dl_info selfInfo{};
        if (dladdr(reinterpret_cast<void*>(&initRealSyms), &selfInfo) && selfInfo.dli_fname) {
            const char* slash = strrchr(selfInfo.dli_fname, '/');
            if (slash) {
                std::string sibling(selfInfo.dli_fname, static_cast<size_t>(slash - selfInfo.dli_fname + 1));
                sibling += "libOplusAudioxAidl.so";
                gReal.aidlHandle = dlopen(sibling.c_str(), RTLD_NOW | RTLD_NOLOAD);
                if (!gReal.aidlHandle) {
                    gReal.aidlHandle = dlopen(sibling.c_str(), RTLD_NOW | RTLD_LOCAL);
                }
            }
        }
        if (!gReal.aidlHandle) {
            const char* paths[] = {
                "libOplusAudioxAidl.so",
                "/vendor/lib64/soundfx/libOplusAudioxAidl.so",
                "/odm/lib64/soundfx/libOplusAudioxAidl.so",
            };
            for (const char* path : paths) {
                gReal.aidlHandle = dlopen(path, RTLD_NOW | RTLD_NOLOAD);
                if (gReal.aidlHandle) break;
            }
            if (!gReal.aidlHandle) {
                for (const char* path : paths) {
                    gReal.aidlHandle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
                    if (gReal.aidlHandle) break;
                }
            }
        }

        gReal.queryEffect = reinterpret_cast<QueryEffectFn>(resolveRealSym("queryEffect"));
        gReal.createEffect = reinterpret_cast<CreateEffectFn>(resolveRealSym("createEffect"));
        gReal.destroyEffect = reinterpret_cast<DestroyEffectFn>(resolveRealSym("destroyEffect"));
        gReal.ctor = reinterpret_cast<CtorFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffectC1EmRKNS3_9Parameter6CommonE"));
        gReal.dtor = reinterpret_cast<DtorFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffectD1Ev"));
        gReal.release = reinterpret_cast<VoidIntFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect7releaseEv"));
        gReal.enable = reinterpret_cast<VoidIntFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect6enableEv"));
        gReal.reset = reinterpret_cast<VoidIntFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect5resetEv"));
        gReal.setCommon = reinterpret_cast<SetCommonFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9setCommonERKNS3_9Parameter6CommonE"));
        gReal.setDevice = reinterpret_cast<SetDeviceFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9setDeviceE15audio_devices_t"));
        gReal.setVolumeStereo = reinterpret_cast<SetVolumeStFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect13EffectContext15setVolumeStereoERKNS3_9Parameter12VolumeStereoE"));
        gReal.setParam = reinterpret_cast<SetParamFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect8setParamEijPv"));
        gReal.processforAidl = reinterpret_cast<ProcessAidlFn>(
            resolveRealSym("_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect14processforAidlEPfS5_i"));

        gReal.algoHandle = dlopen("/vendor/lib64/liboplus_audiox.so", RTLD_NOW | RTLD_NOLOAD);
        if (!gReal.algoHandle) {
            gReal.algoHandle = dlopen("liboplus_audiox.so", RTLD_NOW | RTLD_NOLOAD);
        }
        if (gReal.algoHandle) {
            gReal.spatGetEn = reinterpret_cast<rynx::SpatGetEnFn>(
                dlsym(gReal.algoHandle, "oplus_algo_spat_get_enable_ID"));
            gReal.spatSetEn = reinterpret_cast<rynx::SpatSetEnFn>(
                dlsym(gReal.algoHandle, "oplus_algo_spat_set_enable_ID"));
        }

        ALOGI("Initialized AIDL shim (handle=%p process=%p setParam=%p)",
              gReal.aidlHandle, reinterpret_cast<void*>(gReal.processforAidl),
              reinterpret_cast<void*>(gReal.setParam));
    });
}

uint32_t readCommonSampleRate(const void* common) {
    if (!common) return rynx::kAudioxTargetRate;
    uint32_t rate = 0;
    memcpy(&rate, reinterpret_cast<const uint8_t*>(common) + 0x08, sizeof(rate));
    return (rate >= 8000 && rate <= 384000) ? rate : rynx::kAudioxTargetRate;
}

uint32_t readOutputChannels(const void* effectObj) {
    if (!effectObj) return 2;
    const auto* base = reinterpret_cast<const uint8_t*>(effectObj);
    uint32_t tag = 0, mask = 0;
    memcpy(&mask, base + 0xd4, sizeof(mask));
    memcpy(&tag, base + 0xd8, sizeof(tag));
    if (tag >= 2 && tag <= 4) {
        int ch = __builtin_popcount(mask & 0x7fffffffu);
        if (ch > 0) return static_cast<uint32_t>(ch);
    }
    return 2;
}

} // namespace

// AIDL IFactory entry points (forwarded to libOplusAudioxAidl.so)
extern "C" __attribute__((visibility("default")))
int32_t queryEffect(const void* implUuid, void* desc) {
    initRealSyms();
    if (!gReal.queryEffect) return -3;
    return gReal.queryEffect(implUuid, desc);
}

extern "C" __attribute__((visibility("default")))
int32_t createEffect(const void* implUuid, void* instanceSpp) {
    initRealSyms();
    if (!gReal.createEffect) return -3;
    return gReal.createEffect(implUuid, instanceSpp);
}

extern "C" __attribute__((visibility("default")))
int32_t destroyEffect(const void* instanceSp) {
    initRealSyms();
    if (!gReal.destroyEffect) return -3;
    return gReal.destroyEffect(instanceSp);
}

// PLT-interposed OplusAudioxEffect / EffectContext hooks
extern "C" __attribute__((visibility("default")))
void _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffectC1EmRKNS3_9Parameter6CommonE(
    void* self, size_t statusDepth, const void* common) {
    initRealSyms();
    if (gReal.ctor) {
        gReal.ctor(self, statusDepth, common);
    }
    std::lock_guard<std::mutex> lock(gStateMu);
    auto& st = gStates[self];
    st = rynx::RynxDspState{};
    st.hostRate = readCommonSampleRate(common);
    st.refreshProps();
}

extern "C" __attribute__((visibility("default")))
void _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffectD1Ev(void* self) {
    initRealSyms();
    {
        std::lock_guard<std::mutex> lock(gStateMu);
        gStates.erase(self);
    }
    if (gReal.dtor) {
        gReal.dtor(self);
    }
}

extern "C" __attribute__((visibility("default")))
int _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect7releaseEv(void* self) {
    initRealSyms();
    int ret = gReal.release ? gReal.release(self) : 0;
    std::lock_guard<std::mutex> lock(gStateMu);
    gStates.erase(self);
    return ret;
}

extern "C" __attribute__((visibility("default")))
int _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9setCommonERKNS3_9Parameter6CommonE(
    void* self, const void* common) {
    initRealSyms();
    int ret = gReal.setCommon ? gReal.setCommon(self, common) : 0;
    if (ret == 0) {
        std::lock_guard<std::mutex> lock(gStateMu);
        auto& st = gStates[self];
        st.hostRate = readCommonSampleRate(common);
        st.refreshProps();
    }
    return ret;
}

extern "C" __attribute__((visibility("default")))
int _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9setDeviceE15audio_devices_t(
    void* self, uint32_t device) {
    initRealSyms();
    {
        std::lock_guard<std::mutex> lock(gStateMu);
        gStates[self].currentDevice = device;
    }
    return gReal.setDevice ? gReal.setDevice(self, device) : 0;
}

extern "C" __attribute__((visibility("default")))
int _ZN4aidl7android8hardware5audio6effect13EffectContext15setVolumeStereoERKNS3_9Parameter12VolumeStereoE(
    void* self, const float* volStereo) {
    initRealSyms();
    if (volStereo) {
        std::lock_guard<std::mutex> lock(gStateMu);
        auto it = gStates.find(self);
        if (it != gStates.end()) {
            float maxLin = fmaxf(volStereo[0], volStereo[1]);
            rynx::updateLinearVolume(it->second, maxLin);
        }
    }
    return gReal.setVolumeStereo ? gReal.setVolumeStereo(self, volStereo) : 0;
}

extern "C" __attribute__((visibility("default")))
int _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect8setParamEijPv(
    void* self, int paramId, unsigned int valueSize, void* value) {
    initRealSyms();
    int32_t volIdxOverride = 0;
    void* forwardVal = value;
    {
        std::lock_guard<std::mutex> lock(gStateMu);
        auto& st = gStates[self];
        forwardVal = rynx::trackAudioxParam(st, paramId, valueSize, value, volIdxOverride, LOG_TAG);
    }
    return gReal.setParam ? gReal.setParam(self, paramId, valueSize, forwardVal) : 0;
}

extern "C" __attribute__((visibility("default")))
int _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect6enableEv(void* self) {
    initRealSyms();
    {
        std::lock_guard<std::mutex> lock(gStateMu);
        gStates[self].refreshProps();
    }
    return gReal.enable ? gReal.enable(self) : 0;
}

extern "C" __attribute__((visibility("default")))
int _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect5resetEv(void* self) {
    initRealSyms();
    {
        std::lock_guard<std::mutex> lock(gStateMu);
        auto it = gStates.find(self);
        if (it != gStates.end()) {
            it->second.modeDsp.reset();
        }
    }
    return gReal.reset ? gReal.reset(self) : 0;
}

extern "C" __attribute__((visibility("default")))
AidlEffectStatus _ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect14processforAidlEPfS5_i(
    void* self, float* in, float* out, int samples) {
    initRealSyms();
    if (!gReal.processforAidl) {
        return AidlEffectStatus{-4, 0, 0};
    }

    rynx::RynxDspState* st = nullptr;
    {
        std::lock_guard<std::mutex> lock(gStateMu);
        auto it = gStates.find(self);
        if (it == gStates.end()) {
            auto& created = gStates[self];
            created.refreshProps();
            st = &created;
        } else {
            st = &it->second;
        }
    }

    if (st) {
        rynx::ensureSpeakerImmersiveBrir(self, *st, gReal.spatGetEn, gReal.spatSetEn, LOG_TAG);
    }

    AidlEffectStatus ret = gReal.processforAidl(self, in, out, samples);

    if (st && ret.status == 0 && ret.fmqProduced > 0 && out != nullptr && readOutputChannels(self) == 2) {
        size_t frames = static_cast<size_t>(ret.fmqProduced) / 2;
        rynx::applyModeDsp(*st, out, frames);
        rynx::applyVolumeLeveler(*st, out, frames, LOG_TAG);
    }
    return ret;
}
