# virtio: virtio-9p translator, virtio-balloon and virtio-blk drivers for
# GNU/Hurd.
#
# Built against the x86_64-gnu cross prefix by the hurdkit toolchain
# (`make -C toolchain virtio-9p` passes at least CC, CPATH and LIBS); for
# a manual host-local build set them the same way, plus HURD_PREFIX and
# HURD_SRC (below) when their defaults don't fit.

CC ?= cc
CFLAGS ?= -O2 -g -Wall

all: virtio-9p virtio-balloon virtio-blk vblk-client virtio-net vnet-client

# The transport core is shared by all drivers; the 9p translator adds the
# trivfs/ports stack, the daemons link lean.
TRANSPORT = virtio-pci.o virtio-queue.o
LIBS = shouldbeinlibc ihash ports iohelp fshelp trivfs pciaccess \
       hurduser machuser irqhelp
LIBS_DAEMON = shouldbeinlibc pciaccess machuser irqhelp
# virtio-blk links the trivfs/ports stack too since Stage C: the binary
# doubles as the Mach device translator (`--serve`).
LIBS_BLK = shouldbeinlibc ihash ports iohelp fshelp trivfs pciaccess \
           hurduser machuser irqhelp
# vblk-client is a plain RPC client: file_name_lookup (hurduser),
# device_* (machuser), vm_deallocate (mach).
LIBS_CLIENT = hurduser machuser

OBJS9P = virtio-9p.o virtio-9p-req.o virtio-9p-ios.o $(TRANSPORT)
OBJSBAL = virtio-balloon.o virtio-balloon-dev.o virtio-balloon-mem.o \
          $(TRANSPORT)
# The shared slot-pool engine (virtio-pool.c) and the shared translator
# scaffold (virtio-serve.c) are linked by blk and net.
OBJSBLK = virtio-blk.o virtio-blk-dev.o virtio-blk-req.o virtio-blk-hurd.o \
          virtio-blk-serve.o deviceServer.o notifyServer.o virtio-pool.o \
          virtio-serve.o $(TRANSPORT)
# virtio-net links the trivfs/ports stack too: the binary doubles as the
# Mach device translator (`--serve`), and it compiles in the pinned Hurd
# tree's libbpf (bpf_impl.c + queue.c) for real filter evaluation, the
# same substrate eth-multiplexer uses.
OBJSNET = virtio-net.o virtio-net-dev.o virtio-net-buf.o virtio-net-hurd.o \
          virtio-net-serve.o vnetDeviceServer.o vnetNotifyServer.o \
          bpf_impl.o queue.o virtio-pool.o virtio-serve.o $(TRANSPORT)

# MIG server stubs, generated the way the Hurd tree's Makeconf does it
# (cpp pass with -DSERVERPREFIX=S_, then `mig -ccat -subrprefix __`).
# The device stubs come from the cross prefix's gnumach headers, so the
# wire contract is exactly what the guest kernel's device.defs says.
HURD_PREFIX ?= $(HOME)/opt/x86_64-gnu
# The pinned Hurd source tree, for the libports MIG headers and libbpf.
# The default assumes this repo is cloned next to hurdkit, whose
# `make fetch-3p` checks the tree out into hurdkit/3p/hurd.
HURD_SRC ?= $(abspath ../hurdkit/3p/hurd)
MIG ?= mig
# The defs preprocessing pass runs the host cpp directly: the cross
# compiler would treat the unknown `.defs' extension as a linker input
# ("linker input file unused because linking not done") instead of
# preprocessing it, and make's builtin CPP is $(CC) -E, so this uses a
# dedicated variable (Hurd's Makeconf rules use $(CPP) for the same
# reason).
MIG_CPP ?= cpp
MIGCPPFLAGS = -I$(HURD_PREFIX)/include -imacros mig-mutate.h \
              -DMACH_PAYLOAD_TO_PORT=ports_payload_get_name
MIGCOMFLAGS =
# mig re-runs its internal cpp over the input, so unsupported switches
# (like the older `-ccat -subrprefix __' pair the Hurd tree passes its
# MIGCOM) must not be given to this one: unknown switches are forwarded
# to the preprocessor.

