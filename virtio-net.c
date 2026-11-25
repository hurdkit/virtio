/* virtio-net driver: main, argp, lifecycle, selftest.

   A standalone long-lived daemon like virtio-blk: the MVP has no device
   node unless started with --serve, which turns the process into the
   Mach device translator that network stacks open through file_name_lookup
   + device_open.  Without --serve the daemon owns the device, services
   the receive queue, logs config changes and counters, and proves the
   datapath: --selftest drives the handshake, the Mach device semantics
   layer, and an in-process NET_RCV_MSG construction/delivery round trip
   (filter evaluation plus a real mach_msg send/receive on a private
   port, so the whole message path is exercised without a stack).
   --selftest-traffic additionally proves raw TX/RX against the backend.
   Every exit path is clean: a guest booted without a virtio-net device
   must produce a diagnostic, not a hang.  */

#include <arpa/inet.h>
#include <argp.h>
#include <error.h>
#include <mach/message.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <device/net_status.h>

#include "virtio-net.h"

const char *argp_program_version = "virtio-net 0.1";

int vnet_verbose;
int vnet_event_idx = 1;
int vnet_tx_sync;

/* Keys for the long-only options; 0 would not distinguish them.  */
enum {
  OPT_STATUS = 1000,
  OPT_TRAFFIC,
  OPT_SERVE,
  OPT_NAME,
  OPT_EVENT_IDX,
  OPT_NO_EVENT_IDX
};

static const char *opt_device;
static int opt_selftest;
static int opt_traffic;
static int opt_status;
static int opt_serve;
static const char *opt_name;

static const struct argp_option options[] =
{
  {"device", 'd', "DOM:BUS:DEV.FN", 0,
   "Use the virtio-net device at this PCI slot instead of the first match"},
  {"selftest", 's', 0, 0,
   "Bring the device up, run the Phase 1/2 selftest, and exit"},
  {"selftest-traffic", OPT_TRAFFIC, 0, 0,
   "With --selftest, also send an ARP request and wait for backend "
   "traffic; result depends on the QEMU net backend"},
  {"status", OPT_STATUS, 0, 0,
   "Print the device's MAC, features, and link state and exit, without "
   "disturbing a running driver"},
  {"serve", OPT_SERVE, 0, 0,
   "Run as a Mach device translator (settrans): answer device_open/"
   "write/set_filter/status on the node and deliver NET_RCV_MSG to "
   "registered receive ports; requires the bootstrap port of a "
   "translator startup"},
  {"name", OPT_NAME, "NAME", 0,
   "With --serve: only accept device_open for this name (default: accept "
   "any, including the \"eth\" name pfinet opens with)"},
  {"event-idx", OPT_EVENT_IDX, 0, 0,
   "Negotiate split-ring EVENT_IDX. The RX loop polls in budgeted passes "
   "with race-free interrupt rearming, and a --sync writer arms the TX "
   "queue before it waits. On by default"},
  {"no-event-idx", OPT_NO_EVENT_IDX, 0, 0,
   "Do not negotiate EVENT_IDX. Every completion on both queues "
   "interrupts, the legacy path"},
  {"sync", 'S', 0, 0,
   "Wait for each transmitted frame's backend completion (synchronous "
   "TX); default is netdde-style fire-and-forget"},
  {"verbose", 'v', 0, 0,
   "Log deliveries, completions, and port activity"},
  {0}
};

static error_t
parse_opt (int key, char *arg, struct argp_state *state)
{
  switch (key)
    {
    case 'd':
      opt_device = arg;
      break;
    case 's':
      opt_selftest = 1;
      break;
    case 'v':
      vnet_verbose = 1;
      break;
    case OPT_STATUS:
      opt_status = 1;
      break;
    case OPT_TRAFFIC:
      opt_traffic = 1;
      break;
    case OPT_SERVE:
      opt_serve = 1;
      break;
    case OPT_NAME:
      opt_name = arg;
      break;
    case OPT_EVENT_IDX:
      vnet_event_idx = 1;
      break;
    case OPT_NO_EVENT_IDX:
      vnet_event_idx = 0;
      break;
    case 'S':
      vnet_tx_sync = 1;
      break;
    default:
      return ARGP_ERR_UNKNOWN;
    }
  (void) state;
  return 0;
}

