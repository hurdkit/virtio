/* 9P PDU framing, the request pool, and the Phase 1 selftest.

   This file stays a framing layer, not a protocol interpreter: it checks
   the 4-byte little-endian size prefix, owns the TX/RX DMA buffers
   through the queue module, posts requests and collects their
   completions into the reply queue the stream adapter reads.  The one
   field it reads is the msize of a Tversion, which it lowers to what the
   buffers carry.  It also runs the hand-built exchanges of the selftest,
   which prove the device, the feature negotiation, the queue and the DMA
   addresses all work before a client opens the node.  */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "virtio-9p.h"

static void
put_le16 (void *p, uint16_t v)
{
  uint8_t *b = p;
  b[0] = v;
  b[1] = v >> 8;
}

static void
put_le32 (void *p, uint32_t v)
{
  uint8_t *b = p;
  b[0] = v;
  b[1] = v >> 8;
  b[2] = v >> 16;
  b[3] = v >> 24;
}

static uint16_t
get_le16 (const void *p)
{
  const uint8_t *b = p;
  return b[0] | (b[1] << 8);
}

static uint32_t
get_le32 (const void *p)
{
  const uint8_t *b = p;
  return b[0] | (b[1] << 8) | (b[2] << 16) | ((uint32_t) b[3] << 24);
}

/* Request slots.  The device has one queue, but one queue carries many
   descriptor chains at once: every request owns a slot with its own TX
   and RX DMA buffers for as long as it is in flight, so several 9pfs
   threads can have a request on the device at the same time.

   A slot is FREE, INFLIGHT from the post until its completion is
   collected, and DONE while its reply waits in the done ring for the
   reader.  The done ring orders replies by completion, which need not
   match the order of the requests; the stream adapter hands them to
   9pfs in that order and 9pfs matches each one by tag.  A slot becomes
   FREE only once its reply has been read out, so a reader that falls
   behind slows the writers instead of letting replies pile up
   somewhere else.

   Slots are allocated on first use.  Each costs V9P_TX_BUF_SIZE +
   V9P_RX_BUF_SIZE of contiguous DMA memory, which only concurrency
   needs, so a translator that never sees two requests at once never
   allocates a second slot.  */

struct v9p_slot
{
  uint8_t *tx_buf;
  uint8_t *rx_buf;
  uint64_t tx_phys;
  uint64_t rx_phys;
  /* Head descriptor of the posted chain, the handle a completion
     carries.  NO_DESC while the slot holds no posted chain.  */
  uint16_t head;
  uint32_t reply_len;   /* bytes of reply in rx_buf */
  uint32_t reply_off;   /* bytes of the reply already read out */
  int state;
};

enum
{
  SLOT_FREE,
  SLOT_INFLIGHT,
  SLOT_DONE
};

static struct v9p_slot slots[V9P_MAX_INFLIGHT];
static unsigned int nslots;     /* slots allocated so far */
static unsigned int max_slots;  /* min (V9P_MAX_INFLIGHT, queue size / 2) */
static error_t pool_error;      /* sticky, fatal */

/* Completed requests in completion order, by slot index.  */
static unsigned int done_ring[V9P_MAX_INFLIGHT];
static unsigned int done_head, done_len;
/* Set while a reader copies out of the head slot's rx buffer, outside
   the lock.  */
static int reply_reading;

static pthread_mutex_t pool_lock = PTHREAD_MUTEX_INITIALIZER;
/* Signalled when a slot becomes FREE.  */
static pthread_cond_t free_cv = PTHREAD_COND_INITIALIZER;
/* Signalled when a reply is queued, when the head reply has been read
   out, and when the pool fails.  */
static pthread_cond_t done_cv = PTHREAD_COND_INITIALIZER;

/* Under pool_lock.  Wakes every waiter; each one rechecks its own
   condition and sees the error where it applies.  */
static void
pool_fail_locked (error_t err)
{
  if (pool_error)
    return;
  pool_error = err;
  pthread_cond_broadcast (&free_cv);
  pthread_cond_broadcast (&done_cv);
}

