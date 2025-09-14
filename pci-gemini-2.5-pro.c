/*
pci.c

Configure devices on the PCI bus

Created:	Jan 2000 by Philip Homburg <philip@cs.vu.nl>
*/
#include <minix/acpi.h>
#include <minix/chardriver.h>
#include <minix/driver.h>
#include <minix/param.h>
#include <minix/rs.h>

#include <machine/pci.h>
#include <machine/pci_amd.h>
#include <machine/pci_intel.h>
#include <machine/pci_sis.h>
#include <machine/pci_via.h>
#include <machine/vmparam.h>

#include <dev/pci/pci_verbose.h>

#include <pci.h>
#include <stdlib.h>
#include <stdio.h>

#include "pci.h"

#define PCI_VENDORSTR_LEN	64
#define PCI_PRODUCTSTR_LEN	64

#define irq_mode_pci(irq) ((void)0)

#define PBT_INTEL_HOST	 1
#define PBT_PCIBRIDGE	 2
#define PBT_CARDBUS	 3

#define BAM_NR		6	/* Number of base-address registers */

struct pci_acl pci_acl[NR_DRIVERS];

static struct pcibus
{
	int pb_type;
	int pb_needinit;
	int pb_isabridge_dev;
	int pb_isabridge_type;

	int pb_devind;
	int pb_busnr;
	u8_t (*pb_rreg8)(int busind, int devind, int port);
	u16_t (*pb_rreg16)(int busind, int devind, int port);
	u32_t (*pb_rreg32)(int busind, int devind, int port);
	void (*pb_wreg8)(int busind, int devind, int port, u8_t value);
	void (*pb_wreg16)(int busind, int devind, int port, u16_t value);
	void (*pb_wreg32)(int busind, int devind, int port, u32_t value);
	u16_t (*pb_rsts)(int busind);
	void (*pb_wsts)(int busind, u16_t value);
} pcibus[NR_PCIBUS];
static int nr_pcibus= 0;

static struct pcidev
{
	u8_t pd_busnr;
	u8_t pd_dev;
	u8_t pd_func;
	u8_t pd_baseclass;
	u8_t pd_subclass;
	u8_t pd_infclass;
	u16_t pd_vid;
	u16_t pd_did;
	u16_t pd_sub_vid;
	u16_t pd_sub_did;
	u8_t pd_ilr;

	u8_t pd_inuse;
	endpoint_t pd_proc;

	struct bar
	{
		int pb_flags;
		int pb_nr;
		u32_t pb_base;
		u32_t pb_size;
	} pd_bar[BAM_NR];
	int pd_bar_nr;
} pcidev[NR_PCIDEV];

/* pb_flags */
#define PBF_IO		1	/* I/O else memory */
#define PBF_INCOMPLETE	2	/* not allocated */

static int nr_pcidev= 0;

static struct machine machine;

/*===========================================================================*
 *			helper functions for I/O			     *
 *===========================================================================*/
static unsigned
pci_inb(u16_t port)
{
	u32_t value = 0;
	int status;

	status = sys_inb(port, &value);
	if (status != OK) {
		printf("PCI: warning, sys_inb failed: %d\n", status);
	}

	return value;
}

static unsigned
pci_inw(u16_t port)
{
	u32_t value = (u32_t)-1;
	const int status = sys_inw(port, &value);

	if (status != OK) {
		printf("PCI: warning, sys_inw failed: %d\n", status);
	}

	return value;
}

static u32_t
pci_inl(u16_t port) {
	u32_t value;
	const int s = sys_inl(port, &value);

	if (s != OK) {
		printf("PCI: warning, sys_inl failed: %d\n", s);
		return (u32_t)-1;
	}

	return value;
}

static void
pci_outb(u16_t port, u8_t value)
{
	const int s = sys_outb(port, value);
	if (s != OK) {
		fprintf(stderr, "PCI: warning, sys_outb failed: %d\n", s);
	}
}

static void
pci_outw(u16_t port, u16_t value)
{
	const int status = sys_outw(port, value);
	if (status != OK) {
		fprintf(stderr, "PCI: warning, sys_outw failed: %d\n", status);
	}
}

static void pci_outl(u16_t port, u32_t value)
{
    const int status = sys_outl(port, value);
    if (status != OK) {
        fprintf(stderr, "PCI: warning, sys_outl failed: %d\n", status);
    }
}

static u8_t
pcii_rreg8(int busind, int devind, int port)
{
    const int num_buses = sizeof(pcibus) / sizeof(pcibus[0]);
    const int num_devs = sizeof(pcidev) / sizeof(pcidev[0]);

    if (busind < 0 || busind >= num_buses || devind < 0 || devind >= num_devs)
    {
        printf("PCI: error, pcii_rreg8 index out of bounds: bus=%d, dev=%d\n",
               busind, devind);
        return 0xFF;
    }

    const u8_t v = PCII_RREG8_(pcibus[busind].pb_busnr,
                               pcidev[devind].pd_dev,
                               pcidev[devind].pd_func,
                               port);

    const int s = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (s != OK)
    {
        printf("PCI: warning, sys_outl failed: %d\n", s);
    }

    return v;
}

static u16_t
pcii_rreg16(int busind, int devind, int port)
{
	if (busind < 0 || (unsigned int)busind >= PCI_MAX_BUSES ||
	    devind < 0 || (unsigned int)devind >= PCI_MAX_DEVICES) {
		printf("PCI: error, invalid index busind=%d, devind=%d\n", busind, devind);
		return (u16_t)-1;
	}

	const struct pci_bus * const bus = &pcibus[busind];
	const struct pci_dev * const dev = &pcidev[devind];

	u16_t value = PCII_RREG16_(bus->pb_busnr, dev->pd_dev, dev->pd_func, port);

	const int status = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (status != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", status);
	}

	return value;
}

static u32_t
pcii_rreg32(int busind, int devind, int port)
{
	const u32_t v = PCII_RREG32_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port);

	const int s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}

	return v;
}

static void
pcii_wreg8(int busind, int devind, int port, u8_t value)
{
	const int bus_nr = pcibus[busind].pb_busnr;
	const int dev_nr = pcidev[devind].pd_dev;
	const int func_nr = pcidev[devind].pd_func;

	PCII_WREG8_(bus_nr, dev_nr, func_nr, port, value);

	const int s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		fprintf(stderr, "PCI: warning, sys_outl failed: %d\n", s);
	}
}

static void
pcii_wreg16(int busind, int devind, int port, u16_t value)
{
	const int bus_nr = pcibus[busind].pb_busnr;
	const int dev = pcidev[devind].pd_dev;
	const int func = pcidev[devind].pd_func;

	PCII_WREG16_(bus_nr, dev, func, port, value);

	const int status = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (status != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", status);
	}
}

static void
pcii_wreg32(int busind, int devind, int port, u32_t value)
{
    const int bus_number = pcibus[busind].pb_busnr;
    const int device_number = pcidev[devind].pd_dev;
    const int function_number = pcidev[devind].pd_func;

    PCII_WREG32_(bus_number, device_number, function_number, port, value);

    const int status = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (status != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", status);
    }
}

/*===========================================================================*
 *				ntostr					     *
 *===========================================================================*/
static void
ntostr(unsigned int n, char **str, const char *end)
{
	char tmpstr[20];
	char *p = *str;
	int i = 0;

	if (p < end)
	{
		do
		{
			tmpstr[i++] = '0' + (n % 10);
			n /= 10;
		} while (n > 0);

		while (i > 0 && p < end)
		{
			*p++ = tmpstr[--i];
		}

		if (p < end)
		{
			*p = '\0';
		}
		else
		{
			p[-1] = '\0';
		}
	}
	*str = p;
}

/*===========================================================================*
 *				get_busind					     *
 *===========================================================================*/
static int get_busind(int busnr)
{
    for (int i = 0; i < nr_pcibus; i++)
    {
        if (pcibus[i].pb_busnr == busnr)
        {
            return i;
        }
    }
    return -1;
}

/*===========================================================================*
 *			Unprotected helper functions			     *
 *===========================================================================*/
static u8_t
__pci_attr_r8(int devind, int port)
{
	const int busnr = pcidev[devind].pd_busnr;
	const int busind = get_busind(busnr);

	return pcibus[busind].pb_rreg8(busind, devind, port);
}

static u16_t
__pci_attr_r16(int devind, int port)
{
	const int busnr = pcidev[devind].pd_busnr;
	const int busind = get_busind(busnr);

	if (busind < 0) {
		return (u16_t)-1;
	}

	if (pcibus[busind].pb_rreg16 == NULL) {
		return (u16_t)-1;
	}

	return pcibus[busind].pb_rreg16(busind, devind, port);
}

static u32_t
__pci_attr_r32(int devind, int port)
{
	const int busnr = pcidev[devind].pd_busnr;
	const int busind = get_busind(busnr);

	if (busind < 0) {
		return (u32_t)-1;
	}

	if (pcibus[busind].pb_rreg32 == NULL) {
		return (u32_t)-1;
	}

	return pcibus[busind].pb_rreg32(busind, devind, port);
}