static const struct argp argp =
{
  options, parse_opt,
  "[--device=SLOT] [--selftest [--selftest-traffic]] [--status] "
  "[--serve [--name=NAME]] [--sync] [--event-idx] [--no-event-idx] [--verbose]",
  "Virtio network driver: raw Ethernet I/O on QEMU's virtio-net PCI "
  "device (0x1af4:0x1000), with the Mach net-device semantics layer and "
  "a Mach device translator mode for Hurd network stacks"
};

static struct vnet_dev vnet;

/* --status: probe and read the config region only.  The probe enables
   the PCI device but never touches the virtio status register, so no
   reset, no handshake, no disturbance of a running driver.  */
error_t
vnet_status (const char *override)
{
  struct virtio_pci_dev v;
  uint32_t offered, cfg0, cfg4;
  error_t err;

  v.name = "virtio-net";
  v.device_id = VNET_DEVICE_ID;
  err = virtio_pci_probe (&v, override);
  if (err)
    return err;

  offered = virtio_pci_read_host_features (&v);
  cfg0 = virtio_pci_read_config (&v, VNET_CFG_MAC);
  cfg4 = virtio_pci_read_config (&v, VNET_CFG_DWORD4);
  printf ("MAC %02x:%02x:%02x:%02x:%02x:%02x, offered features 0x%x%s%s%s\n",
          cfg0 & 0xff, (cfg0 >> 8) & 0xff, (cfg0 >> 16) & 0xff,
          cfg0 >> 24, cfg4 & 0xff, (cfg4 >> 8) & 0xff,
          offered,
          offered & (1u << VNET_F_MAC) ? ", MAC" : "",
          offered & (1u << VNET_F_STATUS)
          ? ((cfg4 >> 16) & 1 ? ", link up" : ", link down") : "",
          offered & (1u << VNET_F_MTU)
          ? ", MTU advice" : "");
  if (offered & (1u << VNET_F_MTU))
    printf ("MTU advice: %u\n",
            (virtio_pci_read_config (&v, VNET_CFG_DWORD8) >> 16) & 0xffff);
  return 0;
}

/* pfinet's ether filter (pfinet/ethernet.c): accepts ARP, IPv4, IPv6.
   The selftest installs it verbatim, so the filter evaluation the stack
   will exercise at runtime is the program validated here.  */
static struct bpf_insn selftest_bpf_filter[] =
{
  {NETF_IN|NETF_BPF, 0, 0, 0},		/* Header. */
  {BPF_LD|BPF_H|BPF_ABS, 0, 0, 12},	/* Load Ethernet type */
  {BPF_JMP|BPF_JEQ|BPF_K, 2, 0, 0x0806},	/* Accept ARP */
  {BPF_JMP|BPF_JEQ|BPF_K, 1, 0, 0x0800},	/* Accept IPv4 */
  {BPF_JMP|BPF_JEQ|BPF_K, 0, 1, 0x86DD},	/* Accept IPv6 */
  {BPF_RET|BPF_K, 0, 0, 1500},		/* And return 1500 bytes */
  {BPF_RET|BPF_K, 0, 0, 0},		/* Or discard it all */
};
#define SELFTEST_FILTER_LEN \
  (sizeof (selftest_bpf_filter) / (sizeof (filter_t)))

/* Build the 42-byte ARP request the selftest sends: who-has 10.0.2.2
   (the slirp gateway), broadcast, source = our MAC.  */
static void
selftest_arp_frame (struct vnet_dev *d, uint8_t *f)
{
  static const uint8_t bcast[6] = {0xff,0xff,0xff,0xff,0xff,0xff};
  static const uint8_t tpa[4] = {10,0,2,2};

  memset (f, 0, 42);
  memcpy (f, bcast, 6);
  memcpy (f + 6, d->mac, 6);
  f[12] = 0x08;          /* Ethertype ARP */
  f[13] = 0x06;
  f[14] = 0x00; f[15] = 0x01;   /* hardware type Ethernet */
  f[16] = 0x08; f[17] = 0x00;   /* protocol type IPv4 */
  f[18] = 6;                    /* hardware size */
  f[19] = 4;                    /* protocol size */
  f[20] = 0x00; f[21] = 0x01;   /* operation: request */
  memcpy (f + 22, d->mac, 6);   /* sender hardware address */
  f[28] = 10; f[29] = 0; f[30] = 2; f[31] = 15; /* sender proto addr */
  memcpy (f + 32, bcast, 6);    /* target hardware address (unknown) */
  f[38] = tpa[0]; f[39] = tpa[1]; f[40] = tpa[2]; f[41] = tpa[3];
}

