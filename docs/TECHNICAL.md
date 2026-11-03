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

The wrapper preserves native AS/RT extensions and feature nodes, fixes unstable pNext storage, resolver identity and bounded extension enumeration. Native and proxy AS-size probes returned the same result.

Native RT support does not imply hardware RT cores. There is no general Float16/RayQuery lowering implementation. Shader/pipeline creation success does not prove correct execution.

## Evidence

The menu test captured 1051 distinct shader modules: 673 contained Float16 types, 110 declared RayQueryKHR and none declared MeshShadingNV. The subsequent FSR3 set used FP32. These counts describe one run.

After correction, the game log recorded native vkCmdTraceRaysKHR execution. Initial RT pipeline creation returned VK_OPERATION_DEFERRED_KHR, a deferred-operation status rather than a negative error. Visual success was reported by the user after loading completed.

Raw minidumps, BugSplat reports and proprietary SPIR-V captures are not published.

## Remaining work

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