/* Mark the whole transport failed: the device lost a reply or the
   completion path is out of sync.  Replies already queued are still
   read; everything after them fails with ERR.  */
void
v9p_fail (error_t err)
{
  pthread_mutex_lock (&pool_lock);
  pool_fail_locked (err);
  pthread_mutex_unlock (&pool_lock);
}

static error_t
slot_alloc (struct v9p_slot *s)
{
  error_t err;

  err = virtio_dma_alloc (&v9p_pci, V9P_TX_BUF_SIZE,
                          (void **) &s->tx_buf, &s->tx_phys);
  if (err)
    return err;
  err = virtio_dma_alloc (&v9p_pci, V9P_RX_BUF_SIZE,
                          (void **) &s->rx_buf, &s->rx_phys);
  if (err)
    /* The TX buffer stays allocated.  Nothing in this driver frees
       contiguous memory: the vring is never released either.  */
    return err;
  s->state = SLOT_FREE;
  s->head = VIRTIO_QUEUE_NO_DESC;
  fprintf (stderr, "virtio-9p: request slot %u TX/RX 0x%llx/0x%llx\n",
           (unsigned int) (s - slots),
           (unsigned long long) s->tx_phys,
           (unsigned long long) s->rx_phys);
  return 0;
}

/* Under pool_lock: a FREE slot, allocated now if the pool still has
   room, waiting while every slot is in flight.  */
static error_t
slot_acquire (struct v9p_slot **out)
{
  for (;;)
    {
      unsigned int i;
      error_t err;

      if (pool_error)
        return pool_error;
      for (i = 0; i < nslots; i++)
        if (slots[i].state == SLOT_FREE)
          {
            *out = &slots[i];
            return 0;
          }
      if (nslots < max_slots)
        {
          err = slot_alloc (&slots[nslots]);
          if (!err)
            {
              *out = &slots[nslots];
              nslots++;
              return 0;
            }
          /* Contiguous memory ran out.  Serve with the slots that
             exist rather than retry a failing allocation per request.  */
          fprintf (stderr, "virtio-9p: request slot %u: %s, keeping %u "
                   "slot(s)\n", nslots, strerror (err), nslots);
          max_slots = nslots;
          if (nslots == 0)
            return err;
        }
      pthread_cond_wait (&free_cv, &pool_lock);
    }
}

/* Under pool_lock: the INFLIGHT slot whose posted chain starts at
   descriptor HEAD.  */
static struct v9p_slot *
slot_for_head (uint16_t head)
{
  unsigned int i;

  for (i = 0; i < nslots; i++)
    if (slots[i].state == SLOT_INFLIGHT && slots[i].head == head)
      return &slots[i];
  return NULL;
}

/* Under pool_lock: move every completion in the used ring to the done
   ring.  */
static void
collect_used (void)
{
  for (;;)
    {
      struct virtio_used_elem used;
      struct v9p_slot *s;
      error_t err;

      err = virtio_queue_try_pop_used (&v9p_q, &used);
      if (err == EAGAIN)
        return;
      if (err)
        {
          pool_fail_locked (err);
          return;
        }

      /* The chain's head descriptor identifies the slot.  The pool
         lock serialises this lookup against posts: the pop has already
         returned the chain's descriptors to the free list, so a post
         racing it could hand the same head to a new chain before the
         lookup ran.  */
      s = slot_for_head (used.id);
      if (!s)
        {
          fprintf (stderr, "virtio-9p: completion for unknown chain %u\n",
                   used.id);
          pool_fail_locked (EIO);
          return;
        }
      /* Every reply carries at least the 7-byte header.  A shorter one
         is a device fault, and it must not reach the reader: zero bytes
         read is how the client sees end of stream.  */
      if (used.len < sizeof (struct v9p_hdr) || used.len > V9P_RX_BUF_SIZE)
        {
          fprintf (stderr, "virtio-9p: reply length %u outside 7..%u\n",
                   used.len, V9P_RX_BUF_SIZE);
          pool_fail_locked (EIO);
          return;
        }
      s->state = SLOT_DONE;
      s->reply_len = used.len;
      s->reply_off = 0;
      done_ring[(done_head + done_len) % V9P_MAX_INFLIGHT] = s - slots;
      done_len++;
      pthread_cond_broadcast (&done_cv);
    }
}

