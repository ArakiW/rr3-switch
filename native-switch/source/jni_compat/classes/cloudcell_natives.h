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
 * As of this writing 2 of the 25 are present in the main image's .dynsym and 23
 * are not - the guest spells those with the JNI-mangled inner-class form
 * (CC_1FacebookWorker_1Class_...) where the image exports the flat one
 * (FacebookWorker_...). The image is stripped, so those 23 are absent from
 * .symtab too and cannot be resolved by name at all. That is a separate defect
 * from this one, in files this header does not own; the census line is what
 * makes it visible at boot instead of months later.
 */
inline const char *const *cloudcellNativeNames(unsigned &count) {
    static const char *const kNames[] = {
        "Java_com_firemint_realracing_MainActivity_alertMessageExecuteCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1HttpRequest_1Class_errorCallback",
        "Java_com_firemonkeys_cloudcellapi_AndroidAccountManager_LoginCompleteCallback",
        "Java_com_firemonkeys_cloudcellapi_UserInterfaceManager_1Class_WebBrowserLoadFailCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1FacebookWorker_1Class_LoginCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1FacebookWorker_1Class_LogoutCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1FacebookWorker_1Class_LoadFriendVectorCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GooglePlusWorker_1Class_LoginCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GooglePlusWorker_1Class_LogoutCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GooglePlusWorker_1Class_LoadFriendVectorCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GooglePlusWorker_1Class_LoadProfileCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GooglePlusWorker_1Class_ShareCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_InitializeCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_ProductDetailsErrorCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_PurchaseErrorCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_RestoreCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleStoreServiceV3_1Class_RefreshStorePurchasesCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleAdManager_1Class_OnInterstitialFailed",
        "Java_com_firemonkeys_cloudcellapi_CC_1GoogleAdManager_1Class_OnBannerFailed",
        "Java_com_firemonkeys_cloudcellapi_CC_1AppPromptManager_1Class_OnDontRate",
        "Java_com_firemonkeys_cloudcellapi_CC_1FacebookWorker_1Class_PermissionCheckCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1FacebookWorker_1Class_PermissionGrantCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1FacebookWorker_1Class_FeedPostCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1FacebookWorker_1Class_PhotoPostCallback",
        "Java_com_firemonkeys_cloudcellapi_CC_1FacebookWorker_1Class_FriendInviteCallback",
    };
    count = static_cast<unsigned>(sizeof(kNames) / sizeof(kNames[0]));
    return kNames;
}

template <typename Fn>
static inline Fn cloudcell_native(const char *name)
{
    return (Fn)rr3::resolveMainImageExport(name);
}
