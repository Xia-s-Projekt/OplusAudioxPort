#pragma once

#include <android/log.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <sys/system_properties.h>

namespace rynx {

constexpr uint32_t kAudioxTargetRate = 48000;
constexpr uint32_t kAudioDeviceOutSpeaker = 0x2u;
constexpr size_t kSpatialDelayMask = 255;

constexpr const char* kDownsampleProp = "persist.sys.rianixia.downsample_audiox";
constexpr const char* kLevelerProp = "persist.sys.rianixia.audiox_leveler";
constexpr const char* kLevelerLogProp = "persist.sys.rianixia.audiox_leveler_log";

using SpatGetEnFn = int (*)(void* spatObj, int id, int* val);
using SpatSetEnFn = int (*)(void* spatObj, int id, int val);

inline bool isPropEnabled(const char* name) {
    char val[PROP_VALUE_MAX] = {0};
    int len = __system_property_get(name, val);
    if (len <= 0) return false;
    return (strcmp(val, "true") == 0 || strcmp(val, "1") == 0);
}

inline bool isDownsampleEnabled() { return isPropEnabled(kDownsampleProp); }
inline bool isLevelerEnabled() { return isPropEnabled(kLevelerProp); }
inline bool isLevelerLogEnabled() { return isPropEnabled(kLevelerLogProp); }

struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double z1[2] = {0.0, 0.0};
    double z2[2] = {0.0, 0.0};

    void reset() {
        z1[0] = z1[1] = z2[0] = z2[1] = 0.0;
    }

    void setPeaking(double fs, double f0, double q, double gainDb) {
        double a = pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * M_PI * f0 / fs;
        double cw = cos(w0), sw = sin(w0);
        double alpha = sw / (2.0 * q);
        double a0 = 1.0 + alpha / a;
        b0 = (1.0 + alpha * a) / a0;
        b1 = (-2.0 * cw) / a0;
        b2 = (1.0 - alpha * a) / a0;
        a1 = (-2.0 * cw) / a0;
        a2 = (1.0 - alpha / a) / a0;
    }

    void setHighShelf(double fs, double f0, double q, double gainDb) {
        double a = pow(10.0, gainDb / 40.0);
        double w0 = 2.0 * M_PI * f0 / fs;
        double cw = cos(w0), sw = sin(w0);
        double alpha = sw / (2.0 * q);
        double sa = 2.0 * sqrt(a) * alpha;
        double a0 = (a + 1.0) - (a - 1.0) * cw + sa;
        b0 = (a * ((a + 1.0) + (a - 1.0) * cw + sa)) / a0;
        b1 = (-2.0 * a * ((a - 1.0) + (a + 1.0) * cw)) / a0;
        b2 = (a * ((a + 1.0) + (a - 1.0) * cw - sa)) / a0;
        a1 = (2.0 * ((a - 1.0) - (a + 1.0) * cw)) / a0;
        a2 = ((a + 1.0) - (a - 1.0) * cw - sa) / a0;
    }

    void setLowPass(double fs, double f0, double q) {
        double w0 = 2.0 * M_PI * f0 / fs;
        double cw = cos(w0), sw = sin(w0);
        double alpha = sw / (2.0 * q);
        double a0 = 1.0 + alpha;
        b0 = (1.0 - cw) * 0.5 / a0;
        b1 = (1.0 - cw) / a0;
        b2 = (1.0 - cw) * 0.5 / a0;
        a1 = (-2.0 * cw) / a0;
        a2 = (1.0 - alpha) / a0;
    }

    inline float tick(int ch, float in) {
        double x = in;
        double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return static_cast<float>(y);
    }
};

struct ModeDspState {
    uint32_t configuredRate = 0;
    bool configuredSpeaker = true;

    Biquad fidLowMidCut;
    Biquad fidPresence;
    Biquad fidAirShelf;

    Biquad immBassLp;
    Biquad immWarmth;
    float delayL[kSpatialDelayMask + 1] = {0.0f};
    float delayR[kSpatialDelayMask + 1] = {0.0f};
    size_t delayPos = 0;

    float immWet = 0.0f;
    float fidWet = 0.0f;

    void reset() {
        fidLowMidCut.reset();
        fidPresence.reset();
        fidAirShelf.reset();
        immBassLp.reset();
        immWarmth.reset();
        memset(delayL, 0, sizeof(delayL));
        memset(delayR, 0, sizeof(delayR));
        delayPos = 0;
    }

