#include <vector>

#include "../jni.h"
#include "../jni_internals.h"`n#include "../../eap_port/trace_adapter.h"
#include "os_Build.h"

// See os_Build.h. RR3_BUILD_IDENTITY=0 (the default for this A/B) answers the
// one field this class ever answered before, so the run reproduces the
// pre-2026-10-02 behaviour exactly. =1 restores the 22-field identity.
#if RR3_BUILD_IDENTITY
String osBuild::MANUFACTURER("samsung");
String osBuild::MODEL("SM-G900F");
String osBuild::BRAND("samsung");
String osBuild::DEVICE("klte");
String osBuild::PRODUCT("kltexx");
String osBuild::HARDWARE("qcom");
String osBuild::BOARD("MSM8974");
String osBuild::BOOTLOADER("klte");
String osBuild::RADIO("MSM8974.KTU84M");
String osBuild::TAGS("release-keys");
String osBuild::TYPE("user");
String osBuild::USER("build");
String osBuild::HOST("build");
String osBuild::ID("LRX21V");
String osBuild::DISPLAY("LRX21V");
String osBuild::FINGERPRINT(
    "samsung/kltexx/kltexx:5.1.1/LRX21V/3436227:user/release-keys");
String osBuild::RELEASE("5.1.1");
int32_t osBuild::SDK_INT = 22;
String osBuild::CPU_ABI("arm64-v8a");
String osBuild::CPU_ABI2("");
String osBuild::CPU_ABL("arm64-v8a");
#else
String osBuild::MANUFACTURER("JohnnyonFlame");
#endif

const FieldId osBuildClassFields[] = {
    REGISTER_STATIC_FIELD(osBuild, MANUFACTURER),
#if RR3_BUILD_IDENTITY
    REGISTER_STATIC_FIELD(osBuild, MODEL),
    REGISTER_STATIC_FIELD(osBuild, BRAND),
    REGISTER_STATIC_FIELD(osBuild, DEVICE),
    REGISTER_STATIC_FIELD(osBuild, PRODUCT),
    REGISTER_STATIC_FIELD(osBuild, HARDWARE),
    REGISTER_STATIC_FIELD(osBuild, BOARD),
    REGISTER_STATIC_FIELD(osBuild, BOOTLOADER),
    REGISTER_STATIC_FIELD(osBuild, RADIO),
    REGISTER_STATIC_FIELD(osBuild, TAGS),
    REGISTER_STATIC_FIELD(osBuild, TYPE),
    REGISTER_STATIC_FIELD(osBuild, USER),
    REGISTER_STATIC_FIELD(osBuild, HOST),
    REGISTER_STATIC_FIELD(osBuild, ID),
    REGISTER_STATIC_FIELD(osBuild, DISPLAY),
    REGISTER_STATIC_FIELD(osBuild, FINGERPRINT),
    REGISTER_STATIC_FIELD(osBuild, RELEASE),
    REGISTER_STATIC_FIELD(osBuild, SDK_INT),
    REGISTER_STATIC_FIELD(osBuild, CPU_ABI),
    REGISTER_STATIC_FIELD(osBuild, CPU_ABI2),
    REGISTER_STATIC_FIELD(osBuild, CPU_ABL),
#endif
    {NULL},
};

Class osBuild::clazz = {
    .classpath = "android/os/Build",
    .classname = "os_Build",
    .managed_methods = {NULL},
    .fields = osBuildClassFields,
    .instance_size = 0
};

static const int registered = ClassRegistry::register_class(osBuild::clazz);