/* Under pool_lock, the second half of a completion window: drain the
   used ring, publish the consumed position as the interrupt threshold,
   and recheck the used index, repeating while completions keep landing.
   Returns with the ring drained and the device armed for the next
   completion, and returns that position: the token a sleeper passes to
   virtio_queue_wait_progress.  */
static uint16_t
drain_and_arm (void)
{
  uint16_t token;

  do
    {
      collect_used ();
      token = virtio_queue_enable_used_cb_prepare (&v9p_q);
    }
  while (!pool_error && virtio_queue_poll_used (&v9p_q, token));
  return token;
}

/* Post one request PDU and return without waiting for its reply.  The
   reply is queued for v9p_reply_next when the completion is collected,
   by this call, by another submitter, or by the collector thread.
   Blocks only while every slot is in flight.  */
error_t
v9p_submit (const void *pdu, uint32_t pdu_len)
{
  struct virtio_buf bufs[2];
  struct v9p_slot *s;
  uint16_t head;
  error_t err;

  if (pdu_len < sizeof (struct v9p_hdr) || pdu_len > V9P_TX_BUF_SIZE)
    return EMSGSIZE;

  pthread_mutex_lock (&pool_lock);
  err = slot_acquire (&s);
  if (err)
    {
      pthread_mutex_unlock (&pool_lock);
      return err;
    }
  s->state = SLOT_INFLIGHT;
  s->head = VIRTIO_QUEUE_NO_DESC;
  s->reply_len = s->reply_off = 0;
  pthread_mutex_unlock (&pool_lock);

  memcpy (s->tx_buf, pdu, pdu_len);

  bufs[0].addr = s->tx_phys;
  bufs[0].len = pdu_len;
  bufs[0].flags = 0;
  bufs[1].addr = s->rx_phys;
  bufs[1].len = V9P_RX_BUF_SIZE;
  bufs[1].flags = VIRTQ_DESC_F_WRITE;

  pthread_mutex_lock (&pool_lock);
  /* Post without the kick and record the head before the device can
     complete the chain, so that a drain on any thread finds the slot.
     The pool lock held across the post and the recording is what keeps
     that ahead of the free list handing the head to the next post.  */
  err = virtio_queue_post_head_deferred (&v9p_q, bufs, 2, &head);
  if (err)
    {
      s->state = SLOT_FREE;
      pthread_cond_broadcast (&free_cv);
      pthread_mutex_unlock (&pool_lock);
      return err;
    }
  s->head = head;

  /* A completion window, the same one the collector thread runs minus
     the sleep.  The interrupt is quieted over the kick and the inline
     drain: the emulated device often completes before the kick's exit
     returns, so the drain below usually collects the reply without the
     interrupt, the irqhelp wake and the collector hop.  The window ends
     armed at the consumed position like every window does, which is
     what keeps a sleeping collector wakeable after this thread has
     consumed a completion it never saw.  */
  virtio_queue_disable_used_cb (&v9p_q);
  virtio_queue_notify (&v9p_q);
  drain_and_arm ();
  pthread_mutex_unlock (&pool_lock);
  return 0;
}

int
v9p_reply_ready (void)
{
  int r;

  pthread_mutex_lock (&pool_lock);
  r = done_len > 0;
  pthread_mutex_unlock (&pool_lock);
  return r;
}

/* Copy up to LEN bytes of the oldest queued reply into BUF, waiting for
   a reply when none is queued.  Returns as soon as any reply byte is
   available, so *NREAD may be shorter than LEN; the slot goes back to
   the pool once the last of its reply has been copied out.  Replies
   already queued are returned even after the transport failed; then the
   error is returned.  */