static void
__pci_attr_w8(int devind, int port, u8_t value)
{
	if (devind < 0 || devind >= (sizeof(pcidev) / sizeof(pcidev[0]))) {
		return;
	}

	int busind = get_busind(pcidev[devind].pd_busnr);

	if (busind < 0 || busind >= (sizeof(pcibus) / sizeof(pcibus[0]))) {
		return;
	}

	if (pcibus[busind].pb_wreg8) {
		pcibus[busind].pb_wreg8(busind, devind, port, value);
	}
}

static void
__pci_attr_w16(int devind, int port, u16_t value)
{
	if (devind < 0 || devind >= NR_PCIDEV) {
		return;
	}

	const int busnr = pcidev[devind].pd_busnr;
	const int busind = get_busind(busnr);

	if (busind < 0 || busind >= NR_PCIBUS) {
		return;
	}

	if (pcibus[busind].pb_wreg16) {
		pcibus[busind].pb_wreg16(busind, devind, port, value);
	}
}

static void
__pci_attr_w32(int devind, int port, u32_t value)
{
	/* Assuming PCI_DEV_MAX and PCI_BUS_MAX are defined appropriately */
	if (devind < 0 || devind >= PCI_DEV_MAX) {
		return;
	}

	const int busnr = pcidev[devind].pd_busnr;
	const int busind = get_busind(busnr);

	if (busind < 0 || busind >= PCI_BUS_MAX) {
		return;
	}

	if (pcibus[busind].pb_wreg32) {
		pcibus[busind].pb_wreg32(busind, devind, port, value);
	}
}

/*===========================================================================*
 *				helpers					     *
 *===========================================================================*/
static u16_t
pci_attr_rsts(int devind)
{
	/* These constants representing array sizes are assumed to exist. */
	if (devind < 0 || (size_t)devind >= NUM_PCI_DEVICES) {
		return (u16_t)-1;
	}

	const int busind = get_busind(pcidev[devind].pd_busnr);

	if (busind < 0 || (size_t)busind >= NUM_PCI_BUSES) {
		return (u16_t)-1;
	}

	if (pcibus[busind].pb_rsts != NULL) {
		return pcibus[busind].pb_rsts(busind);
	}

	return (u16_t)-1;
}

static void
pci_attr_wsts(int devind, u16_t value)
{
	if (devind < 0 || (size_t)devind >= (sizeof(pcidev) / sizeof(pcidev[0]))) {
		return;
	}

	const int busnr = pcidev[devind].pd_busnr;
	const int busind = get_busind(busnr);

	if (busind < 0 || (size_t)busind >= (sizeof(pcibus) / sizeof(pcibus[0]))) {
		return;
	}

	if (pcibus[busind].pb_wsts) {
		pcibus[busind].pb_wsts(busind, value);
	}
}

static u16_t
pcii_rsts(int busind)
{
	if (busind < 0) {
		return (u16_t)-1;
	}

	const int pci_device_zero = 0;
	const int pci_function_zero = 0;

	u16_t status_value = PCII_RREG16_(pcibus[busind].pb_busnr,
	                                  pci_device_zero,
	                                  pci_function_zero,
	                                  PCI_SR);

	const int result = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (result != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", result);
	}

	return status_value;
}

static void
pcii_wsts(int busind, u16_t value)
{
	/* Assuming NR_PCIBUS is the size of the pcibus array. */
	if (busind < 0 || busind >= NR_PCIBUS) {
		printf("PCI: error, invalid bus index provided: %d\n", busind);
		return;
	}

	PCII_WREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR, value);

	const int s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
}

#include <stdbool.h>
#include <stddef.h>

/* Forward declaration of the pci_dev struct assumed from context */
struct pci_dev;

static bool
is_duplicate(const struct pci_dev *devices, size_t num_devices, u8_t busnr,
             u8_t dev, u8_t func)
{
	if (!devices) {
		return false;
	}

	for (size_t i = 0; i < num_devices; i++) {
		if (devices[i].pd_busnr == busnr &&
		    devices[i].pd_dev == dev &&
		    devices[i].pd_func == func) {
			return true;
		}
	}

	return false;
}

static int
get_freebus(void)
{
	int freebus = 1;
	for (int i = 0; i < nr_pcibus; i++) {
		if (pcibus[i].pb_needinit || pcibus[i].pb_type == PBT_INTEL_HOST) {
			continue;
		}

		if (pcibus[i].pb_busnr >= freebus) {
			freebus = pcibus[i].pb_busnr + 1;
		}
	}
	return freebus;
}

static const char *
pci_vid_name(u16_t vid)
{
	static char vendor[PCI_VENDORSTR_LEN];

	if (pci_findvendor(vendor, sizeof(vendor), vid) == NULL) {
		return "Unknown";
	}

	return vendor;
}


static void
print_hyper_cap(int devind, u8_t capptr)
{
	u32_t v;
	u16_t cmd;
	int type0;

	v = __pci_attr_r32(devind, capptr);
	cmd = (v >> 16) & 0xffff;
	type0 = (cmd & 0xE000) >> 13;

	printf("\nprint_hyper_cap: @0x%x, off 0 (cap):", capptr);

	if (type0 <= 1) {
		printf(" Capability Type: %s",
			type0 == 0 ? "Slave or Primary Interface" : "Host or Secondary Interface");
		cmd &= ~0xE000;
	} else {
		int type1 = (cmd & 0xF800) >> 11;
		printf(" Capability Type 0x%x", type1);
		cmd &= ~0xF800;
	}

	if (cmd) {
		printf(" undecoded 0x%x", cmd);
	}

	printf("\n");
}

static const char *
get_capability_str(u8_t type)
{
	switch (type) {
	case 1: return "PCI Power Management";
	case 2: return "AGP";
	case 3: return "Vital Product Data";
	case 4: return "Slot Identification";
	case 5: return "Message Signaled Interrupts";
	case 6: return "CompactPCI Hot Swap";
	case 8: return "AMD HyperTransport";
	case 0xf: return "Secure Device";
	default: return "(unknown type)";
	}
}

static const char *
get_secure_subtype_str(u8_t subtype)
{
	switch (subtype) {
	case 0: return "Device Exclusion Vector";
	case 3: return "IOMMU";
	default: return "(unknown type)";
	}
}

static void
print_capabilities(int devind)
{
	u16_t status = __pci_attr_r16(devind, PCI_SR);
	if (!(status & PSR_CAPPTR)) {
		return;
	}

	u8_t capptr = __pci_attr_r8(devind, PCI_CAPPTR) & PCI_CP_MASK;
	while (capptr != 0) {
		u8_t type = __pci_attr_r8(devind, capptr + CAP_TYPE);
		const char *type_str = get_capability_str(type);

		printf(" @0x%x (0x%08x): capability type 0x%x: %s",
			capptr, __pci_attr_r32(devind, capptr), type, type_str);

		if (type == 0x08) {
			print_hyper_cap(devind, capptr);
		} else if (type == 0x0f) {
			u8_t subtype = __pci_attr_r8(devind, capptr + 2) & 0x07;
			const char *subtype_str = get_secure_subtype_str(subtype);
			printf(", sub type 0%o: %s", subtype, subtype_str);
		}

		printf("\n");
		capptr = __pci_attr_r8(devind, capptr + CAP_NEXT) & PCI_CP_MASK;
	}
}

/*===========================================================================*
 *				ISA Bridge Helpers			     *
 *===========================================================================*/
static void
update_bridge4dev_io(int devind, u32_t io_base, u32_t io_size)
{
	const int busnr = pcidev[devind].pd_busnr;
	const int busind = get_busind(busnr);

	if (busind < 0) {
		panic("update_bridge4dev_io: invalid bus index for bus number %d", busnr);
	}

	const int type = pcibus[busind].pb_type;

	switch (type) {
	case PBT_INTEL_HOST:
		return;	/* Nothing to do for host controller */
	case PBT_PCIBRIDGE:
		printf("update_bridge4dev_io: not implemented for PCI bridges\n");
		return;
	case PBT_CARDBUS:
		break;
	default:
		panic("update_bridge4dev_io: strange bus type: %d", type);
	}

	if (debug) {
		printf("update_bridge4dev_io: adding 0x%x at 0x%x\n",
			io_size, io_base);
	}

	const int br_devind = pcibus[busind].pb_devind;
	__pci_attr_w32(br_devind, CBB_IOLIMIT_0, io_base + io_size - 1);
	__pci_attr_w32(br_devind, CBB_IOBASE_0, io_base);

	/* Enable I/O access. Enable busmaster access as well. */
	const u16_t pci_command = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, pci_command | PCI_CR_IO_EN | PCI_CR_MAST_EN);
}

static int do_piix(int devind)
{
	int s;
	u32_t elcr1 = 0;
	u32_t elcr2 = 0;

#if DEBUG
	printf("in piix\n");
#endif

	s = sys_inb(PIIX_ELCR1, &elcr1);
	if (s != OK) {
		printf("Warning, sys_inb failed: %d\n", s);
	}

	s = sys_inb(PIIX_ELCR2, &elcr2);
	if (s != OK) {
		printf("Warning, sys_inb failed: %d\n", s);
	}

	const u32_t elcr = elcr1 | (elcr2 << 8);

	for (int i = 0; i < 4; i++) {
		const int irqrc = __pci_attr_r8(devind, PIIX_PIRQRCA + i);
		const char int_name = 'A' + i;

		if (irqrc & PIIX_IRQ_DI) {
			if (debug) {
				printf("INT%c: disabled\n", int_name);
			}
			continue;
		}

		const int irq = irqrc & PIIX_IRQ_MASK;

		if (debug) {
			printf("INT%c: %d\n", int_name, irq);
			if (!(elcr & (1U << irq))) {
				printf("(warning) IRQ %d is not level triggered\n", irq);
			}
		}

		irq_mode_pci(irq);
	}

	return 0;
}

