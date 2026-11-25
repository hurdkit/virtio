/* virtio-pool.c: the shared DMA slot-pool engine declared in
   virtio-pool.h.  See the header for the locking and stale-completion
   contract.  */

#include <errno.h>
#include <mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "virtio-pool.h"

error_t
virtio_pool_init (struct virtio_slot_pool *p,
                  struct virtio_pci_dev *pci,
                  const char *name, const int *verbose,
                  uint16_t max_slots, size_t slot_size,
                  uint16_t queue_num)
{
  size_t region;
  uint16_t i;
  error_t err;

  p->name = name;
  p->verbose = verbose;
  p->slot_size = slot_size;

  pthread_mutex_init (&p->lock, NULL);
  pthread_cond_init (&p->cv, NULL);
  /* Clamp the pool to the device-reported queue depth: a pool larger
     than a queue would strand slots that can never be posted.  */
  p->nslots = queue_num < max_slots ? queue_num : max_slots;

  region = (size_t) p->nslots * slot_size;
  err = virtio_dma_alloc (pci, region, (void **) &p->slots, &p->slots_phys);
  if (err)
    {
      fprintf (stderr, "%s: cannot allocate slot pool (%zu)\n", name,
               region);
      return ENOMEM;
    }
  memset (p->slots, 0, region);

  p->head2slot = malloc ((size_t) queue_num * sizeof (uint16_t));
  p->slot_used = malloc ((size_t) p->nslots * sizeof (int));
  p->slot_done = malloc ((size_t) p->nslots * sizeof (int));
  p->slot_result = malloc ((size_t) p->nslots * sizeof (struct virtio_used_elem));
  p->slot_error = malloc ((size_t) p->nslots * sizeof (int));
  p->slot_waiters = malloc ((size_t) p->nslots * sizeof (int));
  if (!p->head2slot || !p->slot_used || !p->slot_done
      || !p->slot_result || !p->slot_error || !p->slot_waiters)
    {
      virtio_pool_release (p);
      return ENOMEM;
    }
  for (i = 0; i < queue_num; i++)
    p->head2slot[i] = VIRTIO_POOL_NO_SLOT;
  for (i = 0; i < p->nslots; i++)
    {
      p->slot_used[i] = 0;
      p->slot_done[i] = 0;
      p->slot_error[i] = 0;
      p->slot_waiters[i] = 0;
    }
  p->pool_error_seq = 0;
  return 0;
}

void
virtio_pool_release (struct virtio_slot_pool *p)
{
  free (p->head2slot);
  p->head2slot = NULL;
  free (p->slot_used);
  p->slot_used = NULL;
  free (p->slot_done);
  p->slot_done = NULL;
  free (p->slot_result);
  p->slot_result = NULL;
  free (p->slot_error);
  p->slot_error = NULL;
  free (p->slot_waiters);
  p->slot_waiters = NULL;
  pthread_cond_destroy (&p->cv);
  pthread_mutex_destroy (&p->lock);
  if (p->slots)
    {
      vm_deallocate (mach_task_self (), (vm_address_t) p->slots,
                     (vm_size_t) p->nslots * p->slot_size);
      p->slots = NULL;
    }
}

error_t
virtio_pool_alloc_slot (struct virtio_slot_pool *p, uint16_t *slot)
{
  uint16_t i;
  error_t err = EAGAIN;

  pthread_mutex_lock (&p->lock);
  for (i = 0; i < p->nslots; i++)
    if (!p->slot_used[i] && !p->slot_done[i])
      {
        *slot = i;
        err = 0;
        break;
      }
  pthread_mutex_unlock (&p->lock);
  return err;
}

error_t
virtio_pool_post_wait (struct virtio_slot_pool *p,
                       struct virtio_queue *q, uint16_t slot,
                       const struct virtio_buf *bufs,
                       uint16_t nbufs, int timeout_sec,
                       struct virtio_used_elem *used)
{
  error_t err;

  err = virtio_pool_submit (p, q, slot, bufs, nbufs, NULL);
  if (err)
    return err;
  return virtio_pool_wait_slot (p, q, slot, timeout_sec, used);
}

error_t
virtio_pool_submit (struct virtio_slot_pool *p, struct virtio_queue *q,
                    uint16_t slot, const struct virtio_buf *bufs,
                    uint16_t nbufs, uint16_t *head_out)
{
  uint16_t head;
  error_t err;

  /* Post without the kick and register the slot before kicking: the
     device can complete a chain the moment it sees it, and a reap that
     pops a completion with no registered head logs a foreign id,
     latches pool_error_seq (failing every waiter with EIO) and leaks
     the in-flight slot.  */
  err = virtio_queue_post_head_deferred (q, bufs, nbufs, &head);
  if (err)
    return err;

  pthread_mutex_lock (&p->lock);
  p->head2slot[head] = slot;
  p->slot_used[slot] = 1;
  p->slot_done[slot] = 0;
  p->slot_error[slot] = 0;
  pthread_mutex_unlock (&p->lock);

  virtio_queue_notify (q);

  if (head_out)
    *head_out = head;
  return 0;
}

static int
virtio_pool_reap_locked_impl (struct virtio_slot_pool *p,
                              struct virtio_queue *q)
{
  int count = 0;

