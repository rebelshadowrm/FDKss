static unsigned calls = 0;
extern "C" __declspec(dllexport) void __cdecl SteamAPI_RunCallbacks(void) { ++calls; }
extern "C" __declspec(dllexport) unsigned __cdecl TestPumpCount(void) { return calls; }
