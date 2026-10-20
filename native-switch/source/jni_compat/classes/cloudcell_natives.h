#pragma once

#include "../jni.h"
#include "../../../include/rr3/elf_preflight.hpp"

/*
 * Resolving the game's own Java_* exports.
 *
 * Class::native_methods / jni_resolve_native() cannot be used for these:
 * they match by DT_SONAME and libRealRacing3.so has none, so
 * so_load_module() skips the whole resolution pass (loader/so_util.cpp:489).
 * Every Cloudcell class therefore leaves .native_methods = {NULL} and looks its
 * callbacks up by name, the same way android/input_bridge.cpp:1375 does for the
 * input entry points.
 *
 * The table is the GUEST's, not the host's. This used to go through
 * so_symbol(realracing3_module(), name), and so_symbol ignores its module
 * argument and calls rr3::resolveHostSymbol (eap_port/adapters.cpp) - the host
 * symbol table. For a guest-owned Java_* name that call could only ever return a
 * host symbol or 0, and in practice it always returned 0: the whole Cloudcell
 * callback layer resolved to NULL on every run and stayed silent about it,
 * because a NULL callback just skips the invocation. The module handle was
 * decorative. It now resolves through rr3::resolveMainImageExport, which finds
 * the main image by embeddedSonameIndex("libRealRacing3.so") and walks its
 * .dynsym - the same lookup that already resolves onCreateJNI,
 * onViewCreatedJNI and SetButtonValueJNI.
 *
 * There is deliberately no host fallback. Every name reaching this template is
 * a guest-owned Java_* entry point that the game registers as its own native, so
 * the host table is the wrong answer by construction; a fallback that can only
 * ever succeed for the wrong table is a trap for the next reader. A genuinely
 * host-supplied symbol belongs in a resolveHostSymbol binding row
 * (symbol_resolver.cpp), which is the table meant for those.
 *
 * Lazily, because the facades are constructed long after so_load_module() but
 * the ManagedMethod tables are built during static initialisation, before the
 * module exists.
 *
 * No pcs("aapcs") on any of these prototypes: unlike the input callbacks, not
 * one Cloudcell callback takes a float or a double, and for integer-only
 * argument lists the hard-float and soft-float PCS are identical.
 */

/*
 * Every name this template is asked to resolve, for the one-shot startup census
 * in jni_bridge.cpp (cloudcellNativesResolveSelfTest). Kept next to the lookup so
 * the census measures the real surface instead of a hand-copied subset of it.
 * Read once at startup; never walked from a deferred callback.
 *
 * 25 distinct names behind 22 cloudcell_native<> call sites: 20 written as
 * literals at their call sites, plus the 5 that reach the single runtime-name
 * lookup at cc_services.cpp:325 through fb_defer_bool. The alert callback is
 * written at two sites (jni_bridge.cpp and rr3_Platform.cpp) and so is listed
 * once.
 *
 * 2026-10-21, later: one more, HttpRequest_headerCallback (guest raw
 * 0x1ce4348), for the response headers the guest's ServerTimeManager reads
 * the server time out of. That makes 28 distinct names behind 25 call sites,
 * and 21 of the 28 resolve.
 *
 * The "2 of the 25 are present" claim that used to stand here was WRONG and
 * is replaced by the measurement: the boot census prints
 *     cloudcell-natives: 20/27 resolved in the main image
 * The names that do not resolve are the JNI-mangled inner-class spellings
 * (CC_1FacebookWorker_1Class_...) where the image exports the flat one
 * (FacebookWorker_...), plus the facades this guest does not ship at all.
 * The image is stripped, so the unresolved ones are absent from .symtab too
 * and cannot be resolved by name at all. That is a separate defect from
 * this one, in files this header does not own; the census line is what
 * makes it visible at boot instead of months later.
 *
 * 2026-10-21: two more, both PRESENT in the image, for the fake Cloudcell
 * login response: HttpRequest_dataCallback (guest raw 0x1ce493c) and
 * HttpRequest_completeCallback (guest raw 0x1ce4a3c). That makes 27 distinct
 * names behind 24 call sites.
 *
 * The "how many of them are present" sentence above was STALE and is
 * replaced by the measurement: the boot census in archive
 * 20261021-043823-eden-cloudresp prints
 *     cloudcell-natives: 20/27 resolved in the main image
 * so 20 of the 27 resolve, not 2. The 7 that do not are the
 * JNI-mangled inner-class spellings (CC_1FacebookWorker_1Class_..., and the
 * GoogleStore/GoogleAd facades this guest does not ship at all).
 *
 * The two success natives are named ONCE, here, and used from the call sites
 * through these constants. The lookup name is what tools/check-cc-symbols.py
 * verifies against the image, and a second copy of the same literal in a
 * second file would be a second thing to keep in step; one checked literal
 * used everywhere is both less to maintain and one less place to get wrong.
 */
