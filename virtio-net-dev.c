/* virtio-net device engine: probe, feature negotiation, config, queues.

   Legacy PCI MVP policy: probe 0x1af4:0x1000, reset/ACK/
   DRIVER, accept exactly the implemented feature bits (MAC is required:
   without it the driver would have to invent an address, and the MVP
   rule is to fail explicitly instead), program rxq 0 / txq 1, read
   the negotiated config, reach DRIVER_OK, and service queue and
   config-change interrupts through the transport's IRQ core.

   Config reads follow the feature gates: mac is unconditional, status and
   mtu only under their accepted bits.  The legacy config region is
   addressed with 32-bit reads (the shared transport's read helper), so
   mac[0..3] comes from offset 0 and mac[4..5]/status from offset 4; bytes
   past the negotiated config read as zero, per QEMU's legacy bounds
   behavior.  */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "virtio-net.h"

/* Link-state bit from virtio_net_config.status (VIRTIO_NET_S_LINK_UP).  */
#define VNET_STATUS_LINK_UP 1

static error_t
vnet_dev_bring_up (struct vnet_dev *d, const char *override)
{
  struct virtio_queue *queues[2];
  uint32_t cfg0, cfg4;
  uint16_t mtu;
  error_t err;

  d->pci.name = "virtio-net";
  d->pci.device_id = VNET_DEVICE_ID;

  err = virtio_pci_probe (&d->pci, override);
  if (err)
    return err;

  virtio_pci_reset (&d->pci);
  virtio_pci_set_driver (&d->pci);

  d->offered = virtio_pci_read_host_features (&d->pci);
  if (!(d->offered & (1u << VNET_F_MAC)))
    {
      fprintf (stderr, "virtio-net: device does not offer VIRTIO_NET_F_MAC; "
               "an Ethernet device without an address is unusable\n");
      return ENOTSUP;
    }
  d->accepted = d->offered & VNET_FEATURES;
  /* Opt-in RX moderation: EVENT_IDX is a transport-wide feature, so
     accepting it switches every queue of this device to threshold-based
     notifications.  Both queues are enabled below: the RX loop
     moderates, and the TX queue's only sleeper arms it before
     waiting.  */
  if (vnet_event_idx)
    d->accepted |= d->offered & (1u << VIRTIO_RING_F_EVENT_IDX);
  virtio_pci_set_guest_features (&d->pci, d->accepted);
  fprintf (stderr, "virtio-net: offered features 0x%x, accepted 0x%x%s\n",
           d->offered, d->accepted,
           d->accepted & (1u << VIRTIO_RING_F_EVENT_IDX)
           ? " (RX EVENT_IDX moderation)" : "");
  /* Ack any interrupt the handshake itself raised.  */
  virtio_pci_read_isr (&d->pci);

  err = virtio_queue_setup (&d->pci, &d->rxq, VNET_VQ_RX);
  if (err)
    return err;
  err = virtio_queue_setup (&d->pci, &d->txq, VNET_VQ_TX);
  if (err)
    return err;

  /* Start both queues armed at the consumed position, so the next
     completion on each interrupts.  The RX thread then runs its own
     moderation cycle.  The default TX path reaps inline on every
     submission and never sleeps, so the TX queue stays quiet after
     that first completion apart from the interrupt QEMU forces each
     32768 completions, and a --sync writer arms the queue itself
     before it waits, in virtio_pool_wait_armed.  Single-threaded
     here: the RX thread that reads event_idx_enabled starts later.  */
  virtio_queue_set_event_idx (&d->rxq,
                              (d->accepted
                               & (1u << VIRTIO_RING_F_EVENT_IDX)) != 0);
  virtio_queue_set_event_idx (&d->txq,
                              (d->accepted
                               & (1u << VIRTIO_RING_F_EVENT_IDX)) != 0);

  err = virtio_pci_driver_ok (&d->pci);
  if (err)
    return err;

  /* struct virtio_net_config, little-endian, field order per QEMU's
     include/standard-headers/linux/virtio_net.h (mac, status, mtu):
     optional fields are read only under their accepted bits.  */
  cfg0 = virtio_pci_read_config (&d->pci, VNET_CFG_MAC);
  cfg4 = virtio_pci_read_config (&d->pci, VNET_CFG_DWORD4);
  d->mac[0] = cfg0;
  d->mac[1] = cfg0 >> 8;
  d->mac[2] = cfg0 >> 16;
  d->mac[3] = cfg0 >> 24;
  d->mac[4] = cfg4;
  d->mac[5] = cfg4 >> 8;
  d->have_status = (d->accepted & (1u << VNET_F_STATUS)) != 0;
  d->have_mtu = (d->accepted & (1u << VNET_F_MTU)) != 0;
  d->link_status = d->have_status ? (cfg4 >> 16) & 0xffff : 0;
  mtu = VNET_MTU_DEFAULT;
  if (d->have_mtu)
    mtu = (virtio_pci_read_config (&d->pci, VNET_CFG_DWORD8) >> 16) & 0xffff;

  /* The L2 frame cap bounds both the reported max_packet_size and the
     RX/TX buffers; an MTU advice beyond it is truncated here and the
     effective value logged, so consumers cannot push frames the driver
     will refuse.  */
  d->mtu = mtu;
  if (VNET_ETHER_HDR_SIZE + (size_t) mtu > VNET_FRAME_CAP)
    d->mtu = VNET_FRAME_CAP - VNET_ETHER_HDR_SIZE;
  d->max_frame = VNET_ETHER_HDR_SIZE + d->mtu;
  if (d->mtu != mtu)
    fprintf (stderr, "virtio-net: MTU advice %u exceeds the %u-byte frame "
             "cap, using %u\n", mtu, VNET_FRAME_CAP, d->mtu);

  err = vnet_buf_init (d);
  if (err)
    return err;

  fprintf (stderr, "virtio-net: MAC %02x:%02x:%02x:%02x:%02x:%02x, "
           "mtu %u, max frame %u%s%s\n",
           d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5],
           d->mtu, d->max_frame,
           d->have_status ? (d->link_status & VNET_STATUS_LINK_UP
                             ? ", link up" : ", link down") : "",
           d->have_mtu ? ", device MTU advice" : "");

  queues[0] = &d->rxq;
  queues[1] = &d->txq;
  return virtio_irq_install (&d->irq, &d->pci, queues, 2);
}