    void configure(uint32_t rate, bool isSpeaker) {
        if (configuredRate == rate && configuredSpeaker == isSpeaker) return;
        double fs = (rate > 0) ? static_cast<double>(rate) : 48000.0;
        if (isSpeaker) {
            fidLowMidCut.setPeaking(fs, 280.0, 1.0, -2.2);
            fidPresence.setPeaking(fs, 3200.0, 0.85, +3.0);
            fidAirShelf.setHighShelf(fs, 8000.0, 0.707, +3.5);
            immWarmth.setPeaking(fs, 115.0, 0.85, +2.0);
        } else {
            fidLowMidCut.setPeaking(fs, 250.0, 1.0, -1.6);
            fidPresence.setPeaking(fs, 3500.0, 0.85, +2.2);
            fidAirShelf.setHighShelf(fs, 9000.0, 0.707, +2.8);
            immWarmth.setPeaking(fs, 95.0, 0.85, +1.0);
        }
        immBassLp.setLowPass(fs, 220.0, 0.707);
        reset();
        configuredRate = rate;
        configuredSpeaker = isSpeaker;
    }
};

struct RynxDspState {
    bool levelerEnabled = false;
    bool levelerLogEnabled = false;
    uint32_t hostRate = kAudioxTargetRate;
    uint32_t currentDevice = 0;
    int32_t currentProfile = 0; // 0=Smart, 1=Fidelity, 2=Immersive, 6=Custom
    bool customGeqEnabled = false;
    int32_t lastNonZeroVolIdx = 10;
    float pregainDbParam = NAN;
    float pregainDbCmd = NAN;
    float pregainDbIdx = NAN;
    float powerEnv = 0.0f;
    float peakEnv = 0.0f;
    float curGain = 1.0f;
    uint32_t logCounter = 0;
    ModeDspState modeDsp;

    void refreshProps() {
        levelerEnabled = isLevelerEnabled();
        levelerLogEnabled = isLevelerLogEnabled();
    }
};

inline void updateLinearVolume(RynxDspState& st, float linearVol) {
    if (linearVol <= 3.1623e-07f) {
        st.pregainDbCmd = -130.0f;
    } else if (linearVol >= 1.0f) {
        st.pregainDbCmd = 0.0f;
    } else {
        st.pregainDbCmd = 20.0f * log10f(linearVol);
    }
}

inline void* trackAudioxParam(RynxDspState& st, int paramId, unsigned int valueSize, void* val,
                              int32_t& volIdxOverride, const char* logTag) {
    if (!val) return val;
    if (paramId == 0x100100bb && valueSize >= sizeof(int32_t)) {
        st.currentProfile = *reinterpret_cast<const int32_t*>(val);
        if (st.levelerLogEnabled) {
            __android_log_print(ANDROID_LOG_INFO, logTag,
                                "RynxModeDsp: profile=%d customGeq=%d dev=0x%x",
                                st.currentProfile, st.customGeqEnabled ? 1 : 0, st.currentDevice);
        }
    } else if (paramId == 0x100100c9 && valueSize >= sizeof(int32_t)) {
        st.customGeqEnabled = (*reinterpret_cast<const int32_t*>(val) != 0);
        if (st.levelerLogEnabled) {
            __android_log_print(ANDROID_LOG_INFO, logTag,
                                "RynxModeDsp: customGeq=%d profile=%d dev=0x%x",
                                st.customGeqEnabled ? 1 : 0, st.currentProfile, st.currentDevice);
        }
    } else if (paramId == 0x100100c2 && valueSize >= sizeof(float)) {
        float vdb = *reinterpret_cast<const float*>(val);
        if (vdb <= 0.0f && vdb >= -130.0f) {
            st.pregainDbParam = vdb;
        }
    } else if (paramId == 0x100100be && valueSize >= sizeof(int32_t)) {
        int32_t idx = *reinterpret_cast<const int32_t*>(val);
        if (idx > 0 && idx <= 16) {
            st.lastNonZeroVolIdx = idx;
            st.pregainDbIdx = -40.0f * (1.0f - static_cast<float>(idx) / 16.0f);
        } else if (idx == 0) {
            // Prevent mode-switch vol=0 from locking OplusAudiox onto lvl1.bin
            volIdxOverride = st.lastNonZeroVolIdx;
            return &volIdxOverride;
        }
    }
    return val;
}

inline void ensureSpeakerImmersiveBrir(void* effectObj, const RynxDspState& st,
                                       SpatGetEnFn spatGetEn, SpatSetEnFn spatSetEn,
                                       const char* logTag) {
    if (!effectObj || !spatGetEn || !spatSetEn) return;
    if (st.customGeqEnabled || st.currentProfile != 2) return;
    bool isSpeaker = (st.currentDevice == 0 || (st.currentDevice & kAudioDeviceOutSpeaker) != 0);
    if (!isSpeaker) return;

    auto* base = reinterpret_cast<uint8_t*>(effectObj);
    void* spatObj = nullptr;
    memcpy(&spatObj, base + 0x3d0, sizeof(void*));
    if (!spatObj) return;

    int stupmix = 0, binparfir = 0;
    if (spatGetEn(spatObj, 0, &stupmix) == 0 && stupmix >= 1 &&
        spatGetEn(spatObj, 1, &binparfir) == 0 && binparfir == 0) {
        spatSetEn(spatObj, 1, 1);
        if (st.levelerLogEnabled) {
            __android_log_print(ANDROID_LOG_INFO, logTag,
                                "RynxModeDsp: enabled native BINPARFIR=1 for Speaker Immersive");
        }
    }
}

