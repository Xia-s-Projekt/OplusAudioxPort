#include <dlfcn.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AidlEffectStatus {
    int32_t status;
    int32_t fmqConsumed;
    int32_t fmqProduced;
};

using CtorFn       = void (*)(void* self, size_t statusDepth, const void* common);
using DtorFn       = void (*)(void* self);
using VoidIntFn    = int (*)(void* self);
using SetDeviceFn  = int (*)(void* self, uint32_t device);
using SetParamFn   = int (*)(void* self, int paramId, unsigned int valueSize, void* value);
using SetVolStFn   = int (*)(void* self, const float* vol);
using EffProcFn    = AidlEffectStatus (*)(void* impl, float* in, float* out, int samples);

static void buildCommon(uint8_t buf[512]) {
    memset(buf, 0, 512);
    auto w32 = [&](size_t off, uint32_t v) { memcpy(buf + off, &v, 4); };
    auto w64 = [&](size_t off, uint64_t v) { memcpy(buf + off, &v, 8); };
    w32(0x00, 1);
    w32(0x04, 1);
    w32(0x08, 48000);
    w32(0x0c, 0x3);
    w32(0x10, 3);
    buf[0x18] = 1;
    buf[0x19] = 4;
    w64(0x98, 256);
    w32(0xa0, 48000);
    w32(0xa4, 0x3);
    w32(0xa8, 3);
    buf[0xb0] = 1;
    buf[0xb1] = 4;
    w64(0x130, 256);
}

static double calcRmsDb(const float* buf, size_t n) {
    double s = 0.0;
    for (size_t i = 0; i < n; ++i) s += static_cast<double>(buf[i]) * buf[i];
    return 10.0 * log10(s / n + 1e-12);
}