error_t
v9p_reply_next (void *buf, size_t len, size_t *nread)
{
  struct v9p_slot *s;
  size_t n;
  error_t err;

  pthread_mutex_lock (&pool_lock);
  while (done_len == 0 || reply_reading)
    {
      if (done_len == 0 && pool_error)
        {
          err = pool_error;
          pthread_mutex_unlock (&pool_lock);
          return err;
        }
      pthread_cond_wait (&done_cv, &pool_lock);
    }
  s = &slots[done_ring[done_head]];
  reply_reading = 1;
  pthread_mutex_unlock (&pool_lock);

  /* Nothing else touches a DONE slot's buffers, so the copy runs
     outside the lock.  */
  n = s->reply_len - s->reply_off;
  if (n > len)
    n = len;
  memcpy (buf, s->rx_buf + s->reply_off, n);

  pthread_mutex_lock (&pool_lock);
  s->reply_off += n;
  if (s->reply_off == s->reply_len)
    {
      done_head = (done_head + 1) % V9P_MAX_INFLIGHT;
      done_len--;
      s->state = SLOT_FREE;
      pthread_cond_broadcast (&free_cv);
    }
  reply_reading = 0;
  pthread_cond_broadcast (&done_cv);
  pthread_mutex_unlock (&pool_lock);
  *nread = n;
  return 0;
}

/* Collect completions that arrive when no submitter is draining.  It
   owns the queue's completion-moderation policy: one window per wakeup,
   then a sleep armed at the consumed position.  Submitters run the same
   window without the sleep, and every window ends armed, so a sleeper
   is woken by the next completion whichever thread consumed the last
   one.  */
static void *
collector_thread (void *arg)
{
  (void) arg;

  for (;;)
    {
      unsigned int inflight, i;
      uint16_t token;
      error_t err;

      pthread_mutex_lock (&pool_lock);
      if (pool_error)
        {
          pthread_mutex_unlock (&pool_lock);
          break;
        }
      virtio_queue_disable_used_cb (&v9p_q);
      token = drain_and_arm ();
      inflight = 0;
      for (i = 0; i < nslots; i++)
        inflight += slots[i].state == SLOT_INFLIGHT;
      pthread_mutex_unlock (&pool_lock);

      err = virtio_queue_wait_progress (&v9p_q, token,
                                        V9P_EXCHANGE_TIMEOUT);
      if (err == ETIMEDOUT && inflight)
        {
          fprintf (stderr, "virtio-9p: no completion for %u request(s) in "
                   "%d s\n", inflight, V9P_EXCHANGE_TIMEOUT);
          v9p_fail (ETIMEDOUT);
          break;
        }
    }
  return NULL;
}

error_t
v9p_collect_start (void)
{
  pthread_t thread;
  error_t err;

  /* Two descriptors per request, and a post must never find the ring
     short of descriptors.  */
  max_slots = v9p_q.num / 2;
  if (max_slots > V9P_MAX_INFLIGHT)
    max_slots = V9P_MAX_INFLIGHT;

  err = pthread_create (&thread, NULL, collector_thread, NULL);
  if (err)
    return err;
  pthread_detach (thread);
  return 0;
}

/* Lower the msize of a Tversion, in every dialect, to V9P_MSIZE_MAX.
   9pfs asks for 16 MiB and takes whatever Rversion carries, then sizes
   every Tread, Twrite and Treaddir to it, and the device sizes its own
   replies and its iounit to it as well.  A Tread for more than an RX
   buffer holds is then lost: QEMU finds the reply cannot fit the chain
   and marks the device broken.  The device answers with what is asked,
   so the request is lowered before the device sees it: client and
   device then agree on a size the buffers carry.  Lowering only the
   answer would leave the device free to build a reply, a large Treaddir
   for one, that no buffer holds.  */