static int
do_amd_isabr(int devind)
{
	static const int NUM_PCI_INTS = 4;
	static const int BITS_PER_IRQ = 4;
	static const u16_t IRQ_MASK = 0xF;

	if (nr_pcidev >= NR_PCIDEV) {
		return -1;
	}

	const int temp_devind = nr_pcidev;
	pcidev[temp_devind].pd_busnr = pcidev[devind].pd_busnr;
	pcidev[temp_devind].pd_dev = pcidev[devind].pd_dev;
	pcidev[temp_devind].pd_func = AMD_ISABR_FUNC;
	pcidev[temp_devind].pd_inuse = 1;
	nr_pcidev++;

	const u8_t levmask = __pci_attr_r8(temp_devind, AMD_ISABR_PCIIRQ_LEV);
	const u16_t pciirq = __pci_attr_r16(temp_devind, AMD_ISABR_PCIIRQ_ROUTE);

	for (int i = 0; i < NUM_PCI_INTS; i++) {
		const int is_edge_triggered = (levmask >> i) & 1;
		const int irq = (pciirq >> (i * BITS_PER_IRQ)) & IRQ_MASK;

		if (irq == 0) {
			if (debug) {
				printf("INT%c: disabled\n", 'A' + i);
			}
			continue;
		}

		if (debug) {
			printf("INT%c: %d\n", 'A' + i, irq);
			if (is_edge_triggered) {
				printf("(warning) IRQ %d is not level triggered\n",
				    irq);
			}
		}
		irq_mode_pci(irq);
	}

	nr_pcidev--;
	return 0;
}

static int
do_sis_isabr(int devind)
{
	enum { NUM_SIS_IRQ_LINES = 4 };

	for (int i = 0; i < NUM_SIS_IRQ_LINES; i++) {
		int irq_val = __pci_attr_r8(devind, SIS_ISABR_IRQ_A + i);

		if ((irq_val & SIS_IRQ_DISABLED) != 0) {
			if (debug) {
				printf("INT%c: disabled\n", 'A' + i);
			}
		} else {
			int irq = irq_val & SIS_IRQ_MASK;
			if (debug) {
				printf("INT%c: %d\n", 'A' + i, irq);
			}
			irq_mode_pci(irq);
		}
	}
	return 0;
}

static int
do_via_isabr(int devind)
{
	static const u8_t edge_masks[4] = {
		VIA_ISABR_EL_INTA, VIA_ISABR_EL_INTB,
		VIA_ISABR_EL_INTC, VIA_ISABR_EL_INTD
	};
	static const u8_t irq_regs[4] = {
		VIA_ISABR_IRQ_R2, VIA_ISABR_IRQ_R2,
		VIA_ISABR_IRQ_R3, VIA_ISABR_IRQ_R1
	};
	static const u8_t irq_shifts[4] = { 4, 0, 4, 4 };

	const u8_t levmask = __pci_attr_r8(devind, VIA_ISABR_EL);

	for (int i = 0; i < 4; i++) {
		const int edge = levmask & edge_masks[i];
		const int irq =
			(__pci_attr_r8(devind, irq_regs[i]) >> irq_shifts[i]) & 0x0F;

		if (irq == 0) {
			if (debug) {
				printf("INT%c: disabled\n", 'A' + i);
			}
		} else {
			if (debug) {
				printf("INT%c: %d\n", 'A' + i, irq);
				if (edge) {
					printf(
					"(warning) IRQ %d is not level triggered\n",
						irq);
				}
			}
			irq_mode_pci(irq);
		}
	}

	return 0;
}

static int
get_supported_bridge_type_idx(const pci_dev_t *dev, int dev_idx,
    int generic_isa_idx)
{
	for (int j = 0; pci_isabridge[j].vid != 0; j++) {
		if (pci_isabridge[j].vid != dev->pd_vid ||
		    pci_isabridge[j].did != dev->pd_did) {
			continue;
		}

		int is_generic_isa_bridge = (dev_idx == generic_isa_idx);
		if (!pci_isabridge[j].checkclass || is_generic_isa_bridge) {
			return j;
		}
	}
	return -1;
}

static int
find_isabridge_on_bus(int busnr, int *dev_idx_ptr, int *type_idx_ptr,
    int *unknown_bridge_idx_ptr)
{
	int unknown_bridge_idx = -1;

	for (int i = 0; i < nr_pcidev; i++) {
		if (pcidev[i].pd_busnr != busnr) {
			continue;
		}

		const u32_t classcode = (pcidev[i].pd_baseclass << 16) |
		    (pcidev[i].pd_subclass << 8) | pcidev[i].pd_infclass;

		if (classcode == PCI_T3_ISA) {
			unknown_bridge_idx = i;
		}

		const int type_idx = get_supported_bridge_type_idx(&pcidev[i], i,
		    unknown_bridge_idx);
		if (type_idx != -1) {
			*dev_idx_ptr = i;
			*type_idx_ptr = type_idx;
			*unknown_bridge_idx_ptr = unknown_bridge_idx;
			return 1;
		}
	}

	*unknown_bridge_idx_ptr = unknown_bridge_idx;
	return 0;
}

static int
configure_isabridge(int busind, int dev_idx, int type_idx)
{
	const pci_dev_t *dev = &pcidev[dev_idx];
	const isabridge_t *bridge_def = &pci_isabridge[type_idx];
	const int type = bridge_def->type;

	if (debug) {
		const char *dstr = _pci_dev_name(dev->pd_vid, dev->pd_did);
		printf("found ISA bridge (%04X:%04X) %s\n",
		    dev->pd_vid, dev->pd_did, dstr ? dstr : "unknown device");
	}

	pcibus[busind].pb_isabridge_dev = dev_idx;
	pcibus[busind].pb_isabridge_type = type;

	switch (type) {
	case PCI_IB_PIIX:
		return do_piix(dev_idx);
	case PCI_IB_VIA:
		return do_via_isabr(dev_idx);
	case PCI_IB_AMD:
		return do_amd_isabr(dev_idx);
	case PCI_IB_SIS:
		return do_sis_isabr(dev_idx);
	default:
		panic("unknown ISA bridge type: %d", type);
	}
}

static int
do_isabridge(int busind)
{
	int dev_idx;
	int type_idx;
	int unknown_bridge_idx;
	const int busnr = pcibus[busind].pb_busnr;

	if (find_isabridge_on_bus(busnr, &dev_idx, &type_idx, &unknown_bridge_idx)) {
		return configure_isabridge(busind, dev_idx, type_idx);
	}

	if (unknown_bridge_idx != -1) {
		if (debug) {
			printf(
			    "(warning) unsupported ISA bridge %04X:%04X for bus %d\n",
			    pcidev[unknown_bridge_idx].pd_vid,
			    pcidev[unknown_bridge_idx].pd_did, busind);
		}
	} else if (debug) {
		printf("(warning) no ISA bridge found on bus %d\n", busind);
	}

	return 0;
}

static int
derive_irq(struct pcidev *dev, int pin)
{
	if (!dev) {
		return -1;
	}

	const int bus_index = get_busind(dev->pd_busnr);
	const int bridge_dev_index = pcibus[bus_index].pb_devind;
	struct pcidev *const parent_bridge = &pcidev[bridge_dev_index];

	const int pci_device_shift = 3;
	const int pci_device_mask = 0x1f;
	const int pci_num_irq_pins = 4;

	const int device_num = (dev->pd_func >> pci_device_shift) & pci_device_mask;
	const int swizzled_pin = (pin + device_num) % pci_num_irq_pins;

	return acpi_get_irq(parent_bridge->pd_busnr,
			    parent_bridge->pd_dev,
			    swizzled_pin);
}

