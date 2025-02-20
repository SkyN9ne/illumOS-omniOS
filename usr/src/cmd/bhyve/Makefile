#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2014 Pluribus Networks Inc.
# Copyright 2020 Joyent, Inc.
# Copyright 2020 Oxide Computer Company
# Copyright 2022 OmniOS Community Edition (OmniOSce) Association.
#

PROG =		bhyve

include ../Makefile.cmd
include ../Makefile.cmd.64
include ../Makefile.ctf

SUBDIRS = test

all	:=	TARGET = all
install	:=	TARGET = install
clean	:=	TARGET = clean
clobber	:=	TARGET = clobber

SRCS =	acpi.c			\
	atkbdc.c		\
	bhyvegc.c		\
	bhyverun.c		\
	block_if.c		\
	bootrom.c		\
	console.c		\
	config.c		\
	fwctl.c			\
	gdb.c			\
	inout.c			\
	ioapic.c		\
	mem.c			\
	mevent.c		\
	mptbl.c			\
	net_backends.c		\
	net_utils.c		\
	pci_ahci.c		\
	pci_e82545.c		\
	pci_emul.c		\
	pci_fbuf.c		\
	pci_hostbridge.c	\
	pci_irq.c		\
	pci_lpc.c		\
	pci_nvme.c		\
	pci_passthru.c		\
	pci_uart.c		\
	pci_virtio_9p.c		\
	pci_virtio_block.c	\
	pci_virtio_console.c	\
	pci_virtio_net.c	\
	pci_virtio_rnd.c	\
	pci_virtio_viona.c	\
	pci_xhci.c		\
	pctestdev.c		\
	pm.c			\
	post.c			\
	privileges.c		\
	ps2kbd.c		\
	ps2mouse.c		\
	rfb.c			\
	rtc.c			\
	smbiostbl.c		\
	sockstream.c		\
	task_switch.c		\
	uart_emul.c		\
	usb_emul.c		\
	usb_mouse.c		\
	vga.c			\
	virtio.c		\
	vmgenc.c		\
	xmsr.c			\
	spinup_ap.c		\
	iov.c			\
	bhyve_sol_glue.c

# We are not yet performing instruction emulation in userspace, so going to the
# trouble of fixing the header tangle for this is not worth the complexity.
	#kernemu_dev.c		\

# The virtio-scsi driver appears to include a slew of materials from FreeBSD's
# native SCSI implementation.  We will omit that complexity for now.
	#ctl_util.c		\
	#ctl_scsi_all.c		\
	#pci_virtio_scsi.c	\

# The audio backend in FreeBSD is different than the one found in audio_oss.h
	#audio.c		\
	#hda_codec.c		\
	#pci_hda.c		\

# The virtio input device expects to link to a FreeBSD /dev/input/eventX device
	#pci_virtio_input.c	\

OBJS = $(SRCS:.c=.o)

MEVENT_TEST_PROG = mevent_test
MEVENT_TEST_SRCS = mevent.c mevent_test.c
MEVENT_TEST_OBJS = $(MEVENT_TEST_SRCS:.c=.o)

CLEANFILES =	$(OBJS) $(MEVENT_TEST_OBJS)
CLOBBERFILES =	$(PROG) $(MEVENT_TEST_PROG)

CFLAGS +=	$(CCVERBOSE)
CFLAGS +=	-_gcc=-Wimplicit-function-declaration -_gcc=-Wno-parentheses
CPPFLAGS =	-I$(COMPAT)/bhyve -I$(CONTRIB)/bhyve \
		-I$(COMPAT)/bhyve/amd64 -I$(CONTRIB)/bhyve/amd64 \
		-I$(CONTRIB)/bhyve/dev/usb/controller \
		-I$(CONTRIB)/bhyve/dev/mii \
		-I$(SRC)/lib/lib9p/common \
		-I$(SRC)/uts/common/io/e1000api \
		$(CPPFLAGS.master) \
		-I$(SRC)/uts/intel/io/vmm \
		-I$(SRC)/uts/common \
		-I$(SRC)/uts/intel \
		-DWITHOUT_CAPSICUM

pci_nvme.o := CERRWARN += -_gcc=-Wno-pointer-sign
pci_nvme.o := CERRWARN += -_gcc10=-Wno-address-of-packed-member
pci_nvme.o := CERRWARN += -_gcc11=-Wno-address-of-packed-member
pci_nvme.o := SMOFF += kmalloc_wrong_size

pci_passthru.o := CERRWARN += -_gcc10=-Wno-address-of-packed-member
pci_passthru.o := CERRWARN += -_gcc11=-Wno-address-of-packed-member

pci_virtio_9p.o := SMOFF += kmalloc_wrong_size

pci_xhci.o := CERRWARN += -_gcc10=-Wno-address-of-packed-member
pci_xhci.o := CERRWARN += -_gcc11=-Wno-address-of-packed-member

SMOFF += all_func_returns,leaks,no_if_block

rfb.o := SMOFF=

# Force c99 for everything
CSTD=		$(CSTD_GNU99)

$(PROG) := LDLIBS += \
	-l9p \
	-lcmdutils \
	-lcrypto \
	-ldladm \
	-ldlpi \
	-lidspace \
	-lmd \
	-lnsl \
	-lnvpair \
	-lsocket \
	-lumem \
	-luuid \
	-lvmmapi \
	-lz
NATIVE_LIBS += libz.so libcrypto.so
$(MEVENT_TEST_PROG) := LDLIBS += -lsocket
$(PROG) := LDFLAGS += $(ZASLR)

.KEEP_STATE:

all: $(PROG) $(MEVENT_TEST_PROG) $(SUBDIRS)

$(PROG): $(OBJS)
	$(LINK.c) -o $@ $(OBJS) $(LDFLAGS) $(LDLIBS)
	$(POST_PROCESS)

$(MEVENT_TEST_PROG): $(MEVENT_TEST_OBJS)
	$(LINK.c) -o $@ $(MEVENT_TEST_OBJS) $(LDFLAGS) $(LDLIBS)

install: all $(ROOTUSRSBINPROG) $(SUBDIRS)

clean: $(SUBDIRS)
	$(RM) $(CLEANFILES)

clobber: clean $(SUBDIRS)
	$(RM) $(CLOBBERFILES)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(TARGET)

FRC:
