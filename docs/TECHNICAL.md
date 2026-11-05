# Pascal investigation

The unwrapped game stopped at its shaderFloat16 requirement. The original wrapper bypassed capability checks but crashed through a NULL vkGetAccelerationStructureBuildSizesKHR pointer.

Read-only inspection of the tested executable and minidump identified two issues: resolver calls could escape the proxy, and DOOM conditionally left its own RT function table uninitialized. Its NVIDIA initialization branch included VK_NV_mesh_shader, which the tested Pascal driver does not advertise.

The opt-in PascalRTGate experiment exposes that extension name while retaining native mesh feature bits. Unsupported extensions are filtered before device creation. This passes the tested engine's loader gate but does not implement mesh shaders or Vulkan conformance.

The next build reached menus without a 3D scene. Logs confirmed RT dispatches were intercepted by upstream no-op/debug-clear code. The final correction forwards native direct/indirect ray dispatches and removes those output clears. The user then reported the image appearing and normal operation.

## Native capabilities: GTX 1080 Ti / NVIDIA 582.28

| Capability | Native result |
| --- | --- |
| accelerationStructure | true |
| rayTracingPipeline | true |
| bufferDeviceAddress | true |
| shaderFloat16 | false |
| rayQuery | false |
| VK_NV_mesh_shader | absent |
| robustBufferAccess / robustBufferAccess2 | true / true |
| robustImageAccess2 / nullDescriptor | true / true |
| robustBufferAccessUpdateAfterBind | true |

The wrapper preserves native AS/RT extensions and feature nodes, fixes unstable pNext storage, resolver identity and bounded extension enumeration. Native and proxy AS-size probes returned the same result.

Native RT support does not imply hardware RT cores. There is no general Float16/RayQuery lowering implementation. Shader/pipeline creation success does not prove correct execution.

## Evidence

The menu test captured 1051 distinct shader modules: 673 contained Float16 types, 110 declared RayQueryKHR and none declared MeshShadingNV. The subsequent FSR3 set used FP32. These counts describe one run.

After correction, the game log recorded native vkCmdTraceRaysKHR execution. Initial RT pipeline creation returned VK_OPERATION_DEFERRED_KHR, a deferred-operation status rather than a negative error. Visual success was reported by the user after loading completed.

Raw minidumps, BugSplat reports and proprietary SPIR-V captures are not published.

## Extended gameplay hang, 2026-11-03

The user reported a freeze during ordinary gameplay without changing settings. The process started at 19:46:49; Windows recorded NVIDIA event 153 at 20:16:19. A minidump showed one game worker repeatedly entering the native driver's query-result function while the main thread waited for work completion.

Read-only disassembly of this game build identified its `vkGetQueryPoolResults` polling loop: it retries both `VK_NOT_READY` and `VK_ERROR_DEVICE_LOST`. The latter calls a routine that immediately returns in this build. A second live capture read the driver's device-lost flag as 1, confirming loss of the Vulkan device. This establishes why the process remained stuck, but not which GPU command caused the loss. Event ID 153 alone is not a diagnosis of defective hardware, overheating or memory exhaustion.

The current development build adds an opt-in diagnostics preset:

```ini
[diagnostics]
RuntimeTrace=1
ShaderDumpDirectory=
```

`[GPU]` lines report submissions, presentations and queries every 15 seconds of API activity. `[GPU-ERROR]` preserves native negative result codes. `[GPU-QUERY-STALL]` identifies repeated unsuccessful polls lasting at least five seconds. If supported, `VK_EXT_device_fault` is enabled and queried once after `VK_ERROR_DEVICE_LOST`; descriptions and up to 64 address/vendor entries are logged. Vendor binary captures are disabled. These diagnostics do not recover a lost device or fix the GPU fault, and the application is not terminated automatically.

The probe now executes a small native command buffer with two timestamp writes, waits at most five seconds for its fence, and reads the results. Both native and wrapped paths passed after the hung process was closed. This verifies basic queue/query forwarding, not execution of DOOM's shaders.