static void
record_irq(int devind)
{
	pci_device_t *dev = &pcidev[devind];
	int ilr = __pci_attr_r8(devind, PCI_ILR);
	const int ipr = __pci_attr_r8(devind, PCI_IPR);

	if (ipr && machine.apic_enabled) {
		int irq = acpi_get_irq(dev->pd_busnr, dev->pd_dev, ipr - 1);
		if (irq < 0) {
			irq = derive_irq(dev, ipr - 1);
		}

		if (irq >= 0) {
			ilr = irq;
			__pci_attr_w8(devind, PCI_ILR, ilr);
			if (debug) {
				printf("PCI: ACPI IRQ %d for device %d.%d.%d INT%c\n",
					irq, dev->pd_busnr, dev->pd_dev,
					dev->pd_func, 'A' + ipr - 1);
			}
		} else if (debug) {
			printf("PCI: no ACPI IRQ routing for device %d.%d.%d INT%c\n",
				dev->pd_busnr, dev->pd_dev,
				dev->pd_func, 'A' + ipr - 1);
		}
	}

	if (ilr == 0) {
		static int first = 1;
		if (ipr && first && debug) {
			first = 0;
			printf("PCI: strange, BIOS assigned IRQ0\n");
		}
		ilr = PCI_ILR_UNKNOWN;
	}

	dev->pd_ilr = ilr;

	if (ilr != PCI_ILR_UNKNOWN) {
		if (ipr) {
			if (debug) {
				printf("\tIRQ %d for INT%c\n", ilr, 'A' + ipr - 1);
			}
		} else {
			printf("PCI: IRQ %d is assigned, but device %d.%d.%d does not need it\n",
				ilr, dev->pd_busnr, dev->pd_dev, dev->pd_func);
		}
		return;
	}

	if (!ipr) {
		return;
	}

	int busind = get_busind(dev->pd_busnr);
	if (pcibus[busind].pb_type == PBT_CARDBUS) {
		int cb_devind = pcibus[busind].pb_devind;
		int cb_ilr = pcidev[cb_devind].pd_ilr;
		if (cb_ilr != PCI_ILR_UNKNOWN) {
			if (debug) {
				printf("assigning IRQ %d to Cardbus device\n", cb_ilr);
			}
			__pci_attr_w8(devind, PCI_ILR, cb_ilr);
			dev->pd_ilr = cb_ilr;
			return;
		}
	}

	if (debug) {
		printf("PCI: device %d.%d.%d uses INT%c but is not assigned any IRQ\n",
			dev->pd_busnr, dev->pd_dev,
			dev->pd_func, 'A' + ipr - 1);
	}
}

/*===========================================================================*
 *				BAR helpers				     *
 *===========================================================================*/
#define MAX_BARS_PER_DEV 6

static void
add_pci_bar(int devind, int bar_nr, u32_t base, u32_t size, int flags)
{
	int dev_bar_nr = pcidev[devind].pd_bar_nr;

	if (dev_bar_nr >= MAX_BARS_PER_DEV) {
		return;
	}

	struct pci_bar *pbar = &pcidev[devind].pd_bar[dev_bar_nr];
	pbar->pb_flags = flags;
	pbar->pb_base = base;
	pbar->pb_size = size;
	pbar->pb_nr = bar_nr;

	if (base == 0) {
		pbar->pb_flags |= PBF_INCOMPLETE;
	}

	pcidev[devind].pd_bar_nr++;
}

static u32_t
probe_bar_size(int devind, int reg, u32_t original_bar, u16_t disable_cmd_mask)
{
	u16_t cmd = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, cmd & disable_cmd_mask);

	__pci_attr_w32(devind, reg, 0xFFFFFFFF);
	u32_t probed_val = __pci_attr_r32(devind, reg);

	__pci_attr_w32(devind, reg, original_bar);
	__pci_attr_w16(devind, PCI_CR, cmd);

	return probed_val;
}

static int
record_bar(int devind, int bar_nr, int last)
{
	int reg = PCI_BAR + 4 * bar_nr;
	u32_t bar = __pci_attr_r32(devind, reg);

	if (bar & PCI_BAR_IO) {
		u32_t probed_val = probe_bar_size(devind, reg, bar,
			~PCI_CR_IO_EN);
		u32_t base = bar & PCI_BAR_IO_MASK;
		u32_t size = (~(probed_val & PCI_BAR_IO_MASK) & 0xFFFF) + 1;

		if (debug) {
			printf("\tbar_%d: %u bytes at 0x%x I/O\n",
				bar_nr, size, base);
		}
		add_pci_bar(devind, bar_nr, base, size, PBF_IO);
		return 1;
	} else {
		int width = 1;
		int type = bar & PCI_BAR_TYPE;

		if (type == PCI_TYPE_64) {
			if (last) {
				printf("PCI: device %d.%d.%d BAR %d extends"
					" beyond designated area\n",
					pcidev[devind].pd_busnr,
					pcidev[devind].pd_dev,
					pcidev[devind].pd_func, bar_nr);
				return 1;
			}
			width = 2;

			u32_t bar_high = __pci_attr_r32(devind, reg + 4);
			if (bar_high != 0) {
				if (debug) {
					printf("\tbar_%d: (64-bit BAR with"
						" high bits set)\n", bar_nr);
				}
				return width;
			}
		} else if (type != PCI_TYPE_32 && type != PCI_TYPE_32_1M) {
			if (debug) {
				printf("\tbar_%d: (unknown type %x)\n",
					bar_nr, type);
			}
			return 1;
		}

		u32_t probed_val = probe_bar_size(devind, reg, bar,
			~PCI_CR_MEM_EN);
		if (probed_val == 0) {
			return width;
		}

		u32_t base = bar & PCI_BAR_MEM_MASK;
		u32_t size = (~(probed_val & PCI_BAR_MEM_MASK)) + 1;

		if (debug) {
			int prefetch = !!(bar & PCI_BAR_PREFETCH);
			printf("\tbar_%d: 0x%x bytes at 0x%x%s memory%s\n",
				bar_nr, size, base,
				prefetch ? " prefetchable" : "",
				type == PCI_TYPE_64 ? ", 64-bit" : "");
		}

		add_pci_bar(devind, bar_nr, base, size, 0);
		return width;
	}
}

static void
record_bars(int devind, int last_reg)
{
    int i = 0;
    int reg = PCI_BAR;

    while (reg <= last_reg)
    {
        const int width = record_bar(devind, i, reg == last_reg);

        if (width <= 0)
        {
            break;
        }

        i += width;
        reg += 4 * width;
    }
}

static void
record_bars_normal(int devind)
{
	int i, j, pb_nr;
	int is_ide_controller;
	int clear_primary;
	int clear_secondary;

	/* The BAR area of normal devices is six DWORDs in size. */
	record_bars(devind, PCI_BAR_6);

	is_ide_controller = pcidev[devind].pd_baseclass == PCI_BCR_MASS_STORAGE &&
						pcidev[devind].pd_subclass == PCI_MS_IDE;

	if (!is_ide_controller)
	{
		return;
	}

	clear_primary = !(pcidev[devind].pd_infclass & PCI_IDE_PRI_NATIVE);
	clear_secondary = !(pcidev[devind].pd_infclass & PCI_IDE_SEC_NATIVE);

	if (!clear_primary && !clear_secondary)
	{
		return;
	}

	if (debug)
	{
		if (clear_primary)
		{
			printf(
	"primary channel is not in native mode, clearing BARs 0 and 1\n");
		}
		if (clear_secondary)
		{
			printf(
	"secondary channel is not in native mode, clearing BARs 2 and 3\n");
		}
	}

	j = 0;
	for (i = 0; i < pcidev[devind].pd_bar_nr; i++)
	{
		pb_nr = pcidev[devind].pd_bar[i].pb_nr;

		if ((clear_primary && pb_nr <= 1) ||
			(clear_secondary && (pb_nr == 2 || pb_nr == 3)))
		{
			if (debug)
			{
				printf("skipping bar %d\n", pb_nr);
			}
			continue;
		}

		if (i != j)
		{
			pcidev[devind].pd_bar[j] = pcidev[devind].pd_bar[i];
		}
		j++;
	}
	pcidev[devind].pd_bar_nr = j;
}

static void
record_bridge_mem_window(int devind, const char *name, int base_reg,
                         int limit_reg, u16_t base_mask, u16_t limit_mask)
{
	const u16_t base_val = __pci_attr_r16(devind, base_reg);
	const u32_t base = (u32_t)(base_val & base_mask) << 16;

	const u16_t limit_val = __pci_attr_r16(devind, limit_reg);
	const u16_t limit_reg_aligned = (limit_val & limit_mask) |
					(~limit_mask & 0xFFFF);
	const u32_t limit = ((u32_t)limit_reg_aligned << 16) | 0xFFFF;

	if (base <= limit) {
		const u32_t size = limit - base + 1;
		if (debug) {
			printf("\t%s: base 0x%x, limit 0x%x, size 0x%x\n",
			       name, base, limit, size);
		}
	}
}

static void
record_bridge_io_window(int devind)
{
	const u8_t iobase_low = __pci_attr_r8(devind, PPB_IOBASE);
	const u16_t iobase_high = __pci_attr_r16(devind, PPB_IOBASEU16);
	const u32_t base = ((u32_t)(iobase_low & PPB_IOB_MASK) << 8) |
			   ((u32_t)iobase_high << 16);

	const u8_t iolimit_low = __pci_attr_r8(devind, PPB_IOLIMIT);
	const u16_t iolimit_high = __pci_attr_r16(devind, PPB_IOLIMITU16);
	const u8_t iolimit_low_aligned = (iolimit_low & PPB_IOL_MASK) |
					 (~PPB_IOL_MASK & 0xFF);
	const u32_t limit = ((u32_t)iolimit_high << 16) |
			    ((u32_t)iolimit_low_aligned << 8) | 0xFF;

	if (base <= limit) {
		const u32_t size = limit - base + 1;
		if (debug) {
			printf("\tI/O window: base 0x%x, limit 0x%x, size %d\n",
			       base, limit, size);
		}
	}
}

