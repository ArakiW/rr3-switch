#pragma once
// THE PORT'S VERSION, IN ONE PLACE.
//
// WHY THIS FILE EXISTS.  The version used to live in three places: the
// Makefile's APP_VERSION (which becomes the NRO's NACP), the .nmeta's
// DisplayVersion (which becomes the NSP's control data), and a string
// literal in the on-screen dashboard.  Three copies, and two of them went
// stale -- the dashboard kept saying 0.22.4 while the packages said
// 0.22.10, so the version the user SEES was the one that was wrong.
//
// The Makefile and the .nmeta cannot both be the source (one feeds the NRO,
// the other the NSP, and they are different files in different formats), so
// this header is the third copy and the TESTS are what keep the three in
// agreement:
//
//   test_dashboard_contract.py  ->  this file == APP_VERSION == DisplayVersion
//   test_release_artifact.py   ->  the built NRO and NACP carry that version
//
// So a version bump is one edit here plus one in the Makefile plus one in
// the .nmeta, and a forgotten one is a TEST FAILURE rather than a lie on
// the console's screen.
#define RR3_PORT_VERSION "0.22.10"
