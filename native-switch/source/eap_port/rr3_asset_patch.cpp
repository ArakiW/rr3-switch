/*
 * Asset-accounting patch: stop RR3 from concluding that half a gigabyte of
 * assets is missing.
 *
 * What was measured, over the 21 startup asset lists and the donor tree:
 *
 *     entries in the lists                       12702
 *     present, md5 matching the install cache     5615
 *     considered missing                          7087  (548 124 892 bytes)
 *       of those, .dxt.dds / .atc.dds / .ptc.pvr  7056
 *       genuinely absent non-textures               31
 *
 * 99.6% of the "missing" bytes are texture variants for GPUs that are not this
 * one - Tegra, Adreno and PowerVR encodings that the donor never contained
 * because the Android build downloads only the family its GPU wants. Mali/ETC1
 * is complete.
 *
 * The engine already has the filter for that. CC_AssetManager_Class::
 * LoadAssetList (0x94f374) asks its agent SkipAsset() per entry through
 * vtable+8 (the call at 0x94f57a) before counting it, and
 * AssetDownloadService::SkipAssetImpl (0x322d9c) is
 *
 *     isTextureFilename(name) && !isPrimaryFilename(name)   -> skip
 *
 * In this port that filter is inert: mtTextureManager::isPrimaryFilename
 * (0x81d280) early-returns 1 ("yes, primary") at 0x81d2a8 when the render
 * singleton pointer is null, so nothing is ever skipped and all three foreign
 * formats are counted as missing. (That the null singleton is the concrete
 * reason is inference from the code, not something instrumented at runtime -
 * what is measured is that no entry is skipped.)
 *
 * The consequence is not a slow download, it is a permanent freeze:
 * OnUpdate (0x3276b8) sees a non-zero queued download size, asks for Wi-Fi,
 * gets none, and calls ShowNoWifiMessage (0x323534), which sets
 * CC_AssetManager+0x8d = 1 - the byte OnUpdate tests at its first instruction.
 * See jni/classes/rr3_Platform.cpp for the other half of that story.
 *
 * The fix here is to make SkipAsset answer the question the port can answer
 * exactly: "is this file actually on the card?" Anything not present is skipped
 * from the accounting, so the download queue comes out empty, no Wi-Fi gate is
 * reached, and the asset lists complete into OnAssetListsComplete (0x3272a8) ->
 * OnDownloadingComplete (0x320e20).
 *
 * Why file-existence and not a blanket "skip everything": a blanket skip also
 * empties the list of assets the game knows it has, and LoadAssetList's output
 * is more than a counter. Existence-based skipping keeps every entry the donor
 * can actually serve and drops exactly the ones that would be download work.
 *
 * Why this is safe to hook at all: SkipAsset has exactly three references in
 * the binary - the thunk at 0x322efc (a plain `b` to the impl), the Thn20
 * adjustor at 0x322f00, and one vtable slot in .data.rel.ro at 0xa7162c - and
 * the only caller is LoadAssetList. It takes no part in loading an asset.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>

#include "platform_adapter.h"
#include "so_util_adapter.h"
#include "trace_adapter.h"

static long g_skip_seen = 0;
static long g_skip_skipped = 0;

/*
 * bool AssetDownloadService::SkipAssetImpl(const char *name)
 *
 * r0 is `this` and is unused by the original too: its first act is to build a
 * std::string from r1 (0x322da8-0x322db4 pass sp+4 as the destination and never
 * read r0). Names arrive rooted at the content root, e.g. "/0.dat" - the engine
 * composes "<download path>/<name>" and the run log shows exactly
 * "/game//asset_list_base.txtCache.txt" from that concatenation.
 *
 * Plain C ABI: two pointer arguments, an integer result, no floats anywhere, so
 * the hard-float/soft-float boundary does not apply.
 */
extern "C" int rr3_skip_asset_impl(void *self, const char *name)
{
    (void)self;

    if (!name || !*name)
        return 1;

    char path[PATH_MAX];
    const char *root = io_game_dir();

    /* Names carry their own leading separator; "%s%s" would only be right when
     * they do, and one of the 21 lists is generated rather than shipped. */
    snprintf(path, sizeof(path), "%s/%s", root, name[0] == '/' ? name + 1 : name);

    ++g_skip_seen;
    if (access(path, F_OK) == 0)
        return 0;

    ++g_skip_skipped;

    /* Reported as it happens, not only at exit: the interesting runs are the
     * ones that end in a fault, and those never reach the summary. The first
     * skip also names the file, so a wrong root shows up as "everything is
     * missing" rather than as a silently empty download queue. */
    if (g_skip_skipped == 1)
        trace("asset patch: first absent asset '%s' (looked at %s)", name, path);
    else if (g_skip_skipped % 2000 == 0)
        trace("asset patch: %ld of %ld entries absent so far",
              g_skip_skipped, g_skip_seen);

    return 1;
}

void rr3_apply_asset_patches(so_module *mod)
{
    /* By symbol, not by offset: libRealRacing3.so keeps a .dynsym (it has no
     * .symtab, but every one of these is exported), so there is no need to
     * trust a hard-coded address the way the scaffold's inherited patch table
     * did - and no way for the hook to land mid-function on a donor that
     * turns out to be a different build. */
    static const char kSkipAsset[] = "_ZN20AssetDownloadService13SkipAssetImplEPKc";

    uintptr_t target = so_symbol(mod, kSkipAsset);

    /* AssetDownloadService is ARM, not Thumb (the whole Cloudcell range
     * 0x9298b0-0x962400 is Thumb, this is well below it), so the low bit must
     * be clear. If it is not, the donor is not the build these notes describe
     * and hooking blind is exactly the failure mode the scaffold's hard-coded
     * patch table warned about. */
    if (!target || (target & 1)) {
        trace("asset patch: %s not found or not ARM - download accounting "
              "left alone", kSkipAsset);
        return;
    }

    hook_address(mod, target, (uintptr_t)&rr3_skip_asset_impl);
    trace("asset patch: SkipAsset now answers from the donor tree (%s)", io_game_dir());
}

void rr3_report_asset_patch_stats(void)
{
    trace("asset patch: %ld asset list entries examined, %ld skipped as absent",
          g_skip_seen, g_skip_skipped);
}