inline void applyModeDsp(RynxDspState& st, float* samples, size_t frames) {
    if (!samples || frames == 0) return;

    const bool isSpeaker = (st.currentDevice == 0 || (st.currentDevice & kAudioDeviceOutSpeaker) != 0);
    const uint32_t rate = (st.hostRate > 0) ? st.hostRate : kAudioxTargetRate;
    st.modeDsp.configure(rate, isSpeaker);

    const bool wantImmersive = (!st.customGeqEnabled && st.currentProfile == 2);
    const bool wantFidelity = (!st.customGeqEnabled && st.currentProfile == 1);

    const float targetImm = wantImmersive ? 1.0f : 0.0f;
    const float targetFid = wantFidelity ? 1.0f : 0.0f;

    if (targetImm == 0.0f && st.modeDsp.immWet < 1e-4f &&
        targetFid == 0.0f && st.modeDsp.fidWet < 1e-4f) {
        st.modeDsp.immWet = 0.0f;
        st.modeDsp.fidWet = 0.0f;
        return;
    }

    const float sr = static_cast<float>(rate);
    const float smooth = expf(-1.0f / (0.035f * sr));
    const size_t tapA = static_cast<size_t>(0.00065f * sr);
    const size_t tapB = static_cast<size_t>(0.00145f * sr);
    const float widthGain = isSpeaker ? 1.75f : 1.22f;
    const float depthCross = isSpeaker ? 0.16f : 0.07f;

    float immWet = st.modeDsp.immWet;
    float fidWet = st.modeDsp.fidWet;
    size_t dpos = st.modeDsp.delayPos;

    for (size_t i = 0; i < frames; ++i) {
        immWet = smooth * immWet + (1.0f - smooth) * targetImm;
        fidWet = smooth * fidWet + (1.0f - smooth) * targetFid;

        float dryL = samples[2 * i];
        float dryR = samples[2 * i + 1];
        float outL = dryL;
        float outR = dryR;

        if (immWet > 1e-4f) {
            float mid = 0.5f * (dryL + dryR);
            float side = 0.5f * (dryL - dryR);

            float bassM = st.modeDsp.immBassLp.tick(0, mid);
            float hiM = mid - bassM;
            float bassS = st.modeDsp.immBassLp.tick(1, side);
            float hiS = side - bassS;

            float wideS = bassS * 0.85f + hiS * widthGain;
            float warmM = st.modeDsp.immWarmth.tick(0, mid);

            float wL = warmM + wideS;
            float wR = warmM - wideS;

            st.modeDsp.delayL[dpos] = hiM + hiS;
            st.modeDsp.delayR[dpos] = hiM - hiS;
            float reflL = st.modeDsp.delayR[(dpos - tapA) & kSpatialDelayMask]
                        - 0.5f * st.modeDsp.delayL[(dpos - tapB) & kSpatialDelayMask];
            float reflR = st.modeDsp.delayL[(dpos - tapA) & kSpatialDelayMask]
                        - 0.5f * st.modeDsp.delayR[(dpos - tapB) & kSpatialDelayMask];
            dpos = (dpos + 1) & kSpatialDelayMask;

            float immL = (wL - depthCross * reflL) * 0.90f;
            float immR = (wR - depthCross * reflR) * 0.90f;

            outL = outL * (1.0f - immWet) + immL * immWet;
            outR = outR * (1.0f - immWet) + immR * immWet;
        }

        if (fidWet > 1e-4f) {
            float fL = st.modeDsp.fidLowMidCut.tick(0, dryL);
            float fR = st.modeDsp.fidLowMidCut.tick(1, dryR);
            fL = st.modeDsp.fidPresence.tick(0, fL);
            fR = st.modeDsp.fidPresence.tick(1, fR);
            fL = st.modeDsp.fidAirShelf.tick(0, fL) * 0.94f;
            fR = st.modeDsp.fidAirShelf.tick(1, fR) * 0.94f;

            outL = outL * (1.0f - fidWet) + fL * fidWet;
            outR = outR * (1.0f - fidWet) + fR * fidWet;
        }

        samples[2 * i] = outL;
        samples[2 * i + 1] = outR;
    }

    st.modeDsp.immWet = immWet;
    st.modeDsp.fidWet = fidWet;
    st.modeDsp.delayPos = dpos;
}