error_t
vnet_dev_init (struct vnet_dev *d, const char *override)
{
  error_t err = vnet_dev_bring_up (d, override);

  if (err)
    /* Never leave the device configured: a bring-up failure after
       DRIVER_OK would otherwise leave the queues armed and RX buffers
       posted against this (dying) task's DMA memory, so the device
       could keep writing into pages the kernel reclaims at exit.  The
       status-register reset is the legacy quiescence mechanism: it
       disarms the queues and stills the device before the buffers go.  */
    virtio_pci_reset (&d->pci);
  return err;
}

/* Config-change interrupts: the negotiated config fields the device may
   change are the status word (link state); MAC and MTU advice are static
   per the spec, so only the link state is re-read and reported.  */
void
vnet_dev_reread_config (struct vnet_dev *d)
{
  uint16_t status;

  d->stats.config_changes++;
  if (!d->have_status)
    return;
  status = (virtio_pci_read_config (&d->pci, VNET_CFG_DWORD4) >> 16)
           & 0xffff;
  if (status != d->link_status)
    {
      fprintf (stderr, "virtio-net: link %s\n",
               status & VNET_STATUS_LINK_UP ? "up" : "down");
      d->link_status = status;
    }
}

int
vnet_dev_link_up (struct vnet_dev *d)
{
  return d->have_status ? (d->link_status & VNET_STATUS_LINK_UP) != 0 : 1;
}