void
v9p_clamp_tversion (uint8_t *pdu, uint32_t pdu_len)
{
  struct v9p_tversion *tv = (struct v9p_tversion *) pdu;
  uint32_t msize;

  if (pdu_len < sizeof (struct v9p_tversion) || tv->hdr.type != V9P_TVERSION)
    return;

  msize = get_le32 (&tv->msize);
  if (msize > V9P_MSIZE_MAX)
    {
      /* Every client that opens the node sends a Tversion, so this
         recurs.  Under --verbose only.  */
      if (v9p_verbose)
        fprintf (stderr, "virtio-9p: Tversion msize %u lowered to %u\n",
                 msize, V9P_MSIZE_MAX);
      put_le32 (&tv->msize, V9P_MSIZE_MAX);
    }
}

/* Collect the oldest queued reply into REPLY, at most REPLY_CAP bytes,
   until its size prefix says it is whole.  Its length lands in
   REPLY_LEN.  */
static error_t
read_reply (void *reply, uint32_t reply_cap, uint32_t *reply_len)
{
  size_t got = 0, n;
  uint32_t size;
  error_t err;

  for (got = 0; got < 4 || get_le32 (reply) > got; got += n)
    {
      if (got >= 4)
        {
          size = get_le32 (reply);
          if (size > reply_cap)
            {
              fprintf (stderr, "virtio-9p: reply %u > capacity %u\n",
                       size, reply_cap);
              return EMSGSIZE;
            }
        }
      err = v9p_reply_next ((uint8_t *) reply + got, reply_cap - got, &n);
      if (err)
        {
          fprintf (stderr, "virtio-9p: reply wait failed: %s\n",
                   strerror (err));
          return err;
        }
    }
  *reply_len = got;
  return 0;
}

/* One synchronous request: the complete outbound PDU in TX, reply
   capacity in RX.  The reply lands in REPLY, at most REPLY_CAP bytes,
   and its length in REPLY_LEN.  Only valid while no other request is in
   flight, since it takes the oldest queued reply: that holds for the
   selftest, which runs before the stream adapter accepts a client.  */
error_t
v9p_exchange (const void *pdu, uint32_t pdu_len,
              void *reply, uint32_t reply_cap, uint32_t *reply_len)
{
  error_t err;

  if (reply_cap > V9P_RX_BUF_SIZE)
    return EMSGSIZE;

  err = v9p_submit (pdu, pdu_len);
  if (err)
    return err;

  return read_reply (reply, reply_cap, reply_len);
}

/* The same exchange through the stream adapter, the path 9pfs uses: the
   request goes in as one write and the reply comes out of the read
   side.  */
static error_t
adapter_exchange (const void *pdu, uint32_t pdu_len,
                  void *reply, uint32_t reply_cap, uint32_t *reply_len)
{
  if (v9p_ios_write (pdu, pdu_len) < 0)
    return errno;
  return read_reply (reply, reply_cap, reply_len);
}

/* A Tversion for 9P2000.L asking for 16 MiB, as 9pfs does.  Returns its
   length.  */
static uint32_t
put_tversion (uint8_t *pdu)
{
  struct v9p_version *tv = (struct v9p_version *) pdu;
  uint32_t pdu_len = sizeof (struct v9p_version) + 8;

  put_le32 (&tv->hdr.size, pdu_len);
  tv->hdr.type = V9P_TVERSION;
  put_le16 (&tv->hdr.tag, 0);
  put_le32 (&tv->msize, 16u << 20);
  put_le16 (&tv->version_len, 8);
  memcpy (tv->version, "9P2000.L", 8);
  return pdu_len;
}

/* Check that REPLY holds an Rversion of RLEN bytes for 9P2000.L, and
   return its msize in *MSIZE.  */
