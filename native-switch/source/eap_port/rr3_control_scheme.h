#ifndef REALRACING3_RR3_CONTROL_SCHEME_H
#define REALRACING3_RR3_CONTROL_SCHEME_H

/*
 * Pick the control scheme a device with a gamepad and no accelerometer should
 * boot into, by writing the game's own setting into its own save file. See the
 * file comment in rr3_control_scheme.cpp for the layout and the evidence.
 *
 * Call it before the game module is loaded: SaveManager::LoadPlayerProfile()
 * reads profile.dat during scene_LoadCharacter, which is well after this.
 */
void rr3_seed_control_scheme(const char *game_dir);

#endif /* REALRACING3_RR3_CONTROL_SCHEME_H */
