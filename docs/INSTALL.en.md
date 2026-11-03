# Installation and rollback

Experimental release tested with DOOM: The Dark Ages, GTX 1080 Ti and NVIDIA 582.28. Startup, menus and a 3D scene were confirmed by the user. Full-game stability, other GPUs and FSR compatibility are unverified.

1. Close the game. Back up existing `vulkan-1.dll`, `vulkan-1_real.dll` and `Thanos-wrapper.ini` in its executable directory.
2. Download `Thanos-wrapper-v0.1.0-pascal.1-windows-x64.zip` from [Releases](https://github.com/4erk/Thanos_wrapper/releases/tag/v0.1.0-pascal.1). Root-level ZIPs in this repository are inherited upstream builds.
3. Copy `C:\Windows\System32\vulkan-1.dll` into the directory containing `DOOMTheDarkAges.exe`, naming the copy `vulkan-1_real.dll`. Leave the system original in place.
4. Extract the release's `vulkan-1.dll` and `Thanos-wrapper.ini` into the same game directory. The supplied INI enables `PascalRTGate=1` and disables shader-file capture.
5. Launch the game with FSR off for the initial test. Allow startup/loading to finish; the scene may appear after a delay.

The archive does not redistribute the system Vulkan loader, game shaders or crash dumps. No game-EXE patch or driver installation is part of this setup.

## Limitations

A black screen was reported after enabling FSR together with another dynamic graphics setting. The individual trigger has not been isolated. This release does not claim an FSR fix, complete Float16/RayQuery implementation or mesh-shader emulation.

## Rollback

Close the game, remove the three installed files from the game directory and restore previous wrapper files if backed up. Do not remove the system Vulkan loader or game saves.

## Diagnostics

The wrapper writes `vkproxy_<PID>.log` next to its DLL. To capture SPIR-V, set `ShaderDumpDirectory` under `[diagnostics]` in `Thanos-wrapper.ini` to an existing directory. Capture is deduplicated and capped at 256 MiB per process. Leave it empty to disable. Environment variables `THANOS_PASCAL_RT_GATE` and `THANOS_SHADER_DUMP_DIR` override the INI.

Bug reports should include GPU, driver, game build, wrapper release and reproduction steps. Review logs before sharing; do not publish memory dumps or proprietary game shaders.

Compare the ZIP's SHA-256 with the release's `SHA256SUMS.txt` using `Get-FileHash -Algorithm SHA256`. File-level checksums are included in the archive.