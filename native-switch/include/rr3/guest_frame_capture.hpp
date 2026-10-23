#pragma once
//
// The framebuffer capture, in its own translation unit.
//
// WHY A SEPARATE TU. guest_view_probe.cpp is a 450 KB file and it sits on
// the frame-loop thread. The PNG capture was first written inline there,
// and the next three Eeden runs died 42-55 s in -- earlier than any run
// with the previous binary, which lived 223 s and 858 s. The capture
// itself never executed in those runs (the frame counter never reached
// the first capture point), so the only thing the change did was move
// code and grow the NRO, and the mechanism for that turning into an
// early death is not understood yet.
//
// So the code moves OUT of the probe: this file plus
// guest_frame_capture.cpp, and the probe's frame-loop function changes
// by a call and a include. If the early deaths follow the layout, they
// stop following it; if they do not, the layout was never the cause and
// the early deaths are the intermittent Eeden failure they look like.
//
// THE MEASUREMENT, for the record: the guest's back buffer is NOT black.
// The readback in guest_view_probe.cpp reports, on the 20261024-000648
// archive, "LATE frame=3000 full 1280x720 lit=921600 of 921600
// px0=7f8a46ff quads[r_bins=226703,285945,125911,283041]
// pts=[818c48 7f8080 03438b 182c43 0c0d0f]" -- every pixel lit, all
// four red bins populated, five probe points of five different colours.
// The source comment that said "lit=0 of 921600 ... the guest really
// has nothing drawn" described an archive from before the fixes and is
// no longer true.
//
namespace rr3 {

// Writes the given RGBA8 buffer out as a PNG.
//
// `bottomUpRgba` is in the order glReadPixels returns: the FIRST row is
// the BOTTOM row of the image. PNG stores rows top-down, so this flips.
//
// Returns true only when the whole file reached the disk.
bool writeFramePng(const char* path,
                   const unsigned char* bottomUpRgba,
                   int width,
                   int height);

}  // namespace rr3
