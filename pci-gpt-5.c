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
pci_inb(u16_t port) {
	u32_t value = 0;
	int s = sys_inb(port, &value);
	if (s != OK)
		printf("PCI: warning, sys_inb failed: %d\n", s);
	return (unsigned)(value & 0xFFu);
}

static unsigned pci_inw(u16_t port)
{
    u32_t value = 0;
    int status = sys_inw(port, &value);
    if (status != OK) {
        printf("PCI: warning, sys_inw failed: %d\n", status);
    }
    return (unsigned)(value & 0xFFFFu);
}

static unsigned pci_inl(u16_t port) {
    u32_t value = 0;
    int s = sys_inl(port, &value);
    if (s != OK) {
        printf("PCI: warning, sys_inl failed: %d\n", s);
        return 0U;
    }
    return (unsigned)value;
}

static void pci_outb(u16_t port, u8_t value)
{
    const int status = sys_outb(port, value);
    if (status != OK) {
        (void)printf("PCI: warning, sys_outb failed: %d\n", status);
    }
}

static void
pci_outw(u16_t port, u16_t value)
{
    int status = sys_outw(port, value);
    if (status != OK) {
        printf("PCI: warning, sys_outw failed: %d\n", status);
    }
}

static void pci_outl(u16_t port, u32_t value)
{
    int status = sys_outl(port, value);
    if (status != OK) {
        (void)printf("PCI: warning, sys_outl failed: %d\n", status);
    }
}

static u8_t pcii_rreg8(int busind, int devind, int port)
{
    const u8_t value = PCII_RREG8_(pcibus[busind].pb_busnr,
                                   pcidev[devind].pd_dev,
                                   pcidev[devind].pd_func,
                                   port);
    int status = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (status != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", status);
    }
    return value;
}

static u16_t
pcii_rreg16(int busind, int devind, int port)
{
	u16_t value;
	int status;

	value = PCII_RREG16_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func, port);

	status = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (status != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", status);
	}

	return value;
}

static u32_t
pcii_rreg32(int busind, int devind, int port)
{
	u32_t value;
	int status;
	const int busnr = pcibus[busind].pb_busnr;
	const int dev = pcidev[devind].pd_dev;
	const int func = pcidev[devind].pd_func;

	value = PCII_RREG32_(busnr, dev, func, port);

	status = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (status != OK)
		printf("PCI: warning, sys_outl failed: %d\n", status);

	return value;
}

static void
pcii_wreg8(int busind, int devind, int port, u8_t value)
{
	int s;

	if (busind < 0 || devind < 0 || port < 0) {
		printf("PCI: warning, invalid arguments: busind=%d devind=%d port=%d\n",
		       busind, devind, port);
		return;
	}

	{
		const int busnr = pcibus[busind].pb_busnr;
		const int dev = pcidev[devind].pd_dev;
		const int func = pcidev[devind].pd_func;

		PCII_WREG8_(busnr, dev, func, port, value);
	}

	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK)
		printf("PCI: warning, sys_outl failed: %d\n", s);
}

static void
pcii_wreg16(int busind, int devind, int port, u16_t value)
{
	int s;
	const unsigned int busnr = pcibus[busind].pb_busnr;
	const unsigned int dev = pcidev[devind].pd_dev;
	const unsigned int func = pcidev[devind].pd_func;

	PCII_WREG16_(busnr, dev, func, port, value);

	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
}

static void
pcii_wreg32(int busind, int devind, int port, u32_t value)
{
	int status;
	const int busnr = pcibus[busind].pb_busnr;
	const int dev = pcidev[devind].pd_dev;
	const int func = pcidev[devind].pd_func;

#if 0
	printf("pcii_wreg32(%d, %d, 0x%X, 0x%X): %d.%d.%d\n",
		busind, devind, port, value, busnr, dev, func);
#endif

	PCII_WREG32_(busnr, dev, func, port, value);

	status = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (status != OK)
		printf("PCI: warning, sys_outl failed: %d\n", status);
}

/*===========================================================================*
 *				ntostr					     *
 *===========================================================================*/
static void
ntostr(unsigned int n, char **str, const char *end)
{
	char *dst;
	char tmp[20];
	int i = 0;

	if (str == NULL || *str == NULL || end == NULL)
		return;

	dst = *str;
	if (dst >= end)
		return;

	do {
		if (i >= (int)sizeof(tmp))
			break;
		tmp[i++] = (char)('0' + (n % 10));
		n /= 10;
	} while (n);

	while (i > 0 && dst < end) {
		*dst++ = tmp[--i];
	}

	if (dst == end) {
		dst[-1] = '\0';
	} else {
		*dst = '\0';
	}

	*str = dst;
}

/*===========================================================================*
 *				get_busind					     *
 *===========================================================================*/
static int
get_busind(int busnr)
{
	int i;

	if (nr_pcibus <= 0) {
		panic("get_busind: can't find bus: %d", busnr);
		return -1;
	}

	for (i = 0; i < nr_pcibus; i++) {
		if (pcibus[i].pb_busnr == busnr)
			return i;
	}

	panic("get_busind: can't find bus: %d", busnr);
	return -1;
}

/*===========================================================================*
 *			Unprotected helper functions			     *
 *===========================================================================*/
static u8_t __pci_attr_r8(int devind, int port)
{
    const int busnr = pcidev[devind].pd_busnr;
    const int busind = get_busind(busnr);
    return pcibus[busind].pb_rreg8(busind, devind, port);
}

static u16_t __pci_attr_r16(int devind, int port)
{
    const int bus_number = pcidev[devind].pd_busnr;
    const int bus_index = get_busind(bus_number);
    return pcibus[bus_index].pb_rreg16(bus_index, devind, port);
}

static u32_t
__pci_attr_r32(int devind, int port)
{
    if (devind < 0) {
        return 0;
    }

    const int busnr = pcidev[devind].pd_busnr;
    const int busind = get_busind(busnr);
    if (busind < 0) {
        return 0;
    }

    u32_t (*read_reg32)(int, int, int) = pcibus[busind].pb_rreg32;
    if (read_reg32 == NULL) {
        return 0;
    }

    return read_reg32(busind, devind, port);
}

static void
__pci_attr_w8(int devind, int port, u8_t value)
{
	if (devind < 0)
		return;

	int busnr = pcidev[devind].pd_busnr;
	int busind = get_busind(busnr);
	if (busind < 0)
		return;

	void (*wreg8)(int, int, int, u8_t) = pcibus[busind].pb_wreg8;
	if (wreg8 == NULL)
		return;

	wreg8(busind, devind, port, value);
}

static void __pci_attr_w16(int devind, int port, u16_t value)
{
    if (devind < 0 || port < 0) {
        return;
    }

    const int busnr = pcidev[devind].pd_busnr;
    const int busind = get_busind(busnr);
    if (busind < 0) {
        return;
    }

    if (pcibus[busind].pb_wreg16 == NULL) {
        return;
    }

    pcibus[busind].pb_wreg16(busind, devind, port, value);
}

static void
__pci_attr_w32(const int devind, const int port, const u32_t value)
{
	const int busind = get_busind(pcidev[devind].pd_busnr);
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
	if (devind < 0)
		return 0;

	const int busind = get_busind(pcidev[devind].pd_busnr);
	if (busind < 0)
		return 0;

	if (pcibus[busind].pb_rsts == NULL)
		return 0;

	return pcibus[busind].pb_rsts(busind);
}

static void
pci_attr_wsts(int devind, u16_t value)
{
    if (devind < 0) {
        return;
    }

    const int busnr = pcidev[devind].pd_busnr;
    const int busind = get_busind(busnr);
    if (busind < 0) {
        return;
    }

    if (pcibus[busind].pb_wsts != NULL) {
        pcibus[busind].pb_wsts(busind, value);
    }
}

static u16_t pcii_rsts(int busind)
{
	u16_t status = PCII_RREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR);
	int rc = sys_outl(PCII_CONFADD, PCII_UNSEL);

	if (rc != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", rc);
	}

	return status;
}

static void
pcii_wsts(int busind, u16_t value)
{
	int status;
	int busnr = pcibus[busind].pb_busnr;

	PCII_WREG16_(busnr, 0, 0, PCI_SR, value);

	status = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (status != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", status);
	}
}