  for (;;)
    {
      struct virtio_used_elem used;
      error_t err = virtio_queue_try_pop_used (q, &used);

      if (err == EAGAIN)
        break;
      if (err == EIO)
        {
          fprintf (stderr, "%s: bad used id (K1)\n", p->name);
          p->pool_error_seq++;
          pthread_mutex_lock (&q->lock);
          pthread_cond_broadcast (&q->cv);
          pthread_mutex_unlock (&q->lock);
          continue;
        }
      if (err)
        return -1;

      count++;
      if (used.id >= q->num || p->head2slot[used.id] == VIRTIO_POOL_NO_SLOT)
        {
          fprintf (stderr, "%s: completion id %u (len %u) is not a "
                   "posted chain\n", p->name, used.id, used.len);
          p->pool_error_seq++;
          pthread_mutex_lock (&q->lock);
          pthread_cond_broadcast (&q->cv);
          pthread_mutex_unlock (&q->lock);
          continue;
        }
      {
        uint16_t slot = p->head2slot[used.id];

        p->head2slot[used.id] = VIRTIO_POOL_NO_SLOT;
        p->slot_used[slot] = 0;
        if (p->slot_waiters[slot] > 0)
          {
            p->slot_result[slot] = used;
            p->slot_error[slot] = 0;
            p->slot_done[slot] = 1;
            pthread_cond_broadcast (&p->cv);
          }
        else
          {
            p->slot_done[slot] = 0;
          }
      }
    }
  return count;
}

int
virtio_pool_reap_locked (struct virtio_slot_pool *p, struct virtio_queue *q)
{
  return virtio_pool_reap_locked_impl (p, q);
}

int
virtio_pool_reap (struct virtio_slot_pool *p, struct virtio_queue *q)
{
  int r;

  pthread_mutex_lock (&p->lock);
  r = virtio_pool_reap_locked_impl (p, q);
  pthread_mutex_unlock (&p->lock);
  return r;
}

/* Register a waiter for SLOT before its chain is submitted: a reap
   consuming the completion while the submitter is between submit and
   wait then stores the result and signals the waiter instead of
   discarding it.  Every arm pairs with exactly one wait_armed
   (submission succeeded) or disarm_wait (submission failed).  */
void
virtio_pool_arm_wait (struct virtio_slot_pool *p, uint16_t slot)
{
  pthread_mutex_lock (&p->lock);
  p->slot_waiters[slot]++;
  pthread_mutex_unlock (&p->lock);
}

void
virtio_pool_disarm_wait (struct virtio_slot_pool *p, uint16_t slot)
{
  pthread_mutex_lock (&p->lock);
  p->slot_waiters[slot]--;
  pthread_mutex_unlock (&p->lock);
}

/* Under p->lock: consume SLOT's stored completion if a reap delivered
   it, otherwise classify why the wait must continue.  Returns 0 with
   *USED filled (or the chain's stored error), EIO when the pool latched
   a foreign completion while this waiter was registered, and EAGAIN
   when the completion has not landed yet.  A terminal result consumes
   the armed waiter (the arm/disarm pairing).  */
static error_t
virtio_pool_wait_collect (struct virtio_slot_pool *p, uint16_t slot,
                          struct virtio_used_elem *used,
                          uint32_t observed_seq)
{
  if (p->slot_done[slot])
    {
      *used = p->slot_result[slot];
      p->slot_done[slot] = 0;
      p->slot_waiters[slot]--;
      return p->slot_error[slot];
    }
  if (p->pool_error_seq != observed_seq)
    {
      p->slot_waiters[slot]--;
      return EIO;
    }
  return EAGAIN;
}

error_t
virtio_pool_wait_armed (struct virtio_slot_pool *p, struct virtio_queue *q,
                        uint16_t slot, int timeout_sec,
                        struct virtio_used_elem *used)
{
  struct timespec deadline;
  int werr;
  uint32_t observed_seq;
  uint16_t token;

  clock_gettime (CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_sec;

  pthread_mutex_lock (&p->lock);
  observed_seq = p->pool_error_seq;
  pthread_mutex_unlock (&p->lock);

  for (;;)
    {
      pthread_mutex_lock (&p->lock);
      virtio_pool_reap_locked_impl (p, q);
      werr = virtio_pool_wait_collect (p, slot, used, observed_seq);
      pthread_mutex_unlock (&p->lock);
      if (werr != EAGAIN)
        return werr;

      /* Sleep only armed.  The drain above left the ring empty, so
         publishing the consumed position makes the next completion
         interrupt.  Without that arm the threshold in place was
         already passed and nothing under EVENT_IDX wakes this waiter.
         The recheck runs under the queue lock, the one the IRQ handler
         broadcasts under: a completion that lands between the arm and
         the wait is either seen by the recheck or signals the condvar
         once the wait has begun.  Without the recheck a wakeup that
         arrives between the drain and the wait is lost to it.  */
      token = virtio_queue_enable_used_cb_prepare (q);
      pthread_mutex_lock (&q->lock);
      if (virtio_queue_poll_used (q, token))
        {
          pthread_mutex_unlock (&q->lock);
          continue;
        }
      werr = pthread_cond_timedwait (&q->cv, &q->lock, &deadline);
      pthread_mutex_unlock (&q->lock);
      if (werr != 0)
        {
          /* The sleep ended on the deadline (or a wait error); one
             last reap decides between a completion that raced the
             wakeup and a genuine timeout.  */
          pthread_mutex_lock (&p->lock);
          virtio_pool_reap_locked_impl (p, q);
          werr = virtio_pool_wait_collect (p, slot, used, observed_seq);
          if (werr == EAGAIN)
            {
              /* A timeout is terminal too: give up the armed waiter,
                 so that the late completion releases the slot instead
                 of keeping it as a result nobody collects.  */
              p->slot_waiters[slot]--;
              werr = ETIMEDOUT;
            }
          pthread_mutex_unlock (&p->lock);
          return werr;
        }
    }
}

error_t
virtio_pool_wait_slot (struct virtio_slot_pool *p, struct virtio_queue *q,
                       uint16_t slot, int timeout_sec,
                       struct virtio_used_elem *used)
{
  virtio_pool_arm_wait (p, slot);
  return virtio_pool_wait_armed (p, q, slot, timeout_sec, used);
}