/* In-process NET_RCV_MSG proof: install pfinet's filter against a private
   receive port, push a synthetic ARP frame through the delivery path,
   and receive the message back, validating everything a stack demuxer
   checks: msgh_id, local-port matching, and the header/payload/count
   layout of the message path.  */
static error_t
selftest_rcv_msg (struct vnet_hurd *h)
{
  struct vnet_dev *d = h->dev;
  struct net_rcv_msg msg;
  mach_port_t rcv = MACH_PORT_NULL;
  uint8_t frame[42];
  uint8_t got[64];
  mach_msg_return_t mr;
  size_t datalen;
  error_t err;

  err = mach_port_allocate (mach_task_self (), MACH_PORT_RIGHT_RECEIVE,
                            &rcv);
  if (err)
    return err;
  err = mach_port_insert_right (mach_task_self (), rcv, rcv,
                                MACH_MSG_TYPE_MAKE_SEND);
  if (err)
    goto out;

  pthread_mutex_lock (&h->lock);
  err = net_set_filter (&h->filters, rcv, 0,
                        (filter_t *) selftest_bpf_filter,
                        SELFTEST_FILTER_LEN);
  pthread_mutex_unlock (&h->lock);
  if (err)
    {
      fprintf (stderr, "virtio-net selftest: set_filter rejected pfinet's "
               "BPF program: %d\n", err);
      err = EIO;
      goto out;
    }

  /* Every failed check below leaves through `out'.  The result stays an
     error until the last of them has passed.  */
  err = EIO;

  selftest_arp_frame (d, frame);
  vnet_hurd_rx_all (d, (const char *) frame, sizeof frame);

  memset (&msg, 0, sizeof msg);
  mr = mach_msg (&msg.msg_hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                 sizeof msg, rcv, 1000, MACH_PORT_NULL);
  if (mr != MACH_MSG_SUCCESS)
    {
      fprintf (stderr, "virtio-net selftest: NET_RCV_MSG not received "
               "(0x%x)\n", mr);
      goto out;
    }
  if (msg.msg_hdr.msgh_id != NET_RCV_MSG_ID
      || msg.msg_hdr.msgh_local_port != rcv)
    {
      fprintf (stderr, "virtio-net selftest: received message id %d local "
               "port %d does not match\n", msg.msg_hdr.msgh_id,
               msg.msg_hdr.msgh_local_port);
      goto out;
    }
  /* The consumer math: datalen = ETH_HLEN + packet count - packet_header
     (pfinet/ethernet.c).  */
  datalen = VNET_ETHER_HDR_SIZE + msg.net_rcv_msg_packet_count
            - sizeof (struct packet_header);
  if (datalen != sizeof frame)
    {
      fprintf (stderr, "virtio-net selftest: delivered datalen %zu, "
               "expected %zu\n", datalen, sizeof frame);
      goto out;
    }
  if (memcmp (msg.header, frame, VNET_ETHER_HDR_SIZE) != 0)
    {
      fprintf (stderr, "virtio-net selftest: delivered Ethernet header "
               "mismatch\n");
      goto out;
    }
  memset (got, 0, sizeof got);
  memcpy (got, msg.packet + sizeof (struct packet_header),
          datalen - VNET_ETHER_HDR_SIZE);
  if (memcmp (got, frame + VNET_ETHER_HDR_SIZE,
              datalen - VNET_ETHER_HDR_SIZE) != 0)
    {
      fprintf (stderr, "virtio-net selftest: delivered payload mismatch\n");
      goto out;
    }

  /* Negative: an unfiltered ethertype must not reach the port.  */
  frame[12] = 0x12;
  frame[13] = 0x34;
  vnet_hurd_rx_all (d, (const char *) frame, sizeof frame);
  mr = mach_msg (&msg.msg_hdr, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
                 sizeof msg, rcv, 200, MACH_PORT_NULL);
  if (mr == MACH_MSG_SUCCESS)
    {
      fprintf (stderr, "virtio-net selftest: filtered-out frame was "
               "delivered\n");
      goto out;
    }

  err = 0;

 out:
  /* Drop our rights and remove any installed filter (dead_name walks
     the filter list and deallocates the send right net_set_filter
     stored), so a failed selftest leaves neither a live filter on a
     port nobody reads nor a leaked port.  */
  mach_port_destroy (mach_task_self (), rcv);
  vnet_hurd_dead_name (h, rcv);
  return err;
}

