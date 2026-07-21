# Vulkan Support Model

vkrelay2 is a Vulkan ICD and remote execution backend, not a loader layer over an otherwise complete
Linux driver. Its observable Vulkan surface is the intersection of:

1. what the selected Windows physical device and driver support;
2. what the vkrelay2 ICD can encode and validate;
3. what the Windows worker can faithfully rebuild and execute;
4. the frontend lane selected for the application.

Applications must use normal Vulkan capability discovery. A feature or extension omitted by the ICD
is unavailable even when the Windows driver supports it.

## API Version Policy

The default and OpenGL/Zink frontends use the conservative lane:

- instance functionality is kept to the surface needed by the X11 and Zink path;
- the physical-device API is capped below Vulkan 1.3;
- features that would steer Zink onto unserved command or pipeline shapes are masked out.

`--frontend vulkan13` selects the native lane. The ICD reports Vulkan 1.3 for a physical device only
when the host reports at least Vulkan 1.3 and the worker capability response confirms that the
relay's cumulative required-feature matrix is served. That decision is per selected physical device
and per process.

This policy prevents a host version number from being mistaken for relay support. It does not make a
Khronos conformance claim; current CTS coverage is incomplete.

## Implemented Families

The current RPC and ICD implementation covers these major families:

- instance and physical-device enumeration, properties, features, memory properties, format
  properties, and image-format queries;
- logical-device lifecycle, one exposed queue family with one queue per device, queue retrieval, and
  device/queue idle operations;
- command pools, primary command buffers, batched command recording, queue submit, and queue
  submit2;
- fences, binary and timeline semaphores, events, synchronization1 barriers, synchronization2,
  timestamps, and query pools;
- host-visible mapped-memory shadows, coherent upload-at-submit, explicit flush, completed readback,
  and device-local allocations;
- buffers, buffer views, images, image views, image memory binding, samplers, and subresource
  layout;
- descriptor set layouts, pools, allocation, updates, update-after-bind and variable-count portions
  of descriptor indexing, inline uniform blocks, and layout-support queries;
- shader modules, pipeline layouts, graphics pipelines, compute pipelines, render passes,
  render-pass2, framebuffers, and imageless framebuffers;
- vertex/index/indirect buffers, uniform and sampled-image access, push constants, direct draw,
  indexed draw, core draw indirect and indexed draw indirect, dispatch, dispatch indirect,
  transfers, clears, blits, resolves, and query copy;
- dynamic rendering, synchronization2, extended dynamic state used by the native lane, maintenance4
  memory-requirement queries, private data, buffer device address, multiview, host query reset, and
  copy-commands2;
- Xcb and Xlib surface spellings, swapchains, image acquisition, presentation, surface capability
  queries, and out-of-date handling during resize.

Several extension surfaces used by Mesa Zink are forwarded or emulated, including maintenance,
renderpass2, imageless framebuffer, timeline semaphore, depth/stencil resolve, scalar block layout,
depth clip, line rasterization, vertex attribute divisor, transform feedback, conditional rendering,
and synchronization2. Enumeration is intersected with the host's extension list before device
creation.

The precise list is intentionally discovered at runtime. Use `vulkaninfo` or the application's own
feature queries inside a vkrelay2 native session rather than treating this page as an extension
registry.

## Command Recording and Errors

Primary command buffers are recorded in Linux memory. `vkCmd*` calls append normalized commands;
`vkEndCommandBuffer` sends the complete stream to the worker. This avoids a network round trip for
each recorded command.

Many Vulkan commands return `void`. If one of those commands receives a structure or value outside
the encoded subset, the ICD marks the command buffer invalid and returns a Vulkan error from
`vkEndCommandBuffer` instead of dropping the unsupported state. The worker repeats structural,
feature, ownership, range, and handle validation before calling the host driver.

Host `VK_ERROR_DEVICE_LOST` results latch the worker device as lost. Later operations return the
lost result without continuing to enter the driver, containing failure to that worker session.

## Memory Visibility

The application cannot directly map Windows device memory. For host-visible allocations, the ICD
creates a Linux shadow allocation and returns a pointer into it from `vkMapMemory`.

- `vkFlushMappedMemoryRanges` sends the selected dirty bytes to the worker.
- Coherent mappings are compared at queue submission and changed ranges are uploaded before the
  host submit.
- Completed GPU-to-host copies and query results are downloaded to the shadow before a wait or map
  observation that proves completion.
- The current allocation path rejects host-visible memory types that are not host-coherent.

The implementation tracks non-coherent atom and range rules even though the currently admitted
host-visible class is coherent.

## WSI Model

The application's X11 surface is an identity and geometry anchor. The actual swapchain and Vulkan
surface are created by the Windows worker against a native Win32 window. The sidecar correlates the
guest XID with the worker surface so creation order is safe whether the Vulkan surface or X11
top-level appears first.

Swapchain extent follows the guest client size. A host or guest resize marks the surface geometry
dirty; acquire/present returns `VK_ERROR_OUT_OF_DATE_KHR` until the application recreates the
swapchain at the converged extent. Host Vulkan calls that touch WSI objects execute on the worker's
window thread.

## Important Unsupported Shapes

The following valid Vulkan shapes are currently rejected rather than approximated:

- more than one exposed queue or unsupported queue-family ownership transfers;
- secondary command buffers and `vkCmdExecuteCommands`;
- graphics pipelines that rely on static viewport/scissor state;
- shader specialization constants;
- pipeline derivatives;
- dedicated-allocation `pNext` state;
- render-pass input, resolve, and preserve attachments outside the currently carried forms;
- indirect-count draw variants (`vkCmdDrawIndirectCount*`); the core
  `vkCmdDrawIndirect`/`vkCmdDrawIndexedIndirect` pair is implemented;
- sparse resource binding;
- external Linux file-descriptor memory, semaphore, and fence handles;
- image-class portions of descriptor indexing not included in the served buffer-oriented subset;
- YCbCr multi-plane format breadth;
- host-visible non-coherent memory types.

This is not an exhaustive Vulkan specification delta. The ICD's feature/extension enumeration and
named fail-closed diagnostics remain authoritative.

## Inspecting a Native Session

Run tools through the native frontend so they load the relay ICD and see the native lane:

```bash
./linux/launcher/vkrun --frontend vulkan13 -- vulkaninfo
```

The launcher selects one Windows adapter before the application starts, so `vulkaninfo` sees the
relay view of that adapter rather than all host adapters. Select another adapter with `--gpu`.
