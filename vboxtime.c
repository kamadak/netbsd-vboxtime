/* $NetBSD$ */

/*
 * Copyright (c) 2014 KAMADA Ken'ichi.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <uvm/uvm_extern.h>
#include <dev/pci/pcidevs.h>
#include <dev/pci/pcivar.h>

#include "vboxtime.h"

#define VBOXTIME_VERBOSE		1
#define VBOXTIME_SYNC_INTERVAL		60	/* in seconds */
#define VBOXTIME_STEP_THRESHOLD		5	/* in seconds */

#define VMMDEV_BAR0		(PCI_MAPREG_START + 0)	/* I/O */
#define VMMDEV_BAR1		(PCI_MAPREG_START + 4)	/* Memory */

struct vboxtime_softc {
	/* Generic device information. */
	device_t		sc_dev;
	/* I/O space for VirtualBox VMM device. */
	bus_space_tag_t		sc_iot;
	bus_space_handle_t	sc_ioh;
	bus_size_t		sc_iosize;
	/* Memory space for VirtualBox VMM device. */
	bus_space_tag_t		sc_memt;
	bus_space_handle_t	sc_memh;
	bus_size_t		sc_memsize;
	/* PCI DMA mapping for the request buffer. */
	bus_dma_tag_t		sc_dmat;
	bus_dma_segment_t	sc_segs;
	int			sc_nsegs;
	bus_dmamap_t		sc_dmam;
	void			*sc_vreq;
	paddr_t			sc_preq;
	/* sysctl */
	struct sysctllog	*sc_sysctllog;
	int			sc_verbose;
	int			sc_sync_interval;
	int			sc_step_threshold;
	/* callout */
	struct callout		sc_sync_callout;
};

static int vboxtime_match(device_t, cfdata_t, void *);
static void vboxtime_attach(device_t, device_t, void *);
static int vboxtime_detach(device_t, int);
static int vboxtime_modcmd(modcmd_t, void *);

CFATTACH_DECL_NEW(vboxtime, sizeof(struct vboxtime_softc),
    vboxtime_match, vboxtime_attach, vboxtime_detach, NULL);

MODULE(MODULE_CLASS_DRIVER, vboxtime, "pci");

#ifdef _MODULE
#include "ioconf.c"
#endif

static int vboxtime_sysctl_create(struct vboxtime_softc *sc);
static int vboxtime_sysctl_helper_verbose(SYSCTLFN_PROTO);
static int vboxtime_sysctl_helper_sync_interval(SYSCTLFN_PROTO);
static int vboxtime_sysctl_helper_step_threshold(SYSCTLFN_PROTO);
static int vboxtime_alloc_req(struct vboxtime_softc *, size_t);
static void vboxtime_free_req(struct vboxtime_softc *, size_t);
static int vboxtime_init(struct vboxtime_softc *);
static void vboxtime_sync(void *);
static void vboxtime_request(struct vboxtime_softc *,
    VMMDevRequestType, uint32_t);
static void vboxtime_print_delta(device_t dev,
    const char *action, struct timeval delta);

static int
vboxtime_match(device_t parent, cfdata_t cf, void *aux)
{
	struct pci_attach_args *pa = aux;

	if (PCI_VENDOR(pa->pa_id) == PCI_VENDOR_VIRTUALBOX &&
	    PCI_PRODUCT(pa->pa_id) == PCI_PRODUCT_VIRTUALBOX_GUEST)
		return 1;
	return 0;
}

static void
vboxtime_attach(device_t parent, device_t self, void *aux)
{
	struct vboxtime_softc *sc = device_private(self);
	struct pci_attach_args *pa = aux;
	pcireg_t rev;

	sc->sc_dev = self;
	rev = PCI_REVISION(pa->pa_class);

	/*
	 * Do this by myself.  If we call pci_aprint_devinfo(pa, NULL),
	 * the kprintf family malfunctions.  It autoloads the "pciverbose"
	 * module, and it seems to me that recursive loading of modules
	 * has something to do with this.
	 */
	aprint_normal(": VirtualBox Guest Service (rev. 0x%02x)\n", rev);
	aprint_naive("\n");

	sc->sc_dmat = pa->pa_dmat;
	callout_init(&sc->sc_sync_callout, 0);

	if (rev != 0) {
		aprint_error_dev(sc->sc_dev, "unknown revision\n");
		return;
	}

	if (pci_mapreg_map(pa, VMMDEV_BAR0, PCI_MAPREG_TYPE_IO, 0,
	    &sc->sc_iot, &sc->sc_ioh, NULL, &sc->sc_iosize)) {
		aprint_error_dev(sc->sc_dev, "can't map i/o space\n");
		return;
	}
	if (pci_mapreg_map(pa, VMMDEV_BAR1, PCI_MAPREG_TYPE_MEM,
	    BUS_SPACE_MAP_LINEAR,
	    &sc->sc_memt, &sc->sc_memh, NULL, &sc->sc_memsize)) {
		aprint_error_dev(sc->sc_dev, "can't map mem space\n");
		return;
	}
	if (vboxtime_alloc_req(sc, sizeof(VMMDevRequestStorage)) == -1)
		return;

	if (vboxtime_sysctl_create(sc) == -1)
		return;

	if (vboxtime_init(sc) == -1)
		return;
	callout_setfunc(&sc->sc_sync_callout, vboxtime_sync, sc);
	vboxtime_sync(sc);
}

