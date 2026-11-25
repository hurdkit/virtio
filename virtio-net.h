/* virtio-net driver for GNU/Hurd.

   Device model: legacy virtio-net (PCI 0x1af4:0x1000, virtio device ID 1)
   with one receiveq (vq 0) and one transmitq (vq 1).  Every data-queue
   buffer is prefixed by the 10-byte legacy `struct virtio_net_hdr`; the
   MVP negotiates only MAC/STATUS (plus MTU when offered) and zeroes the
   header for no-offload frames.

   The transport is the shared legacy virtio PCI core (virtio-pci.c,
   virtio-queue.c); this header keeps the net-specific policy: device ID,
   feature set, config offsets, queue roles, the RX/TX buffer pools, and
   the Hurd-facing Mach device layer.

  The Hurd layer (virtio-net-hurd.c) implements the MVP
  subset of the Mach network-device contract on top of the raw datapath:
   device_open/close/write, NET_STATUS/NET_ADDRESS/NET_FLAGS, real BPF
   filter evaluation through the pinned Hurd tree's libbpf (the same
   substrate eth-multiplexer uses), and NET_RCV_MSG delivery built and
   sent from userspace exactly like eth-multiplexer's vdev.c.  The MIG
   port publication lives in virtio-net-serve.c.

  Authoritative references:
  - QEMU include/standard-headers/linux/virtio_net.h: feature bits,
     struct virtio_net_config, the legacy 10-byte header, GSO_NONE.
   - QEMU hw/net/virtio-net.c: RX used.len is the bytes the device wrote
     (header + frame); TX completions are pushed with used.len = 0.
   - gnumach include/device/net_status.h and device/net_io.c: NET_RCV_MSG
     layout, packet_header prefix, size/count construction.
   - hurd eth-multiplexer vdev.c/dev_stat.c: the userspace server precedents
     for message construction, delivery, and status flavors.  */

#ifndef VIRTIO_NET_H
#define VIRTIO_NET_H

#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <mach.h>
#include <device/device_types.h>

/* Userspace BPF filter substrate from the pinned Hurd tree (compiled from
   $(HURD_SRC)/libbpf by the Makefile): if_filter_list_t, net_set_filter,
   bpf_do_filter, remove_dead_filter.  */
#include <bpf_impl.h>

#include "virtio-pci.h"
#include "virtio-queue.h"
#include "virtio-pool.h"

/* PCI 0x1af4:0x1000 (PCI_DEVICE_ID_VIRTIO_NET).  */
#define VNET_DEVICE_ID 0x1000

/* Feature bits this driver implements.  MTU is optional: it only refines
   the RX buffer cap and the reported max_packet_size.  Everything else
   (CSUM/GSO offloads, MRG_RXBUF, CTRL_VQ, MQ, hash/RSS, ...) is refused:
   each would change the header layout or add queues the MVP does not
   implement.  */
#define VNET_F_MTU    3
#define VNET_F_MAC    5
#define VNET_F_STATUS 16

#define VNET_FEATURES \
  ((1u << VNET_F_MTU) | (1u << VNET_F_MAC) | (1u << VNET_F_STATUS))

/* struct virtio_net_config offsets, little-endian on the wire.  mac is
   unconditional; status and mtu are read only under their accepted bits
   (max_virtqueue_pairs at 8 belongs to F_MQ, which is never accepted, so
   the MTU read masks its bytes out).  The legacy config region is
   addressed with 32-bit reads, so the dword offsets below cover the
   fields: dword 4 holds mac[4..5] and status, dword 8 holds the (unused)
   queue-pair count and mtu.  */
#define VNET_CFG_MAC    0
#define VNET_CFG_DWORD4 4
#define VNET_CFG_DWORD8 8

/* Queue roles.  QEMU realizes receiveq/transmitq at indices 0/1.  */
#define VNET_VQ_RX 0
#define VNET_VQ_TX 1

/* Legacy struct virtio_net_hdr (10 bytes) and the no-offload GSO type.  */
#define VNET_HDR_SIZE 10
#define VNET_HDR_GSO_NONE 0

/* Ethernet geometry: a 14-byte header, a 60-byte minimum frame, and a
   frame cap that covers 1514-byte untagged and 1518-byte VLAN-tagged
   frames (RX_FRAME_CAP).  */
#define VNET_ETHER_HDR_SIZE  14
#define VNET_ETHER_MIN_FRAME 60
#define VNET_FRAME_CAP       1522

/* Default payload MTU when VIRTIO_NET_F_MTU is not negotiated.  */
#define VNET_MTU_DEFAULT 1500

/* Buffer pools.  RX buffers default to `hdr + frame cap` (the MTU never
   shrinks them below what the backend may deliver), clamped to
   VNET_RX_BUF_MAX; TX slots carry `hdr + frame`.  The counts are clamped
   to the device-reported queue size at init.  */