static int
is_duplicate(u8_t busnr, u8_t dev, u8_t func)
{
	int i;
	const int n = nr_pcidev;

	if (n <= 0 || pcidev == NULL)
		return 0;

	for (i = 0; i < n; i++)
	{
		if (pcidev[i].pd_busnr == busnr &&
		    pcidev[i].pd_dev == dev &&
		    pcidev[i].pd_func == func)
		{
			return 1;
		}
	}
	return 0;
}

static int get_freebus(void)
{
	int i;
	int freebus = 1;

	for (i = 0; i < nr_pcibus; i++) {
		if (pcibus[i].pb_needinit || pcibus[i].pb_type == PBT_INTEL_HOST)
			continue;

		if (pcibus[i].pb_busnr <= freebus)
			freebus = pcibus[i].pb_busnr + 1;

		printf("get_freebus: should check suboridinate bus number\n");
	}
	return freebus;
}

static const char *
pci_vid_name(u16_t vid)
{
	static char vendor[PCI_VENDORSTR_LEN];

	vendor[0] = '\0';
	(void)pci_findvendor(vendor, sizeof(vendor), vid);
	vendor[sizeof(vendor) - 1] = '\0';

	return vendor;
}


static void
print_hyper_cap(int devind, u8_t capptr)
{
	u32_t reg;
	u16_t cmd;
	unsigned int type0, type1;
	const u16_t TYPE0_MASK = 0xE000u;
	const u16_t TYPE1_MASK = 0xF800u;
	const unsigned int TYPE0_SHIFT = 13u;
	const unsigned int TYPE1_SHIFT = 11u;
	const unsigned int CMD_SHIFT = 16u;

	printf("\n");
	reg = __pci_attr_r32(devind, capptr);
	printf("print_hyper_cap: @0x%x, off 0 (cap):", (unsigned int)capptr);
	cmd = (u16_t)((reg >> CMD_SHIFT) & 0xFFFFu);

	type0 = (unsigned int)((cmd & TYPE0_MASK) >> TYPE0_SHIFT);
	type1 = (unsigned int)((cmd & TYPE1_MASK) >> TYPE1_SHIFT);

	if (type0 == 0u || type0 == 1u) {
		printf("Capability Type: %s\n",
			type0 == 0u ? "Slave or Primary Interface" :
			"Host or Secondary Interface");
		cmd = (u16_t)(cmd & (u16_t)~TYPE0_MASK);
	} else {
		printf(" Capability Type 0x%x", type1);
		cmd = (u16_t)(cmd & (u16_t)~TYPE1_MASK);
	}

	if (cmd != 0u) {
		printf(" undecoded 0x%x\n", cmd);
	}
}

