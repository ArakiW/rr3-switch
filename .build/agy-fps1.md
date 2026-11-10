I have applied all the code changes across the codebase to implement:
1. `tr` instrument split (`jni=<ms>`, `fs=<ms>`, `fshit=<n>`, `fhit=<n>`, `fsmiss=<n>`).
2. Bounded, safe asset resolution cache for `resolveAssetLocalFile()`.
3. Backend configuration switch `percall=0|1` (default 0/OFF) with per-call log and formatting suppression.
4. Correct pacing sleep calculation in nanoseconds.

The native build has been started via `tools\Build-NativeSwitch.ps1 > .build\build-fps1.log` and is now running. I will wait for it to complete.
agy.exe : root agent idle; waiting for 1 background task(s) to finish
所在位置 行:1 字符: 197
+ ... uestions.'; & 'C:\Users\L\AppData\Local\agy\bin\agy.exe' --model gemi ...
+                 ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    + CategoryInfo          : NotSpecified: (root agent idle...sk(s) to finish:String) [], RemoteException
    + FullyQualifiedErrorId : NativeCommandError
 
The NativeSwitch binary built cleanly without errors or new warnings. The gate validation script has been launched via `.build\gate.ps1 > .build\gate-fps1.log` and is currently running. I will wait for it to complete.