static error_t
get_rversion (const uint8_t *reply, uint32_t rlen, uint32_t *msize)
{
  const struct v9p_version *rv = (const struct v9p_version *) reply;
  uint32_t vlen;

  if (rlen < sizeof (struct v9p_version) || get_le32 (&rv->hdr.size) != rlen
      || rv->hdr.type != V9P_RVERSION || get_le16 (&rv->hdr.tag) != 0)
    {
      fprintf (stderr, "virtio-9p selftest: malformed Rversion "
               "(len %u type %u tag %u)\n",
               rlen, rlen > 4 ? rv->hdr.type : 0,
               rlen > 5 ? get_le16 (&rv->hdr.tag) : 0);
      return EIO;
    }
  vlen = get_le16 (&rv->version_len);
  if (rlen < sizeof (struct v9p_version) + vlen || vlen != 8
      || memcmp (rv->version, "9P2000.L", 8) != 0)
    {
      fprintf (stderr, "virtio-9p selftest: bad Rversion version string\n");
      return EIO;
    }
  *msize = get_le32 (&rv->msize);
  return 0;
}

/* The selftest asks for 16 MiB twice.  The first Tversion goes out as
   written and reads what the device offers: QEMU 11.1 answers with its
   own cap, QEMU 10.x and earlier with the 16 MiB.  The second goes
   through the stream adapter, which lowers it to V9P_MSIZE_MAX, so its
   answer must be the smaller of the device's offer and that.  */
error_t
v9p_selftest (const char *required_tag)
{
  uint8_t pdu[32], reply[128];
  const struct v9p_version *rv = (const struct v9p_version *) reply;
  uint32_t pdu_len, rlen, device_msize, msize, expect;
  error_t err;

  pdu_len = put_tversion (pdu);
  err = v9p_exchange (pdu, pdu_len, reply, sizeof reply, &rlen);
  if (err)
    {
      fprintf (stderr, "virtio-9p selftest: Tversion exchange: %s\n",
               strerror (err));
      return err;
    }
  err = get_rversion (reply, rlen, &device_msize);
  if (err)
    return err;

  if (strcmp (required_tag, v9p_tag) != 0)
    {
      fprintf (stderr, "virtio-9p selftest: mount tag mismatch "
               "(wanted \"%s\", device has \"%s\")\n",
               required_tag, v9p_tag);
      return ENODEV;
    }

  {
    uint32_t guest_features = virtio_pci_read_guest_features (&v9p_pci);
    if (!(guest_features & (1u << V9P_F_MOUNT_TAG)))
      {
        fprintf (stderr, "virtio-9p selftest: mount tag feature not "
                 "acknowledged in guest_features (0x%x)\n", guest_features);
        return EIO;
      }
  }

  {
    uint32_t pfn = virtio_pci_read_queue_pfn (&v9p_pci);
    if (pfn != v9p_vring_phys >> 12)
      {
        fprintf (stderr, "virtio-9p selftest: QUEUE_PFN readback 0x%x != "
                 "phys >> 12 (0x%llx)\n", pfn,
                 (unsigned long long) (v9p_vring_phys >> 12));
        return EIO;
      }
    printf ("pfn=%u == phys 0x%llx >> 12\n", pfn,
            (unsigned long long) v9p_vring_phys);
  }

  put_tversion (pdu);
  err = adapter_exchange (pdu, pdu_len, reply, sizeof reply, &rlen);
  if (err)
    {
      fprintf (stderr, "virtio-9p selftest: Tversion through the stream "
               "adapter: %s\n", strerror (err));
      return err;
    }
  err = get_rversion (reply, rlen, &msize);
  if (err)
    return err;

  expect = device_msize < V9P_MSIZE_MAX ? device_msize : V9P_MSIZE_MAX;
  if (msize != expect)
    {
      fprintf (stderr, "virtio-9p selftest: msize %u through the stream "
               "adapter, expected %u, the smaller of the device's %u and "
               "the buffer size %u\n",
               msize, expect, device_msize, V9P_MSIZE_MAX);
      return EIO;
    }

  printf ("mount tag feature negotiated\n");
  printf ("Rversion ok\n");
  printf ("device msize=%u\n", device_msize);
  printf ("msize=%u\n", msize);
  printf ("version=%.*s\n", 8, rv->version);
  return 0;
}
