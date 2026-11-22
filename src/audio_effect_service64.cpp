#include <android/log.h>
#include <stdint.h>
#include <string.h>

#define LOG_TAG "AudioEffectService64"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Binary-compatible 64-bit little-endian libc++ std::__1::basic_string<char>
struct LibcxxString {
    union {
        struct {
            size_t cap;
            size_t size;
            const char* data;
        } long_str;
        struct {
            uint8_t size_x2;
            char data[23];
        } short_str;
    };

    static LibcxxString fromLiteral(const char* s) {
        LibcxxString out{};
        size_t len = strlen(s);
        if (len <= 22) {
            out.short_str.size_x2 = static_cast<uint8_t>(len << 1);
            memcpy(out.short_str.data, s, len + 1);
        } else {
            out.long_str.cap = ((len + 16) & ~size_t(1)) | size_t(1);
            out.long_str.size = len;
            out.long_str.data = s;
        }
        return out;
    }
};

static_assert(sizeof(LibcxxString) == 24, "std::__1::string must be 24 bytes on aarch64");

extern "C" {
void _ZN7android8hardware22configureRpcThreadpoolEmb(size_t maxThreads, bool callerWillJoin);
void _ZN7android8hardware17joinRpcThreadpoolEv();
int _ZN7android8hardware40registerPassthroughServiceImplementationERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_S9_(
    const LibcxxString& interfaceName,
    const LibcxxString& expectInterfaceName,
    const LibcxxString& serviceName);
}

int main() {
    ALOGI("Starting 64-bit HIDL IEffectsFactory service...");
    _ZN7android8hardware22configureRpcThreadpoolEmb(16, true);

    const char* versions[] = {
        "android.hardware.audio.effect@7.0::IEffectsFactory",
        "android.hardware.audio.effect@6.0::IEffectsFactory",
    };
    LibcxxString inst = LibcxxString::fromLiteral("default");

    int status = -1;
    const char* registered = nullptr;
    for (const char* ifaceName : versions) {
        LibcxxString iface = LibcxxString::fromLiteral(ifaceName);
        status = _ZN7android8hardware40registerPassthroughServiceImplementationERKNSt3__112basic_stringIcNS1_11char_traitsIcEENS1_9allocatorIcEEEES9_S9_(
            iface, iface, inst);
        if (status == 0) {
            registered = ifaceName;
            break;
        }
    }
    if (!registered) {
        ALOGE("Failed to register IEffectsFactory (@7.0/@6.0)/default: %d", status);
        return 1;
    }
    ALOGI("Registered %s/default (64-bit)", registered);
    _ZN7android8hardware17joinRpcThreadpoolEv();
    return 0;
}