inline constexpr const char *kCCHttpDataCallback =
    "Java_com_firemonkeys_cloudcellapi_HttpRequest_dataCallback";
inline constexpr const char *kCCHttpCompleteCallback =
    "Java_com_firemonkeys_cloudcellapi_HttpRequest_completeCallback";
/*
 * The response HEADERS. The guest's own implementation (guest raw 0x1ce4348)
 * walks the java.util.Map it is handed and fills the guest's header map at
 * worker+0x180, and the guest's ServerTimeManager reads the server time out
 * of THAT map (guest raw 0x1cef8a4 looks the key "X-Request-Start" up in it
 * and strtod's the value).
 *
 * Present in the image: .dynsym gives
 * Java_com_firemonkeys_cloudcellapi_HttpRequest_headerCallback at 0x1ce4348.
 */
inline constexpr const char *kCCHttpHeaderCallback =
    "Java_com_firemonkeys_cloudcellapi_HttpRequest_headerCallback";

inline const char *const *cloudcellNativeNames(unsigned &count) {
    static const char *const kNames[] = {
        "Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback",
        "Java_com_firemonkeys_cloudcellapi_HttpRequest_errorCallback",
        kCCHttpDataCallback,
        kCCHttpCompleteCallback,
        kCCHttpHeaderCallback,
        "Java_com_firemonkeys_cloudcellapi_AndroidAccountManager_LoginCompleteCallback",
        "Java_com_firemonkeys_cloudcellapi_UserInterfaceManager_WebBrowserLoadFailCallback",
        "Java_com_firemonkeys_cloudcellapi_FacebookWorker_LoginCallback",
        "Java_com_firemonkeys_cloudcellapi_FacebookWorker_LogoutCallback",
        "Java_com_firemonkeys_cloudcellapi_FacebookWorker_LoadFriendVectorCallback",
        "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_NativeLoginCallback",
        "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_LogoutCallback",
        "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_LoadFriendVectorCallback",
        "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_LoadProfileCallback",
        "Java_com_firemonkeys_cloudcellapi_GooglePlayWorker_ShareCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_InitializeCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_ProductDetailsErrorCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_PurchaseErrorCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_RestoreCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_RefreshStorePurchasesCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleAdManager_1Class_OnInterstitialFailed",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleAdManager_1Class_OnBannerFailed",
        "Java_com_firemonkeys_cloudcellapi_AppPromptManager_OnDontRate",
        "Java_com_firemonkeys_cloudcellapi_FacebookWorker_PermissionCheckCallback",
        "Java_com_firemonkeys_cloudcellapi_FacebookWorker_PermissionGrantCallback",
        "Java_com_firemonkeys_cloudcellapi_FacebookWorker_FeedPostCallback",
        "Java_com_firemonkeys_cloudcellapi_FacebookWorker_PhotoPostCallback",
        "Java_com_firemonkeys_cloudcellapi_FacebookWorker_FriendInviteCallback",
    };
    count = static_cast<unsigned>(sizeof(kNames) / sizeof(kNames[0]));
    return kNames;
}

template <typename Fn>
static inline Fn cloudcell_native(const char *name)
{
    return (Fn)rr3::resolveMainImageExport(name);
}