/* Build and post one TX chain: [zeroed 10-byte legacy header] [frame],
   both device-readable.  Submission is serialized under tx_lock; the
   lock is released before waiting for completion so writers can pipeline
   frames (slot count, not tx_lock, bounds the in-flight frames).  A slot
   whose wait timed out stays marked so the device can never race a
   reused buffer.  Default is netdde-style fire-and-forget: the frame is
   queued and later submissions reap its completion, so the slot count
   bounds the in-flight frames and a stalled backend becomes EAGAIN
   backpressure instead of a per-frame stall.  With --sync the writer
   waits for the backend completion and tracks the wait in tx_inflight
   so teardown does not free the pool under a sleeper.  QEMU pushes TX
   completions with used.len = 0 (hw/net/virtio-net.c), so the length is
   not validated beyond the id mapping the pool engine applies.  */

/* The submit half, under tx_lock: reap finished completions, take a
   slot, copy the frame in, and submit.  ARM_WAIT registers the
   completion waiter before submission so a later reap cannot consume
   this chain's completion in the window before the waiter starts
   waiting (the reap would discard the result and stall this writer
   until its timeout).  */
static error_t
vnet_tx_submit (struct vnet_dev *d, const void *frame, size_t len,
                uint16_t *slot_idx, int arm_wait)
{
  struct virtio_buf buf;
  char *slot;
  error_t err;

  virtio_pool_reap (&d->txpool, &d->txq);
  err = virtio_pool_alloc_slot (&d->txpool, slot_idx);
  if (err)
    {
      /* Reap any completions that landed since the last submission
         before declaring exhaustion.  */
      virtio_pool_reap (&d->txpool, &d->txq);
      err = virtio_pool_alloc_slot (&d->txpool, slot_idx);
      if (err)
        {
          /* Only reachable after prior timeouts left every slot marked.  */
          fprintf (stderr, "virtio-net: no free TX slot\n");
          return EAGAIN;
        }
    }

  slot = virtio_pool_slot (&d->txpool, *slot_idx);
  /* The no-offload header contract (flags = 0, GSO_NONE) is per-frame,
     not just an init-time state: nothing else writes these bytes, but
     zeroing them here keeps the header correct independent of the
     slot's history.  */
  vnet_hdr_zero (slot);
  memcpy (slot + VNET_HDR_SIZE, frame, len);

  buf.addr = virtio_pool_slot_phys (&d->txpool, *slot_idx);
  buf.len = VNET_HDR_SIZE + len;
  buf.flags = 0;  /* the device reads header and frame */

  if (arm_wait)
    virtio_pool_arm_wait (&d->txpool, *slot_idx);

  /* No threshold publish here.  The reap above collects what has
     landed, and a writer that waits arms the queue itself before it
     sleeps, in virtio_pool_wait_armed.  */
  err = virtio_pool_submit (&d->txpool, &d->txq, *slot_idx, &buf, 1, NULL);
  if (err && arm_wait)
    virtio_pool_disarm_wait (&d->txpool, *slot_idx);
  return err;
}

