#pragma once

namespace rr3 {

// JOB 0: a one-shot host probe of the port's OWN sscanf, with the exact
// inputs the guest feeds it.
//
// WHY IT IS ON THE HOST AND NOT A GUEST SHIM. The guest's sscanf import is
// bound to the host newlib's sscanf (symbol_resolver.cpp, RR3_BIND(sscanf)),
// so calling the same function from the host with the same arguments answers
// the same question the guest's call would answer, without the risk of a
// shim: the guest calls sscanf 169 times, with many different formats, and
// a shim that changed the general behaviour would be a much larger change
// than a measurement.
//
// The probe also runs a POSITIVE CONTROL in both directions -- one input that
// must parse and one that must not -- so a probe that reported "r=0" for
// everything would be visibly broken rather than merely unlucky.
void runSscanfProbe();

// JOB 1: the guest's cc::ImageResManager resolution list, dumped.
//
// The manager is a circular intrusive list: this+0 = tail, this+8 = head,
// this+0x10 = count; the nodes are 0x48 bytes with prev@+0, next@+8, the
// width/height floats at +0x10/+0x14 and the path std::string at +0x30.
//
// The same line carries the OTHER end of the chain -- the
// cc::RookieChampionship::RookieChampionshipManager pointer and the
// Cloudcell's +0x4c1 byte -- because the question being answered is "which
// link of the menu chain is missing", and one line answers all of it.
void sampleImageResList(unsigned long long frames, const char* carrier);

} // namespace rr3
