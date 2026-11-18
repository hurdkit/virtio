# virtio

Userspace virtio drivers for GNU Hurd. AI patches are ENCOURAGED.

Two separate programs share the generic legacy virtio PCI transport
(`virtio-pci.c`, `virtio-queue.c`) and run as independent processes, each
probing and owning its own PCI device:

- `virtio-9p` exposes QEMU's virtio-9p PCI device (`0x1af4:0x1009`) as a
  character device carrying the raw 9P message stream, so the existing,
  unmodified 9pfs translator can mount a host directory in the guest.
- `virtio-balloon` makes the guest participate in virtio memory ballooning
  through QEMU's virtio-balloon PCI device (`0x1af4:0x1002`), so the host
  can dynamically reclaim or return guest RAM at runtime.
- `virtio-blk` performs raw block I/O on QEMU's virtio-blk PCI device
  (`0x1af4:0x1001`), exposing record-based block access (recnum /
  record_size units, `DEV_GET_SIZE` / `DEV_GET_RECORDS`, the `D_*` error
  codes) that libstore/storeio consumers expect.
- `virtio-net` sends and receives Ethernet frames on QEMU's virtio-net PCI
  device (`0x1af4:0x1000`) and exposes the Mach `device_open` interface
  (write/get_status/set_status/set_filter), with the intent that pfinet or
  lwip can attach to it like an eth-multiplexer port.

## How it works

All drivers use the same transport: PCI discovery through libpciaccess (a
pci-arbiter client, so the guest must run the pci-arbiter translator and
the driver must not touch the PCI config ports itself), the legacy virtio
register interface in an I/O BAR, the reset/ACK/DRIVER/feature/DRIVER_OK
handshake, split virtqueues in physically contiguous memory
(`vm_allocate_contiguous`, which returns the machine physical address the
device DMAes to), and INTx interrupts driven by libirqhelp with the
read-to-clear ISR discipline.

```
QEMU host
  |  virtio-9p PCI device                virtio-balloon PCI device
  v                                        v
/dev/virtio-9p0  this translator      /hurd/virtio-balloon  standalone daemon
  |  9P message stream                   |  config target/actual + PFN queues
  v                                      v
/hurd/9pfs         unmodified           Mach VM  driver-owned chunks whose
  |  Hurd filesystem RPCs               PFNs are posted on inflateq/deflateq
  v                                      v
/host              mount point          host reclaims/returns guest RAM
```

The balloon driver is a plain daemon, not a translator: ballooning is driven
entirely through the device config region and the two PFN virtqueues, so
there is no device node and the 9p translator is not on its path.

## Requirements

The guest must run the pci-arbiter translator, the devices must present the
legacy virtio PCI interface (the QEMU default on i440fx), and the balloon
daemon needs the privileges behind `get_privileged_ports` (it runs
`vm_allocate_contiguous` for its page pool), i.e. it runs as root.

## Building

Build with the x86_64-gnu cross toolchain. The Hurd toolchain harness at
https://github.com/hurdkit/hurdkit/ has a `virtio-9p` target that sets
CC/CPATH/LIBS; for a manual build, set them the same way. Two more
variables locate the toolchain and the Hurd sources:

- `HURD_PREFIX` is the cross toolchain's install prefix (default
  `~/opt/x86_64-gnu`). The MIG stubs are generated from its `device.defs`
  and `notify.defs`.
- `HURD_SRC` is the pinned Hurd source tree, for the libports MIG headers
  and libbpf (default `../hurdkit/3p/hurd`, for a clone next to hurdkit,
  whose `make fetch-3p` checks it out).

```
make                     # builds virtio-9p, virtio-balloon, virtio-blk,
                         # virtio-net and the vblk-client harness client
make virtio-balloon      # balloon driver only
make virtio-blk          # block driver only
make virtio-net          # network driver only
make vblk-client         # direct Mach device RPC client only
make CFLAGS=-g           # debugging build
```

## virtio-9p usage

On the host, start QEMU with a share:

```
-fsdev local,id=virtfs0,path=/share,security_model=none
-device virtio-9p-pci,fsdev=virtfs0,mount_tag=hostshare
```