inline float resolvePregainDb(const RynxDspState& st) {
    float db = 0.0f;
    bool haveParam = !isnan(st.pregainDbParam);
    bool haveCmd = !isnan(st.pregainDbCmd);
    if (haveParam && st.pregainDbParam < -0.05f) {
        db = st.pregainDbParam;
        if (haveCmd && st.pregainDbCmd < db) db = st.pregainDbCmd;
    } else if (haveCmd && st.pregainDbCmd < -0.05f) {
        db = st.pregainDbCmd;
    } else if (!isnan(st.pregainDbIdx) && st.pregainDbIdx < -0.05f) {
        db = st.pregainDbIdx;
    } else if (haveParam) {
        db = st.pregainDbParam;
    } else if (haveCmd) {
        db = st.pregainDbCmd;
    }
    if (db < -40.0f) db = -40.0f;
    if (db > 0.0f) db = 0.0f;
    return db;
}

inline void applyVolumeLeveler(RynxDspState& st, float* samples, size_t frames, const char* logTag) {
    if (!st.levelerEnabled || !samples || frames == 0) return;

    const float sr = (st.hostRate > 0) ? static_cast<float>(st.hostRate) : 48000.0f;
    const float pregainDb = resolvePregainDb(st);

    double sumSq = 0.0;
    for (size_t i = 0; i < frames * 2; ++i) {
        double s = samples[i];
        sumSq += s * s;
    }
    float blockPower = static_cast<float>(sumSq / static_cast<double>(frames * 2));

    float dt = static_cast<float>(frames) / sr;
    float pwrCoeff = expf(-dt / ((blockPower > st.powerEnv) ? 0.025f : 0.250f));
    st.powerEnv = pwrCoeff * st.powerEnv + (1.0f - pwrCoeff) * blockPower;

    float inDb = 10.0f * log10f(st.powerEnv + 1.0e-10f);
    float trueDb = inDb - pregainDb;

    constexpr float kInTargetDb = -16.0f;
    constexpr float kOutTargetDb = -4.0f;
    constexpr float kBaseMakeupDb = kOutTargetDb - kInTargetDb;
    constexpr float kLevelerAmount = 0.35f;
    constexpr float kMaxHeadroomRmsDb = -7.5f;
    constexpr float kCeiling = 0.966f;

    float desiredGain = 1.0f;
    if (trueDb > -55.0f) {
        float diffDb = kInTargetDb - trueDb;
        if (diffDb > 12.0f) diffDb = 12.0f;
        if (diffDb < -10.0f) diffDb = -10.0f;

        float desiredGainDb = kBaseMakeupDb + kLevelerAmount * diffDb;
        if (desiredGainDb < 4.0f) desiredGainDb = 4.0f;
        if (desiredGainDb > 15.0f) desiredGainDb = 15.0f;

        float headroomMaxDb = kMaxHeadroomRmsDb - inDb;
        if (headroomMaxDb < 0.0f) headroomMaxDb = 0.0f;
        if (desiredGainDb > headroomMaxDb) desiredGainDb = headroomMaxDb;

        desiredGain = powf(10.0f, desiredGainDb / 20.0f);
    }

    const float peakAtk = expf(-1.0f / (0.0003f * sr));
    const float peakRel = expf(-1.0f / (0.060f * sr));
    const float gainSmooth = expf(-1.0f / (0.012f * sr));

    float peakEnv = st.peakEnv;
    float curGain = st.curGain;
    for (size_t i = 0; i < frames; ++i) {
        curGain = gainSmooth * curGain + (1.0f - gainSmooth) * desiredGain;
        float l = samples[2 * i] * curGain;
        float r = samples[2 * i + 1] * curGain;
        float pk = fabsf(l);
        float pr = fabsf(r);
        if (pr > pk) pk = pr;

        float pc = (pk > peakEnv) ? peakAtk : peakRel;
        peakEnv = pc * peakEnv + (1.0f - pc) * pk;

        float lim = (peakEnv > kCeiling) ? (kCeiling / peakEnv) : 1.0f;
        samples[2 * i] = l * lim;
        samples[2 * i + 1] = r * lim;
    }
    st.peakEnv = peakEnv;
    st.curGain = curGain;

    if (st.levelerLogEnabled && (++st.logCounter % 250) == 1 && trueDb > -55.0f) {
        __android_log_print(ANDROID_LOG_INFO, logTag,
                            "RynxVolumeLeveler: mode=%d custom=%d dev=0x%x pregain=%.1f dB in=%.1f dB true=%.1f dB gain=+%.1f dB (%.2fx)",
                            st.currentProfile, st.customGeqEnabled ? 1 : 0, st.currentDevice,
                            pregainDb, inDb, trueDb, 20.0f * log10f(curGain), curGain);
    }
}

} // namespace rynx