virtio-9p: $(OBJS9P)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS),-l$(LIB))

virtio-balloon: $(OBJSBAL)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS_DAEMON),-l$(LIB))

virtio-blk: $(OBJSBLK)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS_BLK),-l$(LIB))

virtio-net: $(OBJSNET)
	$(CC) $(CFLAGS) $^ -o $@ -pthread $(foreach LIB,$(LIBS_BLK),-l$(LIB))

vblk-client: vblk-client.o
	$(CC) $(CFLAGS) $^ -o $@ $(foreach LIB,$(LIBS_CLIENT),-l$(LIB))

# vnet-client is a plain RPC client: file_name_lookup (hurduser),
# device_* (machuser), vm_deallocate (mach).
LIBS_VNET_CLIENT = hurduser machuser

vnet-client: vnet-client.o
	$(CC) $(CFLAGS) $^ -o $@ $(foreach LIB,$(LIBS_VNET_CLIENT),-l$(LIB))

# device.defs (subsystem 2800) carries device_open_new only under
# DEVICE_ENABLE_DEVICE_OPEN_NEW; with it, ds_device_open_new is
# generated and aliased in virtio-blk-serve.c, matching the
# gnumach/devnode/libmachdev precedent.
DEFS_CPPFLAGS = $(MIGCPPFLAGS) -DDEVICE_ENABLE_DEVICE_OPEN_NEW \
                -DHURD_SERVER=1 -DSERVERPREFIX=S_

device.sdefsi: $(HURD_PREFIX)/include/device/device.defs mig-mutate.h
	$(MIG_CPP) $(DEFS_CPPFLAGS) -MD -MF $@.d $< -o $@

deviceServer.c device_S.h: device.sdefsi
	$(MIG) $(MIGCOMFLAGS) -sheader device_S.h -server deviceServer.c \
	    -user /dev/null -header /dev/null $<

notify.sdefsi: $(HURD_PREFIX)/include/mach/notify.defs
	$(MIG_CPP) $(MIGCPPFLAGS) -DHURD_SERVER=1 -DSERVERPREFIX=S_ -MD -MF $@.d \
	    $< -o $@

notifyServer.c notify_S.h: notify.sdefsi
	$(MIG) $(MIGCOMFLAGS) -sheader notify_S.h -server notifyServer.c \
	    -user /dev/null -header /dev/null $<

# The net server needs its own mutation file (different per-binary
# imports than blk's), so its defs pass mirrors the blk one with
# -imacros vnet-mutate.h and vnet-prefixed outputs.
MIGCPPFLAGS_NET = -I$(HURD_PREFIX)/include -imacros vnet-mutate.h \
                  -DMACH_PAYLOAD_TO_PORT=ports_payload_get_name
DEFS_CPPFLAGS_NET = $(MIGCPPFLAGS_NET) -DDEVICE_ENABLE_DEVICE_OPEN_NEW \
                    -DHURD_SERVER=1 -DSERVERPREFIX=S_

vnet-device.sdefsi: $(HURD_PREFIX)/include/device/device.defs vnet-mutate.h
	$(MIG_CPP) $(DEFS_CPPFLAGS_NET) -MD -MF $@.d $< -o $@

vnetDeviceServer.c vnet-device_S.h: vnet-device.sdefsi
	$(MIG) $(MIGCOMFLAGS) -sheader vnet-device_S.h \
	    -server vnetDeviceServer.c -user /dev/null -header /dev/null $<

vnet-notify.sdefsi: $(HURD_PREFIX)/include/mach/notify.defs
	$(MIG_CPP) $(DEFS_CPPFLAGS_NET) -MD -MF $@.d $< -o $@

vnetNotifyServer.c vnet-notify_S.h: vnet-notify.sdefsi
	$(MIG) $(MIGCOMFLAGS) -sheader vnet-notify_S.h \
	    -server vnetNotifyServer.c -user /dev/null -header /dev/null $<