#define VNET_RX_SLOTS 1024
#define VNET_TX_SLOTS 1024
#define VNET_RX_BUF_MAX 2048

/* Sentinel for "no slot"; valid slots are below the clamped counts.  */
#define VNET_NO_SLOT 0xffff

/* Per-completion interrupt wait; the fallback for a lost interrupt, not
   the normal completion path.  Used by the RX service thread, which may
   wait indefinitely for frames.  */
#define VNET_TIMEOUT 30

/* TX completions arrive within microseconds when the backend works, so a
   stalled backend must not block a D_NOWAIT write for thirty seconds.
   One second is the lost-interrupt fallback for TX.  */
#define VNET_TX_TIMEOUT 1

/* Bounded retries for RX replenish after a slot detach (K3).  */
#define VNET_RX_REPLENISH_RETRIES 3

/* Completions consumed per polling pass in EVENT_IDX mode before the RX
   thread stops suppressing interrupts, recovers retired slots, rearms
   and sleeps.  */
#define VNET_RX_POLL_BUDGET 64

/* How long --selftest-traffic waits for a backend reply (seconds).  */
#define VNET_SELFTEST_RX_WAIT 5

/* I/O counters, printed at teardown and by --verbose completions.
   Each field belongs to one thread except bad_completions (RX thread
   and TX waiters both consume foreign completions), which is maintained
   with atomic adds; single-writer fields are plain.  */
struct vnet_stats {
  uint64_t rx_packets, rx_bytes;
  uint64_t rx_bad;          /* malformed/oversize/unknown-completion drops */
  uint64_t rx_delivered;    /* NET_RCV_MSG sends accepted by the kernel */
  uint64_t rx_delivery_fail; /* send timeout/queue-full/mismatch drops */
  uint64_t tx_packets, tx_bytes;
  uint64_t tx_errors;
  uint64_t bad_completions; /* cross-thread: updated with atomic adds */
  uint64_t config_changes;
  /* RX moderation (EVENT_IDX mode only): budget-exhausted polling
     passes, armed condvar sleeps and how many timed out, and rearm
     races (progress observed right after arming).  */
  uint64_t rx_poll_passes;
  uint64_t rx_idle_waits;
  uint64_t rx_wait_timeouts;
  uint64_t rx_rearm_races;
};

struct vnet_dev {
  struct virtio_pci_dev pci;
  struct virtio_queue rxq, txq;
  struct virtio_irq irq;

  uint32_t offered, accepted;

  uint8_t mac[6];
  int have_status;
  int have_mtu;
  uint16_t link_status;  /* virtio_net_config.status (VIRTIO_NET_S_*) */
  uint16_t mtu;          /* payload MTU the device/config implies */
  uint16_t max_frame;    /* Ethernet header + mtu, capped at VNET_FRAME_CAP */
  size_t rx_buf_size;    /* hdr + frame cap per RX buffer */

  /* RX pool: one contiguous DMA region, one writable buffer per slot.
     The RX service thread is the only poster/reaper, so slot state needs
     no lock; completions are mapped back through the head-descriptor
     index (the used entry's id) via rx_desc2slot.  */
  char *rx_bufs;
  uint64_t rx_bufs_phys;
  uint16_t rx_nslots;
  uint16_t *rx_desc2slot;

  /* Retired RX slots (replenish failed) awaiting recovery.  */
  uint16_t rx_retired[VNET_RX_SLOTS];
  uint16_t rx_nretired;

  /* TX pool: the shared slot-pool engine (one contiguous DMA region, one
     device-readable buffer per slot, head2slot completion mapping with
     the stale-discard wait discipline).  Submissions are serialized by
     tx_lock; the pool's slot count bounds the in-flight frames, and
     timed-out slots stay marked so the device never races a reused
     buffer.  */
  struct virtio_slot_pool txpool;
  pthread_mutex_t tx_lock;

  /* RX service thread.  pthread_self() is invalid until created, so track
     creation explicitly; quiesce joins it before freeing the DMA pools so
     the serve path (trivfs_goaway) never races a completion handler.  */
  pthread_t rx_thread;
  int rx_created;
  int stopping;  /* set at teardown; wakes the RX thread's timeout loop */
  int tx_inflight; /* number of TX requests currently in flight */

  struct vnet_stats stats;
};

/* The Mach device semantics layer: the MVP subset (open/close,
   device_write TX, NET_STATUS/NET_ADDRESS/NET_FLAGS, BPF filter install and
   evaluation, NET_RCV_MSG delivery), without the MIG demux/port publication
   that virtio-net-serve.c adds.  One instance per device_open handle.  */
struct vnet_hurd {
  struct vnet_dev *dev;
  int opened;
  int mode;         /* D_READ or D_READ|D_WRITE as opened */
  int if_flags;     /* client-managed NET_FLAGS storage */