static void
record_bars_bridge(int devind)
{
	record_bars(devind, PCI_BAR_2);
	record_bridge_io_window(devind);
	record_bridge_mem_window(devind, "Memory window", PPB_MEMBASE,
				 PPB_MEMLIMIT, PPB_MEMB_MASK, PPB_MEML_MASK);
	record_bridge_mem_window(devind, "Prefetchable memory window",
				 PPB_PFMEMBASE, PPB_PFMEMLIMIT,
				 PPB_PFMEMB_MASK, PPB_PFMEML_MASK);
}

static void
record_cardbus_window(int devind, const char *name, u32_t base_reg,
			u32_t limit_reg, u32_t mask)
{
	u32_t base = __pci_attr_r32(devind, base_reg);
	u32_t limit = __pci_attr_r32(devind, limit_reg);
	u32_t size;

	limit |= ~mask;
	size = limit - base + 1;

	if (debug) {
		printf("\t%s: base 0x%x, limit 0x%x, size %u\n", name, base,
		       limit, size);
	}
}

static void
record_bars_cardbus(int devind)
{
	/* The generic BAR area of CardBus devices is one DWORD in size. */
	record_bars(devind, PCI_BAR);

	record_cardbus_window(devind, "Memory window 0", CBB_MEMBASE_0,
				CBB_MEMLIMIT_0, CBB_MEML_MASK);
	record_cardbus_window(devind, "Memory window 1", CBB_MEMBASE_1,
				CBB_MEMLIMIT_1, CBB_MEML_MASK);
	record_cardbus_window(devind, "I/O window 0", CBB_IOBASE_0,
				CBB_IOLIMIT_0, CBB_IOL_MASK);
	record_cardbus_window(devind, "I/O window 1", CBB_IOBASE_1,
				CBB_IOLIMIT_1, CBB_IOL_MASK);
}

#define PCI_MEM_GAP_HIGH 0xfe000000
#define PCI_IO_GAP_HIGH 0x10000
#define PCI_IO_GAP_LOW 0x400
#define ISA_IO_MASK 0xfcff

static void
find_memory_gap(u32_t *memgap_low, u32_t *memgap_high)
{
	kinfo_t kinfo;
	if (OK != sys_getkinfo(&kinfo)) {
		panic("can't get kinfo");
	}

	*memgap_low = kinfo.mem_high_phys;
	*memgap_high = PCI_MEM_GAP_HIGH;

	if (debug) {
		printf("complete_bars: initial mem gap: [0x%x .. 0x%x>\n",
			*memgap_low, *memgap_high);
	}

	for (int i = 0; i < nr_pcidev; i++) {
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++) {
			pci_bar_t *bar = &pcidev[i].pd_bar[j];
			if ((bar->pb_flags & (PBF_IO | PBF_INCOMPLETE)) != 0) {
				continue;
			}

			u32_t base = bar->pb_base;
			u32_t size = bar->pb_size;

			if (base >= *memgap_high || (base + size) <= *memgap_low) {
				continue;
			}

			if ((base + size - *memgap_low) < (*memgap_high - base)) {
				*memgap_low = base + size;
			} else {
				*memgap_high = base;
			}
		}
	}

	if (debug) {
		printf("complete_bars: intermediate mem gap: [0x%x .. 0x%x>\n",
			*memgap_low, *memgap_high);
	}

	if (*memgap_high < *memgap_low) {
		printf("PCI: bad memory gap: [0x%x .. 0x%x>\n",
			*memgap_low, *memgap_high);
		panic("PCI: bad memory gap");
	}
}

static void
find_io_gap(u32_t *iogap_low, u32_t *iogap_high)
{
	*iogap_low = PCI_IO_GAP_LOW;
	*iogap_high = PCI_IO_GAP_HIGH;

	for (int i = 0; i < nr_pcidev; i++) {
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++) {
			pci_bar_t *bar = &pcidev[i].pd_bar[j];
			if (!(bar->pb_flags & PBF_IO) ||
			    (bar->pb_flags & PBF_INCOMPLETE)) {
				continue;
			}

			u32_t base = bar->pb_base;
			u32_t size = bar->pb_size;

			if (base >= *iogap_high || (base + size) <= *iogap_low) {
				continue;
			}

			if ((base + size - *iogap_low) < (*iogap_high - base)) {
				*iogap_low = base + size;
			} else {
				*iogap_high = base;
			}
		}
	}

	if (*iogap_high < *iogap_low) {
		panic("iogap_high too low: %d", *iogap_high);
	}

	if (debug) {
		printf("I/O range = [0x%x..0x%x>\n", *iogap_low, *iogap_high);
	}
}

static void
allocate_memory_bars_for_device(int dev_idx, u32_t memgap_low,
	u32_t *memgap_high)
{
	pci_dev_t *pdev = &pcidev[dev_idx];

	for (int j = 0; j < pdev->pd_bar_nr; j++) {
		pci_bar_t *bar = &pdev->pd_bar[j];
		if ((bar->pb_flags & PBF_IO) || !(bar->pb_flags & PBF_INCOMPLETE)) {
			continue;
		}

		u32_t size = bar->pb_size;
		if (size < PAGE_SIZE) {
			size = PAGE_SIZE;
		}

		u32_t base = *memgap_high - size;
		base &= ~(size - 1);

		if (base < memgap_low) {
			panic("memory base too low: %d", base);
		}
		*memgap_high = base;

		int bar_nr = bar->pb_nr;
		int reg = PCI_BAR + 4 * bar_nr;
		u32_t v32 = __pci_attr_r32(dev_idx, reg);
		__pci_attr_w32(dev_idx, reg, v32 | base);

		if (debug) {
			printf(
		"complete_bars: allocated 0x%x size %d to %d.%d.%d, bar_%d\n",
				base, size, pdev->pd_busnr, pdev->pd_dev,
				pdev->pd_func, bar_nr);
		}

		bar->pb_base = base;
		bar->pb_flags &= ~PBF_INCOMPLETE;
	}
}

static void
allocate_io_bars_for_device(int dev_idx, u32_t iogap_low, u32_t *iogap_high)
{
	pci_dev_t *pdev = &pcidev[dev_idx];
	u32_t initial_io_high = *iogap_high;

	for (int j = 0; j < pdev->pd_bar_nr; j++) {
		pci_bar_t *bar = &pdev->pd_bar[j];
		if (!(bar->pb_flags & PBF_IO) ||
		    !(bar->pb_flags & PBF_INCOMPLETE)) {
			continue;
		}

		u32_t size = bar->pb_size;
		u32_t base = *iogap_high - size;
		base &= ~(size - 1);
		base &= ISA_IO_MASK;

		if (base < iogap_low) {
			printf("I/O base too low: %d", base);
		}
		*iogap_high = base;

		int bar_nr = bar->pb_nr;
		int reg = PCI_BAR + 4 * bar_nr;
		u32_t v32 = __pci_attr_r32(dev_idx, reg);
		__pci_attr_w32(dev_idx, reg, v32 | base);

		if (debug) {
			printf(
		"complete_bars: allocated 0x%x size %d to %d.%d.%d, bar_%d\n",
				base, size, pdev->pd_busnr, pdev->pd_dev,
				pdev->pd_func, bar_nr);
		}

		bar->pb_base = base;
		bar->pb_flags &= ~PBF_INCOMPLETE;
	}

	if (*iogap_high != initial_io_high) {
		update_bridge4dev_io(dev_idx, *iogap_high,
			initial_io_high - *iogap_high);
	}
}

static void
report_unallocated_bars(void)
{
	for (int i = 0; i < nr_pcidev; i++) {
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++) {
			if (pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE) {
				printf("should allocate resources for device %d\n", i);
			}
		}
	}
}

static void
complete_bars(void)
{
	u32_t memgap_low, memgap_high;
	u32_t iogap_low, iogap_high;

	find_memory_gap(&memgap_low, &memgap_high);
	find_io_gap(&iogap_low, &iogap_high);

	for (int i = 0; i < nr_pcidev; i++) {
		allocate_memory_bars_for_device(i, memgap_low, &memgap_high);
		allocate_io_bars_for_device(i, iogap_low, &iogap_high);
	}

	report_unallocated_bars();
}

/*===========================================================================*
 *				PCI Bridge Helpers			     *
 *===========================================================================*/
#define PCI_MAX_DEVICES 32
#define PCI_MAX_FUNCTIONS 8

static void
handle_bad_status_flags(u16_t sts)
{
	if (sts & (PSR_SSE | PSR_RMAS | PSR_RTAS)) {
		static int warned = 0;
		if (!warned) {
			printf("PCI: ignoring bad value 0x%x in sts for QEMU\n",
			       sts & (PSR_SSE | PSR_RMAS | PSR_RTAS));
			warned = 1;
		}
	}
}

static void
log_found_device(int busnr, uint32_t dev, uint32_t func, int devind,
                 u16_t vid, u16_t did, u16_t sub_vid, u16_t sub_did)
{
	const char *dstr = _pci_dev_name(vid, did);

	if (dstr) {
		printf("%d.%u.%u: %s (%04X:%04X)\n", busnr, dev, func, dstr,
		       vid, did);
	} else {
		printf("%d.%u.%u: Unknown device, vendor %04X (%s), device %04X\n",
		       busnr, dev, func, vid, pci_vid_name(vid), did);
	}
	printf("Device index: %d\n", devind);
	printf("Subsystem: Vid 0x%x, did 0x%x\n", sub_vid, sub_did);
}

