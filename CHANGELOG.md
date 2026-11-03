# Changelog

## Unreleased

- Record a confirmed extended-gameplay hang on GTX 1080 Ti / NVIDIA 582.28: device loss followed by repeated query-result polling. The cause of device loss is unresolved.
- Add opt-in `RuntimeTrace=1` diagnostics for queue submissions, presentation, query polling, waits and allocation errors. Native arguments, output data and result codes are preserved.
- Enable native `VK_EXT_device_fault` reporting when diagnostics are requested and the driver supports it. Capture bounded fault details once per device; do not fabricate successful results after device loss.
- Extend the probe with a real GPU timestamp submission and a bounded fence wait. Add synthetic error/data forwarding and fault-reporting regression tests.

## v0.1.0-pascal.1 — 2026-11-03

First experimental DOOM: The Dark Ages release of this fork. Startup, menus and 3D output confirmed on GTX 1080 Ti / NVIDIA 582.28; extended gameplay remains unverified.

- Keep recursive Vulkan procedure resolution inside the proxy.
- Correct extension enumeration counts, truncated output and layer handling.
- Keep copied device feature-chain nodes at stable addresses.
- Preserve Vulkan 1.3 and supported native AS/RT device features.
- Add opt-in Pascal RT initialization compatibility for the tested DOOM build.
- Forward native ray dispatches instead of swallowing them or clearing outputs.
- Disable automatic ShadowLift and acquire-time postprocessing.
- Add shader/pipeline diagnostics, bounded optional captures and live-readable logs.
- Add llvm-mingw builds, native/proxy probes, installation guides and checksums.

Known issue: a black screen followed enabling FSR with another dynamic graphics option. No FSR compatibility or whole-series GTX 10xx support claim is made.
