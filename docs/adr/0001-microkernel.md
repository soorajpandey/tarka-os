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
- IPC performance is critical and must be designed for from the start
  (register-based fast path, no copies for small messages).
- The kernel's code is much smaller, which makes review, fuzzing and,
  eventually, formal reasoning feasible.

## References

seL4, QNX Neutrino, MINIX 3, Fuchsia Zircon.
