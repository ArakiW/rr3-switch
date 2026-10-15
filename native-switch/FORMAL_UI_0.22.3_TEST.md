# Formal native UI 0.22.3 - JNI bridge expansion test

This test validates the 0.22.3 JNI bridge expansion after the 0.22.1 guest-boot
pass. The main constructors and the onCreate/onStart/onResume lifecycle chain
already returned on real hardware; this build implements the high-frequency JNI
slots identified by that run and supplies stable method/object handles plus
Switch platform values.

## Install

Copy the `switch` folder from `dist/rr3-native-rehost-complete-0.22.3/` to the
TF-card root. The final layout is:

```
/switch/rr3-native-rehost/rr3-native-rehost.nro
/switch/rr3-native-rehost/data/core.romfs
/switch/rr3-native-rehost/data/gui-hd.romfs
/switch/rr3-native-rehost/data/gui-sd.romfs
/switch/rr3-native-rehost/data/tracks.romfs
/switch/rr3-native-rehost/data/vehicles-classic.romfs
/switch/rr3-native-rehost/data/vehicles-mid.romfs
/switch/rr3-native-rehost/data/vehicles-modern.romfs
```

The package root contains `checksums.sha256`; all ten entries must report `OK`.
The NRO hash for this build is:

```
3ed8cde0a98c7db019f71093303fcfd01794c541b19bafaed032667e6e69f115
```

## Run the guarded probe

1. Launch the NRO and inspect the Overview/Runtime pages.
2. If Recovery is shown, preserve the marker and any crash report, then press
   **B** once to clear the old guarded action.
3. Press **ZR** once. Confirm the dependency stage reaches `complete=yes`,
   `active=yes`, and `constructors=3/3`.
4. Without leaving the application, press **ZL** once. The guarded action now
   runs JNI_OnLoad, all 181 main constructors, onCreateJNI, onStartJNI, and
   onResumeJNI in one guest TLS scope.
5. On success, Loader page 2 should show main constructors `181/181`, all three
   lifecycle callbacks `resolved/called/returned=yes`, and the JNI slot counts.
   The new wrappers should prevent the previous high-frequency hits for slots
   `113, 115, 29, 19, 20, 35, 142, 38, 50, 62, 130`.
6. Press **PLUS** once to run finalizers, reverse-unmap the four images, clear
   the guarded marker, and return cleanly to hbmenu.

## Export the report

Hold **L+R** and tap **X** once to enable developer mode. Release **L+R**, then
press **X** alone. Retrieve:

```
/switch/rr3-native-rehost/loader-report.txt
```

Relevant lines:

```
main_init attempted=1 preflight=1 completed=1 active=1 ... called=181 planned=181 ...
guest_entry attempted=1 resolved=1 called=1 returned=1 code=0
guest_lifecycle start=1/1/1 resume=1/1/1
jni_phase create unhandled=<n> bindings=<n> last=<n>
jni_phase start unhandled=<n> last=<n>
jni_phase resume unhandled=<n> last=<n>
jni_slots_top ...
```

The important comparison is `jni_slots_top`: a remaining slot is a concrete
implementation target, while `last=4294967295` means that phase had no
unsupported-table call. Keep the complete report if a new failure occurs; the
`guest_boot_progress=` line identifies the last durable phase even without an
Atmosphere crash file.

## Automation

`--rr3-auto-boot` still chains dependency initialization and the guest boot at
startup. Normal validation is **ZR**, then **ZL**, report export, and **PLUS**.