static void
log_device_class(u8_t baseclass, u8_t subclass, u8_t infclass)
{
	const char *s = pci_subclass_name(baseclass << 24 | subclass << 16);

	if (!s) {
		s = pci_baseclass_name(baseclass << 24);
	}
	if (!s) {
		s = "(unknown class)";
	}
	printf("\tclass %s (%X/%X/%X)\n", s, baseclass, subclass, infclass);
}

static void
process_pci_bars(int devind, u8_t headt)
{
	switch (headt & PHT_MASK) {
	case PHT_NORMAL:
		record_bars_normal(devind);
		break;
	case PHT_BRIDGE:
		record_bars_bridge(devind);
		break;
	case PHT_CARDBUS:
		record_bars_cardbus(devind);
		break;
	default:
		printf("\t%d.%u.%u: unknown header type %u\n",
		       pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
		       pcidev[devind].pd_func,
		       (unsigned int)(headt & PHT_MASK));
		break;
	}
}

static void
record_device_details(int devind, u8_t baseclass, u8_t subclass,
                      u8_t infclass, u16_t vid, u16_t did, u16_t sub_vid,
                      u16_t sub_did)
{
	pcidev[devind].pd_baseclass = baseclass;
	pcidev[devind].pd_subclass = subclass;
	pcidev[devind].pd_infclass = infclass;
	pcidev[devind].pd_vid = vid;
	pcidev[devind].pd_did = did;
	pcidev[devind].pd_sub_vid = sub_vid;
	pcidev[devind].pd_sub_did = sub_did;
	pcidev[devind].pd_inuse = 0;
	pcidev[devind].pd_bar_nr = 0;
}

static bool
probe_pci_function(int busnr, uint32_t dev, uint32_t func, u8_t *headt_out)
{
	if (nr_pcidev >= NR_PCIDEV) {
		panic("too many PCI devices: %d", nr_pcidev);
	}
	const int devind = nr_pcidev;

	pcidev[devind].pd_busnr = busnr;
	pcidev[devind].pd_dev = dev;
	pcidev[devind].pd_func = func;

	pci_attr_wsts(devind, PSR_SSE | PSR_RMAS | PSR_RTAS);
	const u16_t vid = __pci_attr_r16(devind, PCI_VID);
	const u16_t did = __pci_attr_r16(devind, PCI_DID);

	if (vid == NO_VID && did == NO_VID) {
		return false;
	}

	*headt_out = __pci_attr_r8(devind, PCI_HEADT);
	const u16_t sts = pci_attr_rsts(devind);

	handle_bad_status_flags(sts);

	const u16_t sub_vid = __pci_attr_r16(devind, PCI_SUBVID);
	const u16_t sub_did = __pci_attr_r16(devind, PCI_SUBDID);
	const u8_t baseclass = __pci_attr_r8(devind, PCI_BCR);
	const u8_t subclass = __pci_attr_r8(devind, PCI_SCR);
	const u8_t infclass = __pci_attr_r8(devind, PCI_PIFR);

	if (debug) {
		log_found_device(busnr, dev, func, devind, vid, did, sub_vid,
		                 sub_did);
		log_device_class(baseclass, subclass, infclass);
	}

	if (is_duplicate(busnr, dev, func)) {
		if (debug) {
			printf("\tduplicate!\n");
		}
		return true;
	}

	record_device_details(devind, baseclass, subclass, infclass, vid, did,
	                      sub_vid, sub_did);
	record_irq(devind);
	process_pci_bars(devind, *headt_out);

	if (debug) {
		print_capabilities(devind);
	}

	nr_pcidev++;
	return true;
}

static void
probe_bus(int busind)
{
	if (debug) {
		printf("probe_bus(%d)\n", busind);
	}

	const int busnr = pcibus[busind].pb_busnr;

	for (uint32_t dev = 0; dev < PCI_MAX_DEVICES; dev++) {
		u8_t headt = 0;
		bool device_on_func0 =
		    probe_pci_function(busnr, dev, 0, &headt);

		if (!device_on_func0) {
			continue;
		}

		if (!(headt & PHT_MULTIFUNC)) {
			continue;
		}

		for (uint32_t func = 1; func < PCI_MAX_FUNCTIONS; func++) {
			probe_pci_function(busnr, dev, func, &headt);
		}
	}
}


static u16_t
pcibr_std_rsts(int busind)
{
	return __pci_attr_r16(pcibus[busind].pb_devind, PPB_SSTS);
}

static void
pcibr_std_wsts(int busind, u16_t value)
{
	if (busind < 0 || busind >= PCIBUS_MAX) {
		return;
	}

	const int devind = pcibus[busind].pb_devind;

	if (devind >= 0) {
		__pci_attr_w16(devind, PPB_SSTS, value);
	}
}

static u16_t
pcibr_cb_rsts(int busind)
{
	if ((unsigned int)busind >= NUM_PCI_BUSES) {
		return (u16_t)-1;
	}

	return __pci_attr_r16(pcibus[busind].pb_devind, CBB_SSTS);
}

static void
pcibr_cb_wsts(int busind, u16_t value)
{
	/* sizeof is evaluated at compile time for static arrays */
	if (busind < 0 || busind >= (int)(sizeof(pcibus) / sizeof(pcibus[0]))) {
		/* Invalid bus index, do nothing to prevent out-of-bounds access */
		return;
	}

	const int devind = pcibus[busind].pb_devind;
	__pci_attr_w16(devind, CBB_SSTS, value);
}

static u16_t
pcibr_via_rsts(int busind)
{
    (void)busind;
    return 0;
}

static void
pcibr_via_wsts(int busind, u16_t value)
{
	(void)busind;
	(void)value;
}

static void
complete_bridges(void)
{
	int i, freebus, devind, prim_busnr;

	for (i = 0; i < nr_pcibus; i++)
	{
		if (pcibus[i].pb_needinit)
		{
			freebus = get_freebus();
			devind = pcibus[i].pb_devind;
			prim_busnr = pcidev[devind].pd_busnr;

			pcibus[i].pb_needinit = 0;
			pcibus[i].pb_busnr = freebus;

			__pci_attr_w8(devind, PPB_PRIMBN, prim_busnr);
			__pci_attr_w8(devind, PPB_SECBN, freebus);
			__pci_attr_w8(devind, PPB_SUBORDBN, freebus);
		}
	}
}

static int
get_and_validate_bridge_type(int devind, u16_t vid, u16_t did)
{
	u8_t headt = __pci_attr_r8(devind, PCI_HEADT);
	int type;

	switch (headt & PHT_MASK) {
	case PHT_BRIDGE:
		type = PCI_PPB_STD;
		break;
	case PHT_CARDBUS:
		type = PCI_PPB_CB;
		break;
	default:
		return 0; /* Not a bridge */
	}

	u8_t baseclass = __pci_attr_r8(devind, PCI_BCR);
	u8_t subclass = __pci_attr_r8(devind, PCI_SCR);
	u8_t infclass = __pci_attr_r8(devind, PCI_PIFR);
	u32_t class_code = ((u32_t)baseclass << 16) | ((u32_t)subclass << 8) |
	    infclass;

	if (type == PCI_PPB_STD && class_code != PCI_T3_PCI2PCI &&
	    class_code != PCI_T3_PCI2PCI_SUBTR) {
		printf(
		    "Unknown PCI class %02x/%02x/%02x for PCI-to-PCI bridge, device %04X:%04X\n",
		    baseclass, subclass, infclass, vid, did);
		return 0;
	}

	if (type == PCI_PPB_CB && class_code != PCI_T3_CARDBUS) {
		printf(
		    "Unknown PCI class %02x/%02x/%02x for Cardbus bridge, device %04X:%04X\n",
		    baseclass, subclass, infclass, vid, did);
		return 0;
	}

	return type;
}

static void
setup_new_pcibus(int new_bus_ind, int parent_bus_ind, int devind, int type,
    u8_t sbusn)
{
	pcibus[new_bus_ind].pb_devind = devind;
	pcibus[new_bus_ind].pb_busnr = sbusn;
	pcibus[new_bus_ind].pb_isabridge_dev = -1;
	pcibus[new_bus_ind].pb_isabridge_type = 0;
	pcibus[new_bus_ind].pb_needinit = 1;

	pcibus[new_bus_ind].pb_rreg8 = pcibus[parent_bus_ind].pb_rreg8;
	pcibus[new_bus_ind].pb_rreg16 = pcibus[parent_bus_ind].pb_rreg16;
	pcibus[new_bus_ind].pb_rreg32 = pcibus[parent_bus_ind].pb_rreg32;
	pcibus[new_bus_ind].pb_wreg8 = pcibus[parent_bus_ind].pb_wreg8;
	pcibus[new_bus_ind].pb_wreg16 = pcibus[parent_bus_ind].pb_wreg16;
	pcibus[new_bus_ind].pb_wreg32 = pcibus[parent_bus_ind].pb_wreg32;

	if (type == PCI_PPB_CB) {
		pcibus[new_bus_ind].pb_type = PBT_CARDBUS;
		pcibus[new_bus_ind].pb_rsts = pcibr_cb_rsts;
		pcibus[new_bus_ind].pb_wsts = pcibr_cb_wsts;
	} else { /* PCI_PPB_STD */
		pcibus[new_bus_ind].pb_type = PBT_PCIBRIDGE;
		pcibus[new_bus_ind].pb_rsts = pcibr_std_rsts;
		pcibus[new_bus_ind].pb_wsts = pcibr_std_wsts;
	}
}

