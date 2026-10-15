# Formal UI 0.18.3 renderer result

Device result: pressing Y terminated the application and Horizon displayed a
generic software error.

The loader, relocation and ModuleCode permission results were already healthy
before the action. The failure therefore belongs to the display-backend
transition introduced in 0.18.3: the running software console released its
framebuffer and the same default NWindow was immediately handed to Mesa EGL in
the same process.

Resolution for 0.18.4:

- remove the runtime `consoleExit -> EGL -> consoleInit` transition;
- make the Mesa probe an exclusive startup mode selected by holding Y;
- complete EGL teardown before the software console is initialized;
- keep runtime Y read-only and informational;
- keep guest constructors disabled.

Do not use the 0.18.3 Y action again.