static int
vboxtime_detach(device_t self, int flags)
{
	struct vboxtime_softc *sc = device_private(self);

	callout_halt(&sc->sc_sync_callout, NULL);
	callout_destroy(&sc->sc_sync_callout);

	if (sc->sc_sysctllog != NULL)
		sysctl_teardown(&sc->sc_sysctllog);

	vboxtime_free_req(sc, sizeof(VMMDevRequestStorage));
	if (sc->sc_iosize != 0)
		bus_space_unmap(sc->sc_iot, sc->sc_ioh, sc->sc_iosize);
	if (sc->sc_memsize != 0)
		bus_space_unmap(sc->sc_memt, sc->sc_memh, sc->sc_memsize);

	return 0;
}

static int
vboxtime_modcmd(modcmd_t cmd, void *aux)
{
	switch (cmd) {
	case MODULE_CMD_INIT:
#ifdef _MODULE
		return config_init_component(cfdriver_ioconf_vboxtime,
		    cfattach_ioconf_vboxtime, cfdata_ioconf_vboxtime);
#else
		return 0;
#endif
	case MODULE_CMD_FINI:
#ifdef _MODULE
		return config_fini_component(cfdriver_ioconf_vboxtime,
		    cfattach_ioconf_vboxtime, cfdata_ioconf_vboxtime);
#else
		return 0;
#endif
	case MODULE_CMD_AUTOUNLOAD:
		return EBUSY;
	default:
		return ENOTTY;
	}
}

static int
vboxtime_sysctl_create(struct vboxtime_softc *sc)
{
	const struct sysctlnode *node = NULL;
	int error;

	sc->sc_sysctllog = NULL;
	sc->sc_verbose = VBOXTIME_VERBOSE;
	sc->sc_sync_interval = VBOXTIME_SYNC_INTERVAL;
	sc->sc_step_threshold = VBOXTIME_STEP_THRESHOLD;

	error = sysctl_createv(&sc->sc_sysctllog, 0, NULL, &node,
	    0, CTLTYPE_NODE, device_xname(sc->sc_dev), NULL, NULL, 0, NULL, 0,
	    CTL_HW, CTL_CREATE, CTL_EOL);
	if (error != 0 || node == NULL)
		goto fail;

	error = sysctl_createv(&sc->sc_sysctllog, 0, &node, NULL,
	    CTLFLAG_OWNDESC | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "verbose",
	    SYSCTL_DESCR("Verbose level"),
	    &vboxtime_sysctl_helper_verbose, 0, (void *)sc, 0,
	    CTL_CREATE, CTL_EOL);
	if (error != 0)
		goto fail;

	error = sysctl_createv(&sc->sc_sysctllog, 0, &node, NULL,
	    CTLFLAG_OWNDESC | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "sync_interval",
	    SYSCTL_DESCR("Synchronization interval in seconds"),
	    &vboxtime_sysctl_helper_sync_interval, 0, (void *)sc, 0,
	    CTL_CREATE, CTL_EOL);
	if (error != 0)
		goto fail;

	error = sysctl_createv(&sc->sc_sysctllog, 0, &node, NULL,
	    CTLFLAG_OWNDESC | CTLFLAG_READWRITE,
	    CTLTYPE_INT, "step_threshold",
	    SYSCTL_DESCR("Threshold for step adjustment in seconds"),
	    &vboxtime_sysctl_helper_step_threshold, 0, (void *)sc, 0,
	    CTL_CREATE, CTL_EOL);
	if (error != 0)
		goto fail;

	return 0;
fail:
	aprint_error_dev(sc->sc_dev, "cannot create sysctl nodes\n");
	return -1;
}

static int
vboxtime_sysctl_helper_verbose(SYSCTLFN_ARGS)
{
	struct vboxtime_softc *sc = rnode->sysctl_data;
	struct sysctlnode node;
	int t, error;

	t = sc->sc_verbose;
	node = *rnode;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	if (t < 0 || t > 3)
		return EINVAL;
	sc->sc_verbose = t;
	return 0;
}