`security_model=none` gives the guest read-write access to the host
directory; point it at a throwaway directory.

In the guest:

```
# settrans -c /dev/virtio-9p0 /hurd/virtio-9p --mount-tag=hostshare
# settrans -c /host /hurd/9pfs /dev/virtio-9p0
# ls /host
```

Options:

- `--device=SLOT`: use the virtio-9p device at this PCI slot instead of the
  first match
- `--mount-tag=NAME`: require the device's mount tag to be NAME (default:
  hostshare)
- `--selftest`: bring the device up, do one 9P Tversion round trip, and exit

The node is single-open (a second open gets EBUSY), matching 9pfs, which
opens it once and duplicates the fd into input and output streams. seek and
map are refused (ESPIPE and EOPNOTSUPP).

## virtio-balloon usage

On the host, start QEMU with the device:

```
-device virtio-balloon-pci
```

In the guest, start the daemon and keep it running:

```
# /hurd/virtio-balloon
```

Then drive it from the host monitor:

```
(qemu) info balloon      # guest actual, as the host sees it
(qemu) balloon 512       # lower the target: guest inflates
(qemu) balloon 1024      # raise it: guest deflates
```

Options:

- `--device=SLOT`: use the virtio-balloon device at this PCI slot instead
  of the first match
- `--selftest`: bring the device up, verify the handshake, features, both
  queues and the config region, run one inflate/deflate round trip with a
  single page, and print PASS/FAIL diagnostics
- `--status`: print the device's current target/actual and exit; reads the
  config region only, so it never resets or disturbs a running driver
- `--verbose`: log every inflate/deflate completion

Runtime logs (stderr) show the initial target, every batch's page counts and
the reported `actual`, and bring-up/teardown diagnostics.

## Balloon protocol notes

MVP scope is the classic inflate/deflate pair only: legacy PCI transport,
no stats/free-page-hint/reporting/poison features, no deflate-on-OOM, no
MSI-X. The only accepted feature is `VIRTIO_BALLOON_F_MUST_TELL_HOST`,
accepted when offered; the deflate path enforces its ordering regardless.

Both queues carry driver-written, device-readable arrays of little-endian
32-bit PFNs (4 KiB units) as single output descriptors. The accounting rule
is fixed: a page counts towards `actual` only after the used ring has
acknowledged the batch carrying it, and pages selected for deflation stay
mapped until that acknowledgement, so the host is always told before a page
is reusable. The page pool is driver-owned `vm_allocate_contiguous` chunks
of 16 pages; chunk granularity and allocation failures bound the reachable
balloon size. The convergence loop wakes on config-change interrupts with a
1 s tick, and shutdown deflates best-effort before resetting the device.

## virtio-blk usage

On the host, start QEMU with a disk backend and the device (a disposable
image for anything that writes):

```
-drive if=none,id=vd0,file=disk.img,format=raw,cache=none
-device virtio-blk-pci,drive=vd0
```

In the guest, run the selftest to prove the datapath end to end:

```
# /hurd/virtio-blk --selftest
```

Options:

- `--device=SLOT`: use the virtio-blk device at this PCI slot instead of
  the first match
- `--selftest`: bring the device up, verify the handshake, features, queue
  and config, read sector 0, prove the read-past-capacity bound, and
  exercise block access (size/records consistency, open modes, unit and
  range errors, flush flavor, inband/map refusals)
- `--write-test`: with `--selftest`, also write a pattern to sector 0,
  flush, and read it back; destroys the first sector of the backing image,
  so use a disposable image
- `--status`: print the device's capacity, offered features and limits and
  exit; reads the config region only, so it never resets or disturbs a
  running driver
- `--read-range=SECTOR[:COUNT]`: read COUNT sectors (default 1) at SECTOR
  through the normal request path and write the raw bytes to standard
  output (diagnostics go to stderr); a debugging aid for harness data
  checks, capped at 64 MiB per invocation
- `--verbose`: log every request completion

Without options the driver is a long-lived daemon: it owns the device,
logs config-change interrupts and capacity changes, and prints I/O
counters on exit. Runtime logs (stderr) show offered/accepted features,
capacity, limits, and every error.