/* Environment-dependent raw datapath proof: send an ARP
   request and let the RX thread watch for any backend reply.  With
   slirp, the gateway answers ARP for 10.0.2.2; with other backends the
   result depends on the environment, so a timeout is reported, not
   failed.  */
static error_t
selftest_traffic (struct vnet_dev *d)
{
  uint8_t frame[42];
  pthread_t thread;
  int created = 0, i;
  error_t err;

  selftest_arp_frame (d, frame);
  __atomic_store_n (&d->stopping, 0, __ATOMIC_SEQ_CST);
  err = pthread_create (&thread, NULL,
                        (void *(*) (void *)) vnet_buf_rx_loop, d);
  if (err)
    return err;
  created = 1;

  err = vnet_dev_tx (d, frame, sizeof frame);
  if (err)
    {
      fprintf (stderr, "virtio-net selftest: ARP TX failed: %s\n",
               strerror (err));
      goto out;
    }
  printf ("ARP request sent; waiting %ds for backend traffic\n",
          VNET_SELFTEST_RX_WAIT);
  for (i = 0; i < VNET_SELFTEST_RX_WAIT * 10; i++)
    {
      /* Atomic: the RX thread updates these counters concurrently.  */
      uint64_t pkts = __atomic_load_n (&d->stats.rx_packets,
                                       __ATOMIC_RELAXED);
      uint64_t bytes = __atomic_load_n (&d->stats.rx_bytes,
                                        __ATOMIC_RELAXED);

      usleep (100 * 1000);
      if (pkts)
        {
          printf ("raw RX observed: %llu packet(s), %llu bytes\n",
                  (unsigned long long) pkts, (unsigned long long) bytes);
          break;
        }
    }
  if (!__atomic_load_n (&d->stats.rx_packets, __ATOMIC_RELAXED))
    printf ("no backend traffic observed (backend-dependent; not a "
            "selftest failure)\n");

 out:
  __atomic_store_n (&d->stopping, 1, __ATOMIC_SEQ_CST);
  if (created)
    pthread_join (thread, NULL);
  return err;
}

/* Phase 1/2 gate (plan T2/T5): device found, features negotiated
   exactly, queues programmed, config readable; the Mach semantics layer
   consistent (status flavors, MAC, write policy, filter/refusal
   behavior); and the NET_RCV_MSG construction/delivery path proven
   in-process.  */