error_t
vnet_dev_tx (struct vnet_dev *d, const void *frame, size_t len)
{
  struct virtio_used_elem used;
  uint16_t slot_idx;
  error_t err;

  if (__atomic_load_n (&d->stopping, __ATOMIC_SEQ_CST))
    return EIO;

  if (!vnet_tx_sync)
    {
      pthread_mutex_lock (&d->tx_lock);
      err = vnet_tx_submit (d, frame, len, &slot_idx, 0);
      if (err)
        {
          pthread_mutex_unlock (&d->tx_lock);
          return err;
        }
      /* Counted at submit and under tx_lock.  The completion result
         belongs to whichever later submitter reaps it; the slot stays
         marked until then.  */
      d->stats.tx_packets++;
      d->stats.tx_bytes += len;
      if (vnet_verbose)
        fprintf (stderr, "virtio-net: TX %zu bytes queued (slot %u)\n",
                 len, slot_idx);
      pthread_mutex_unlock (&d->tx_lock);
      return 0;
    }

  pthread_mutex_lock (&d->tx_lock);
  err = vnet_tx_submit (d, frame, len, &slot_idx, 1);
  if (err)
    {
      /* Submission failed; slot was never marked in flight.  */
      pthread_mutex_unlock (&d->tx_lock);
      return err;
    }

  __atomic_fetch_add (&d->tx_inflight, 1, __ATOMIC_SEQ_CST);

  /* Release the submission lock before waiting for completion so other
     writers can pipeline their frames.  */
  pthread_mutex_unlock (&d->tx_lock);

  err = virtio_pool_wait_armed (&d->txpool, &d->txq, slot_idx,
                                VNET_TX_TIMEOUT, &used);
  __atomic_fetch_sub (&d->tx_inflight, 1, __ATOMIC_SEQ_CST);

  pthread_mutex_lock (&d->tx_lock);
  if (err)
    {
      if (err == EIO)
        __atomic_fetch_add (&d->stats.bad_completions, 1,
                            __ATOMIC_RELAXED);
      else
        d->stats.tx_errors++;
      pthread_mutex_unlock (&d->tx_lock);
      return err;
    }

  d->stats.tx_packets++;
  d->stats.tx_bytes += len;
  if (vnet_verbose)
    fprintf (stderr, "virtio-net: TX %zu bytes ok (slot %u)\n", len,
             slot_idx);
  pthread_mutex_unlock (&d->tx_lock);
  return 0;
}

void
vnet_dev_quiesce (struct vnet_dev *d)
{
  int i;

  /* Atomic: written here (libports thread) and in the daemon stop path
     while the RX loop reads it concurrently; a torn or cached value
     would strand the join below.  */
  __atomic_store_n (&d->stopping, 1, __ATOMIC_SEQ_CST);
  /* Let TX waiters recheck their completion state; each still ends at
     its own one-second deadline, which the in-flight wait below always
     outlasts.  */
  pthread_mutex_lock (&d->txq.lock);
  pthread_cond_broadcast (&d->txq.cv);
  pthread_mutex_unlock (&d->txq.lock);
  /* Wait for in-flight TX requests to finish (or time out) before
     freeing DMA pools; synchronous-mode waiters only, since netdde-style
     async frames have none, and their DMA is covered by the device
     reset below happening before the pools are freed.  */
  for (i = 0; i < 20 && __atomic_load_n (&d->tx_inflight, __ATOMIC_SEQ_CST) > 0; i++)
    usleep (100000);
  /* Join the RX service thread before freeing the DMA pools: a completion
     handler mid-vnet_rx_one touches rx_bufs, so freeing underneath it is
     a use-after-free.  The serve path (trivfs_goaway) never joins the
     thread itself, so quiesce must.  Idempotent: rx_created is 0 when no
     thread was ever started (selftest mode drives its own local thread).  */
  /* Exchange keeps the idempotence safe even if two goaway threads
     race: only the one that swaps 1 -> 0 joins the thread.  */
  if (__atomic_exchange_n (&d->rx_created, 0, __ATOMIC_SEQ_CST))
    pthread_join (d->rx_thread, NULL);
  /* Cleared so a repeated quiesce (defensive callers) cannot join an
     already-joined thread, which is undefined behavior.  */
  /* Reset first: it clears the ISR and deasserts INTx, so the RX loop's
     next one-second wait times out instead of hanging behind a dead
     device, and the IRQ teardown mirrors blk's reset-then-uninstall
     order.  */
  virtio_irq_uninstall (&d->irq);
  vnet_buf_release (d);
}

const struct vnet_stats *
vnet_dev_stats (struct vnet_dev *d)
{
  return &d->stats;
}
