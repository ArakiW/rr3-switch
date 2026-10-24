// The scripted gamepad's line parser, EXECUTED against the documented format.
//
// The parser itself is not duplicated here: the test extracts the real
// parseFloat() and parsePadLine() out of
// native-switch/source/jni_compat/classes/rr3_InputDoors.cpp and writes
// them into pad_parser.inc, which this file includes. So the code that runs
// here is the code that ships.
//
// WHY A SEPARATE FILE. The parser lives in a translation unit that also
// pulls in libnx, EGL and the guest bridge, none of which a host compiler
// can build. The two parser functions depend on nothing but <cstring>,
// <cstdlib> and VirtualPadState, which is a plain struct in a header that
// includes only <cstdint> -- so they can be lifted out and run here.

#include "rr3/guest_input_doors.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

// The parser's own step type, copied from the port's source.
struct PadStep {
    unsigned long long at;
    rr3::VirtualPadState state;
};

// The real parser, extracted by the test driver.
#include "pad_parser.inc"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("FAIL: %s\n", what);
    }
}

bool near(float a, float b) {
    const float delta = a - b;
    return (delta < 0.0001f && delta > -0.0001f);
}

// A line that the parser must accept.
bool parse(const char* line, PadStep* out) {
    return parsePadLine(line, out);
}

}  // namespace

int main() {
    // ---- the OLD format still parses -----------------------------------
    //
    // "lx=<float> rx=<float> b=<hex mask>" is the format the earlier
    // experiments used. It has to keep working, or every archived script
    // silently stops meaning what it meant.
    {
        PadStep step{};
        check(parse("[at 100] lx=0.5 rx=-0.25 b=0x3", &step),
              "the old format must still parse");
        check(step.at == 100, "the old format must still set the frame");
        check(near(step.state.leftX, 0.5f), "lx must reach the left stick X");
        check(near(step.state.rightX, -0.25f),
              "rx must reach the right stick X");
        check(step.state.buttons == 0x3u, "b must be read as a HEX mask");
        // The keys the old format does not mention keep the default.
        check(near(step.state.leftY, 0.0f), "an absent key keeps its value");
        check(near(step.state.rightY, 0.0f), "an absent key keeps its value");
        check(near(step.state.leftTrigger, 0.0f),
              "an absent key keeps its value");
        check(near(step.state.rightTrigger, 0.0f),
              "an absent key keeps its value");
    }

    // ---- ALL SIX AXES ---------------------------------------------------
    {
        PadStep step{};
        check(parse("[at 1700] lx=0.1 ly=-0.2 rx=0.3 ry=0.4 lt=0.6 rt=1 "
                    "b=0x81",
                    &step),
              "the full six-axis format must parse");
        check(step.at == 1700, "the frame must be read");
        check(near(step.state.leftX, 0.1f), "lx -> AXIS_LTHUMB_X");
        check(near(step.state.leftY, -0.2f), "ly -> AXIS_LTHUMB_Y");
        check(near(step.state.rightX, 0.3f), "rx -> AXIS_RTHUMB_X");
        check(near(step.state.rightY, 0.4f), "ry -> AXIS_RTHUMB_Y");
        check(near(step.state.leftTrigger, 0.6f), "lt -> AXIS_LTRIGGER");
        check(near(step.state.rightTrigger, 1.0f), "rt -> AXIS_RTRIGGER");
        check(step.state.buttons == 0x81u, "b must be read as a HEX mask");
    }

    // ---- THE TRIGGERS, AND WHERE THEY LAND ------------------------------
    //
    // The guest's ControllerAxis ordinals are AXIS_LTRIGGER=4 and
    // AXIS_RTRIGGER=5. The parser has to put lt in the LEFT trigger and rt
    // in the RIGHT one: the two are not interchangeable, and a swap would
    // look like "the throttle works" while actually driving the brake.
    {
        PadStep step{};
        check(parse("[at 5] lt=0.35", &step), "a trigger-only line must parse");
        check(near(step.state.leftTrigger, 0.35f),
              "lt must be the LEFT trigger");
        check(near(step.state.rightTrigger, 0.0f),
              "lt must not touch the RIGHT trigger");
        check(near(step.state.leftX, 0.0f), "lt must not touch a stick");
        check(near(step.state.rightX, 0.0f), "lt must not touch a stick");

        PadStep other{};
        check(parse("[at 5] rt=0.35", &step), "a trigger-only line must parse");
        check(near(step.state.rightTrigger, 0.35f),
              "rt must be the RIGHT trigger");
        check(near(step.state.leftTrigger, 0.0f),
              "rt must not touch the LEFT trigger");
    }

    // ---- THE TRIGGER VALUE IS 0..1, UNSCALED ----------------------------
    {
        PadStep step{};
        check(parse("[at 1] lt=1 rt=0", &step), "a full trigger must parse");
        check(near(step.state.leftTrigger, 1.0f),
              "1.0 must mean fully pressed, not 100% of some other scale");
        check(near(step.state.rightTrigger, 0.0f),
              "0.0 must mean released");
    }

    // ---- THE STICKS ARE SIGNED ------------------------------------------
    {
        PadStep step{};
        check(parse("[at 1] lx=-1 rx=1", &step), "a signed stick must parse");
        check(near(step.state.leftX, -1.0f), "a stick is signed");
        check(near(step.state.rightX, 1.0f), "a stick is signed");
    }

    // ---- THE FRAME IS READ FROM THE BRACKETED FORM ----------------------
    {
        PadStep step{};
        check(parse("[at 4242] rt=1", &step), "a bracketed frame must parse");
        check(step.at == 4242, "the bracketed frame must be read");
    }

    // ---- LINES THAT MUST BE REJECTED -------------------------------------
    {
        PadStep step{};
        check(!parse("", &step), "an empty line must be rejected");
        check(!parse("# a comment", &step), "a comment must be rejected");
        check(!parse("nonsense", &step),
              "a line with no known key must be rejected");
        check(!parse("[at 1] zzz=1", &step),
              "an unknown key must not make the line valid");
    }

    std::printf("%s: %d checks, %d failures\n",
                g_failures == 0 ? "PASS" : "FAIL", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
