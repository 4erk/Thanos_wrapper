# Changelog

## Unreleased

- Record failure of the robustness candidate: another `WRITE_INVALID` fault in address interval 0–4095 after 13 minutes, with guards enabled. Pre-fault samples show 83–84 °C, 99% GPU utilization and approximately 1.1 GiB free adapter memory; these values alone do not identify the cause.
- Add opt-in native NVIDIA GPU checkpoints around draw, dispatch and selected RT/AS commands, bounded marker history and once-per-device reporting after real device loss. Log pipeline-to-shader-module mappings for correlation.
- Add opt-in `FrameLimit=30` host pacing before presentation, including the separate GDPA presentation path. Preserve native presentation results and GPU command ordering. This is a load-reduction experiment, not a confirmed stability fix.
- Investigate a second hang on 2026-11-05: native device-fault reporting identifies an invalid GPU write in the address interval 0–4095, followed by device-loss query polling. The responsible shader/command is still unknown.
- Add opt-in `[experiments] RobustAccess=1` to enable native buffer/image robustness and null descriptors when supported, with update-after-bind compatibility checks. Subsequent gameplay showed this candidate was insufficient to prevent the observed fault.
- Back off query polling by 10 ms after `VK_ERROR_DEVICE_LOST`, including with tracing disabled, to prevent the observed CPU busy loop. Preserve the real error; do not claim GPU recovery.
- Test feature-chain handling and real GPU null-buffer reads/writes and out-of-range stores through both device-feature creation paths. Add native robustness capability reporting to the probe.
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