static void
do_pcibridge(int busind)
{
	int busnr = pcibus[busind].pb_busnr;

	for (int devind = 0; devind < nr_pcidev; devind++) {
		if (pcidev[devind].pd_busnr != busnr) {
			continue;
		}

		u16_t vid = pcidev[devind].pd_vid;
		u16_t did = pcidev[devind].pd_did;

		int type = get_and_validate_bridge_type(devind, vid, did);
		if (type == 0) {
			continue;
		}

		if (debug) {
			printf("%u.%u.%u: PCI-to-PCI bridge: %04X:%04X\n",
			    pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
			    pcidev[devind].pd_func, vid, did);
		}

		u8_t sbusn = __pci_attr_r8(devind, PPB_SECBN);

		if (nr_pcibus >= NR_PCIBUS) {
			panic("too many PCI busses: %d", nr_pcibus);
		}
		int ind = nr_pcibus++;

		setup_new_pcibus(ind, busind, devind, type, sbusn);

		if (machine.apic_enabled) {
			acpi_map_bridge(pcidev[devind].pd_busnr,
			    pcidev[devind].pd_dev, sbusn);
		}

		if (debug) {
			printf(
			    "bus(table) = %d, bus(sec) = %d, bus(subord) = %d\n",
			    ind, sbusn, __pci_attr_r8(devind, PPB_SUBORDBN));
		}

		if (sbusn == 0) {
			printf("Secondary bus number not initialized\n");
			continue;
		}
		pcibus[ind].pb_needinit = 0;

		probe_bus(ind);

		do_pcibridge(ind);
	}
}

/*===========================================================================*
 *				pci_intel_init				     *
 *===========================================================================*/
static void
initialize_pci_bus(int bus_index)
{
	pcibus[bus_index].pb_type = PBT_INTEL_HOST;
	pcibus[bus_index].pb_needinit = 0;
	pcibus[bus_index].pb_isabridge_dev = -1;
	pcibus[bus_index].pb_isabridge_type = 0;
	pcibus[bus_index].pb_devind = -1;
	pcibus[bus_index].pb_busnr = 0;
	pcibus[bus_index].pb_rreg8 = pcii_rreg8;
	pcibus[bus_index].pb_rreg16 = pcii_rreg16;
	pcibus[bus_index].pb_rreg32 = pcii_rreg32;
	pcibus[bus_index].pb_wreg8 = pcii_wreg8;
	pcibus[bus_index].pb_wreg16 = pcii_wreg16;
	pcibus[bus_index].pb_wreg32 = pcii_wreg32;
	pcibus[bus_index].pb_rsts = pcii_rsts;
	pcibus[bus_index].pb_wsts = pcii_wsts;
}

static void
disable_devices_on_bus(int bus_number)
{
	for (int i = 0; i < nr_pcidev; i++) {
		if (pcidev[i].pd_busnr == bus_number) {
			pcidev[i].pd_inuse = 1;
		}
	}
}

static void
pci_intel_init(void)
{
	const u32_t bus = 0;
	const u32_t dev = 0;
	const u32_t func = 0;

	u16_t vid = PCII_RREG16_(bus, dev, func, PCI_VID);
	u16_t did = PCII_RREG16_(bus, dev, func, PCI_DID);

	int s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}

	if (nr_pcibus >= NR_PCIBUS) {
		panic("too many PCI busses: %d", nr_pcibus);
	}

	int busind = nr_pcibus;
	nr_pcibus++;
	initialize_pci_bus(busind);

	if (debug) {
		const char *dstr = _pci_dev_name(vid, did);
		if (!dstr) {
			dstr = "unknown device";
		}
		printf("pci_intel_init: %s (%04X:%04X)\n", dstr, vid, did);
	}

	probe_bus(busind);

	if (do_isabridge(busind) != OK) {
		disable_devices_on_bus(pcibus[busind].pb_busnr);
		return;
	}

	do_pcibridge(busind);
	complete_bridges();
	complete_bars();
}

#if 0
/*===========================================================================*
 *				report_vga				     *
 *===========================================================================*/
static void
report_vga(int devind)
{
	/* Report the amount of video memory. This is needed by the X11R6
	 * postinstall script to chmem the X server. Hopefully this can be
	 * removed when we get virtual memory.
	 */
	size_t amount, size;
	int i;

	amount= 0;
	for (i= 0; i<pcidev[devind].pd_bar_nr; i++)
	{
		if (pcidev[devind].pd_bar[i].pb_flags & PBF_IO)
			continue;
		size= pcidev[devind].pd_bar[i].pb_size;
		if (size < amount)
			continue;
		amount= size;
	}
	if (size != 0)
	{
		printf("PCI: video memory for device at %d.%d.%d: %d bytes\n",
			pcidev[devind].pd_busnr,
			pcidev[devind].pd_dev,
			pcidev[devind].pd_func,
			amount);
	}
}
#endif


/*===========================================================================*
 *				visible					     *
 *===========================================================================*/
static int
device_is_a_match(const struct rs_pci_device *acl_entry,
                  const typeof(pcidev[0]) *dev)
{
	return (acl_entry->vid == dev->pd_vid &&
		acl_entry->did == dev->pd_did &&
		(acl_entry->sub_vid == NO_SUB_VID ||
		 acl_entry->sub_vid == dev->pd_sub_vid) &&
		(acl_entry->sub_did == NO_SUB_DID ||
		 acl_entry->sub_did == dev->pd_sub_did));
}

static int
class_is_a_match(const struct rs_pci_class *acl_entry, u32_t class_id)
{
	return acl_entry->pciclass == (class_id & acl_entry->mask);
}

static int
visible(const struct rs_pci *aclp, int devind)
{
	int i;

	if (!aclp) {
		return TRUE;
	}

	const typeof(pcidev[0]) *dev = &pcidev[devind];

	for (i = 0; i < aclp->rsp_nr_device; i++) {
		if (device_is_a_match(&aclp->rsp_device[i], dev)) {
			return TRUE;
		}
	}

	if (aclp->rsp_nr_class == 0) {
		return FALSE;
	}

	const u32_t class_id = ((u32_t)dev->pd_baseclass << 16) |
			       ((u32_t)dev->pd_subclass << 8) |
			       dev->pd_infclass;

	for (i = 0; i < aclp->rsp_nr_class; i++) {
		if (class_is_a_match(&aclp->rsp_class[i], class_id)) {
			return TRUE;
		}
	}

	return FALSE;
}

/*===========================================================================*
 *				sef_cb_init_fresh			     *
 *===========================================================================*/
static void
map_boot_services(sef_init_info_t *info)
{
	struct rprocpub rprocpub[NR_BOOT_PROCS];
	int r;

	r = sys_safecopyfrom(RS_PROC_NR, info->rproctab_gid, 0,
		(vir_bytes)rprocpub, sizeof(rprocpub));
	if (r != OK) {
		panic("sys_safecopyfrom failed: %d", r);
	}

	for (int i = 0; i < NR_BOOT_PROCS; i++) {
		if (rprocpub[i].in_use) {
			r = map_service(&rprocpub[i]);
			if (r != OK) {
				panic("unable to map service: %d", r);
			}
		}
	}
}

int
sef_cb_init(int type, sef_init_info_t *info)
{
	long v = 0;
	env_parse("pci_debug", "d", 0, &v, 0, 1);
	debug = v;

	if (sys_getmachine(&machine)) {
		printf("PCI: no machine\n");
		return ENODEV;
	}

	if (machine.apic_enabled && acpi_init() != OK) {
		panic("PCI: Cannot use APIC mode without ACPI!\n");
	}

	pci_intel_init();

	map_boot_services(info);

	bool do_announce_driver;
	switch (type) {
	case SEF_INIT_FRESH:
	case SEF_INIT_RESTART:
		do_announce_driver = true;
		break;
	case SEF_INIT_LU:
		do_announce_driver = false;
		break;
	default:
		panic("Unknown type of restart");
	}

	if (do_announce_driver) {
		chardriver_announce();
	}

	return OK;
}

/*===========================================================================*
 *		               map_service                                   *
 *===========================================================================*/
int
map_service(struct rprocpub *rpub)
{
	int i;

	if (rpub->pci_acl.rsp_nr_device == 0 && rpub->pci_acl.rsp_nr_class == 0)
	{
		return OK;
	}

	for (i = 0; i < NR_DRIVERS; i++)
	{
		if (!pci_acl[i].inuse)
		{
			pci_acl[i].inuse = 1;
			pci_acl[i].acl = rpub->pci_acl;
			return OK;
		}
	}

	printf("PCI: map_service: table is full\n");
	return ENOMEM;
}

/*===========================================================================*
 *				_pci_find_dev				     *
 *===========================================================================*/
