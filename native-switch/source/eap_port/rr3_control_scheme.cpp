/*
 * Boot a gamepad-only device into a control scheme the game itself offers.
 *
 * Real Racing 3 on Android defaults to ControlMethods_e 0, "Tilt A", because
 * every phone has an accelerometer. An R36S does not, and the engine tells the
 * player so on every controller connect: "Note: Tilt A (default) control scheme
 * still active". That note comes from CGlobal::scene_ProcessConnectedJoysticks
 * (0x43f748) and is printed whenever game_GetCurrentControlMethod() <= 7.
 *
 * What the scheme actually decides, measured in the donor binary:
 *
 *   Car::CalculateJoystickSteeringAngles (0x33ad38) takes the steering angle
 *   from JoystickInput::getSteering() for every scheme once a pad is present,
 *   so steering is not the problem.
 *
 *   Car::ReadPlayerAccelerationInput (0x33b104..0x33b14c) is the one that
 *   changes: schemes 6, 0 and 5 each store a literal 1.0 - the game drives the
 *   throttle for you - and every other scheme reads
 *   JoystickInput::getAcceleration(), i.e. the pad's own trigger.
 *
 * Scheme 7 (Wheel_Manual, "Wheel B") is the only entry the game's own Controls
 * menu offers that both takes steering from the stick through the explicit
 * 0xE0-mask branch and leaves the throttle to the player. That is the scheme a
 * gamepad wants, so that is what a fresh install is seeded with.
 *
 * Why writing the file is not "patching":
 *
 *   SaveManager::SavePlayerProfile (0x922e3c) is literally
 *   "PlayerProfile::InitFromGlobalSettings(); AssetSaveFile(name, buf, 124)" -
 *   a raw 124-byte dump with no header, no checksum and no encryption, and
 *   SaveManager::LoadPlayerProfile (0x922ef4) memcpy()s it straight back over
 *   the defaults. PlayerProfile::InitGlobalSettings (0x419e84) then does
 *
 *       ldr r1, [r4, #20] ; cmp r1, #7 ; movhi r1, #0 ; strhi r1, [r4, #20]
 *       b   game_SetControlMethod
 *
 *   so offset 20 is the ControlMethods_e (clamped to 0..7, which is why 8 is
 *   not reachable this way and does not matter - 8 is RuleSet_DragRace's
 *   per-mode override, not a pad scheme) and it reaches exactly the same
 *   CGlobal::game_SetControlMethod that FrontEnd2::ControlsMenu::
 *   OnSetControlMethod calls. Offset 24 is the "flipped" flag (CGlobal+0x43dc).
 *
 * Seeded once and never again: the marker file below is what makes the
 * player's own choice in the Controls menu survive the next launch. The
 * game's menu remains the canonical way to change this; this only decides
 * what a first boot starts from.
 *
 * REALRACING3_CONTROL_METHOD=N (0..7) forces a re-seed, for A/B runs.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "rr3_control_scheme.h"
#include "trace_adapter.h"

/* PlayerProfile::SetDefaults() writes version 13 and the struct is 124 bytes;
 * both are checked before touching anything, so a profile from a different
 * donor build is left alone rather than corrupted. */
static const long kProfileSize = 124;
static const uint32_t kProfileVersion = 13;
static const long kControlMethodOffset = 20;
static const long kFlippedOffset = 24;

/* ControlMethods_e 7 = Wheel_Manual, the game's "Wheel B". */
static const uint32_t kDefaultControlMethod = 7;

static const char kProfileName[] = "profile.dat";
static const char kMarkerName[] = ".control_scheme_seeded";

void rr3_seed_control_scheme(const char *game_dir)
{
    if (!game_dir)
        return;

    char profile_path[4096];
    char marker_path[4096];
    snprintf(profile_path, sizeof(profile_path), "%s/%s", game_dir, kProfileName);
    snprintf(marker_path, sizeof(marker_path), "%s/%s", game_dir, kMarkerName);

    uint32_t method = kDefaultControlMethod;
    bool forced = false;
    const char *env = getenv("REALRACING3_CONTROL_METHOD");
    if (env && *env) {
        char *end = NULL;
        long value = strtol(env, &end, 10);
        if (end && !*end && value >= 0 && value <= 7) {
            method = (uint32_t)value;
            forced = true;
        } else {
            trace("control scheme: REALRACING3_CONTROL_METHOD=%s ignored "
                  "(the profile clamps this field to 0..7)", env);
        }
    }

    struct stat marker_st;
    if (!forced && stat(marker_path, &marker_st) == 0) {
        trace("control scheme: already seeded, leaving the player's choice "
              "alone (%s)", marker_path);
        return;
    }

    FILE *file = fopen(profile_path, "r+b");
    if (!file) {
        /* A first-ever launch has no profile yet; SetDefaults() will run and
         * the next launch gets seeded. Not an error. */
        trace("control scheme: no %s yet (%s), nothing to seed",
              kProfileName, strerror(errno));
        return;
    }

    unsigned char buffer[kProfileSize];
    size_t read = fread(buffer, 1, sizeof(buffer), file);
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    if (read != sizeof(buffer) || size != kProfileSize) {
        trace("control scheme: %s is %ld bytes, expected %ld - left untouched",
              kProfileName, size, kProfileSize);
        fclose(file);
        return;
    }

    uint32_t version = (uint32_t)buffer[0] | ((uint32_t)buffer[1] << 8) |
                       ((uint32_t)buffer[2] << 16) | ((uint32_t)buffer[3] << 24);
    if (version != kProfileVersion) {
        trace("control scheme: %s is version %u, expected %u - left untouched",
              kProfileName, version, kProfileVersion);
        fclose(file);
        return;
    }

    uint32_t current = (uint32_t)buffer[kControlMethodOffset] |
                       ((uint32_t)buffer[kControlMethodOffset + 1] << 8) |
                       ((uint32_t)buffer[kControlMethodOffset + 2] << 16) |
                       ((uint32_t)buffer[kControlMethodOffset + 3] << 24);

    unsigned char encoded[4] = {
        (unsigned char)(method & 0xff), 0, 0, 0
    };
    unsigned char flipped = 0;
    if (fseek(file, kControlMethodOffset, SEEK_SET) != 0 ||
        fwrite(encoded, 1, sizeof(encoded), file) != sizeof(encoded) ||
        fseek(file, kFlippedOffset, SEEK_SET) != 0 ||
        fwrite(&flipped, 1, 1, file) != 1) {
        trace("control scheme: could not write %s (%s)", kProfileName,
              strerror(errno));
        fclose(file);
        return;
    }
    fclose(file);

    /* Read back from disk rather than trusting the write: the whole point of
     * the trace is telling "the seed was applied" apart from "the engine
     * ignored it". */
    uint32_t written = 0xffffffff;
    file = fopen(profile_path, "rb");
    if (file) {
        unsigned char check[4] = {};
        if (fseek(file, kControlMethodOffset, SEEK_SET) == 0 &&
            fread(check, 1, sizeof(check), file) == sizeof(check))
            written = (uint32_t)check[0] | ((uint32_t)check[1] << 8) |
                      ((uint32_t)check[2] << 16) | ((uint32_t)check[3] << 24);
        fclose(file);
    }

    trace("control scheme: profile control method %u -> %u (read back %u)%s",
          current, method, written, forced ? " [forced by env]" : "");

    if (!forced) {
        FILE *marker = fopen(marker_path, "wb");
        if (marker) {
            fputs("seeded by the port; delete to re-seed the control scheme\n",
                  marker);
            fclose(marker);
        }
    }
}