static const char* capability_type_str(u8_t type)
{
	switch (type)
	{
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

static const char* secure_subtype_str(u8_t subtype)
{
	switch (subtype & 0x07)
	{
	case 0: return "Device Exclusion Vector";
	case 3: return "IOMMU";
	default: return "(unknown type)";
	}
}

static void
print_capabilities(int devind)
{
	u8_t capptr, type, next;
	unsigned int status;
	const char *str;
	u8_t visited[256] = {0};
	int iterations = 0;

	status = __pci_attr_r16(devind, PCI_SR);
	if (!(status & PSR_CAPPTR))
		return;

	capptr = (__pci_attr_r8(devind, PCI_CAPPTR) & PCI_CP_MASK);

	while (capptr != 0)
	{
		if (visited[capptr] || iterations++ >= 256)
			break;
		visited[capptr] = 1;

		type = __pci_attr_r8(devind, capptr + CAP_TYPE);
		next = (__pci_attr_r8(devind, capptr + CAP_NEXT) & PCI_CP_MASK);
		str = capability_type_str(type);

		{
			unsigned int caphdr = __pci_attr_r32(devind, capptr);
			printf(" @0x%x (0x%08x): capability type 0x%x: %s",
			       (unsigned int)capptr, (unsigned int)caphdr, (unsigned int)type, str);
		}

		if (type == 0x08)
		{
			print_hyper_cap(devind, capptr);
		}
		else if (type == 0x0f)
		{
			u8_t subtype = (__pci_attr_r8(devind, capptr + 2) & 0x07);
			const char *sub_str = secure_subtype_str(subtype);
			printf(", sub type 0%o: %s", (unsigned int)subtype, sub_str);
		}

		printf("\n");
		capptr = next;
	}
}

/*===========================================================================*
 *				ISA Bridge Helpers			     *
 *===========================================================================*/
static void
update_bridge4dev_io(int devind, u32_t io_base, u32_t io_size)
{
	int busnr, busind, type, br_devind;
	u16_t v16;
	u32_t io_limit;

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	if (busind < 0)
		panic("update_bridge4dev_io: invalid bus index for bus %d", busnr);

	type = pcibus[busind].pb_type;
	if (type == PBT_INTEL_HOST)
		return;
	if (type == PBT_PCIBRIDGE) {
		printf("update_bridge4dev_io: not implemented for PCI bridges\n");
		return;
	}
	if (type != PBT_CARDBUS)
		panic("update_bridge4dev_io: strange bus type: %d", type);

	if (io_size == 0)
		return;

	if (io_size - 1 > (u32_t)(~(u32_t)0) - io_base)
		io_limit = (u32_t)(~(u32_t)0);
	else
		io_limit = io_base + io_size - 1;

	if (debug)
		printf("update_bridge4dev_io: adding 0x%x at 0x%x\n", io_size, io_base);

	br_devind = pcibus[busind].pb_devind;
	if (br_devind < 0)
		panic("update_bridge4dev_io: invalid bridge device index for bus %d", busnr);

	__pci_attr_w32(br_devind, CBB_IOLIMIT_0, io_limit);
	__pci_attr_w32(br_devind, CBB_IOBASE_0, io_base);

	v16 = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, v16 | PCI_CR_IO_EN | PCI_CR_MAST_EN);
}

static int
do_piix(int devind)
{
	int i;
	int s;
	int irqrc;
	int irq;
	u32_t elcr1 = 0, elcr2 = 0, elcr;

#if DEBUG
	printf("in piix\n");
#endif

	s = sys_inb(PIIX_ELCR1, &elcr1);
	if (s != OK)
		printf("Warning, sys_inb failed: %d\n", s);

	s = sys_inb(PIIX_ELCR2, &elcr2);
	if (s != OK)
		printf("Warning, sys_inb failed: %d\n", s);

	elcr = elcr1 | (elcr2 << 8);

	for (i = 0; i < 4; i++)
	{
		unsigned int val = (unsigned int)__pci_attr_r8(devind, PIIX_PIRQRCA + i) & 0xFF;

		if (val & PIIX_IRQ_DI)
		{
			if (debug)
				printf("INT%c: disabled\n", (char)('A' + i));
			continue;
		}

		irqrc = (int)val;
		irq = irqrc & PIIX_IRQ_MASK;

		if (debug)
			printf("INT%c: %d\n", (char)('A' + i), irq);

		if ((unsigned int)irq < (sizeof(elcr) * 8U))
		{
			if ((elcr & (1U << (unsigned int)irq)) == 0)
			{
				if (debug)
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
	enum { INT_LINES = 4, IRQ_NIBBLE_MASK = 0xF };
	int i, busnr, dev, xdevind;
	u8_t levmask;
	u16_t pciirq;

	busnr = pcidev[devind].pd_busnr;
	dev = pcidev[devind].pd_dev;

	if (nr_pcidev >= NR_PCIDEV)
		panic("too many PCI devices: %d", nr_pcidev);
	xdevind = nr_pcidev;
	pcidev[xdevind].pd_busnr = busnr;
	pcidev[xdevind].pd_dev = dev;
	pcidev[xdevind].pd_func = AMD_ISABR_FUNC;
	pcidev[xdevind].pd_inuse = 1;
	nr_pcidev++;

	levmask = __pci_attr_r8(xdevind, AMD_ISABR_PCIIRQ_LEV);
	pciirq = __pci_attr_r16(xdevind, AMD_ISABR_PCIIRQ_ROUTE);

	for (i = 0; i < INT_LINES; i++) {
		int edge = (levmask >> i) & 1;
		int irq = (pciirq >> (4 * i)) & IRQ_NIBBLE_MASK;
		char intc = (char)('A' + i);

		if (irq == 0) {
			if (debug)
				printf("INT%c: disabled\n", intc);
			continue;
		}

		if (debug) {
			printf("INT%c: %d\n", intc, irq);
			if (edge)
				printf("(warning) IRQ %d is not level triggered\n", irq);
		}
		irq_mode_pci(irq);
	}
	nr_pcidev--;
	return 0;
}

static int
do_sis_isabr(int devind)
{
	int i;
	const int intr_count = 4;

	for (i = 0; i < intr_count; i++)
	{
		int raw = __pci_attr_r8(devind, SIS_ISABR_IRQ_A + i);
		char label = (char)('A' + i);

		if ((raw & SIS_IRQ_DISABLED) != 0)
		{
			if (debug)
				printf("INT%c: disabled\n", label);
			continue;
		}

		{
			int masked_irq = raw & SIS_IRQ_MASK;
			if (debug)
				printf("INT%c: %d\n", label, masked_irq);
			irq_mode_pci(masked_irq);
		}
	}
	return 0;
}

static int
do_via_isabr(int devind)
{
	int i, irq, edge;
	static const int irq_regs[4] = { VIA_ISABR_IRQ_R2, VIA_ISABR_IRQ_R2, VIA_ISABR_IRQ_R3, VIA_ISABR_IRQ_R1 };
	static const unsigned int irq_shifts[4] = { 4, 0, 4, 4 };
	static const u8_t edge_masks[4] = { VIA_ISABR_EL_INTA, VIA_ISABR_EL_INTB, VIA_ISABR_EL_INTC, VIA_ISABR_EL_INTD };
	u8_t levmask;

	levmask = __pci_attr_r8(devind, VIA_ISABR_EL);

	for (i = 0; i < 4; i++)
	{
		irq = __pci_attr_r8(devind, irq_regs[i]);
		if (irq_shifts[i]) irq >>= irq_shifts[i];
		irq &= 0xF;

		edge = (levmask & edge_masks[i]) != 0;

		if (!irq)
		{
			if (debug)
				printf("INT%c: disabled\n", 'A' + i);
			continue;
		}

		if (debug)
			printf("INT%c: %d\n", 'A' + i, irq);

		if (edge && debug)
			printf("(warning) IRQ %d is not level triggered\n", irq);

		irq_mode_pci(irq);
	}

	return 0;
}

static int do_isabridge(int busind)
{
	int i, j = 0, r = 0;
	int type = 0;
	int busnr = pcibus[busind].pb_busnr;
	int isa_class_index = -1;
	int bridge_dev = -1;
	u16_t vid = 0, did = 0;
	u32_t class_signature;
	const char *dstr;

	for (i = 0; i < nr_pcidev; i++)
	{
		if (pcidev[i].pd_busnr != busnr)
			continue;

		class_signature = ((pcidev[i].pd_baseclass << 16) |
		                   (pcidev[i].pd_subclass << 8) |
		                   pcidev[i].pd_infclass);

		if (class_signature == PCI_T3_ISA)
			isa_class_index = i;

		vid = pcidev[i].pd_vid;
		did = pcidev[i].pd_did;

		for (j = 0; pci_isabridge[j].vid != 0; j++)
		{
			if (pci_isabridge[j].vid != vid)
				continue;
			if (pci_isabridge[j].did != did)
				continue;
			if (pci_isabridge[j].checkclass && isa_class_index != i)
				continue;
			break;
		}

		if (pci_isabridge[j].vid != 0)
		{
			bridge_dev = i;
			break;
		}
	}

	if (bridge_dev != -1)
	{
		vid = pcidev[bridge_dev].pd_vid;
		did = pcidev[bridge_dev].pd_did;

		dstr = _pci_dev_name(vid, did);
		if (!dstr)
			dstr = "unknown device";

		if (debug)
			printf("found ISA bridge (%04X:%04X) %s\n", vid, did, dstr);

		pcibus[busind].pb_isabridge_dev = bridge_dev;
		type = pci_isabridge[j].type;
		pcibus[busind].pb_isabridge_type = type;

		switch (type)
		{
		case PCI_IB_PIIX:
			r = do_piix(bridge_dev);
			break;
		case PCI_IB_VIA:
			r = do_via_isabr(bridge_dev);
			break;
		case PCI_IB_AMD:
			r = do_amd_isabr(bridge_dev);
			break;
		case PCI_IB_SIS:
			r = do_sis_isabr(bridge_dev);
			break;
		default:
			panic("unknown ISA bridge type: %d", type);
		}
		return r;
	}

	if (isa_class_index == -1)
	{
		if (debug)
			printf("(warning) no ISA bridge found on bus %d\n", busind);
		return 0;
	}

	if (debug)
	{
		printf("(warning) unsupported ISA bridge %04X:%04X for bus %d\n",
		       pcidev[isa_class_index].pd_vid,
		       pcidev[isa_class_index].pd_did, busind);
	}
	return 0;
}

static int derive_irq(struct pcidev *dev, int pin)
{
    if (dev == NULL) {
        return -1;
    }

    int bus_index = get_busind(dev->pd_busnr);
    if (bus_index < 0) {
        return -1;
    }

    int dev_index = pcibus[bus_index].pb_devind;
    if (dev_index < 0) {
        return -1;
    }

    struct pcidev *parent_bridge = &pcidev[dev_index];
    unsigned slot = ((unsigned)dev->pd_func >> 3) & 0x1Fu;
    unsigned irq_pin = ((unsigned)pin + slot) & 3u;

    return acpi_get_irq(parent_bridge->pd_busnr, parent_bridge->pd_dev, (int)irq_pin);
}

static void
record_irq(int devind)
{
	int ilr = __pci_attr_r8(devind, PCI_ILR);
	int ipr = __pci_attr_r8(devind, PCI_IPR);
	int busnr = pcidev[devind].pd_busnr;
	int devno = pcidev[devind].pd_dev;
	int func = pcidev[devind].pd_func;

	if (ipr && machine.apic_enabled) {
		int pin = ipr - 1;
		int irq = acpi_get_irq(busnr, devno, pin);

		if (irq < 0) {
			irq = derive_irq(&pcidev[devind], pin);
		}

		if (irq >= 0) {
			ilr = irq;
			__pci_attr_w8(devind, PCI_ILR, ilr);
			if (debug) {
				printf("PCI: ACPI IRQ %d for device %d.%d.%d INT%c\n",
					irq, busnr, devno, func, 'A' + pin);
			}
		} else if (debug) {
			printf("PCI: no ACPI IRQ routing for device %d.%d.%d INT%c\n",
				busnr, devno, func, 'A' + pin);
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

	pcidev[devind].pd_ilr = ilr;

	if (ilr == PCI_ILR_UNKNOWN && !ipr) {
	} else if (ilr != PCI_ILR_UNKNOWN && ipr) {
		if (debug) {
			printf("\tIRQ %d for INT%c\n", ilr, 'A' + ipr - 1);
		}
	} else if (ilr != PCI_ILR_UNKNOWN) {
		printf("PCI: IRQ %d is assigned, but device %d.%d.%d does not need it\n",
			ilr, busnr, devno, func);
	} else {
		int busind = get_busind(busnr);
		if (pcibus[busind].pb_type == PBT_CARDBUS) {
			int cb_devind = pcibus[busind].pb_devind;
			int cb_ilr = pcidev[cb_devind].pd_ilr;
			if (cb_ilr != PCI_ILR_UNKNOWN) {
				if (debug) {
					printf("assigning IRQ %d to Cardbus device\n", cb_ilr);
				}
				__pci_attr_w8(devind, PCI_ILR, cb_ilr);
				pcidev[devind].pd_ilr = cb_ilr;
				return;
			}
		}
		if (debug) {
			printf("PCI: device %d.%d.%d uses INT%c but is not assigned any IRQ\n",
				busnr, devno, func, 'A' + ipr - 1);
		}
	}
}

/*===========================================================================*
 *				BAR helpers				     *
 *===========================================================================*/
static int
record_bar(int devind, int bar_nr, int last)
{
	int reg, prefetch, type, dev_bar_nr, width;
	u32_t bar, bar2;
	u16_t cmd;

	width = 1;
	reg = PCI_BAR + 4 * bar_nr;
	bar = __pci_attr_r32(devind, reg);

	if (bar & PCI_BAR_IO) {
		cmd = __pci_attr_r16(devind, PCI_CR);
		__pci_attr_w16(devind, PCI_CR, (u16_t)(cmd & ~PCI_CR_IO_EN));

		__pci_attr_w32(devind, reg, 0xffffffff);
		bar2 = __pci_attr_r32(devind, reg);

		__pci_attr_w32(devind, reg, bar);
		__pci_attr_w16(devind, PCI_CR, cmd);

		bar &= PCI_BAR_IO_MASK;
		bar2 &= PCI_BAR_IO_MASK;
		bar2 = (u32_t)((~bar2 & 0xffff) + 1);

		if (debug) {
			printf("\tbar_%d: %d bytes at 0x%x I/O\n", bar_nr, bar2, bar);
		}

		dev_bar_nr = pcidev[devind].pd_bar_nr++;
		pcidev[devind].pd_bar[dev_bar_nr].pb_flags = PBF_IO;
		pcidev[devind].pd_bar[dev_bar_nr].pb_base = bar;
		pcidev[devind].pd_bar[dev_bar_nr].pb_size = bar2;
		pcidev[devind].pd_bar[dev_bar_nr].pb_nr = bar_nr;
		if (bar == 0) {
			pcidev[devind].pd_bar[dev_bar_nr].pb_flags |= PBF_INCOMPLETE;
		}

		return width;
	}

	type = (bar & PCI_BAR_TYPE);
	switch (type) {
		case PCI_TYPE_32:
		case PCI_TYPE_32_1M:
			break;

		case PCI_TYPE_64:
			if (last) {
				printf("PCI: device %d.%d.%d BAR %d extends"
				       " beyond designated area\n",
				       pcidev[devind].pd_busnr,
				       pcidev[devind].pd_dev,
				       pcidev[devind].pd_func, bar_nr);
				return width;
			}

			width++;
			bar2 = __pci_attr_r32(devind, reg + 4);

			if (bar2 != 0) {
				if (debug) {
					printf("\tbar_%d: (64-bit BAR with"
					       " high bits set)\n", bar_nr);
				}
				return width;
			}
			break;

		default:
			if (debug) {
				printf("\tbar_%d: (unknown type %x)\n", bar_nr, type);
			}
			return width;
	}

	cmd = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, (u16_t)(cmd & ~PCI_CR_MEM_EN));

	__pci_attr_w32(devind, reg, 0xffffffff);
	bar2 = __pci_attr_r32(devind, reg);

	__pci_attr_w32(devind, reg, bar);
	__pci_attr_w16(devind, PCI_CR, cmd);

	if (bar2 == 0) {
		return width;
	}

	prefetch = !!(bar & PCI_BAR_PREFETCH);
	bar &= PCI_BAR_MEM_MASK;
	bar2 &= PCI_BAR_MEM_MASK;
	bar2 = ~bar2 + 1;

	if (debug) {
		printf("\tbar_%d: 0x%x bytes at 0x%x%s memory%s\n",
		       bar_nr, bar2, bar,
		       prefetch ? " prefetchable" : "",
		       type == PCI_TYPE_64 ? ", 64-bit" : "");
	}

	dev_bar_nr = pcidev[devind].pd_bar_nr++;
	pcidev[devind].pd_bar[dev_bar_nr].pb_flags = 0;
	pcidev[devind].pd_bar[dev_bar_nr].pb_base = bar;
	pcidev[devind].pd_bar[dev_bar_nr].pb_size = bar2;
	pcidev[devind].pd_bar[dev_bar_nr].pb_nr = bar_nr;
	if (bar == 0) {
		pcidev[devind].pd_bar[dev_bar_nr].pb_flags |= PBF_INCOMPLETE;
	}

	return width;
}

static void record_bars(int devind, int last_reg)
{
	int index = 0;
	int reg = PCI_BAR;

	while (reg <= last_reg)
	{
		int width = record_bar(devind, index, reg == last_reg);
		if (width <= 0)
		{
			break;
		}
		index += width;
		reg += 4 * width;
	}
}

static void
record_bars_normal(int devind)
{
	int i, j, clear_primary, clear_secondary;

	record_bars(devind, PCI_BAR_6);

	if (!(pcidev[devind].pd_baseclass == PCI_BCR_MASS_STORAGE &&
	      pcidev[devind].pd_subclass == PCI_MS_IDE))
	{
		return;
	}

	clear_primary = (pcidev[devind].pd_infclass & PCI_IDE_PRI_NATIVE) == 0;
	clear_secondary = (pcidev[devind].pd_infclass & PCI_IDE_SEC_NATIVE) == 0;

	if (clear_primary && debug)
	{
		printf("primary channel is not in native mode, clearing BARs 0 and 1\n");
	}
	if (clear_secondary && debug)
	{
		printf("secondary channel is not in native mode, clearing BARs 2 and 3\n");
	}

	j = 0;
	for (i = 0; i < pcidev[devind].pd_bar_nr; i++)
	{
		int pb_nr = pcidev[devind].pd_bar[i].pb_nr;
		int skip = 0;

		if ((pb_nr == 0 || pb_nr == 1) && clear_primary)
		{
			skip = 1;
		}
		else if ((pb_nr == 2 || pb_nr == 3) && clear_secondary)
		{
			skip = 1;
		}

		if (skip)
		{
			if (debug) printf("skipping bar %d\n", pb_nr);
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

static u32_t compute_size(u32_t base, u32_t limit)
{
	return (u32_t)(limit - base + 1u);
}

static void compute_io_window(int devind, u32_t *base, u32_t *limit)
{
	u32_t base_lo = (((u32_t)__pci_attr_r8(devind, PPB_IOBASE)) & (u32_t)PPB_IOB_MASK) << 8;
	u32_t base_hi = ((u32_t)__pci_attr_r16(devind, PPB_IOBASEU16)) << 16;
	u32_t limit_lo_ones = 0xFFu;
	u32_t limit_mid = ((((u32_t)__pci_attr_r8(devind, PPB_IOLIMIT)) & (u32_t)PPB_IOL_MASK) |
	                   ((~(u32_t)PPB_IOL_MASK) & 0xFFu)) << 8;
	u32_t limit_hi = ((u32_t)__pci_attr_r16(devind, PPB_IOLIMITU16)) << 16;

	*base = base_lo | base_hi;
	*limit = limit_lo_ones | limit_mid | limit_hi;
}

static void compute_mem_window(int devind, u32_t *base, u32_t *limit)
{
	u32_t base_hi = (((u32_t)__pci_attr_r16(devind, PPB_MEMBASE)) & (u32_t)PPB_MEMB_MASK) << 16;
	u32_t limit_lo_ones = 0xFFFFu;
	u32_t limit_hi = ((((u32_t)__pci_attr_r16(devind, PPB_MEMLIMIT)) & (u32_t)PPB_MEML_MASK) |
	                  ((~(u32_t)PPB_MEML_MASK) & 0xFFFFu)) << 16;

	*base = base_hi;
	*limit = limit_lo_ones | limit_hi;
}

static void compute_pfm_window(int devind, u32_t *base, u32_t *limit)
{
	u32_t base_hi = (((u32_t)__pci_attr_r16(devind, PPB_PFMEMBASE)) & (u32_t)PPB_PFMEMB_MASK) << 16;
	u32_t limit_lo_ones = 0xFFFFu;
	u32_t limit_hi = ((((u32_t)__pci_attr_r16(devind, PPB_PFMEMLIMIT)) & (u32_t)PPB_PFMEML_MASK) |
	                  ((~(u32_t)PPB_PFMEML_MASK) & 0xFFFFu)) << 16;

	*base = base_hi;
	*limit = limit_lo_ones | limit_hi;
}

static void record_bars_bridge(int devind)
{
	u32_t base, limit, size;

	record_bars(devind, PCI_BAR_2);

	compute_io_window(devind, &base, &limit);
	size = compute_size(base, limit);
	if (debug)
	{
		printf("\tI/O window: base 0x%x, limit 0x%x, size %d\n",
		       (unsigned)base, (unsigned)limit, (int)size);
	}

	compute_mem_window(devind, &base, &limit);
	size = compute_size(base, limit);
	if (debug)
	{
		printf("\tMemory window: base 0x%x, limit 0x%x, size 0x%x\n",
		       (unsigned)base, (unsigned)limit, (unsigned)size);
	}

	compute_pfm_window(devind, &base, &limit);
	size = compute_size(base, limit);
	if (debug)
	{
		printf("\tPrefetchable memory window: base 0x%x, limit 0x%x, size 0x%x\n",
		       (unsigned)base, (unsigned)limit, (unsigned)size);
	}
}

static void print_cbb_window(const char *label, int devind, u32_t base_reg, u32_t limit_reg, u32_t mask)
{
	u32_t base = __pci_attr_r32(devind, base_reg);
	u32_t limit = __pci_attr_r32(devind, limit_reg) | (u32_t)(~mask);
	u32_t size = 0;

	if (base <= limit)
		size = (limit - base) + 1;

	if (debug)
		printf("\t%s: base 0x%x, limit 0x%x, size %u\n", label, base, limit, size);
}

static void
record_bars_cardbus(int devind)
{
	record_bars(devind, PCI_BAR);

	print_cbb_window("Memory window 0", devind, CBB_MEMBASE_0, CBB_MEMLIMIT_0, CBB_MEML_MASK);
	print_cbb_window("Memory window 1", devind, CBB_MEMBASE_1, CBB_MEMLIMIT_1, CBB_MEML_MASK);
	print_cbb_window("I/O window 0", devind, CBB_IOBASE_0, CBB_IOLIMIT_0, CBB_IOL_MASK);
	print_cbb_window("I/O window 1", devind, CBB_IOBASE_1, CBB_IOLIMIT_1, CBB_IOL_MASK);
}

static void complete_bars(void)
{
	int i, j, bar_nr, reg;
	u32_t memgap_low, memgap_high, iogap_low, iogap_high, io_high;
	u32_t base, size, v32, diff1, diff2;
	kinfo_t kinfo;
	const u32_t MEMGAP_TOP = 0xfe000000; /* Leave space for the CPU (APIC) */
	const u32_t IOGAP_TOP = 0x10000;
	const u32_t IOGAP_BOTTOM = 0x400;
	const u32_t ISA_MASK = 0xfcff;
	const u32_t U32_MAX_V = (u32_t)~0U;

	if (OK != sys_getkinfo(&kinfo))
		panic("can't get kinfo");

	/* Set memgap_low to just above physical memory */
	memgap_low = kinfo.mem_high_phys;
	memgap_high = MEMGAP_TOP;

	if (debug) {
		printf("complete_bars: initial gap: [0x%x .. 0x%x>\n",
		    memgap_low, memgap_high);
	}

	/* Find the lowest memory base */
	for (i = 0; i < nr_pcidev; i++) {
		for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
			u32_t flags = pcidev[i].pd_bar[j].pb_flags;
			u32_t end;

			if (flags & PBF_IO)
				continue;
			if (flags & PBF_INCOMPLETE)
				continue;

			base = pcidev[i].pd_bar[j].pb_base;
			size = pcidev[i].pd_bar[j].pb_size;

			end = (size > (U32_MAX_V - base)) ? U32_MAX_V : (base + size);

			if (base >= memgap_high)
				continue; /* Not in the gap */
			if (end <= memgap_low)
				continue; /* Not in the gap */

			/* Reduce the gap by the smallest amount */
			diff1 = end - memgap_low;
			diff2 = memgap_high - base;

			if (diff1 < diff2)
				memgap_low = end;
			else
				memgap_high = base;
		}
	}

	if (debug) {
		printf("complete_bars: intermediate gap: [0x%x .. 0x%x>\n",
		    memgap_low, memgap_high);
	}

	/* Should check main memory size */
	if (memgap_high < memgap_low) {
		printf("PCI: bad memory gap: [0x%x .. 0x%x>\n",
		    memgap_low, memgap_high);
		panic(NULL);
	}

	iogap_high = IOGAP_TOP;
	iogap_low = IOGAP_BOTTOM;

	/* Find the free I/O space */
	for (i = 0; i < nr_pcidev; i++) {
		for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
			u32_t flags = pcidev[i].pd_bar[j].pb_flags;
			u32_t end;

			if (!(flags & PBF_IO))
				continue;
			if (flags & PBF_INCOMPLETE)
				continue;

			base = pcidev[i].pd_bar[j].pb_base;
			size = pcidev[i].pd_bar[j].pb_size;

			end = (size > (U32_MAX_V - base)) ? U32_MAX_V : (base + size);

			if (base >= iogap_high)
				continue;
			if (end <= iogap_low)
				continue;

			if (end - iogap_low < iogap_high - base)
				iogap_low = end;
			else
				iogap_high = base;
		}
	}

	if (iogap_high < iogap_low) {
		if (debug) {
			printf("iogap_high too low, should panic\n");
		} else {
			panic("iogap_high too low: %u", iogap_high);
		}
	}
	if (debug)
		printf("I/O range = [0x%x..0x%x>\n", iogap_low, iogap_high);

	for (i = 0; i < nr_pcidev; i++) {
		/* Allocate memory BARs */
		for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
			u32_t flags = pcidev[i].pd_bar[j].pb_flags;

			if (flags & PBF_IO)
				continue;
			if (!(flags & PBF_INCOMPLETE))
				continue;

			size = pcidev[i].pd_bar[j].pb_size;
			if (size < PAGE_SIZE)
				size = PAGE_SIZE;

			/* Align base from the top of the current gap */
			if (size == 0) {
				/* Defensive: avoid undefined alignment on zero size */
				continue;
			}
			if (size > memgap_high) {
				panic("memory size too large: %u", size);
			}
			base = memgap_high - size;
			base &= ~(u32_t)(size - 1);

			if (base < memgap_low)
				panic("memory base too low: %u", base);

			memgap_high = base;
			bar_nr = pcidev[i].pd_bar[j].pb_nr;
			reg = PCI_BAR + 4 * bar_nr;
			v32 = __pci_attr_r32(i, reg);
			__pci_attr_w32(i, reg, v32 | base);
			if (debug) {
				printf(
				    "complete_bars: allocated 0x%x size %u to %d.%d.%d, bar_%d\n",
				    base, size, pcidev[i].pd_busnr,
				    pcidev[i].pd_dev, pcidev[i].pd_func,
				    bar_nr);
			}
			pcidev[i].pd_bar[j].pb_base = base;
			pcidev[i].pd_bar[j].pb_flags &= ~PBF_INCOMPLETE;
		}

		/* Allocate I/O BARs */
		io_high = iogap_high;
		for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
			u32_t flags = pcidev[i].pd_bar[j].pb_flags;

			if (!(flags & PBF_IO))
				continue;
			if (!(flags & PBF_INCOMPLETE))
				continue;

			size = pcidev[i].pd_bar[j].pb_size;
			if (size == 0) {
				/* Defensive: avoid undefined alignment on zero size */
				continue;
			}
			if (size > iogap_high) {
				if (!debug) {
					panic("I/O size too large: %u", size);
				} else {
					printf("I/O size too large: %u\n", size);
					continue;
				}
			}

			base = iogap_high - size;
			base &= ~(u32_t)(size - 1);

			/* Assume that ISA compatibility is required. Only
			 * use the lowest 256 bytes out of every 1024 bytes.
			 */
			base &= ISA_MASK;

			if (base < iogap_low)
				printf("I/O base too low: %u", base);

			iogap_high = base;
			bar_nr = pcidev[i].pd_bar[j].pb_nr;
			reg = PCI_BAR + 4 * bar_nr;
			v32 = __pci_attr_r32(i, reg);
			__pci_attr_w32(i, reg, v32 | base);
			if (debug) {
				printf(
				    "complete_bars: allocated 0x%x size %u to %d.%d.%d, bar_%d\n",
				    base, size, pcidev[i].pd_busnr,
				    pcidev[i].pd_dev, pcidev[i].pd_func,
				    bar_nr);
			}
			pcidev[i].pd_bar[j].pb_base = base;
			pcidev[i].pd_bar[j].pb_flags &= ~PBF_INCOMPLETE;
		}
		if (iogap_high != io_high) {
			update_bridge4dev_io(i, iogap_high, io_high - iogap_high);
		}
	}

	for (i = 0; i < nr_pcidev; i++) {
		for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
			if (!(pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE))
				continue;
			printf("should allocate resources for device %d\n", i);
		}
	}
}

/*===========================================================================*
 *				PCI Bridge Helpers			     *
 *===========================================================================*/
static void
probe_bus(int busind)
{
	uint32_t dev, func;
	u16_t vid, did, sts, sub_vid, sub_did;
	u8_t headt;
	u8_t baseclass, subclass, infclass;
	int devind, busnr;
	const char *s, *dstr;
	const u16_t sts_mask = PSR_SSE | PSR_RMAS | PSR_RTAS;

	if (debug)
		printf("probe_bus(%d)\n", busind);
	if (nr_pcidev >= NR_PCIDEV)
		panic("too many PCI devices: %d", nr_pcidev);

	devind = nr_pcidev;
	busnr = pcibus[busind].pb_busnr;

	for (dev = 0; dev < 32; dev++) {
		for (func = 0; func < 8; func++) {
			pcidev[devind].pd_busnr = busnr;
			pcidev[devind].pd_dev = dev;
			pcidev[devind].pd_func = func;

			pci_attr_wsts(devind, sts_mask);
			vid = __pci_attr_r16(devind, PCI_VID);
			did = __pci_attr_r16(devind, PCI_DID);
			headt = __pci_attr_r8(devind, PCI_HEADT);
			sts = pci_attr_rsts(devind);

			if (vid == NO_VID && did == NO_VID) {
				if (func == 0)
					break;
				continue;
			}

			if (sts & sts_mask) {
				static int warned = 0;
				if (!warned) {
					printf("PCI: ignoring bad value 0x%x in sts for QEMU\n",
						sts & sts_mask);
					warned = 1;
				}
			}

			sub_vid = __pci_attr_r16(devind, PCI_SUBVID);
			sub_did = __pci_attr_r16(devind, PCI_SUBDID);

			dstr = _pci_dev_name(vid, did);
			if (debug) {
				if (dstr) {
					printf("%d.%lu.%lu: %s (%04X:%04X)\n",
						busnr, (unsigned long)dev,
						(unsigned long)func, dstr,
						vid, did);
				} else {
					printf("%d.%lu.%lu: Unknown device, vendor %04X (%s), device %04X\n",
						busnr, (unsigned long)dev,
						(unsigned long)func, vid,
						pci_vid_name(vid), did);
				}
				printf("Device index: %d\n", devind);
				printf("Subsystem: Vid 0x%x, did 0x%x\n",
					sub_vid, sub_did);
			}

			baseclass = __pci_attr_r8(devind, PCI_BCR);
			subclass = __pci_attr_r8(devind, PCI_SCR);
			infclass = __pci_attr_r8(devind, PCI_PIFR);

			s = pci_subclass_name((uint32_t)baseclass << 24 | (uint32_t)subclass << 16);
			if (!s)
				s = pci_baseclass_name((uint32_t)baseclass << 24);
			if (!s)
				s = "(unknown class)";

			if (debug) {
				printf("\tclass %s (%X/%X/%X)\n", s,
					baseclass, subclass, infclass);
			}

			if (is_duplicate(busnr, dev, func)) {
				printf("\tduplicate!\n");
				if (func == 0 && !(headt & PHT_MULTIFUNC))
					break;
				continue;
			}

			devind = nr_pcidev;
			nr_pcidev++;
			pcidev[devind].pd_baseclass = baseclass;
			pcidev[devind].pd_subclass = subclass;
			pcidev[devind].pd_infclass = infclass;
			pcidev[devind].pd_vid = vid;
			pcidev[devind].pd_did = did;
			pcidev[devind].pd_sub_vid = sub_vid;
			pcidev[devind].pd_sub_did = sub_did;
			pcidev[devind].pd_inuse = 0;
			pcidev[devind].pd_bar_nr = 0;

			record_irq(devind);

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
					printf("\t%d.%d.%d: unknown header type %d\n",
						busind, dev, func, headt & PHT_MASK);
					break;
			}

			if (debug)
				print_capabilities(devind);

			if (nr_pcidev >= NR_PCIDEV)
				panic("too many PCI devices: %d", nr_pcidev);

			devind = nr_pcidev;

			if (func == 0 && !(headt & PHT_MULTIFUNC))
				break;
		}
	}
}


static u16_t
pcibr_std_rsts(const int busind)
{
	const int devind = pcibus[busind].pb_devind;
	return __pci_attr_r16(devind, PPB_SSTS);
}

static void pcibr_std_wsts(int busind, u16_t value)
{
    if (busind < 0) {
        return;
    }

    size_t idx = (size_t)busind;
    size_t count = sizeof(pcibus) / sizeof(pcibus[0]);

    if (idx >= count) {
        return;
    }

    const int devind = pcibus[idx].pb_devind;

    if (devind < 0) {
        return;
    }

    __pci_attr_w16(devind, PPB_SSTS, value);
}

static u16_t pcibr_cb_rsts(int busind)
{
    if (busind < 0) {
        return (u16_t)0;
    }

    const int devind = pcibus[busind].pb_devind;
    if (devind < 0) {
        return (u16_t)0;
    }

    return __pci_attr_r16(devind, CBB_SSTS);
}

static void pcibr_cb_wsts(int busind, u16_t value)
{
    int device_index;

    if (busind < 0) {
        return;
    }

    device_index = pcibus[busind].pb_devind;
    if (device_index < 0) {
        return;
    }

    __pci_attr_w16(device_index, CBB_SSTS, value);
}

static u16_t
pcibr_via_rsts(int busind)
{
	(void)busind;
	return (u16_t)0U;
}

static void pcibr_via_wsts(int busind, u16_t value)
{
    (void)busind;
    (void)value;
}

static void
complete_bridges(void)
{
    int i;

    for (i = 0; i < nr_pcibus; i++)
    {
        if (!pcibus[i].pb_needinit)
            continue;

        printf("should allocate bus number for bus %d\n", i);

        int freebus = get_freebus();
        printf("got bus number %d\n", freebus);
        if (freebus < 0)
            continue;

        int devind = pcibus[i].pb_devind;
        if (devind < 0)
            continue;

        int prim_busnr = pcidev[devind].pd_busnr;
        if (prim_busnr != 0)
        {
            printf("complete_bridge: updating subordinate bus number not implemented\n");
        }

        pcibus[i].pb_needinit = 0;
        pcibus[i].pb_busnr = freebus;

        printf("devind = %d\n", devind);
        printf("prim_busnr= %d\n", prim_busnr);

        __pci_attr_w8(devind, PPB_PRIMBN, prim_busnr);
        __pci_attr_w8(devind, PPB_SECBN, freebus);
        __pci_attr_w8(devind, PPB_SUBORDBN, freebus);

        printf("CR = 0x%x\n", __pci_attr_r16(devind, PCI_CR));
        printf("SECBLT = 0x%x\n", __pci_attr_r8(devind, PPB_SECBLT));
        printf("BRIDGECTRL = 0x%x\n", __pci_attr_r16(devind, PPB_BRIDGECTRL));
    }
}

static void
do_pcibridge(int busind)
{
	int devind, busnr;
	int new_index, type;
	u16_t vid, did;
	u8_t sbusn, baseclass, subclass, infclass, headt;
	u32_t class_code;

	if (busind < 0 || busind >= nr_pcibus)
		panic("invalid PCI bus index: %d", busind);

	busnr = pcibus[busind].pb_busnr;

	for (devind = 0; devind < nr_pcidev; devind++)
	{
		if (pcidev[devind].pd_busnr != busnr)
			continue;

		vid = pcidev[devind].pd_vid;
		did = pcidev[devind].pd_did;

		headt = __pci_attr_r8(devind, PCI_HEADT);
		switch (headt & PHT_MASK)
		{
		case PHT_BRIDGE:
			type = PCI_PPB_STD;
			break;
		case PHT_CARDBUS:
			type = PCI_PPB_CB;
			break;
		default:
			continue;
		}

		baseclass = __pci_attr_r8(devind, PCI_BCR);
		subclass = __pci_attr_r8(devind, PCI_SCR);
		infclass = __pci_attr_r8(devind, PCI_PIFR);
		class_code = ((u32_t)baseclass << 16) | ((u32_t)subclass << 8) | infclass;

		if (type == PCI_PPB_STD &&
		    class_code != PCI_T3_PCI2PCI &&
		    class_code != PCI_T3_PCI2PCI_SUBTR)
		{
			printf("Unknown PCI class %02x/%02x/%02x for PCI-to-PCI bridge, device %04X:%04X\n",
				baseclass, subclass, infclass, vid, did);
			continue;
		}
		if (type == PCI_PPB_CB && class_code != PCI_T3_CARDBUS)
		{
			printf("Unknown PCI class %02x/%02x/%02x for Cardbus bridge, device %04X:%04X\n",
				baseclass, subclass, infclass, vid, did);
			continue;
		}

		if (debug)
		{
			printf("%u.%u.%u: PCI-to-PCI bridge: %04X:%04X\n",
				pcidev[devind].pd_busnr,
				pcidev[devind].pd_dev,
				pcidev[devind].pd_func, vid, did);
		}

		sbusn = __pci_attr_r8(devind, PPB_SECBN);

		if (nr_pcibus >= NR_PCIBUS)
			panic("too many PCI busses: %d", nr_pcibus);

		new_index = nr_pcibus++;
		pcibus[new_index].pb_type = PBT_PCIBRIDGE;
		pcibus[new_index].pb_needinit = 1;
		pcibus[new_index].pb_isabridge_dev = -1;
		pcibus[new_index].pb_isabridge_type = 0;
		pcibus[new_index].pb_devind = devind;
		pcibus[new_index].pb_busnr = sbusn;

		pcibus[new_index].pb_rreg8  = pcibus[busind].pb_rreg8;
		pcibus[new_index].pb_rreg16 = pcibus[busind].pb_rreg16;
		pcibus[new_index].pb_rreg32 = pcibus[busind].pb_rreg32;
		pcibus[new_index].pb_wreg8  = pcibus[busind].pb_wreg8;
		pcibus[new_index].pb_wreg16 = pcibus[busind].pb_wreg16;
		pcibus[new_index].pb_wreg32 = pcibus[busind].pb_wreg32;

		switch (type)
		{
		case PCI_PPB_STD:
			pcibus[new_index].pb_rsts = pcibr_std_rsts;
			pcibus[new_index].pb_wsts = pcibr_std_wsts;
			break;
		case PCI_PPB_CB:
			pcibus[new_index].pb_type = PBT_CARDBUS;
			pcibus[new_index].pb_rsts = pcibr_cb_rsts;
			pcibus[new_index].pb_wsts = pcibr_cb_wsts;
			break;
		case PCI_AGPB_VIA:
			pcibus[new_index].pb_rsts = pcibr_via_rsts;
			pcibus[new_index].pb_wsts = pcibr_via_wsts;
			break;
		default:
			panic("unknown PCI-PCI bridge type: %d", type);
		}

		if (machine.apic_enabled)
			acpi_map_bridge(pcidev[devind].pd_busnr,
					pcidev[devind].pd_dev, sbusn);

		if (debug)
		{
			printf("bus(table) = %d, bus(sec) = %d, bus(subord) = %d\n",
				new_index, sbusn, __pci_attr_r8(devind, PPB_SUBORDBN));
		}
		if (sbusn == 0)
		{
			printf("Secondary bus number not initialized\n");
			continue;
		}
		pcibus[new_index].pb_needinit = 0;

		probe_bus(new_index);

		do_pcibridge(new_index);
	}
}

/*===========================================================================*
 *				pci_intel_init				     *
 *===========================================================================*/
static void
pci_intel_init(void)
{
	u16_t vid, did;
	int s, i, r, busind, busnr;
	const char *dstr;

	vid = PCII_RREG16_(0, 0, 0, PCI_VID);
	did = PCII_RREG16_(0, 0, 0, PCI_DID);
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK)
		printf("PCI: warning, sys_outl failed: %d\n", s);

	if (nr_pcibus >= NR_PCIBUS)
		panic("too many PCI busses: %d", nr_pcibus);

	busind = nr_pcibus;
	nr_pcibus++;
	pcibus[busind].pb_type = PBT_INTEL_HOST;
	pcibus[busind].pb_needinit = 0;
	pcibus[busind].pb_isabridge_dev = -1;
	pcibus[busind].pb_isabridge_type = 0;
	pcibus[busind].pb_devind = -1;
	pcibus[busind].pb_busnr = 0;
	pcibus[busind].pb_rreg8 = pcii_rreg8;
	pcibus[busind].pb_rreg16 = pcii_rreg16;
	pcibus[busind].pb_rreg32 = pcii_rreg32;
	pcibus[busind].pb_wreg8 = pcii_wreg8;
	pcibus[busind].pb_wreg16 = pcii_wreg16;
	pcibus[busind].pb_wreg32 = pcii_wreg32;
	pcibus[busind].pb_rsts = pcii_rsts;
	pcibus[busind].pb_wsts = pcii_wsts;

	dstr = _pci_dev_name(vid, did);
	if (!dstr)
		dstr = "unknown device";
	if (debug)
		printf("pci_intel_init: %s (%04X:%04X)\n", dstr, vid, did);

	probe_bus(busind);

	r = do_isabridge(busind);
	if (r != OK) {
		busnr = pcibus[busind].pb_busnr;
		for (i = 0; i < nr_pcidev; i++) {
			if (pcidev[i].pd_busnr != busnr)
				continue;
			pcidev[i].pd_inuse = 1;
		}
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
static int visible(struct rs_pci *aclp, int devind)
{
	u16_t dev_vid, dev_did, dev_sub_vid, dev_sub_did;
	u32_t baseclass, subclass, infclass, class_id;
	int idx;

	if (!aclp)
		return TRUE;

	dev_vid = pcidev[devind].pd_vid;
	dev_did = pcidev[devind].pd_did;
	dev_sub_vid = pcidev[devind].pd_sub_vid;
	dev_sub_did = pcidev[devind].pd_sub_did;

	for (idx = 0; idx < aclp->rsp_nr_device; idx++)
	{
		u16_t acl_vid = aclp->rsp_device[idx].vid;
		u16_t acl_did = aclp->rsp_device[idx].did;
		u16_t acl_sub_vid = aclp->rsp_device[idx].sub_vid;
		u16_t acl_sub_did = aclp->rsp_device[idx].sub_did;

		if (acl_vid == dev_vid &&
		    acl_did == dev_did &&
		    (acl_sub_vid == NO_SUB_VID || acl_sub_vid == dev_sub_vid) &&
		    (acl_sub_did == NO_SUB_DID || acl_sub_did == dev_sub_did))
		{
			return TRUE;
		}
	}

	if (!aclp->rsp_nr_class)
		return FALSE;

	baseclass = (u32_t)pcidev[devind].pd_baseclass;
	subclass = (u32_t)pcidev[devind].pd_subclass;
	infclass = (u32_t)pcidev[devind].pd_infclass;

	class_id = (baseclass << 16) | (subclass << 8) | infclass;

	for (idx = 0; idx < aclp->rsp_nr_class; idx++)
	{
		u32_t mask = aclp->rsp_class[idx].mask;
		u32_t pciclass = aclp->rsp_class[idx].pciclass;

		if (pciclass == (class_id & mask))
			return TRUE;
	}

	return FALSE;
}

/*===========================================================================*
 *				sef_cb_init_fresh			     *
 *===========================================================================*/
int sef_cb_init(int type, sef_init_info_t *info)
{
	int announce = FALSE;
	long env_val = 0;
	int i, r;
	struct rprocpub procs[NR_BOOT_PROCS];

	env_parse("pci_debug", "d", 0, &env_val, 0, 1);
	debug = (int)env_val;

	if (sys_getmachine(&machine) != OK) {
		printf("PCI: no machine\n");
		return ENODEV;
	}
	if (machine.apic_enabled && acpi_init() != OK) {
		panic("PCI: Cannot use APIC mode without ACPI!\n");
	}

	pci_intel_init();

	if (info == NULL) {
		return EINVAL;
	}

	r = sys_safecopyfrom(RS_PROC_NR, info->rproctab_gid, 0,
	    (vir_bytes)procs, sizeof(procs));
	if (r != OK) {
		panic("sys_safecopyfrom failed: %d", r);
	}

	for (i = 0; i < NR_BOOT_PROCS; i++) {
		if (procs[i].in_use) {
			r = map_service(&procs[i]);
			if (r != OK) {
				panic("unable to map service: %d", r);
			}
		}
	}

	switch (type) {
	case SEF_INIT_FRESH:
	case SEF_INIT_RESTART:
		announce = TRUE;
		break;
	case SEF_INIT_LU:
		break;
	default:
		panic("Unknown type of restart");
	}

	if (announce) {
		chardriver_announce();
	}

	return OK;
}

/*===========================================================================*
 *		               map_service                                   *
 *===========================================================================*/
static int find_free_pci_acl_slot(void)
{
	int i;
	for (i = 0; i < NR_DRIVERS; i++) {
		if (!pci_acl[i].inuse) {
			return i;
		}
	}
	return -1;
}

int map_service(struct rprocpub *rpub)
{
	int idx;

	if (rpub == NULL) {
		return EINVAL;
	}

	if (rpub->pci_acl.rsp_nr_device == 0 && rpub->pci_acl.rsp_nr_class == 0) {
		return OK;
	}

	idx = find_free_pci_acl_slot();
	if (idx < 0) {
		printf("PCI: map_service: table is full\n");
		return ENOMEM;
	}

	pci_acl[idx].acl = rpub->pci_acl;
	pci_acl[idx].inuse = 1;

	return OK;
}

/*===========================================================================*
 *				_pci_find_dev				     *
 *===========================================================================*/
int
_pci_find_dev(u8_t bus, u8_t dev, u8_t func, int *devindp)
{
	int i;

	if (devindp == NULL)
		return 0;

	for (i = 0; i < nr_pcidev; i++)
	{
		if (pcidev[i].pd_busnr == bus &&
			pcidev[i].pd_dev == dev &&
			pcidev[i].pd_func == func)
		{
			*devindp = i;
			return 1;
		}
	}

	return 0;
}

/*===========================================================================*
 *				_pci_first_dev				     *
 *===========================================================================*/
int _pci_first_dev(struct rs_pci *aclp, int *devindp, u16_t *vidp, u16_t *didp)
{
	if (devindp == NULL || vidp == NULL || didp == NULL)
		return 0;

	if (nr_pcidev <= 0)
		return 0;

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
	int prev, devind;

	if (devindp == NULL || vidp == NULL || didp == NULL)
		return 0;

	prev = *devindp;
	if (prev < -1)
		prev = -1;

	if (nr_pcidev <= 0 || prev >= nr_pcidev - 1)
		return 0;

	devind = prev + 1;

	while (devind < nr_pcidev && !visible(aclp, devind)) {
		devind++;
	}

	if (devind >= nr_pcidev)
		return 0;

	*devindp = devind;
	*vidp = pcidev[devind].pd_vid;
	*didp = pcidev[devind].pd_did;
	return 1;
}

/*===========================================================================*
 *				_pci_grant_access			     *
 *===========================================================================*/
static int call_sys_privctl(endpoint_t proc, int req, void *arg)
{
	int r = sys_privctl(proc, req, arg);
	if (r != OK) {
		printf("sys_privctl failed for proc %d: %d\n", proc, r);
	}
	return r;
}

int
_pci_grant_access(int devind, endpoint_t proc)
{
	int i, ilr;
	int r = OK;
	struct io_range ior;
	struct minix_mem_range mr;

	for (i = 0; i < pcidev[devind].pd_bar_nr; i++) {
		if (pcidev[devind].pd_bar[i].pb_flags & PBF_INCOMPLETE) {
			printf("pci_reserve_a: BAR %d is incomplete\n", i);
			continue;
		}

		if (pcidev[devind].pd_bar[i].pb_flags & PBF_IO) {
			ior.ior_base = pcidev[devind].pd_bar[i].pb_base;
			ior.ior_limit = ior.ior_base +
				pcidev[devind].pd_bar[i].pb_size - 1;

			if (debug) {
				printf(
		"pci_reserve_a: for proc %d, adding I/O range [0x%x..0x%x]\n",
		proc, ior.ior_base, ior.ior_limit);
			}
			r = call_sys_privctl(proc, SYS_PRIV_ADD_IO, &ior);
		} else {
			mr.mr_base = pcidev[devind].pd_bar[i].pb_base;
			mr.mr_limit = mr.mr_base +
				pcidev[devind].pd_bar[i].pb_size - 1;

			r = call_sys_privctl(proc, SYS_PRIV_ADD_MEM, &mr);
		}
	}

	ilr = pcidev[devind].pd_ilr;
	if (ilr != PCI_ILR_UNKNOWN) {
		if (debug)
			printf("pci_reserve_a: adding IRQ %d\n", ilr);
		r = call_sys_privctl(proc, SYS_PRIV_ADD_IRQ, &ilr);
	}

	return r;
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
		printf("pci_reserve_a: %u is not allowed to reserve %d\n", proc, devind);
		return EPERM;
	}

	if (pcidev[devind].pd_inuse && pcidev[devind].pd_proc != proc) {
		return EBUSY;
	}

	pcidev[devind].pd_inuse = 1;
	pcidev[devind].pd_proc = proc;

	return _pci_grant_access(devind, proc);
}

/*===========================================================================*
 *				_pci_release				     *
 *===========================================================================*/
void _pci_release(endpoint_t proc)
{
    int i;

    if (nr_pcidev <= 0) {
        return;
    }

    for (i = 0; i < nr_pcidev; i++) {
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
	if (vidp == NULL || didp == NULL)
		return EINVAL;

	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;

	*vidp = pcidev[devind].pd_vid;
	*didp = pcidev[devind].pd_did;
	return OK;
}

/*===========================================================================*
 *				_pci_rescan_bus				     *
 *===========================================================================*/
void _pci_rescan_bus(u8_t busnr)
{
	const int busind = get_busind(busnr);
	if (busind < 0) {
		return;
	}

	(void)probe_bus(busind);
	(void)complete_bridges();
	(void)complete_bars();
}

/*===========================================================================*
 *				_pci_slot_name				     *
 *===========================================================================*/
#include <stdio.h>

int
_pci_slot_name(int devind, char **cpp)
{
    static char label[sizeof("ddd.ddd.ddd.ddd")];

    if (cpp == NULL || devind < 0 || devind >= nr_pcidev)
        return EINVAL;

    (void)snprintf(
        label, sizeof(label), "%u.%u.%u.%u",
        0U,
        (unsigned)pcidev[devind].pd_busnr,
        (unsigned)pcidev[devind].pd_dev,
        (unsigned)pcidev[devind].pd_func
    );

    *cpp = label;
    return OK;
}

/*===========================================================================*
 *				_pci_dev_name				     *
 *===========================================================================*/
const char *
_pci_dev_name(u16_t vid, u16_t did)
{
	static char product[PCI_PRODUCTSTR_LEN];

	pci_findproduct(product, sizeof(product), vid, did);

	if (sizeof(product) > 0 && product[sizeof(product) - 1] != '\0') {
		product[sizeof(product) - 1] = '\0';
	}

	return product;
}

/*===========================================================================*
 *				_pci_get_bar				     *
 *===========================================================================*/
int _pci_get_bar(int devind, int port, u32_t *base, u32_t *size, int *ioflag)
{
    int i;

    if (devind < 0 || devind >= nr_pcidev || base == NULL || size == NULL || ioflag == NULL)
        return EINVAL;

    {
        int bar_count = pcidev[devind].pd_bar_nr;
        for (i = 0; i < bar_count; i++) {
            if (PCI_BAR + 4 * pcidev[devind].pd_bar[i].pb_nr != port)
                continue;

            {
                int flags = pcidev[devind].pd_bar[i].pb_flags;
                if (flags & PBF_INCOMPLETE)
                    return EINVAL;

                *base = pcidev[devind].pd_bar[i].pb_base;
                *size = pcidev[devind].pd_bar[i].pb_size;
                *ioflag = (flags & PBF_IO) ? 1 : 0;
                return OK;
            }
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
	if (vp == NULL) return EINVAL;
	if ((unsigned)devind >= (unsigned)nr_pcidev) return EINVAL;
	if ((unsigned)port > 255u) return EINVAL;

	*vp = __pci_attr_r8(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_r16				     *
 *===========================================================================*/
int _pci_attr_r16(int devind, int port, u16_t *vp)
{
	const int cfg_size = 256;

	if (vp == NULL)
		return EINVAL;
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > cfg_size - (int)sizeof(*vp))
		return EINVAL;

	*vp = __pci_attr_r16(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_r32				     *
 *===========================================================================*/
int
_pci_attr_r32(int devind, int port, u32_t *vp)
{
	enum { PCI_MAX_PORT = 256, PCI_ACCESS_WIDTH = 4 };

	if (vp == NULL)
		return EINVAL;
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > PCI_MAX_PORT - PCI_ACCESS_WIDTH)
		return EINVAL;

	*vp = __pci_attr_r32(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w8				     *
 *===========================================================================*/
int
_pci_attr_w8(int devind, int port, u8_t value)
{
	const int MAX_PORT = 255;

	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}
	if (port < 0 || port > MAX_PORT) {
		return EINVAL;
	}

	__pci_attr_w8(devind, port, value);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w16				     *
 *===========================================================================*/
int _pci_attr_w16(int devind, int port, u16_t value)
{
    const int cfg_space_size = 256;
    const int max_offset = cfg_space_size - (int)sizeof(value);

    if (devind < 0 || devind >= nr_pcidev)
        return EINVAL;
    if (port < 0 || port > max_offset)
        return EINVAL;

    __pci_attr_w16(devind, port, value);
    return OK;
}

/*===========================================================================*
 *				_pci_attr_w32				     *
 *===========================================================================*/
int _pci_attr_w32(int devind, int port, u32_t value)
{
	if (devind < 0 || devind >= nr_pcidev || port < 0 || port > 252)
		return EINVAL;

	__pci_attr_w32(devind, port, value);
	return OK;
}