int
_pci_find_dev(const u8_t bus, const u8_t dev, const u8_t func, int *devindp)
{
	if (devindp == NULL)
	{
		return 0;
	}

	for (int devind = 0; devind < nr_pcidev; devind++)
	{
		if (pcidev[devind].pd_busnr == bus &&
			pcidev[devind].pd_dev == dev &&
			pcidev[devind].pd_func == func)
		{
			*devindp = devind;
			return 1;
		}
	}

	return 0;
}

/*===========================================================================*
 *				_pci_first_dev				     *
 *===========================================================================*/
int
_pci_first_dev(struct rs_pci *aclp, int *devindp, u16_t *vidp,
	u16_t *didp)
{
	if (!devindp || !vidp || !didp)
	{
		return 0;
	}

	for (int devind = 0; devind < nr_pcidev; devind++)
	{
		if (visible(aclp, devind))
		{
			*devindp = devind;
			*vidp = pcidev[devind].pd_vid;
			*didp = pcidev[devind].pd_did;
			return 1;
		}
	}

	return 0;
}

/*===========================================================================*
 *				_pci_next_dev				     *
 *===========================================================================*/
int
_pci_next_dev(struct rs_pci *aclp, int *devindp, u16_t *vidp, u16_t *didp)
{
	if (!aclp || !devindp || !vidp || !didp) {
		return 0;
	}

	for (int devind = *devindp + 1; devind < nr_pcidev; devind++) {
		if (visible(aclp, devind)) {
			*devindp = devind;
			*vidp = pcidev[devind].pd_vid;
			*didp = pcidev[devind].pd_did;
			return 1;
		}
	}

	return 0;
}

/*===========================================================================*
 *				_pci_grant_access			     *
 *===========================================================================*/
int
_pci_grant_access(int devind, endpoint_t proc)
{
	int i;
	int result = OK;
	struct pci_device *dev = &pcidev[devind];

	for (i = 0; i < dev->pd_bar_nr; i++) {
		struct pci_bar *bar = &dev->pd_bar[i];
		int status;
		int priv_cmd;
		void *priv_data;
		struct io_range ior;
		struct minix_mem_range mr;

		if (bar->pb_flags & PBF_INCOMPLETE) {
			printf("_pci_grant_access: BAR %d is incomplete\n", i);
			continue;
		}

		if (bar->pb_flags & PBF_IO) {
			ior.ior_base = bar->pb_base;
			ior.ior_limit = ior.ior_base + bar->pb_size - 1;
			priv_cmd = SYS_PRIV_ADD_IO;
			priv_data = &ior;

			if (debug) {
				printf(
		"_pci_grant_access: for proc %d, adding I/O range [0x%x..0x%x]\n",
		proc, ior.ior_base, ior.ior_limit);
			}
		} else {
			mr.mr_base = bar->pb_base;
			mr.mr_limit = mr.mr_base + bar->pb_size - 1;
			priv_cmd = SYS_PRIV_ADD_MEM;
			priv_data = &mr;
		}

		status = sys_privctl(proc, priv_cmd, priv_data);
		if (status != OK) {
			printf("sys_privctl failed for proc %d: %d\n", proc,
				status);
			if (result == OK) {
				result = status;
			}
		}
	}

	int irq_line = dev->pd_ilr;
	if (irq_line != PCI_ILR_UNKNOWN) {
		if (debug) {
			printf("_pci_grant_access: adding IRQ %d\n", irq_line);
		}

		int status = sys_privctl(proc, SYS_PRIV_ADD_IRQ, &irq_line);
		if (status != OK) {
			printf("sys_privctl failed for proc %d: %d\n", proc,
				status);
			if (result == OK) {
				result = status;
			}
		}
	}

	return result;
}

/*===========================================================================*
 *				_pci_reserve				     *
 *===========================================================================*/
int
_pci_reserve(int devind, endpoint_t proc, struct rs_pci *aclp)
{
	if (devind < 0 || devind >= nr_pcidev) {
		printf("pci_reserve_a: bad devind: %d\n", devind);
		return EINVAL;
	}

	if (!visible(aclp, devind)) {
		printf("pci_reserve_a: %u is not allowed to reserve %d\n",
			proc, devind);
		return EPERM;
	}

	struct pci_device *dev = &pcidev[devind];

	if (dev->pd_inuse && dev->pd_proc != proc) {
		return EBUSY;
	}

	dev->pd_inuse = 1;
	dev->pd_proc = proc;

	return _pci_grant_access(devind, proc);
}

/*===========================================================================*
 *				_pci_release				     *
 *===========================================================================*/
void
_pci_release(endpoint_t proc)
{
	for (int i = 0; i < nr_pcidev; i++) {
		if (pcidev[i].pd_inuse && pcidev[i].pd_proc == proc) {
			pcidev[i].pd_inuse = 0;
		}
	}
}

/*===========================================================================*
 *				_pci_ids				     *
 *===========================================================================*/
int
_pci_ids(int devind, u16_t *vidp, u16_t *didp)
{
	if (vidp == NULL || didp == NULL) {
		return EINVAL;
	}

	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}

	*vidp = pcidev[devind].pd_vid;
	*didp = pcidev[devind].pd_did;

	return OK;
}

/*===========================================================================*
 *				_pci_rescan_bus				     *
 *===========================================================================*/
void
_pci_rescan_bus(u8_t busnr)
{
	int busind = get_busind(busnr);
	if (busind < 0) {
		return;
	}

	probe_bus(busind);

	/* Allocate bus numbers for uninitialized bridges */
	complete_bridges();

	/* Allocate I/O and memory resources for uninitialized devices */
	complete_bars();
}

/*===========================================================================*
 *				_pci_slot_name				     *
 *===========================================================================*/
int
_pci_slot_name(int devind, char **cpp)
{
	static char label[] = "ddd.ddd.ddd.ddd";
	int n;

	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}

	/* FIXME: domain nb is always 0 on 32bit system, but we should
	 *        retrieve it properly, somehow. */
	n = snprintf(label, sizeof(label), "%d.%d.%d.%d", 0,
	    pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
	    pcidev[devind].pd_func);

	if (n < 0 || (size_t)n >= sizeof(label)) {
		return EINVAL;
	}

	*cpp = label;
	return OK;
}

/*===========================================================================*
 *				_pci_dev_name				     *
 *===========================================================================*/
const char *
_pci_dev_name(u16_t vid, u16_t did)
{
	static _Thread_local char product[PCI_PRODUCTSTR_LEN];

	if (pci_findproduct(product, sizeof(product), vid, did) != 0) {
		return "Unknown";
	}

	return product;
}

/*===========================================================================*
 *				_pci_get_bar				     *
 *===========================================================================*/
int
_pci_get_bar(int devind, int port, u32_t *base, u32_t *size,
	int *ioflag)
{
	if (base == NULL || size == NULL || ioflag == NULL) {
		return EINVAL;
	}

	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}

	const struct pci_device *dev = &pcidev[devind];
	for (int i = 0; i < dev->pd_bar_nr; i++) {
		const struct pci_bar *bar = &dev->pd_bar[i];
		int reg = PCI_BAR + (4 * bar->pb_nr);

		if (reg == port) {
			if ((bar->pb_flags & PBF_INCOMPLETE) != 0) {
				return EINVAL;
			}

			*base = bar->pb_base;
			*size = bar->pb_size;
			*ioflag = (bar->pb_flags & PBF_IO) != 0;
			return OK;
		}
	}

	return EINVAL;
}

/*===========================================================================*
 *				_pci_attr_r8				     *
 *===========================================================================*/
int
_pci_attr_r8(int devind, int port, u8_t *vp)
{
	if (vp == NULL || devind < 0 || devind >= nr_pcidev || port < 0 || port > 255) {
		return EINVAL;
	}

	*vp = __pci_attr_r8(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_r16				     *
 *===========================================================================*/
int
_pci_attr_r16(int devind, int port, u16_t *value)
{
	if (!value || devind < 0 || devind >= nr_pcidev || port < 0 || port > 254)
		return EINVAL;

	*value = __pci_attr_r16(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_r32				     *
 *===========================================================================*/
int
_pci_attr_r32(int devind, int port, u32_t *vp)
{
	if (vp == NULL || devind < 0 || devind >= nr_pcidev || port < 0 ||
	    port > 252 || (port & 3) != 0) {
		return EINVAL;
	}

	*vp = __pci_attr_r32(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w8				     *
 *===========================================================================*/
int
_pci_attr_w8(int devind, int port, u8_t value)
{
	if (devind < 0 || devind >= nr_pcidev || port < 0 || port > 255) {
		return EINVAL;
	}

	__pci_attr_w8(devind, port, value);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w16				     *
 *===========================================================================*/
int
_pci_attr_w16(int devind, int port, u16_t value)
{
	const unsigned int pci_conf_space_size = 256;
	const int max_port = pci_conf_space_size - sizeof(u16_t);

	if (devind < 0 || devind >= nr_pcidev ||
	    port < 0 || port > max_port || (port % sizeof(u16_t) != 0)) {
		return EINVAL;
	}

	__pci_attr_w16(devind, port, value);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w32				     *
 *===========================================================================*/
int
_pci_attr_w32(int devind, int port, u32_t value)
{
	const int max_port = 256 - sizeof(u32_t);

	if (devind < 0 || devind >= nr_pcidev || port < 0 || port > max_port) {
		return EINVAL;
	}

	__pci_attr_w32(devind, port, value);
	return OK;
}
