#ifndef REALRACING3_RR3_ASSET_PATCH_H
#define REALRACING3_RR3_ASSET_PATCH_H

#include "so_util_adapter.h"

/*
 * Redirect AssetDownloadService::SkipAsset at the donor tree, so the engine's
 * download accounting only counts assets that exist. See the file comment in
 * rr3_asset_patch.cpp for the measurements behind it.
 *
 * Must run after so_relocate_all() and before so_initialize(), i.e. from
 * so_after_relocate(): the text is still writable there and none of the game's
 * own code has executed.
 */
void rr3_apply_asset_patches(so_module *mod);

/* One line of accounting for the run log; the numbers are what tell you whether
 * the filter did anything at all. */
void rr3_report_asset_patch_stats(void);

#endif /* REALRACING3_RR3_ASSET_PATCH_H */