int main(int argc, char** argv) {
    const char* shimPath = (argc > 1) ? argv[1] : "/data/local/tmp/aidl_test/liboplus_audiox_aidl_shim.so";
    void* h = dlopen(shimPath, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "FAIL: dlopen(%s): %s\n", shimPath, dlerror());
        return 1;
    }
    printf("OK: dlopen(%s) succeeded\n", shimPath);

    void* qSym = dlsym(h, "queryEffect");
    void* cSym = dlsym(h, "createEffect");
    void* dSym = dlsym(h, "destroyEffect");
    if (!qSym || !cSym || !dSym) {
        fprintf(stderr, "FAIL: missing AIDL entrypoints q=%p c=%p d=%p\n", qSym, cSym, dSym);
        return 1;
    }
    printf("OK: AIDL entrypoints resolved (query=%p create=%p destroy=%p)\n", qSym, cSym, dSym);

    void* realH = dlopen("libOplusAudioxAidl.so", RTLD_NOW | RTLD_NOLOAD);
    if (!realH) {
        fprintf(stderr, "FAIL: libOplusAudioxAidl.so was NOT auto-loaded via DT_NEEDED + $ORIGIN\n");
        return 1;
    }
    printf("OK: libOplusAudioxAidl.so auto-loaded via DT_NEEDED + $ORIGIN (%p)\n", realH);

    // Call OplusAudioxImpl::effectProcessImpl inside libOplusAudioxAidl.so to verify PLT interposition
    auto effProc = reinterpret_cast<EffProcFn>(
        dlsym(realH, "_ZN4aidl7android8hardware5audio6effect15OplusAudioxImpl17effectProcessImplEPfS5_i"));
    auto ctor = reinterpret_cast<CtorFn>(
        dlsym(h, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffectC1EmRKNS3_9Parameter6CommonE"));
    auto dtor = reinterpret_cast<DtorFn>(
        dlsym(h, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffectD1Ev"));
    auto initFn = reinterpret_cast<VoidIntFn>(
        dlsym(realH, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect4initEv"));
    auto enableFn = reinterpret_cast<VoidIntFn>(
        dlsym(h, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect6enableEv"));
    auto releaseFn = reinterpret_cast<VoidIntFn>(
        dlsym(h, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect7releaseEv"));
    auto setDev = reinterpret_cast<SetDeviceFn>(
        dlsym(h, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect9setDeviceE15audio_devices_t"));
    auto setParam = reinterpret_cast<SetParamFn>(
        dlsym(h, "_ZN4aidl7android8hardware5audio6effect17OplusAudioxEffect8setParamEijPv"));
    auto setVol = reinterpret_cast<SetVolStFn>(
        dlsym(h, "_ZN4aidl7android8hardware5audio6effect13EffectContext15setVolumeStereoERKNS3_9Parameter12VolumeStereoE"));

    if (!effProc || !ctor || !dtor || !initFn || !enableFn || !releaseFn || !setDev || !setParam || !setVol) {
        fprintf(stderr, "FAIL: symbol lookup failed\n");
        return 1;
    }

    void* effectObj = nullptr;
    if (posix_memalign(&effectObj, 16, 0xde38) != 0) return 1;
    memset(effectObj, 0, 0xde38);

    alignas(16) uint8_t common[512];
    buildCommon(common);
    ctor(effectObj, 1, common);
    initFn(effectObj);
    setDev(effectObj, 0x2);
    float vol[2] = {0.5f, 0.5f};
    setVol(effectObj, vol);
    int32_t enableVal = 1;
    setParam(effectObj, 0x100100ba, 4, &enableVal);
    int32_t profileSmart = 0;
    setParam(effectObj, 0x100100bb, 4, &profileSmart);
    enableFn(effectObj);

    // Build fake OplusAudioxImpl holding effectObj at +0x130 so we call libOplusAudioxAidl.so's own effectProcessImpl!
    alignas(16) uint8_t fakeImpl[0x180];
    memset(fakeImpl, 0, sizeof(fakeImpl));
    memcpy(fakeImpl + 0x130, &effectObj, sizeof(void*));

    const int frames = 256;
    const int samples = frames * 2;
    float inBuf[samples], outSmart[samples], outFid[samples], outImm[samples];
    for (int i = 0; i < frames; ++i) {
        float t = static_cast<float>(i) / 48000.0f;
        inBuf[2 * i]     = 0.05f * sinf(2.0f * 3.14159265f * 1000.0f * t) + 0.03f * sinf(2.0f * 3.14159265f * 4000.0f * t);
        inBuf[2 * i + 1] = 0.05f * sinf(2.0f * 3.14159265f * 1000.0f * t + 0.7f);
    }

    AidlEffectStatus st{};
    for (int b = 0; b < 40; ++b) {
        st = effProc(fakeImpl, inBuf, outSmart, samples);
    }
    double inDb = calcRmsDb(inBuf, samples);
    double smartDb = calcRmsDb(outSmart, samples);
    printf("Smart:    status=%d consumed=%d produced=%d inDb=%.2f outDb=%.2f delta=%+.2f dB\n",
           st.status, st.fmqConsumed, st.fmqProduced, inDb, smartDb, smartDb - inDb);

    int32_t profileFid = 1;
    setParam(effectObj, 0x100100bb, 4, &profileFid);
    for (int b = 0; b < 40; ++b) {
        st = effProc(fakeImpl, inBuf, outFid, samples);
    }
    double fidDb = calcRmsDb(outFid, samples);

    int32_t profileImm = 2;
    setParam(effectObj, 0x100100bb, 4, &profileImm);
    for (int b = 0; b < 40; ++b) {
        st = effProc(fakeImpl, inBuf, outImm, samples);
    }
    double immDb = calcRmsDb(outImm, samples);

    double diffFid = 0.0, diffImm = 0.0;
    for (int i = 0; i < samples; ++i) {
        diffFid += fabs(outFid[i] - outSmart[i]);
        diffImm += fabs(outImm[i] - outSmart[i]);
    }
    printf("Fidelity: outDb=%.2f diffVsSmart=%.4f\n", fidDb, diffFid);
    printf("Immersiv: outDb=%.2f diffVsSmart=%.4f\n", immDb, diffImm);

    releaseFn(effectObj);
    dtor(effectObj);
    free(effectObj);

    if (st.status != 0 || st.fmqProduced != samples || diffFid < 0.01 || diffImm < 0.01) {
        fprintf(stderr, "FAIL: AIDL shim interposition check failed!\n");
        return 1;
    }
    printf("PASS: AIDL shim PLT interposition + RynxModeDsp + RynxVolumeLeveler verified!\n");
    return 0;
}
