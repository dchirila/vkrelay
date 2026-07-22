# Roadmap

This roadmap tracks optional work after the initial release. It is not a compatibility promise or
release schedule. Priorities are evidence-driven: reproduce a real application or conformance gap,
reduce it to a focused probe, and preserve vkrelay2's faithful-or-fail-closed behavior.

## Driver investigations

- **AMD event-status divergence.** On the Radeon 610M, `vkCmdSetEvent2` followed by
  `vkGetEventStatus` reports RESET, while NVIDIA reports SET. The related synchronization2 compute
  barrier and readback path passes on AMD, so this needs an isolated native probe and specification
  review before any relay change.
- **AMD vertex-tail follow-up.** Extend `vkrelay2-native-vertex-tail-probe` with the remaining
  center-point failure shape: one binding, stride 12, `R32G32B32_SFLOAT` at offset 0, a dedicated
  12-byte buffer, `POINT_LIST`, one vertex, and VS+GS+FS expansion. Run it natively and through the
  relay on both adapters. A native failure belongs in the AMD report; a relay-only failure points
  back to replay or driver interaction. Use live/readback evidence—`PrintWindow` can trigger a clean
  repaint and conceal the corruption. The existing synthetic vec3-tail case passes unguarded on
  driver 2.0.353; the older 2.0.302 diagonal-mirror attribution remains unresolved.
- **Output-owning-adapter preference.** Revisit whether automatic GPU selection should prefer the
  adapter driving the selected display. The earlier AMD corruption no longer forces the decision,
  so policy should follow cross-adapter performance and reliability measurements.

## Display and windowing

### Per-monitor guest outputs

The initial release uses one physical-pixel guest canvas. A prototype has already demonstrated that
true multi-head Weston → Xwayland/RandR output is feasible: three staggered/rotated outputs exposed
the expected geometry and physical-DPI metadata, toolkit enumeration and centering worked, and
cross-output input remained byte-exact.

The likely implementation is a small, upstreamable Weston headless-output enhancement plus
sidecar policy for selected-output fullscreen and guest work areas. Important constraints:

- create the host-primary head first; some toolkits infer primary status from output order;
- never use generated `XWAYLANDn` names as stable identities;
- report natural-axis physical dimensions for rotated heads to avoid double-transforming DPI;
- keep virtual-desktop holes unreachable to placement and recovery policy; and
- treat per-output logical scaling as an X11 limitation: the gain is correct geometry and DPI
  metadata, not automatic per-monitor UI scaling.

### Remaining window-system work

| Item | Next step | Proof |
|---|---|---|
| Worker diagnostic environment | Carry selected debug variables through supervisor session overrides instead of requiring a manually restarted daemon | One real snap/coalescing trace plus lifecycle coverage |
| Private Xwayland retirement | Submit the seatless pointer-warp and damage guards upstream; replace the known-unsafe distro table with a capability/version decision when possible | Stock server passes the rotate/move smokes |
| Reproducible Xwayland baselines | Extend the offline hash-pinned baseline beyond Jammy while retaining signed distro-source acquisition and provenance checks | Rebuilt package identity + app smoke on each distro |
| Popup first-paint flash | Tune the first-paint holdoff that can expose a short black frame | Screenshot burst during menu creation |
| Non-Qt DPI defaults | Set an appropriate `xrdb`/`Xft.dpi` policy for GTK and other X11 clients | Cross-DPI toolkit comparison |
| Dynamic monitor changes | Add a transactional display/RandR update, or a clearer restart-required workflow | Hotplug, rotation, resolution, and work-area tests |
| Mock host extents | Model the host envelope explicitly so malformed imageless-framebuffer containment is testable | Mock/real admission parity tests |

## Vulkan coverage priorities

The two main oracles are Khronos `deqp-vk` through
`vkrelay2/scripts/dev/run_vk_cts.sh` and real GL-via-Zink applications through `vkrun`. Native-lane
enumeration and pure allocation coverage are clean; the following are the largest known rendering
gaps.