static int
vboxtime_sysctl_helper_sync_interval(SYSCTLFN_ARGS)
{
	struct vboxtime_softc *sc = rnode->sysctl_data;
	struct sysctlnode node;
	int t, error;

	t = sc->sc_sync_interval;
	node = *rnode;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	/* Limit the maximum so that "int ticks" does not overflow. */
	if (t < 1 || t > 86400)
		return EINVAL;
	sc->sc_sync_interval = t;
	/* Reschedule now. */
	callout_schedule(&sc->sc_sync_callout, sc->sc_sync_interval * hz);
	return 0;
}

static int
vboxtime_sysctl_helper_step_threshold(SYSCTLFN_ARGS)
{
	struct vboxtime_softc *sc = rnode->sysctl_data;
	struct sysctlnode node;
	int t, error;

	t = sc->sc_step_threshold;
	node = *rnode;
	node.sysctl_data = &t;
	error = sysctl_lookup(SYSCTLFN_CALL(&node));
	if (error || newp == NULL)
		return error;

	/* Adjusting a big deviation with adjtime is not sane. */
	if (t < 0 || t > 3600)
		return EINVAL;
	sc->sc_step_threshold = t;
	return 0;
}

static int
vboxtime_alloc_req(struct vboxtime_softc *sc, size_t size)
{
	int error;

	/*
	 * A request to the VMM device is actually not sent by DMA on
	 * the PCI bus, but the physical address of the request buffer
	 * must be within the 32-bit address space.
	 * The "high" address of pci_attach_args->pa_dmat is limited to
	 * the 32-bit space (see pci_bus_dma{,64}_tag in
	 * src/sys/arch/x86/pci/pci_machdep.c), so we can (ab)use it.
	 *
	 * The nsegs parameter is 1 to allocate a continuous memory region.
	 */
	if ((error = bus_dmamem_alloc(sc->sc_dmat, size,
	    sizeof(uint32_t), PAGE_SIZE, &sc->sc_segs, 1, &sc->sc_nsegs,
	    BUS_DMA_WAITOK)) != 0) {
		aprint_error_dev(sc->sc_dev,
		    "unable to allocate memory, error = %d\n", error);
		goto fail_alloc;
	}
	if ((error = bus_dmamem_map(sc->sc_dmat, &sc->sc_segs, sc->sc_nsegs,
	    sizeof(uint32_t), &sc->sc_vreq, BUS_DMA_WAITOK)) != 0) {
		aprint_error_dev(sc->sc_dev,
		    "unable to map memory, error = %d\n", error);
		goto fail_map;
	}
	if ((error = bus_dmamap_create(sc->sc_dmat, size, 1, size, 0,
	    BUS_DMA_WAITOK, &sc->sc_dmam)) != 0) {
		aprint_error_dev(sc->sc_dev,
		    "unable to create DMA map, error = %d\n", error);
		goto fail_create;
	}
	if ((error = bus_dmamap_load(sc->sc_dmat, sc->sc_dmam,
	    sc->sc_vreq, size, NULL, BUS_DMA_WAITOK)) != 0) {
		aprint_error_dev(sc->sc_dev,
		    "unable to load DMA map, error = %d\n", error);
		goto fail_load;
	}

	sc->sc_preq = sc->sc_dmam->dm_segs[0].ds_addr;
	if (sc->sc_preq > UINT32_MAX - size) {
		/* This must not happen. */
		aprint_error_dev(sc->sc_dev, "buffer address too high\n");
		goto fail;
	}
	return 0;

fail:
	bus_dmamap_unload(sc->sc_dmat, sc->sc_dmam);
fail_load:
	bus_dmamap_destroy(sc->sc_dmat, sc->sc_dmam);
fail_create:
	bus_dmamem_unmap(sc->sc_dmat, sc->sc_vreq, size);
fail_map:
	bus_dmamem_free(sc->sc_dmat, &sc->sc_segs, sc->sc_nsegs);
fail_alloc:
	sc->sc_vreq = NULL;
	return -1;
}

static void
vboxtime_free_req(struct vboxtime_softc *sc, size_t size)
{
	if (sc->sc_vreq == NULL)
		return;
	bus_dmamap_unload(sc->sc_dmat, sc->sc_dmam);
	bus_dmamap_destroy(sc->sc_dmat, sc->sc_dmam);
	bus_dmamem_unmap(sc->sc_dmat, sc->sc_vreq, size);
	bus_dmamem_free(sc->sc_dmat, &sc->sc_segs, sc->sc_nsegs);
}

