# ADR 0001: Microkernel architecture

Status: accepted

## Context

In a monolithic kernel every driver, filesystem and network protocol runs
with full kernel privilege. A single memory-safety bug in any of them, many
of which parse untrusted input from devices, disks or the network, gives an
attacker the whole machine. Isolating drivers later is famously hard
because the driver model, locking and memory sharing all assume a single
address space.

## Decision

Tarka is a microkernel. The kernel provides only:

- address spaces and threads
- synchronous IPC with capability transfer
- capability management
- interrupt delivery to user space (as `CAP_IRQ` notifications)
- the minimum of physical memory management needed to back the above

Everything else runs as an isolated user-space server: device drivers,
filesystems, the network stack, executable loading and the update agent.

## Consequences

- A compromised driver can only reach the capabilities it was given: its own
  device's registers, I/O ports, IRQ and DMA buffers.
- **Driver isolation requires the IOMMU.** Without it, a driver can program
  its device to DMA over arbitrary physical memory. The device manager must
  set up VT-d / AMD-Vi domains before handing a device to a driver. Machines
  without an IOMMU get a reduced guarantee, and the system says so.
- **IOMMU enabled is not enough by itself; DMA needs an ownership model.**
  See "DMA ownership" below.
- IPC performance is critical and must be designed for from the start
  (register-based fast path, no copies for small messages).
- The kernel's code is much smaller, which makes review, fuzzing and,
  eventually, formal reasoning feasible.

## DMA ownership

DMA access is granted through capabilities like everything else:

```text
DeviceFrame / IrqHandler / IoPorts caps   ──► driver task
DmaDomain cap (one IOMMU domain per device) ──► the same driver
driver maps Frame caps it holds into its DmaDomain ──► device can reach only those frames
```

- The kernel creates one `DmaDomain` per device at boot, with an empty
  IOMMU page table. A device reaches no memory until its driver maps
  frames into the domain.
- Mapping a frame into a DmaDomain requires a Frame capability with the
  matching rights. A driver can only expose memory it already holds.
- Revoking or deleting the frame capability removes the IOMMU mapping and
  flushes the IOTLB **before** the frame can be reused. This is invariant
  S1 applied to devices.
- Device MMIO registers are `DeviceFrame` capabilities: never zeroed,
  never retyped, never handed to more than one driver.
- Invariant **D1**: a device cannot DMA outside its assigned domain. It is
  tested in QEMU with an emulated IOMMU (`-device intel-iommu`) by having QEMU's
  `edu` test device DMA to an unmapped address and checking that the access is
  blocked.

## References

seL4, QNX Neutrino, MINIX 3, Fuchsia Zircon.