| Gap | Current behavior | Remaining work | Effort |
|---|---|---|---|
| Dedicated allocations (`VkMemoryDedicatedAllocateInfo`) | Allocation `pNext` rejected except for flags | Forward the dedicated buffer/image handle to the worker | Low–Med |
| Static viewport/scissor | Dynamic state required | Carry static viewport and scissor pipeline state; this covers three of four remaining `api.smoke` failures | Med |
| Resolve/input/preserve attachments | Rejected | Implement MSAA resolve references first, then input/preserve breadth | Med |
| YCbCr multi-plane properties | Eight two-plane format-property failures | Report multi-plane properties faithfully | Low–Med |
| Host-visible non-coherent memory | Allocation rejected | Add worker flush-after-write, `vkInvalidateMappedMemoryRanges`, and atom-safe whole-allocation handling, but only when a target adapter exposes this class | Med |

Focused completeness work:

- implement the indexed transform-feedback query pair needed for honest
  `GL_ARB_transform_feedback_overflow_query` support;
- make readback downloads range-precise instead of copying full allocations;
- admit equal explicit queue-family indices in otherwise non-transfer buffer barriers; and
- provide a repeatable validation-layer run mode with known Mesa SPIR-V noise classified.

### Vulkan 1.3 and extension breadth

Each extension family must be admitted at the ICD and both backends together, resolve its KHR/EXT
entry points fail-closed, require the corresponding feature bit, and land with a focused canary.

- **Descriptor indexing:** image/texel update-after-bind classes, shader non-uniform indexing,
  dynamic indexing, and finally the aggregate `descriptorIndexing` bit once its full minimum set is
  served.
- **Dynamic rendering:** MSAA resolve, suspend/resume, secondary-command-buffer contents, supported
  attachment `pNext` chains, and a validation-layer pass.
- **Synchronization2:** queue-family ownership transfer, supported barrier/dependency/submit
  `pNext` chains, and a validation-layer pass. Synchronization2 is also used by the default Zink
  lane, so these improvements benefit both paths.

### Modern native-engine gaps

This is a lower-bound survey, not a sufficiency claim. When a representative engine is available,
one fail-closed run should replace speculation with its exact named-reject list.

| Gap | Current behavior | Effort |
|---|---|---|
| Secondary command buffers | Inline-only recording model | High |
| Linux external-FD memory/semaphore/fence | No external handle types advertised | Policy decision: continue honest rejection or define a documented shim tier |

## Reliability and maintainability

| Item | Work | Effort | Proof |
|---|---|---|---|
| Device feature-chain hardening | Exact-size validation for every known feature-structure type while retaining pass-through for unknown types | Low | Real-backend negative tests |
| Mock `create_image` parity | Broaden mock formats, usages, and tiling to match real admission | Low–Med | Unit and mock/real parity tests |
| Imageless-framebuffer cache eviction | Add a reverse index so attachment-view destruction reclaims cached regular framebuffers | Low–Med | Unit tests + resize soak |
| Queue-filtered idle promotion | Use the queue already carried by records when multi-queue support begins | Low | Unit + canary |
| Worker lifecycle flakes | Eliminate intermittent heartbeat/kill/teardown fast failures seen only under residual load | Low–Med | Repeated full-gate loop |

## Performance

The target is the same order of magnitude as native rendering. Measure first and keep correctness
gates green; deferred-error pipelining is out of scope because it changes observable semantics.

| Bottleneck | Current measurement | Candidate improvement | Effort |
|---|---|---|---|
| Descriptor-update storm | About 36 RPCs/frame × 172 µs ≈ 6.2 ms/frame in ExtremeTuxRacer | Support push descriptors or update templates so Zink can batch writes into the recorded command stream | Med |
| Buffer-destroy descriptor scan | Every descriptor set scanned, about 1.3 destroys/frame | Maintain a resource → descriptor-set reverse index | Low–Med |
| Upload sweep eligibility | About 4.1 sweeps/frame × 407 µs client CPU | Reduce the set of allocations eligible for each submit; soft-dirty filtering is already active | Med |

## Prioritization rule

Prefer work that unlocks a real application or a broad conformance family. Keep each change narrow,
add a reduced reproducer, verify both adapters where driver behavior is involved, and never advertise
functionality before the relay can execute it faithfully.