%.o: %.c virtio-pci.h virtio-queue.h virtio-9p.h virtio-balloon.h \
     virtio-blk.h virtio-blk-serve.h virtio-net.h
	$(CC) $(CFLAGS) -D_GNU_SOURCE -c $< -o $@

# The MIG stubs include "vnet-device_S.h"/"vnet-notify_S.h" (generated
# here) and libports/ports.h + libports/mig-decls.h (not installed in
# the prefix; they live in the pinned Hurd source tree).  The _S.h
# headers carry the same imports, so virtio-net-serve.o needs the paths
# too.  All net objects include <bpf_impl.h> through virtio-net.h, from
# the pinned tree's libbpf directory.
vnetDeviceServer.o vnetNotifyServer.o virtio-net-serve.o: \
    CFLAGS += -I. -I$(HURD_SRC)
virtio-net.o virtio-net-dev.o virtio-net-buf.o virtio-net-hurd.o \
virtio-net-serve.o vnetDeviceServer.o vnetNotifyServer.o: \
    CFLAGS += -I$(HURD_SRC)/libbpf
vnetDeviceServer.o vnetNotifyServer.o virtio-net-serve.o: \
    virtio-net-serve.h
virtio-net-serve.o: vnet-device_S.h vnet-notify_S.h

# Every object that reads the Hurd source tree checks for it first, so
# a wrong HURD_SRC fails with a message that names the variable instead
# of a missing-header error from a generated MIG stub.
HURD_SRC_OBJS = deviceServer.o notifyServer.o virtio-blk-serve.o \
    virtio-serve.o vnetDeviceServer.o vnetNotifyServer.o virtio-net.o \
    virtio-net-dev.o virtio-net-buf.o virtio-net-hurd.o virtio-net-serve.o \
    bpf_impl.o queue.o
$(HURD_SRC_OBJS): | check-hurd-src
check-hurd-src:
	@test -f '$(HURD_SRC)/libports/mig-decls.h' || { \
	    echo "virtio: HURD_SRC=$(HURD_SRC) is not a Hurd source tree" \
	        "(no libports/mig-decls.h)." >&2; \
	    echo "virtio: set HURD_SRC to the pinned tree, e.g." \
	        "make HURD_SRC=/path/to/hurdkit/3p/hurd" >&2; \
	    exit 1; }

# libbpf compiled from the pinned Hurd tree into local objects, so the
# shared 3p checkout is not polluted with build products.
bpf_impl.o: $(HURD_SRC)/libbpf/bpf_impl.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I$(HURD_SRC)/libbpf -c $< -o $@

queue.o: $(HURD_SRC)/libbpf/queue.c
	$(CC) $(CFLAGS) -D_GNU_SOURCE -I$(HURD_SRC)/libbpf -c $< -o $@

# The MIG stubs include "device_S.h"/"notify_S.h" (generated here) and
# libports/ports.h + libports/mig-decls.h (not installed in the prefix;
# they live in the pinned Hurd source tree).  The _S.h headers carry the
# same imports, so virtio-blk-serve.o needs the paths too.
deviceServer.o notifyServer.o virtio-blk-serve.o: virtio-blk-serve.h
deviceServer.o notifyServer.o virtio-blk-serve.o virtio-serve.o: \
    CFLAGS += -I. -I$(HURD_SRC)
virtio-blk-serve.o: device_S.h notify_S.h
virtio-serve.o: virtio-serve.h

clean:
	rm -f $(OBJS9P) $(OBJSBAL) $(OBJSBLK) $(OBJSNET) virtio-pool.o virtio-9p \
	    virtio-balloon virtio-blk vblk-client vblk-client.o virtio-net \
	    vnet-client vnet-client.o \
	    device.sdefsi* deviceServer.c device_S.h notify.sdefsi* \
	    notifyServer.c notify_S.h vnet-device.sdefsi* vnetDeviceServer.c \
	    vnet-device_S.h vnet-notify.sdefsi* vnetNotifyServer.c \
	    vnet-notify_S.h

.PHONY: clean all check-hurd-src