## virtio-blk protocol notes

MVP scope is legacy PCI transport, single request queue (virtqueue 0), and
the `VIRTIO_BLK_T_IN`/`T_OUT`/`T_FLUSH` request types only; no MQ, SCSI
passthrough, topology, discard/write-zeroes/zoned, or modern transport.
Accepted features are exactly the implemented ones, each with matching
behavior: `VIRTIO_BLK_F_RO` rejects writes, `VIRTIO_BLK_F_FLUSH` serves
`DEV_FLUSH_CACHE`, `VIRTIO_BLK_F_SIZE_MAX`/`_SEG_MAX` bound requests, and
`VIRTIO_BLK_F_BLK_SIZE` sets the transfer unit; anything else offered is
rejected.

Each request is one descriptor chain over a fixed-layout DMA slot:
`virtio_blk_outhdr` (type, ioprio, sector in 512-byte units) as a
device-readable buffer, the data buffer in the transfer direction, and the
1-byte device-written status last. Completions are validated defensively
(descriptor head, used length, status byte) and mapped to errno (`S_OK` to
success, `S_IOERR` to `EIO`, `S_UNSUPP` to `EOPNOTSUPP`). On top of this
layer, block access reports `record_size` 512 (or the negotiated
blk_size), keeps `DEV_GET_SIZE`/`DEV_GET_RECORDS` consistent by
construction, maps `recnum` records to virtio sectors, and answers
unexpected flavors/modes with `D_INVALID_OPERATION` rather than crashing.
I/O is serialized per the MVP; the request table is keyed by descriptor
head, so in-flight concurrency can be added without changing the
completion path.

Field layouts, feature bits, and request/status values follow QEMU's
`include/standard-headers/linux/virtio_blk.h` (the guest-visible contract,
cited with anchors in `stuff/virtio_disk_plan.md` §4); the Mach RPC shapes
and `D_*` codes follow gnumach's `include/device/device.defs` and
`include/device/device_types.h`, with `hurd/libstore/device.c` as the
consumer baseline; the virtio specification's legacy PCI transport and
Block Device sections arbitrate any disagreement.

## virtio-net usage

On the host, start QEMU with a network backend and the device (user-mode
networking answers ARP and DHCP without a tap):

```
-netdev user,id=n0 -device virtio-net-pci,netdev=n0
```

In the guest, run the selftest to prove the datapath end to end:

```
# /hurd/virtio-net --selftest
```

Options:

- `--device=SLOT`: use the virtio-net device at this PCI slot instead of
  the first match
- `--selftest`: bring the device up, verify the handshake, feature policy,
  both queues and the config region, prove the status flavors (NET_STATUS,
  NET_ADDRESS round-trips the MAC, NET_FLAGS set/get), the write/read
  refusal policy, and the `NET_RCV_MSG` delivery path in-process: a real
  mach_msg receive of a driver-built message through a libbpf filter (the
  pfinet 7-instruction ethertype program), rejecting an unlisted ethertype
  by timeout; then transmits one raw ARP frame
- `--selftest-traffic`: with `--selftest`, also TX an ARP request on the
  wire and wait up to 5 s for any RX completion; a timeout is reported, not
  failed, since whether the backend answers is a network property
- `--status`: print the MAC, MTU, link status, offered features and queue
  counts and exit; reads the config region only, so it never resets or
  disturbs a running driver
- `--serve`: run as a Mach device translator instead of a daemon (see
  below)
- `--name=NAME`: with `--serve`, only open requests for this device name
  are accepted (default: any)
- `--verbose`: log every TX completion and RX frame delivery

Without `--serve` the driver is a long-lived daemon: it owns the device,
services the RX queue from its receive thread, follows link-status config
changes, and prints I/O counters on exit. Runtime logs (stderr) show
offered/accepted features, MAC, MTU and link state at bring-up.

With `--serve` the process becomes a trivfs translator speaking the Mach
`device.defs` protocol, so a network stack can attach to it with the same
`file_name_lookup` + `device_open(..., "eth")` sequence an eth-multiplexer
port would get:

```
# settrans -c /dev/vnet0 /hurd/virtio-net --serve
```

A client then does `device_open (Devname, D_READ|D_WRITE, "eth")`, writes
whole Ethernet frames with `device_write`, installs its BPF program with
`device_set_filter`, and receives matching frames as `NET_RCV_MSG` Mach
messages (timeout-0 send: a full socket queue drops the frame rather than
blocking the driver). `device_read`/map/inband-read are refused
(`D_INVALID_OPERATION`), which is the eth-multiplexer shape too.

Attaching pfinet to virtio-net alongside an existing management interface
uses a single pfinet instance with multiple `-i` options, so both
interfaces stay in one TCP/IP stack:

```
# settrans -fg /servers/socket/2 /hurd/pfinet \
    -i /dev/eth0  -a 10.0.2.15 -m 255.255.255.0 -g 10.0.2.2 \
    -i /dev/vnet0 -a 10.0.2.20 -m 255.255.255.0
```

The translator restart drops the in-flight SSH session; reconnect afterward
and test with `ping -S 10.0.2.20 10.0.2.2`.

## virtio-net protocol notes

MVP scope is legacy PCI transport, one RX and one TX virtqueue (queue
pairs 0), 10-byte `virtio_net_hdr` (no mrg-rxbuf, no checksum offloads:
`VNET_HDR_GSO_NONE`, no `VIRTIO_NET_HDR_F_*` flags), and the features
`VIRTIO_NET_F_MAC` (required: the driver refuses the device without it,
the address comes from the config space) plus `VIRTIO_NET_F_STATUS` and
`VIRTIO_NET_F_MTU` when offered. MTU is read from the high half of the
dword at config offset 8 (max_virtqueue_pairs + mtu share the dword) and
bounds `device_write` sizes; the frame cap is 1522 bytes (VLAN), matching
QEMU's 2-queue default sizes.

Status flavors follow eth-multiplexer/pfinet expectations: NET_STATUS is
7 ints with `HDR_ETHERNET`/14/6 and `min_packet_size` 60 (the minimum
Ethernet frame without FCS, not counting the 4-byte record-count prefix
pfinet subtracts), NET_ADDRESS is the MAC packed into ints in network
byte order, NET_FLAGS is a stored int; `device_set_filter` validates and
installs the caller's BPF program through Hurd's libbpf (`net_set_filter`),
and RX delivery runs each frame past every open's filter
(`bpf_do_filter`), dead-name notifications removing filters when a client
port dies. TX is serialized (one frame in flight), completions are
validated by descriptor-head-to-slot map, and RX bad completions are
counted and dropped with the buffer always reposted, so the queue never
starves. Frame delivery builds the `net_rcv_msg` exactly as
eth-multiplexer's broadcast path does (packed size, rounded msgh_size,
packet length including the 4-byte prefix, `COPY_SEND` right, deallocate
on refused delivery).

The Mach RPC shapes and `D_*` codes follow gnumach's
`include/device/device.defs`, with `hurd/pfinet/ethernet.c` and
`hurd/eth-multiplexer` as the consumer baselines (cited with anchors in
`stuff/virtio_network_plan.md`); the virtio specification's legacy PCI
transport and Network Device sections arbitrate any disagreement. The
QEMU runtime bring-up (S2–S6 in the plan: interrupt delivery, link-status
change, external traffic, pfinet attach) is still pending — everything
above is compile- and in-process-verified only.

## Why not NetBSD's vio9p

NetBSD ships a complete kernel virtio-9p driver, but it is kernel code, and
the Hurd's microkernel has no virtio support at all. The rump route would
need a vio9p rump module that NetBSD never built, a character-device bridge
from rump into the Hurd, an unproven virtio-over-rump interrupt path, and a
whole NetBSD kernel personality per translator instance, all to feed the same
9pfs front end the Hurd already has. A native userspace driver costs less than
that bridge, so vio9p.c is kept as a design reference, not a dependency.

## Contributing

Post your AI vibeslop to https://github.com/hurdkit/virtio-9p/.

## License

virtio is distributed under the GNU General Public License, version 3
(see LICENSE).
