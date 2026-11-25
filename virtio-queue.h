#ifndef VIRTIO_QUEUE_H
#define VIRTIO_QUEUE_H

/* Split virtqueues over the generic legacy PCI transport.

   Each queue's vring lives in one physically contiguous, page-granular
   region (the legacy QUEUE_PFN register addresses the whole ring by page
   number, so the allocation must be page aligned), and completions are
   signalled per queue: the IRQ handler broadcasts the condvar of every
   queue that has unconsumed completions, plus a separate config-change
   condvar, and waiters re-check the used index instead of trusting the
   wakeup, the way virtio-9p always has.

   Descriptors come from a free list, so any number of chains may be in
   flight.  virtio-9p keeps many requests in flight and consumes their
   completions as they arrive; a driver that posts one buffer at a time
   consumes them in order.  */

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <hurd/irqhelp.h>

#include "virtio-pci.h"

/* Sanity bound on queue size; QEMU's virtio queues are far below this.  */
#define VIRTIO_QUEUE_MAX 1024
#define VIRTIO_QUEUE_ALIGN 4096
/* Sentinel for "no descriptor"; valid indices are below VIRTIO_QUEUE_MAX.  */
#define VIRTIO_QUEUE_NO_DESC 0xffff

/* Split-ring notification-threshold feature, bit 29 of the 32-bit legacy
   feature word.  When negotiated, the driver publishes its
   used-notification threshold in the avail ring's used_event tail field
   (the device interrupts when the used index passes it), and the device
   publishes its kick threshold in the used ring's avail_event tail
   field.  */
#define VIRTIO_RING_F_EVENT_IDX 29

/* One buffer posted to a virtqueue: one descriptor.  */
struct virtio_buf {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
};

struct virtio_desc {
  uint64_t addr;
  uint32_t len;
  uint16_t flags;
  uint16_t next;
};

struct virtio_used_elem {
  uint32_t id;
  uint32_t len;
};

struct virtio_queue {
  struct virtio_pci_dev *pci;
  uint16_t index;       /* QUEUE_SEL index on the device */
  uint16_t num;
  struct virtio_desc *desc;
  volatile uint16_t *avail_idx;
  uint16_t *avail_ring;
  volatile uint16_t *used_idx;
  struct virtio_used_elem *used_ring;
  uint16_t last_used;
  uint16_t free_head;
  /* Split EVENT_IDX event fields.  The vring layout reserves the tail
     fields whether or not the feature is negotiated; the pointers are
     valid from queue setup on, but the driver must only publish into
     used_event once the feature is accepted, because the device
     interprets the field the moment the feature bit is set.  avail_event
     is the device's kick threshold, read-only here (kick suppression is
     deliberately not implemented).  */
  volatile uint16_t *avail_flags;
  volatile uint16_t *used_event;
  volatile const uint16_t *avail_event;
  /* Per-queue notification-policy enable; written only at init, before
     the queue's completion-consumer thread starts.  */
  int event_idx_enabled;
  pthread_mutex_t lock;
  pthread_cond_t cv;
  uint64_t region_phys;
};

#define VIRTIO_IRQ_MAX_QUEUES 8

struct virtio_irq {
  struct virtio_pci_dev *pci;
  struct irq *irq;
  struct virtio_queue *queues[VIRTIO_IRQ_MAX_QUEUES];
  size_t nqueues;
  pthread_mutex_t lock;
  pthread_cond_t cv;
  /* Bumped on every config-change interrupt; a waiter compares it against
     the value it last saw.  */
  uint32_t config_seq;
};

/* Contiguous, page-granular DMA memory; the physical address returned is
   the machine address the device DMAes to.  */
error_t virtio_dma_alloc (struct virtio_pci_dev *, size_t size, void **vaddr,
                          uint64_t *paddr);

/* Program one queue (QUEUE_SEL/NUM/PFN) and map its vring.  */
error_t virtio_queue_setup (struct virtio_pci_dev *, struct virtio_queue *,
                            uint16_t index);

/* Post a chain of buffers as one buffer and notify the device.  The first
   descriptor is the head; the core links the chain with F_NEXT.  */
error_t virtio_queue_post (struct virtio_queue *, const struct virtio_buf *,
                           uint16_t nbufs);
/* Like virtio_queue_post, but also returns the posted chain's head
   descriptor index through *HEAD_OUT (may be NULL).  */
error_t virtio_queue_post_head (struct virtio_queue *, const struct virtio_buf *,
                                uint16_t nbufs, uint16_t *head_out);

/* Like virtio_queue_post_head, but returns without kicking the device:
   the caller batches a pass worth of posts and publishes them with one
   virtio_queue_notify, saving one VM exit per buffer.  */
error_t virtio_queue_post_head_deferred (struct virtio_queue *,
                                         const struct virtio_buf *,
                                         uint16_t nbufs, uint16_t *head_out);
/* Kick the device after deferred posts.  Safe (if pointless) with
   nothing new posted.  */
void virtio_queue_notify (struct virtio_queue *);

/* Wait for one used entry and return it; the completed descriptor chain is
   recycled.  TIMEOUT_SEC bounds the interrupt wait (the fallback for a lost
   interrupt, not the normal completion path).  */
error_t virtio_queue_wait_used (struct virtio_queue *, int timeout_sec,
                                struct virtio_used_elem *);
/* Nonblocking, K1-safe pop of one completed used entry.  Returns 0 with
   *OUT filled when a completion is available, EAGAIN when none is, and
   EIO for an invalid used id (after advancing last_used, without
   recycling descriptors).  */
error_t virtio_queue_try_pop_used (struct virtio_queue *,
                                   struct virtio_used_elem *);


/* Used-notification moderation over split EVENT_IDX: the device interrupts when the used
   index passes the published threshold, so arming at the consumed
   position yields exactly one interrupt for the next completion, and
   sleeping on the queue condvar is safe only after that publish, a
   barrier, and a recheck of the used index.  The arm/suppress/wait trio
   and the poll belong to the queue's completion-consumer thread, and a
   thread that sleeps arms after its own drain. set_event_idx runs once
   at init, before any consumer or submitter exists.

   With event_idx_enabled clear they are no-ops or legacy-equivalent:
   without the negotiated feature the device interrupts per completion
   (avail flags stay zero), which is the behavior every driver had
   before this API existed.  */

/* Enable moderation on this queue. Only valid once EVENT_IDX was
   negotiated device-wide and before the consumer thread starts.
   Publishes the consumed position as the threshold, so the next
   completion interrupts. No threshold keeps a queue interrupting per
   completion under EVENT_IDX: after that interrupt the queue stays
   quiet until a thread arms it again, apart from one forced interrupt
   each 32768 completions. A sleeper arms before every wait, and a
   queue that is only polled inline never needs the interrupt.  */
void virtio_queue_set_event_idx (struct virtio_queue *, int enabled);

/* Quiet the device while the consumer polls (budgeted drain).  At most
   one more interrupt can arrive afterwards; any imperfection only
   costs a harmless wakeup, never a missed one, because sleeps always
   rearm first.  */
void virtio_queue_disable_used_cb (struct virtio_queue *);

/* Arm the next completion and return the poll token: publish the
   consumed position as the threshold, with the store ordered before the
   caller's recheck.  */
uint16_t virtio_queue_enable_used_cb_prepare (struct virtio_queue *);

/* True when the device published completions beyond TOKEN; call after
   enable_used_cb_prepare and before sleeping to close the arm race.  */
int virtio_queue_poll_used (const struct virtio_queue *, uint16_t token);

/* Sleep until the used index differs from TOKEN or the timeout elapses.
   Unlike virtio_queue_wait_used nothing is consumed, no chain is
   recycled and last_used is untouched; the caller drains with
   try_pop_used after this returns.  Config-change wakeups with no
   progress keep waiting (the used index is the condition).  Returns 0
   on observed progress, ETIMEDOUT.  */
error_t virtio_queue_wait_progress (struct virtio_queue *, uint16_t token,
                                    int timeout_sec);


/* Install/remove the INTx handler (libirqhelp).  Removal resets the device
   first (stopping new INTx asserts and clearing the ISR), then drops the
   handler, mirroring the NetBSD/Linux teardown order.  */
error_t virtio_irq_install (struct virtio_irq *, struct virtio_pci_dev *,
                            struct virtio_queue **queues, size_t nqueues);
void virtio_irq_uninstall (struct virtio_irq *);

/* Block until the config-change sequence number differs from *SEQ, or the
   timeout elapses; on success *SEQ holds the new value.  */
error_t virtio_irq_wait_config (struct virtio_irq *, int timeout_sec,
                                uint32_t *seq);

#endif /* VIRTIO_QUEUE_H */
