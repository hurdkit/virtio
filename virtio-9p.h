#ifndef VIRTIO_9P_H
#define VIRTIO_9P_H

/* virtio-9p device translator for GNU/Hurd.

   Boundary conventions: the legacy PCI interface (BAR0 in I/O space), one
   split virtqueue carrying up to V9P_MAX_INFLIGHT requests at once, and a
   byte-stream front end for the unmodified 9pfs translator.  The transport
   is the generic legacy virtio PCI core (virtio-pci.c, virtio-queue.c); this
   header keeps the 9p-specific policy (device ID, mount tag, msize caps,
   request concurrency) and the adapter entry points.  */

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <pciaccess.h>
#include <hurd.h>

#include "virtio-pci.h"
#include "virtio-queue.h"

/* VIRTIO_9P_MOUNT_TAG is feature bit 0 and is required: without it the
   mount tag is not delivered.  */
#define V9P_F_MOUNT_TAG 0

#define V9P_DEVICE_VIRTIO_9P 0x1009

/* Every 9P message on this transport fits one TX and one RX buffer of
   this size.  vm_allocate_contiguous rounds to power-of-two blocks, so
   these must not be silently undersized.  */
#define V9P_TX_BUF_SIZE (4u << 20)
#define V9P_RX_BUF_SIZE (4u << 20)

/* The largest msize both buffers can serve.  9pfs asks for 16 MiB and
   sizes every message to the msize the device answers.  QEMU 11.1 lowers
   the answer to (VIRTQUEUE_MAX_SIZE - 2) * 4096 and QEMU 10.x and earlier
   echo the 16 MiB back, so the transport lowers the request itself in
   v9p_clamp_tversion.  */
#define V9P_MSIZE_MAX \
  ((V9P_TX_BUF_SIZE < V9P_RX_BUF_SIZE) ? V9P_TX_BUF_SIZE : V9P_RX_BUF_SIZE)

/* The 9P message types the transport reads by field.  */
#define V9P_TVERSION 100
#define V9P_RVERSION 101

/* The header at the front of every 9P message: a 4-byte size counting
   the whole message, the type byte and a 2-byte tag, with no padding
   between them.  The structs below describe the wire, so they are packed
   and a PDU at any byte alignment can be cast to them.  Their
   multi-byte fields are little-endian: reach them through the byte-wise
   helpers of virtio-9p-req.c, get_le32, put_le32 and their kin, never
   through the host's byte order.  */
struct v9p_hdr
{
  uint32_t size;
  uint8_t type;
  uint16_t tag;
} __attribute__ ((packed));

_Static_assert (sizeof (struct v9p_hdr) == 7,
                 "the 9P message header is 7 bytes");
_Static_assert (offsetof (struct v9p_hdr, size) == 0,
                 "the size comes first");
_Static_assert (offsetof (struct v9p_hdr, type) == 4,
                 "the type byte follows the size");
_Static_assert (offsetof (struct v9p_hdr, tag) == 5,
                 "the tag follows the type byte");

/* The head of a Tversion: the msize follows the header.  This is as far
   as v9p_clamp_tversion reads and writes.  */
struct v9p_tversion
{
  struct v9p_hdr hdr;
  uint32_t msize;
} __attribute__ ((packed));

_Static_assert (sizeof (struct v9p_tversion) == 11,
                 "the msize ends a Tversion head at byte 11");
_Static_assert (offsetof (struct v9p_tversion, hdr) == 0,
                 "a Tversion starts with the header");
_Static_assert (offsetof (struct v9p_tversion, msize) == 7,
                 "the msize follows the header");

/* A whole Tversion, and the Rversion that answers it with the same
   fields: after the msize comes the version string, a 2-byte length and
   that many bytes.  */
struct v9p_version
{
  struct v9p_hdr hdr;
  uint32_t msize;
  uint16_t version_len;
  char version[];
} __attribute__ ((packed));

_Static_assert (sizeof (struct v9p_version) == 13,
                 "a version message is 13 bytes before its string");
_Static_assert (offsetof (struct v9p_version, hdr) == 0,
                 "a version message starts with the header");
_Static_assert (offsetof (struct v9p_version, msize) == 7,
                 "the msize follows the header");
_Static_assert (offsetof (struct v9p_version, version_len) == 11,
                 "the version string length follows the msize");
_Static_assert (offsetof (struct v9p_version, version) == 13,
                 "the version string follows its length");

#define V9P_MAX_TAG_LEN 32
#define V9P_DEFAULT_MOUNT_TAG "hostshare"

/* Per-exchange bound on the interrupt wait; the fallback for a lost
   interrupt, not the normal completion path.  */
#define V9P_EXCHANGE_TIMEOUT 30

/* Requests on the device at once, each in its own pair of full-size DMA
   buffers.  Slots are allocated as concurrency first needs them, so this
   is a ceiling on DMA memory, not the steady cost.  */
#define V9P_MAX_INFLIGHT 16

extern struct virtio_pci_dev v9p_pci;
extern struct virtio_queue v9p_q;
/* True when the device and driver negotiated VIRTIO_RING_F_EVENT_IDX.  */
extern int v9p_event_idx;
extern struct virtio_irq v9p_irq;
extern char v9p_tag[V9P_MAX_TAG_LEN + 1];
extern uint16_t v9p_tag_len;
extern uint64_t v9p_vring_phys;

/* virtio-9p.c */
error_t v9p_pci_probe (const char *override);
error_t v9p_pci_init (void);
error_t v9p_queue_setup (void);
error_t v9p_irq_install (void);
void v9p_irq_uninstall (void);

/* virtio-9p-req.c */
/* Post one request PDU and return without waiting for its reply.  Blocks
   only while every request slot is in flight.  */
error_t v9p_submit (const void *pdu, uint32_t pdu_len);
/* Copy up to LEN bytes of the oldest queued reply, waiting for one when
   none is queued and returning short as soon as any byte is available.  */
error_t v9p_reply_next (void *buf, size_t len, size_t *nread);
int v9p_reply_ready (void);
/* Start the completion collector thread, after the queue and the IRQ are
   up.  */
error_t v9p_collect_start (void);
/* Fail the transport: queued replies are still read, then ERR.  */
void v9p_fail (error_t err);
/* Synchronous request and reply, for the selftest only.  */
error_t v9p_exchange (const void *pdu, uint32_t pdu_len,
                      void *reply, uint32_t reply_cap, uint32_t *reply_len);
/* Lower the msize of a Tversion PDU to V9P_MSIZE_MAX when it is above
   that.  Any other message is left alone.  */
void v9p_clamp_tversion (uint8_t *pdu, uint32_t pdu_len);

/* Set by --verbose.  Gates the prints that can recur on every client.  */
extern int v9p_verbose;
error_t v9p_selftest (const char *required_tag);

/* virtio-9p-ios.c */
error_t v9p_ios_init (void);
ssize_t v9p_ios_write (const void *buf, size_t len);
ssize_t v9p_ios_read (void *buf, size_t len);
int v9p_ios_can_read (void);
int v9p_ios_can_write (void);

#endif /* VIRTIO_9P_H */
