# Installation and supported builds

## Build from source

Use Windows x64, Visual Studio 2022 Build Tools with C++ support, and CMake 3.24
or newer. CMake downloads hash-pinned Lua, MinHook and Dear ImGui dependencies.

```powershell
python -m pip install -r tools/test-requirements.txt
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --prefix dist
```

This is a source preview, not a general-user binary release. CMake must select
the Python interpreter containing the pinned dependencies. The public suite
explicitly skips local original-byte research checks; see the
[fixture policy](../../tests/fixtures/README.md) and [contribution guide](../../CONTRIBUTING.md).
Public tests do not provide fresh live game acceptance.

The install includes binaries and examples in `dist/bin`, native headers, tools,
the README and these public reference guides. A standalone lifecycle test host
can run without a game:

```powershell
.\build\Release\fdkss_host.exe .\examples\Mods
```

Enter `tick`, `reload HelloFDK` or `quit`. Engine APIs are unavailable in this host;
`emit map.loaded Rolent` simulates an event rather than observing a real map.

## Pinned game builds

| Executable | SHA-256 |
| --- | --- |
| `sora_1st.exe` | `4f84cb3be200cfd0485d9d6b2fec0ba70a63d73d3fb8a29a515e3a7abdbf13cf` |
| `sora_2nd.exe` | `d8b2911d1576216bdc22d070550e4f531e105de7ed2981885849669f4acf8aaf` |
| `ysx_pn.exe` (core mod platform) | `34badc3071d5c571524a2234ad64a4c4251214de578eaa18c04b96432501765e` |
| `ysx.exe` (core mod platform) | `224df0db3e0867cc83d4f71b2e3e78f71f47c430899c76b9beb12da5403ae246` |

Other hashes are rejected before DLL loading and again during attachment.
Original and Proud Nordics support Lua/native mod lifecycle, frame updates, reload and the
external console. It uses an independently verified Framework frame bridge on
the main thread. Actor, property, VM, content and render APIs remain unavailable
there. Daybreak I/II have no runtime adapter. Shared engine
lineage does not establish ABI compatibility.

## Attach and control

Start a supported game normally. Keep the installed DLL, injector and Mods in a
writable location outside the game folder. Use a fresh PID and absolute DLL path:

```powershell
Get-Process sora_1st,sora_2nd,ysx_pn,ysx -ErrorAction SilentlyContinue | Select-Object Id,StartTime,Path
.\dist\bin\fdkss_inject.exe <pid> "C:\FDKss\dist\bin\fdkss.dll"
```

Replace the PID placeholder and example path. Inspect `FDKss.log` beside the DLL.
Injection success alone does not prove Lua readiness: the game update thread must
advance before initialization and queued commands execute. Title/loading states
may have no player or scene.

In Sky, with game focus, F9 opens the console, F10 reloads enabled mods and F11 stops Lua.
Escape closes the console. Reload/stop/detach can also be requested through the
injector using the same PID/path and `--reload`, `--stop` or `--detach`.
Check completion in the log; a queued request is not a completed operation.
Both Ys builds use the external console and injector commands; their render and
in-game console are unavailable. Frame callbacks can continue while gameplay is
paused, so advancing mod counts do not prove background world simulation.

Stop and detach do not provide a public runtime restart. The same DLL remains
pinned and inactive; restart the game before reattaching that module. Do not
replace a loaded DLL file. Cleanup callbacks are not guaranteed on process
termination. A console timeout does not cancel a queued mutation. Background
pause can prevent requests from executing even without system overload.