static int
vboxtime_init(struct vboxtime_softc *sc)
{
	void *membase;
	VMMDevMemory_head *vmmdev;
	VMMDevReportGuestInfo *req;

	/*
	 * Validate the version and size of the MMIO region.
	 */
	membase = bus_space_vaddr(sc->sc_memt, sc->sc_memh);
	vmmdev = membase;
	if (vmmdev->version != VMMDEV_MEMORY_VERSION ||
	    vmmdev->size < 32 ||
	    vmmdev->size > sc->sc_memsize) {
		aprint_error_dev(sc->sc_dev, "invalid VMM device memory "
		    "(version=%" PRIu32 " size=%" PRIu32 ")\n",
		    vmmdev->version, vmmdev->size);
		return -1;
	}

	/*
	 * Report the guest information to the host.
	 * The old interface (ReportGuestInfo) is used rather than the new
	 * one (ReportGuestInfo2) for simplicity.
	 */
	req = sc->sc_vreq;
	req->guest_info.interface_version = VMMDEV_VERSION;
#ifdef __x86_64__
	req->guest_info.os_type = VBOXOSTYPE_NetBSD_x64;
#else
	req->guest_info.os_type = VBOXOSTYPE_NetBSD;
#endif

	vboxtime_request(sc, VMMDevReq_ReportGuestInfo, sizeof(*req));
	if (req->header.rc < VINF_SUCCESS) {
		aprint_error_dev(sc->sc_dev,
		    "VMMDevReq_ReportGuestInfo: %d\n", req->header.rc);
		return -1;
	}

	return 0;
}

static void
vboxtime_sync(void *arg)
{
	struct vboxtime_softc *sc = arg;
	VMMDevReqHostTime *req;
	struct timeval guest, host, delta;
	struct timespec step;

	/* There seems no harm in adjusting too early, but wasteful. */
	if (boottime.tv_sec == 0) {
		if (sc->sc_verbose >= 3)
			device_printf(sc->sc_dev,
			    "waiting for the system time to be initialized\n");
		callout_schedule(&sc->sc_sync_callout, hz);	/* 1 sec */
		return;
	}

	req = sc->sc_vreq;
	req->time = UINT64_MAX;

	vboxtime_request(sc, VMMDevReq_GetHostTime, sizeof(*req));
	if (req->header.rc < VINF_SUCCESS) {
		device_printf(sc->sc_dev,
		    "VMMDevReq_GetHostTime: %d\n", req->header.rc);
		goto fail;
	}
	microtime(&guest);
	host.tv_sec = req->time / 1000;
	host.tv_usec = req->time % 1000 * 1000;
	timersub(&host, &guest, &delta);

	if ((delta.tv_sec == 0 && delta.tv_usec < 3000) ||
	    (delta.tv_sec == -1 && delta.tv_usec >= 1000000 - 3000)) {
		/*
		 * Do not adjust a small offset.  The granularity of the host
		 * time is 1 ms.  There is jitter in the request latency.
		 */
		if (sc->sc_verbose >= 3)
			vboxtime_print_delta(sc->sc_dev, "idle", delta);
	} else if (delta.tv_sec < sc->sc_step_threshold &&
	    delta.tv_sec >= -sc->sc_step_threshold) {
		adjtime1(&delta, NULL, NULL);
		if (sc->sc_verbose >= 2)
			vboxtime_print_delta(sc->sc_dev, "adjust", delta);
	} else {
		step.tv_sec = host.tv_sec;
		step.tv_nsec = host.tv_usec * 1000;
		(void)settime(NULL, &step);
		if (sc->sc_verbose >= 1)
			vboxtime_print_delta(sc->sc_dev, "step", delta);
	}

fail:
	callout_schedule(&sc->sc_sync_callout, sc->sc_sync_interval * hz);
}

static void
vboxtime_request(struct vboxtime_softc *sc,
    VMMDevRequestType type, uint32_t size)
{
	VMMDevRequestHeader *header;

	header = sc->sc_vreq;
	header->size = size;
	header->version = VMMDEV_REQUEST_HEADER_VERSION;
	header->request_type = type;
	header->rc = VERR_GENERAL_FAILURE;
	header->reserved1 = 0;
	header->reserved2 = 0;

	/* No DMA/bouncing is actually involved, so skip bus_dmamap_sync(). */
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, VMMDEV_PORT_OFF_REQUEST,
	    sc->sc_preq);

	/*
	 * Instruct the optimizer not to assume the liveness of register
	 * values, because the request buffer was rewritten by the VMM
	 * device without the compiler's awareness.
	 * The standard way to do this is "volatile", but accessing always
	 * via a volatile pointer is inefficient.
	 */
	__insn_barrier();
}

static void
vboxtime_print_delta(device_t dev, const char *action, struct timeval delta)
{
	long long sec = delta.tv_sec;
	unsigned int usec = delta.tv_usec;

	if (sec >= 0 || usec == 0)
		device_printf(dev, "%s %lld.%06u s\n",
		    action, sec, usec);
	else
		device_printf(dev, "%s -%lld.%06u s\n",
		    action, -sec - 1, 1000000 - usec);
}
