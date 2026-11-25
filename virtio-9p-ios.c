/* The stream adapter: the boundary 9pfs actually sees.

   9pfs opens the node once and uses the fd from two threads, a sender
   and whoever holds its reply-reading turn, so this must behave like a
   byte pipe, not a message queue: writes accumulate until a 4-byte
   little-endian size prefix says the PDU is complete, and reads return
   as soon as any reply byte is available (short reads are mandatory,
   never wait for a full buffer).

   The message size is agreed here as well.  9pfs asks for 16 MiB in its
   Tversion and sizes every later message to the answer, so a Tversion
   goes through v9p_clamp_tversion before it is posted and the device
   answers with a size the request buffers carry.

   Many requests are in flight at once.  A complete PDU is posted to the
   device and the write returns without waiting for its reply, so the
   next request from another thread goes straight out.  Completions are
   collected in arrival order and queued as whole replies, and reads
   hand them out in that order; 9pfs matches each reply to its request
   by tag, so the order need not match the order of the requests.  The
   request pool bounds the concurrency and throttles a writer when every
   slot is in flight.  */

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "virtio-9p.h"

static uint8_t *acc;
static size_t acc_len, acc_off;

/* Serialises the accumulator: a write call appends atomically, and the
   submitter of a complete PDU holds it until the PDU is on the
   device.  */
static pthread_mutex_t acc_lock = PTHREAD_MUTEX_INITIALIZER;

error_t
v9p_ios_init (void)
{
  acc = malloc (V9P_TX_BUF_SIZE);
  if (!acc)
    return ENOMEM;
  return 0;
}

static uint32_t
acc_size (void)
{
  return acc[acc_off] | (acc[acc_off + 1] << 8)
         | (acc[acc_off + 2] << 16) | ((uint32_t) acc[acc_off + 3] << 24);
}

ssize_t
v9p_ios_write (const void *buf, size_t len)
{
  error_t err;

  pthread_mutex_lock (&acc_lock);
  if (acc_len - acc_off + len > V9P_TX_BUF_SIZE)
    {
      pthread_mutex_unlock (&acc_lock);
      errno = EMSGSIZE;
      return -1;
    }
  memcpy (acc + acc_len, buf, len);
  acc_len += len;

  for (;;)
    {
      uint32_t size;

      if (acc_len - acc_off < 4)
        break;
      size = acc_size ();
      if (size < 7 || size > V9P_TX_BUF_SIZE)
        {
          pthread_mutex_unlock (&acc_lock);
          v9p_fail (EMSGSIZE);
          errno = EMSGSIZE;
          return -1;
        }
      if (acc_len - acc_off < size)
        break;

      v9p_clamp_tversion (acc + acc_off, size);

      err = v9p_submit (acc + acc_off, size);
      if (err)
        {
          pthread_mutex_unlock (&acc_lock);
          v9p_fail (err);
          errno = err;
          return -1;
        }
      acc_off += size;
    }

  if (acc_off == acc_len)
    acc_off = acc_len = 0;
  else if (acc_off > 0)
    {
      memmove (acc, acc + acc_off, acc_len - acc_off);
      acc_len -= acc_off;
      acc_off = 0;
    }
  pthread_mutex_unlock (&acc_lock);
  return len;
}

ssize_t
v9p_ios_read (void *buf, size_t len)
{
  size_t n;
  error_t err;

  err = v9p_reply_next (buf, len, &n);
  if (err)
    {
      errno = err;
      return -1;
    }
  return n;
}

int
v9p_ios_can_read (void)
{
  return v9p_reply_ready ();
}

int
v9p_ios_can_write (void)
{
  int r;
  pthread_mutex_lock (&acc_lock);
  r = acc_len - acc_off < V9P_TX_BUF_SIZE;
  pthread_mutex_unlock (&acc_lock);
  return r;
}