error_t
vnet_selftest (int traffic)
{
  struct vnet_hurd h;
  dev_status_data_t status;
  mach_msg_type_number_t count;
  uint8_t arp[42];
  int written = 0, flags_out = 0;
  error_t err;

  /* Handshake and feature policy: the readback must equal exactly what
     was accepted, the same check the balloon/blk selftests make.  */
  if (virtio_pci_read_guest_features (&vnet.pci) != vnet.accepted)
    {
      fprintf (stderr, "virtio-net selftest: guest_features 0x%x does not "
               "match accepted 0x%x\n",
               virtio_pci_read_guest_features (&vnet.pci), vnet.accepted);
      return EIO;
    }

  /* The semantics layer: status flavors, MAC layout, and write policy.  */
  err = vnet_hurd_init (&h, &vnet);
  if (err)
    return err;
  err = vnet_hurd_open (&h, D_READ | D_WRITE);
  if (err)
    {
      fprintf (stderr, "virtio-net selftest: open: err %d\n", err);
      return err;
    }
  /* The delivery path routes frames to registered handles only.  */
  vnet_hurd_register (&h);

  memset (status, 0, sizeof status);
  count = DEV_STATUS_MAX;
  err = vnet_hurd_get_status (&h, NET_STATUS, status, &count);
  if (err || count != NET_STATUS_COUNT
      || status[2] != HDR_ETHERNET
      || status[3] != VNET_ETHER_HDR_SIZE
      || status[4] != 6
      || status[1] != (int) vnet.max_frame)
    {
      fprintf (stderr, "virtio-net selftest: NET_STATUS: err %s count %u "
               "[min %d max %d fmt %d hdr %d addr %d]\n",
               strerror (err), count, status[0], status[1], status[2],
               status[3], status[4]);
      return EIO;
    }

  memset (status, 0, sizeof status);
  count = DEV_STATUS_MAX;
  err = vnet_hurd_get_status (&h, NET_ADDRESS, status, &count);
  if (err || count != 2)
    {
      fprintf (stderr, "virtio-net selftest: NET_ADDRESS: err %s count %u\n",
               strerror (err), count);
      return EIO;
    }
  status[0] = ntohl (status[0]);
  status[1] = ntohl (status[1]);
  if (memcmp (status, vnet.mac, 6) != 0)
    {
      fprintf (stderr, "virtio-net selftest: NET_ADDRESS does not match "
               "the configured MAC\n");
      return EIO;
    }

  count = 1;
  err = vnet_hurd_get_status (&h, NET_FLAGS, status, &count);
  if (err || count != 1)
    {
      fprintf (stderr, "virtio-net selftest: NET_FLAGS: err %s\n",
               strerror (err));
      return EIO;
    }
  {
    int saved = status[0];

    flags_out = 0x5a5a;
    err = vnet_hurd_set_status (&h, NET_FLAGS, &flags_out, 1);
    if (err)
      {
        fprintf (stderr, "virtio-net selftest: set_status NET_FLAGS: "
                 "err %s\n", strerror (err));
        return err;
      }
    count = 1;
    vnet_hurd_get_status (&h, NET_FLAGS, status, &count);
    if (count != 1 || status[0] != flags_out)
      {
        fprintf (stderr, "virtio-net selftest: NET_FLAGS round trip "
                 "mismatch\n");
        return EIO;
      }
    flags_out = saved;
    vnet_hurd_set_status (&h, NET_FLAGS, &flags_out, 1);
  }

  /* Refusal policy: unknown flavors, inband, map, write-mode and range
     checks all answer the compatible D_* error (plan R16).  */
  count = DEV_STATUS_MAX;
  if (vnet_hurd_get_status (&h, 0x6e0003 /* NET_DSTADDR */, status, &count)
      != D_INVALID_OPERATION)
    {
      fprintf (stderr, "virtio-net selftest: unknown get_status flavor "
               "accepted\n");
      return EIO;
    }
  if (vnet_hurd_set_status (&h, NET_STATUS, status, NET_STATUS_COUNT)
      != D_INVALID_OPERATION)
    {
      fprintf (stderr, "virtio-net selftest: NET_STATUS set accepted\n");
      return EIO;
    }
  if (vnet_hurd_read_inband (&h, 0, 0, 0, NULL) != D_INVALID_OPERATION
      || vnet_hurd_write_inband (&h, 0, 0, NULL, 0, &written)
      != D_INVALID_OPERATION)
    {
      fprintf (stderr, "virtio-net selftest: inband I/O not refused\n");
      return EIO;
    }
  {
    memory_object_t pager;

    if (vnet_hurd_map (&h, 0, 0, 0, &pager, 0) != D_INVALID_OPERATION)
      {
        fprintf (stderr, "virtio-net selftest: device_map not refused\n");
        return EIO;
      }
  }
  if (vnet_hurd_write (&h, 0, 1, arp, sizeof arp, &written)
      != D_INVALID_RECNUM)
    {
      fprintf (stderr, "virtio-net selftest: nonzero recnum accepted\n");
      return EIO;
    }
  {
    uint8_t oversized[VNET_FRAME_CAP + 1];

    if (vnet_hurd_write (&h, 0, 0, oversized, sizeof oversized, &written)
        != D_INVALID_SIZE)
      {
        fprintf (stderr, "virtio-net selftest: oversized write accepted\n");
        return EIO;
      }
  }
  if (vnet_hurd_write (&h, 0, 0, arp, 8, &written) != D_INVALID_SIZE)
    {
      fprintf (stderr, "virtio-net selftest: sub-header write accepted\n");
      return EIO;
    }
  if (vnet_hurd_write (&h, D_NOWAIT | 0x100, 0, arp, sizeof arp, &written)
      != D_INVALID_OPERATION)
    {
      fprintf (stderr, "virtio-net selftest: bad write mode accepted\n");
      return EIO;
    }

  /* NET_RCV_MSG construction, filter evaluation, and delivery: the
     in-process stack-demuxer proof.  */
  err = selftest_rcv_msg (&h);
  if (err)
    return err;

  /* Raw TX of one real frame through the datapath (the ARP request the
     traffic test reuses), so the TX engine is exercised even without
     --selftest-traffic.  */
  selftest_arp_frame (&vnet, arp);
  err = vnet_dev_tx (&vnet, arp, sizeof arp);
  if (err)
    {
      fprintf (stderr, "virtio-net selftest: TX of the ARP request "
               "failed: %s\n", strerror (err));
      return err;
    }

  if (vnet_hurd_close (&h) != 0)
    {
      fprintf (stderr, "virtio-net selftest: close failed\n");
      return EIO;
    }
  if (vnet_hurd_write (&h, 0, 0, arp, sizeof arp, &written)
      != D_INVALID_OPERATION)
    {
      fprintf (stderr, "virtio-net selftest: I/O after close not "
               "refused\n");
      return EIO;
    }

  if (traffic)
    {
      err = selftest_traffic (&vnet);
      if (err)
        return err;
    }

  /* Pair the registration at the top of the test; the error paths above
     all end in main's error (1, ...) exit, so nothing can observe the
     list afterwards, but keep the success path paired.  */
  vnet_hurd_unregister (&h);

  printf ("offered features 0x%x, accepted 0x%x\n", vnet.offered,
          vnet.accepted);
  printf ("MAC %02x:%02x:%02x:%02x:%02x:%02x, mtu %u, max frame %u, "
          "link %s\n",
          vnet.mac[0], vnet.mac[1], vnet.mac[2], vnet.mac[3], vnet.mac[4],
          vnet.mac[5], vnet.mtu, vnet.max_frame,
          vnet_dev_link_up (&vnet) ? "up" : "down");
  printf ("Mach semantics: NET_STATUS/ADDRESS/FLAGS consistent, "
          "open/mode/range policy enforced\n");
  printf ("NET_RCV_MSG round trip: pfinet's filter installed, ARP frame "
          "delivered, filtered-out frame withheld\n");
  printf ("raw TX of one %zu-byte ARP frame accepted\n", sizeof arp);
  {
    const struct vnet_stats *s = vnet_dev_stats (&vnet);

    printf ("counters: rx %llu (%llu bytes, %llu bad, %llu delivered, "
            "%llu delivery failures), tx %llu (%llu bytes)\n",
            (unsigned long long) s->rx_packets,
            (unsigned long long) s->rx_bytes,
            (unsigned long long) s->rx_bad,
            (unsigned long long) s->rx_delivered,
            (unsigned long long) s->rx_delivery_fail,
            (unsigned long long) s->tx_packets,
            (unsigned long long) s->tx_bytes);
  }
  return 0;
}