  /* Registered packet filters (libbpf).  Guarded by lock; the RX thread
     takes it per frame while evaluating and delivering.  */
  if_filter_list_t filters;
  pthread_mutex_t lock;

  struct vnet_hurd *next;  /* registry of open handles, see rx_all */
};

/* virtio-net.c */
extern int vnet_verbose;
extern int vnet_tx_sync;

/* Used-notification policy, set by --event-idx: when set, the driver
   accepts the split-ring EVENT_IDX feature.  The RX loop then runs in
   budgeted-polling mode with race-free interrupt rearming, and the TX
   queue interrupts only a --sync writer, through the arm that writer
   publishes before its wait.  On by default.  */
extern int vnet_event_idx;

/* virtio-net.c */
error_t vnet_start_device (const char *override);
void vnet_stop_device (void);
error_t vnet_selftest (int traffic);
error_t vnet_status (const char *override);

/* virtio-net-serve.c: MIG device server / trivfs translator (plan Stage
   B/C).  Brings up no device itself: DEV must be initialized already.
   Never returns; exits when trivfs_goaway tears the translator down.  */
void vnet_serve (struct vnet_dev *dev, const char *name);

/* virtio-net-dev.c */
error_t vnet_dev_init (struct vnet_dev *, const char *override);
void vnet_dev_quiesce (struct vnet_dev *);
void vnet_dev_reread_config (struct vnet_dev *);
/* Post one frame for TX.  Default is netdde-style fire-and-forget: the
   frame is queued and its completion is reaped by a later submission;
   with --sync the call waits for the backend completion.  On success
   the frame is handed to the backend and the full length is accepted.  */
error_t vnet_dev_tx (struct vnet_dev *, const void *frame, size_t len);
const struct vnet_stats *vnet_dev_stats (struct vnet_dev *);
int vnet_dev_link_up (struct vnet_dev *);

/* virtio-net-buf.c */
error_t vnet_buf_init (struct vnet_dev *);
void vnet_buf_release (struct vnet_dev *);
/* Zero the 10-byte legacy virtio-net header at a TX buffer start (the
   no-offload form: flags = 0, GSO_NONE).  */
void vnet_hdr_zero (char *);
/* RX service loop: consume rxq completions, validate, deliver, replenish.
   Returns on stopping; otherwise runs forever.  */
void vnet_buf_rx_loop (struct vnet_dev *);

/* virtio-net-hurd.c */
error_t vnet_hurd_init (struct vnet_hurd *, struct vnet_dev *);
void vnet_hurd_register (struct vnet_hurd *);
void vnet_hurd_unregister (struct vnet_hurd *);
error_t vnet_hurd_open (struct vnet_hurd *, int mode);
error_t vnet_hurd_close (struct vnet_hurd *);
error_t vnet_hurd_write (struct vnet_hurd *, int mode, recnum_t recnum,
                         const void *data, int bytes, int *bytes_written);
error_t vnet_hurd_get_status (struct vnet_hurd *, dev_flavor_t flavor,
                              int *status, mach_msg_type_number_t *count);
error_t vnet_hurd_set_status (struct vnet_hurd *, dev_flavor_t flavor,
                              const int *status, mach_msg_type_number_t count);
error_t vnet_hurd_set_filter (struct vnet_hurd *, mach_port_t receive_port,
                              int priority, const filter_t *filter,
                              mach_msg_type_number_t filterlen);
/* Deliver one received Ethernet frame to every registered open handle
   whose filters accept it (NET_RCV_MSG send per eth-multiplexer's
   deliver_msg).  */
void vnet_hurd_rx_all (struct vnet_dev *, const char *frame,
                       size_t frame_len);
/* A receive port registered by a filter died; drop its filter from
   every open handle (the dead-name hook; the notify port does not
   identify which handle registered it).  */
void vnet_hurd_all_dead_name (mach_port_t dead_port);
/* Remove the filter entries of one port from one handle's lists (and
   drop the stored send right); also used by the selftest to uninstall
   its filter without going through a kernel dead-name notification.  */
void vnet_hurd_dead_name (struct vnet_hurd *h, mach_port_t dead_port);
/* Inband and map are not implemented (IO_INBAND_MAX is far too small for
   frames and no MVP consumer maps the device); they return the compatible
   error rather than crashing.  */
error_t vnet_hurd_read_inband (struct vnet_hurd *, int mode, recnum_t recnum,
                               int bytes_wanted, io_buf_ptr_inband_t data);
error_t vnet_hurd_write_inband (struct vnet_hurd *, int mode,
                                recnum_t recnum,
                                const io_buf_ptr_inband_t data,
                                int bytes, int *bytes_written);
error_t vnet_hurd_map (struct vnet_hurd *, vm_prot_t prot, vm_offset_t offset,
                       vm_size_t size, memory_object_t *pager, int unmap);

#endif /* VIRTIO_NET_H */