## Repeat hang and robustness experiment, 2026-11-05

The second recorded failure happened during combat or movement in a different location. The process started at 01:48:32 and Windows recorded NVIDIA event 153 at 03:52:09, about 2 hours 4 minutes later. The earlier failure was about 30 minutes after launch; these observations do not establish a fixed timer or a single problematic location. An intervening diagnostic session ended after approximately 37 minutes without a logged device-loss error.

The runtime log first reported `VK_ERROR_DEVICE_LOST` from `vkQueueSubmit`. Native `VK_EXT_device_fault` reporting then returned a `WRITE_INVALID` address at `0x0` with precision 4096, identifying an invalid write somewhere in the interval 0–4095. Two other entries were `INSTRUCTION_POINTER_UNKNOWN`; they do not identify a shader or prove that those instructions caused the fault. The minidump again showed the game's query-result polling loop after device loss. Successful submissions/presentations continued until shortly before the fault; there was no new shader/pipeline creation logged immediately before it. GPU temperatures measured after the freeze cannot establish the temperature at failure.

The candidate mitigation enables driver-supported memory robustness at device creation:

```ini
[experiments]
PascalRTGate=1
RobustAccess=1
[diagnostics]
RuntimeTrace=1
ShaderDumpDirectory=
```

`RobustAccess` is off by default. When enabled, the wrapper uses native feature queries, adds `VK_EXT_robustness2` only if supported and enables `robustBufferAccess`, `robustBufferAccess2`, `robustImageAccess2` and `nullDescriptor` as available. Buffer robustness is not newly enabled alongside buffer update-after-bind features unless the driver supports that combination. The changes affect the wrapper's copied feature chain; the application's structures are not modified. The previous failing game's device request did not include extended robustness2. Its original core `robustBufferAccess` value was not separately logged.

On the tested GTX 1080 Ti, a standalone compute test passed with both `pEnabledFeatures` and `VkPhysicalDeviceFeatures2` device-creation paths: a null-buffer read returned zero, null-buffer writes and an out-of-range buffer write were discarded, a valid write completed and sentinel data was preserved. The test uses original test shader source, not captured game shaders. Basic queue/fence/timestamp forwarding and feature-chain tests also passed. The restarted game logged all four robustness features enabled and resumed successful presentations.

This targets descriptor-based accesses covered by Vulkan robustness guarantees. It does not make arbitrary buffer-device-address pointers, dangling or uninitialized descriptors safe, and does not implement unsupported Float16/RayQuery shader features. A successful small compute test does not establish that the offending game operation is covered. Sustained gameplay validation is still required.

Separately, query polling now yields the CPU for 10 ms only after the native function returns `VK_ERROR_DEVICE_LOST`, with tracing either on or off. Native return codes and output data are preserved; `VK_NOT_READY` polling is unchanged. This addresses the post-failure CPU busy loop, not device recovery. The wrapper does not terminate the game automatically.

## Remaining work

- Identify the GPU operation responsible for the confirmed device loss.
- Isolate FSR and the additional dynamic-setting failure independently.
- Test levels, lighting correctness, performance and sustained stability.
- Test additional Pascal GPUs before extending compatibility claims.
- Replace capability spoofing with implemented compatibility paths where needed.
- Reduce inherited experimental code and add GPU execution tests.

## References

- [Original Thanos wrapper](https://github.com/jase212007/Thanos_wrapper)
- [Vulkan extension enumeration](https://docs.vulkan.org/refpages/latest/refpages/source/vkEnumerateDeviceExtensionProperties.html)
- [NV mesh shader](https://docs.vulkan.org/refpages/latest/refpages/source/VK_NV_mesh_shader.html)
- [AMD FSR integration](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)
- [Vulkan robustness guarantees and limits](https://docs.vulkan.org/guide/latest/robustness.html)
- [Device-fault address types](https://docs.vulkan.org/refpages/latest/refpages/source/VkDeviceFaultAddressTypeKHR.html)