static volatile sig_atomic_t quit_flag;

static void
quit_handler (int sig)
{
  quit_flag = 1;
}

error_t
vnet_start_device (const char *override)
{
  return vnet_dev_init (&vnet, override);
}

void
vnet_stop_device (void)
{
  vnet_dev_quiesce (&vnet);
}

/* Daemon mode: own the device, service the receive queue, log config
   changes and counters, and leave cleanly on SIGINT/SIGTERM.  */
int
main (int argc, char **argv)
{
  error_t err;
  struct sigaction sa;
  uint32_t seq = 0;

  argp_parse (&argp, argc, argv, 0, 0, 0);

  if (opt_traffic && !opt_selftest)
    error (1, 0, "--selftest-traffic only applies to --selftest");

  if (opt_name && !opt_serve)
    error (1, 0, "--name only applies to --serve");

  if (opt_status)
    {
      err = vnet_status (opt_device);
      if (err)
        error (1, err, "virtio-net device not usable");
      return 0;
    }

  if (opt_selftest)
    {
      err = vnet_start_device (opt_device);
      if (err)
        error (1, err, "device bring-up failed");
      err = vnet_selftest (opt_traffic);
      if (err)
        error (1, err, "selftest failed");
      printf ("selftest ok\n");
      vnet_stop_device ();
      return 0;
    }

  if (opt_serve)
    {
      /* Become the Mach device translator.  Bring-up failure is fatal
         before trivfs_startup replies to the parent, so settrans
         reports it instead of hanging on a dead node.  */
      err = vnet_start_device (opt_device);
      if (err)
        error (4, err, "virtio-net device not usable");
      /* The receive thread must run or rxq completions are never
         serviced; serve does not return, so the thread is left live and
         joined by vnet_dev_quiesce at teardown (trivfs_goaway).  */
      err = pthread_create (&vnet.rx_thread, NULL,
                            (void *(*) (void *)) vnet_buf_rx_loop, &vnet);
      if (err)
        error (4, err, "cannot start the receive thread");
      __atomic_store_n (&vnet.rx_created, 1, __ATOMIC_SEQ_CST);
      vnet_serve (&vnet, opt_name);
    }

  sa.sa_handler = quit_handler;
  sa.sa_flags = 0;
  sigemptyset (&sa.sa_mask);
  sigaction (SIGINT, &sa, NULL);
  sigaction (SIGTERM, &sa, NULL);

  err = vnet_start_device (opt_device);
  if (err)
    /* No device, or one that will not talk: die visibly instead of
       leaving a daemon behind that looks alive.  */
    error (4, err, "virtio-net device not usable");

  err = pthread_create (&vnet.rx_thread, NULL,
                        (void *(*) (void *)) vnet_buf_rx_loop, &vnet);
  if (err)
    error (1, err, "cannot start the receive thread");
  __atomic_store_n (&vnet.rx_created, 1, __ATOMIC_SEQ_CST);

  for (; !quit_flag; )
    {
      err = virtio_irq_wait_config (&vnet.irq, 1, &seq);
      if (err && err != ETIMEDOUT)
        error (1, err, "waiting for config change");
      if (!err)
        vnet_dev_reread_config (&vnet);
    }

  /* Teardown owns the RX-thread join (a second join of an already
     joined thread is undefined behavior), so no explicit join here.  */
  vnet_stop_device ();

  {
    const struct vnet_stats *s = vnet_dev_stats (&vnet);

    fprintf (stderr,
             "virtio-net: rx %llu packets (%llu bytes, %llu bad, %llu "
             "delivered, %llu delivery failures), tx %llu packets "
             "(%llu bytes, %llu errors), bad completions %llu, config "
             "changes %llu\n",
             (unsigned long long) s->rx_packets,
             (unsigned long long) s->rx_bytes,
             (unsigned long long) s->rx_bad,
             (unsigned long long) s->rx_delivered,
             (unsigned long long) s->rx_delivery_fail,
             (unsigned long long) s->tx_packets,
             (unsigned long long) s->tx_bytes,
             (unsigned long long) s->tx_errors,
             (unsigned long long) s->bad_completions,
             (unsigned long long) s->config_changes);
    if (s->rx_poll_passes || s->rx_idle_waits || s->rx_wait_timeouts
        || s->rx_rearm_races)
      fprintf (stderr,
               "virtio-net: RX moderation: %llu budget-exhausted passes, "
               "%llu armed waits (%llu timed out), %llu rearm races\n",
               (unsigned long long) s->rx_poll_passes,
               (unsigned long long) s->rx_idle_waits,
               (unsigned long long) s->rx_wait_timeouts,
               (unsigned long long) s->rx_rearm_races);
  }
  return 0;
}
