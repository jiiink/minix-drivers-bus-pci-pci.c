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
	if (s != OK) {
		printf("PCI: warning, sys_inb failed: %d\n", s);
	}
	return value;
}

static unsigned
pci_inw(u16_t port) {
	u32_t value = 0;
	int s = sys_inw(port, &value);
	if (s != OK) {
		printf("PCI: warning, sys_inw failed: %d\n", s);
	}
	return value;
}

static unsigned
pci_inl(u16_t port) {
    u32_t value = 0;
    int status = sys_inl(port, &value);
    if (status != OK) {
        printf("PCI: warning, sys_inl failed: %d\n", status);
    }
    return value;
}

static void
pci_outb(u16_t port, u8_t value) {
	int result = sys_outb(port, value);
	if (result != OK) {
		printf("PCI: warning, sys_outb failed: %d\n", result);
	}
}

static void
pci_outw(u16_t port, u16_t value) {
	int result = sys_outw(port, value);
	if (result != OK) {
		printf("PCI: warning, sys_outw failed: %d\n", result);
	}
}

static void
pci_outl(u16_t port, u32_t value) {
	int result = sys_outl(port, value);
	if (result != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", result);
	}
}

static u8_t
pcii_rreg8(int busind, int devind, int port)
{
	u8_t v;
	int s;

	v = PCII_RREG8_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port);
	
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
	
	return v;
}

static u16_t
pcii_rreg16(int busind, int devind, int port)
{
	u16_t v;
	int s;

	v = PCII_RREG16_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port);
	
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}

	return v;
}

static u32_t
pcii_rreg32(int busind, int devind, int port)
{
	u32_t v;
	int s;

	v = PCII_RREG32_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port);
	
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
	
	return v;
}

static void
pcii_wreg8(int busind, int devind, int port, u8_t value)
{
	int s;
	
	if (busind < 0 || devind < 0) {
		printf("PCI: invalid bus or device index\n");
		return;
	}
	
	PCII_WREG8_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port, value);
	
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
}

static void
pcii_wreg16(int busind, int devind, int port, u16_t value)
{
	int s;
	
	if (busind < 0 || devind < 0) {
		printf("PCI: invalid bus or device index\n");
		return;
	}
	
	PCII_WREG16_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port, value);
	
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
}

static void
pcii_wreg32(int busind, int devind, int port, u32_t value)
{
	int s;
	
	if (busind < 0 || devind < 0) {
		printf("PCI: invalid bus/device index\n");
		return;
	}
	
	PCII_WREG32_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port, value);
	
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
}

/*===========================================================================*
 *				ntostr					     *
 *===========================================================================*/
static void
ntostr(unsigned int n, char **str, const char *end)
{
	char tmpstr[20];
	int i = 0;

	if (n == 0)
	{
		tmpstr[0] = '0';
		i = 1;
	}
	else
	{
		while (n > 0 && i < 19)
		{
			tmpstr[i] = '0' + (n % 10);
			n /= 10;
			i++;
		}
	}

	while (i > 0 && *str < end)
	{
		i--;
		**str = tmpstr[i];
		(*str)++;
	}

	if (*str >= end && str > (char**)0)
	{
		(*str)--;
	}
	
	if (*str < end)
	{
		**str = '\0';
	}
}

/*===========================================================================*
 *				get_busind					     *
 *===========================================================================*/
static int
get_busind(int busnr)
{
	if (busnr < 0) {
		panic("get_busind: invalid bus number: %d", busnr);
	}

	for (int i = 0; i < nr_pcibus; i++) {
		if (pcibus[i].pb_busnr == busnr) {
			return i;
		}
	}
	panic("get_busind: can't find bus: %d", busnr);
}

/*===========================================================================*
 *			Unprotected helper functions			     *
 *===========================================================================*/
static u8_t
__pci_attr_r8(int devind, int port)
{
	int busnr, busind;

	if (devind < 0 || devind >= MAX_PCIDEV || !pcidev[devind].pd_inuse)
		return 0xFF;

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0 || busind >= MAX_PCIBUS || !pcibus[busind].pb_rreg8)
		return 0xFF;

	return pcibus[busind].pb_rreg8(busind, devind, port);
}

static u16_t
__pci_attr_r16(int devind, int port)
{
	int busnr, busind;

	if (devind < 0 || devind >= nr_pcidev)
		return 0xFFFF;

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0 || busind >= nr_pcibus)
		return 0xFFFF;

	if (!pcibus[busind].pb_rreg16)
		return 0xFFFF;

	return pcibus[busind].pb_rreg16(busind, devind, port);
}

static u32_t
__pci_attr_r32(int devind, int port)
{
	int busnr, busind;

	if (devind < 0 || devind >= nr_pcidev) {
		return 0xFFFFFFFF;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0) {
		return 0xFFFFFFFF;
	}

	return pcibus[busind].pb_rreg32(busind, devind, port);
}

static void
__pci_attr_w8(int devind, int port, u8_t value)
{
	int busnr, busind;

	if (devind < 0 || devind >= nr_pcidev) {
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0 || busind >= nr_pcibus) {
		return;
	}

	if (pcibus[busind].pb_wreg8 == NULL) {
		return;
	}

	pcibus[busind].pb_wreg8(busind, devind, port, value);
}

static void
__pci_attr_w16(int devind, int port, u16_t value)
{
	int busnr, busind;

	if (devind < 0 || devind >= NR_PCIDEV || !pcidev[devind].pd_inuse) {
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0 || busind >= NR_PCIBUS || !pcibus[busind].pb_wreg16) {
		return;
	}

	pcibus[busind].pb_wreg16(busind, devind, port, value);
}

static void
__pci_attr_w32(int devind, int port, u32_t value)
{
	int busnr, busind;

	if (devind < 0 || devind >= nr_pcidev) {
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0 || busind >= nr_pcibus) {
		return;
	}

	if (pcibus[busind].pb_wreg32 == NULL) {
		return;
	}

	pcibus[busind].pb_wreg32(busind, devind, port, value);
}

/*===========================================================================*
 *				helpers					     *
 *===========================================================================*/
static u16_t
pci_attr_rsts(int devind)
{
	int busnr, busind;

	if (devind < 0 || devind >= NR_PCIDEV || !pcidev[devind].pd_inuse)
		return 0;

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0 || busind >= NR_PCIBUS || !pcibus[busind].pb_rsts)
		return 0;

	return pcibus[busind].pb_rsts(busind);
}

static void
pci_attr_wsts(int devind, u16_t value)
{
	int busnr, busind;

	if (devind < 0 || devind >= nr_pcidev) {
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0 || busind >= nr_pcibus) {
		return;
	}

	if (pcibus[busind].pb_wsts == NULL) {
		return;
	}

	pcibus[busind].pb_wsts(busind, value);
}

static u16_t
pcii_rsts(int busind)
{
	u16_t status_reg;
	int result;

	if (busind < 0 || busind >= MAX_NR_PCIBUS) {
		return 0;
	}

	status_reg = PCII_RREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR);
	
	result = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (result != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", result);
	}
	
	return status_reg;
}

static void
pcii_wsts(int busind, u16_t value)
{
	int s;
	
	if (busind < 0 || busind >= NR_PCIBUS || !pcibus[busind].pb_busnr) {
		return;
	}
	
	PCII_WREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR, value);
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
}

static int
is_duplicate(u8_t busnr, u8_t dev, u8_t func)
{
	int i;

	for (i = 0; i < nr_pcidev; i++)
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

static int
get_freebus(void)
{
	int freebus = 1;

	for (int i = 0; i < nr_pcibus; i++)
	{
		if (pcibus[i].pb_needinit || pcibus[i].pb_type == PBT_INTEL_HOST)
			continue;
		
		if (pcibus[i].pb_busnr >= freebus)
			freebus = pcibus[i].pb_busnr + 1;
	}
	return freebus;
}

static const char *
pci_vid_name(u16_t vid)
{
	static char vendor[PCI_VENDORSTR_LEN];
	int result = pci_findvendor(vendor, sizeof(vendor), vid);
	
	if (result != 0) {
		vendor[0] = '\0';
	}
	
	return vendor;
}


static void
print_hyper_cap(int devind, u8_t capptr)
{
	u32_t v;
	u16_t cmd;
	int type0, type1;

	printf("\n");
	v = __pci_attr_r32(devind, capptr);
	printf("print_hyper_cap: @0x%x, off 0 (cap):", capptr);
	cmd = (v >> 16) & 0xffff;

	type0 = (cmd & 0xE000) >> 13;
	type1 = (cmd & 0xF800) >> 11;
	if (type0 == 0 || type0 == 1)
	{
		printf("Capability Type: %s\n",
			type0 == 0 ? "Slave or Primary Interface" :
			"Host or Secondary Interface");
		cmd &= ~0xE000;
	}
	else
	{
		printf(" Capability Type 0x%x", type1);
		cmd &= ~0xF800;
	}
	if (cmd)
		printf(" undecoded 0x%x\n", cmd);
}

static void
print_capabilities(int devind)
{
	u8_t status, capptr, type, next, subtype;
	const char *str;

	status = __pci_attr_r16(devind, PCI_SR);
	if (!(status & PSR_CAPPTR))
		return;

	capptr = (__pci_attr_r8(devind, PCI_CAPPTR) & PCI_CP_MASK);
	while (capptr != 0)
	{
		type = __pci_attr_r8(devind, capptr + CAP_TYPE);
		next = (__pci_attr_r8(devind, capptr + CAP_NEXT) & PCI_CP_MASK);
		
		str = get_capability_type_string(type);

		printf(" @0x%x (0x%08x): capability type 0x%x: %s",
			capptr, __pci_attr_r32(devind, capptr), type, str);
		
		if (type == 0x08)
		{
			print_hyper_cap(devind, capptr);
		}
		else if (type == 0x0f)
		{
			subtype = (__pci_attr_r8(devind, capptr + 2) & 0x07);
			str = get_secure_device_subtype_string(subtype);
			printf(", sub type 0%o: %s", subtype, str);
		}
		printf("\n");
		capptr = next;
	}
}

static const char *
get_capability_type_string(u8_t type)
{
	switch(type)
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

static const char *
get_secure_device_subtype_string(u8_t subtype)
{
	switch(subtype)
	{
	case 0: return "Device Exclusion Vector";
	case 3: return "IOMMU";
	default: return "(unknown type)";
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

	if (devind < 0) {
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	
	if (busind < 0) {
		return;
	}

	type = pcibus[busind].pb_type;
	
	if (type == PBT_INTEL_HOST) {
		return;
	}
	
	if (type == PBT_PCIBRIDGE) {
		printf("update_bridge4dev_io: not implemented for PCI bridges\n");
		return;
	}
	
	if (type != PBT_CARDBUS) {
		panic("update_bridge4dev_io: strange bus type: %d", type);
	}

	if (io_size == 0) {
		return;
	}

	if (debug) {
		printf("update_bridge4dev_io: adding 0x%x at 0x%x\n", io_size, io_base);
	}

	br_devind = pcibus[busind].pb_devind;
	
	if (br_devind < 0) {
		return;
	}

	__pci_attr_w32(br_devind, CBB_IOLIMIT_0, io_base + io_size - 1);
	__pci_attr_w32(br_devind, CBB_IOBASE_0, io_base);

	v16 = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, v16 | PCI_CR_IO_EN | PCI_CR_MAST_EN);
}

static int
do_piix(int devind)
{
	int i, s, irqrc, irq;
	u32_t elcr1, elcr2, elcr;

#if DEBUG
	printf("in piix\n");
#endif
	
	s = sys_inb(PIIX_ELCR1, &elcr1);
	if (s != OK) {
		printf("Warning, sys_inb failed: %d\n", s);
		return -1;
	}
	
	s = sys_inb(PIIX_ELCR2, &elcr2);
	if (s != OK) {
		printf("Warning, sys_inb failed: %d\n", s);
		return -1;
	}
	
	elcr = elcr1 | (elcr2 << 8);
	
	for (i = 0; i < 4; i++) {
		irqrc = __pci_attr_r8(devind, PIIX_PIRQRCA + i);
		
		if (irqrc & PIIX_IRQ_DI) {
			if (debug) {
				printf("INT%c: disabled\n", 'A' + i);
			}
			continue;
		}
		
		irq = irqrc & PIIX_IRQ_MASK;
		if (debug) {
			printf("INT%c: %d\n", 'A' + i, irq);
		}
		
		if (!(elcr & (1 << irq)) && debug) {
			printf("(warning) IRQ %d is not level triggered\n", irq);
		}
		
		irq_mode_pci(irq);
	}
	
	return 0;
}

static int
do_amd_isabr(int devind)
{
	int i, busnr, dev, func, xdevind, irq, edge;
	u8_t levmask;
	u16_t pciirq;

	if (devind < 0 || devind >= nr_pcidev) {
		return -1;
	}

	func = AMD_ISABR_FUNC;
	busnr = pcidev[devind].pd_busnr;
	dev = pcidev[devind].pd_dev;

	if (nr_pcidev >= NR_PCIDEV) {
		panic("too many PCI devices: %d", nr_pcidev);
	}
	
	xdevind = nr_pcidev;
	pcidev[xdevind].pd_busnr = busnr;
	pcidev[xdevind].pd_dev = dev;
	pcidev[xdevind].pd_func = func;
	pcidev[xdevind].pd_inuse = 1;
	nr_pcidev++;

	levmask = __pci_attr_r8(xdevind, AMD_ISABR_PCIIRQ_LEV);
	pciirq = __pci_attr_r16(xdevind, AMD_ISABR_PCIIRQ_ROUTE);
	
	for (i = 0; i < 4; i++) {
		edge = (levmask >> i) & 1;
		irq = (pciirq >> (4 * i)) & 0xf;
		
		if (irq == 0) {
			if (debug) {
				printf("INT%c: disabled\n", 'A' + i);
			}
		} else {
			if (debug) {
				printf("INT%c: %d\n", 'A' + i, irq);
			}
			if (edge && debug) {
				printf("(warning) IRQ %d is not level triggered\n", irq);
			}
			irq_mode_pci(irq);
		}
	}
	
	nr_pcidev--;
	return 0;
}

static int
do_sis_isabr(int devind)
{
	int i, irq;

	for (i = 0; i < 4; i++)
	{
		irq = __pci_attr_r8(devind, SIS_ISABR_IRQ_A + i);
		if (irq & SIS_IRQ_DISABLED)
		{
			if (debug)
				printf("INT%c: disabled\n", 'A' + i);
		}
		else
		{
			irq &= SIS_IRQ_MASK;
			if (debug)
				printf("INT%c: %d\n", 'A' + i, irq);
			irq_mode_pci(irq);
		}
	}
	return 0;
}

static int
do_via_isabr(int devind)
{
    u8_t levmask;
    int i, irq, edge;
    
    struct irq_config {
        u8_t edge_mask;
        u8_t reg_addr;
        int shift;
    };
    
    static const struct irq_config configs[4] = {
        {VIA_ISABR_EL_INTA, VIA_ISABR_IRQ_R2, 4},
        {VIA_ISABR_EL_INTB, VIA_ISABR_IRQ_R2, 0},
        {VIA_ISABR_EL_INTC, VIA_ISABR_IRQ_R3, 4},
        {VIA_ISABR_EL_INTD, VIA_ISABR_IRQ_R1, 4}
    };
    
    levmask = __pci_attr_r8(devind, VIA_ISABR_EL);
    
    for (i = 0; i < 4; i++)
    {
        edge = (levmask & configs[i].edge_mask);
        irq = __pci_attr_r8(devind, configs[i].reg_addr);
        if (configs[i].shift > 0)
            irq >>= configs[i].shift;
        irq &= 0xf;
        
        if (!irq)
        {
            if (debug)
                printf("INT%c: disabled\n", 'A' + i);
        }
        else
        {
            if (debug)
                printf("INT%c: %d\n", 'A' + i, irq);
            if (edge && debug)
            {
                printf("(warning) IRQ %d is not level triggered\n", irq);
            }
            irq_mode_pci(irq);
        }
    }
    return 0;
}

static int
do_isabridge(int busind)
{
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return 0;
    }

    int unknown_bridge = -1;
    int bridge_dev = -1;
    int j = 0;
    u16_t vid = 0;
    u16_t did = 0;
    int busnr = pcibus[busind].pb_busnr;

    for (int i = 0; i < nr_pcidev; i++) {
        if (pcidev[i].pd_busnr != busnr) {
            continue;
        }

        u32_t class_code = ((pcidev[i].pd_baseclass << 16) |
                           (pcidev[i].pd_subclass << 8) | 
                           pcidev[i].pd_infclass);
        
        if (class_code == PCI_T3_ISA) {
            unknown_bridge = i;
        }

        vid = pcidev[i].pd_vid;
        did = pcidev[i].pd_did;
        
        for (j = 0; pci_isabridge[j].vid != 0; j++) {
            if (pci_isabridge[j].vid != vid || 
                pci_isabridge[j].did != did) {
                continue;
            }
            
            if (pci_isabridge[j].checkclass && unknown_bridge != i) {
                continue;
            }
            break;
        }
        
        if (pci_isabridge[j].vid != 0) {
            bridge_dev = i;
            break;
        }
    }

    if (bridge_dev != -1) {
        const char *dstr = _pci_dev_name(vid, did);
        if (!dstr) {
            dstr = "unknown device";
        }
        
        if (debug) {
            printf("found ISA bridge (%04X:%04X) %s\n", vid, did, dstr);
        }
        
        pcibus[busind].pb_isabridge_dev = bridge_dev;
        int type = pci_isabridge[j].type;
        pcibus[busind].pb_isabridge_type = type;
        
        int result = 0;
        switch (type) {
        case PCI_IB_PIIX:
            result = do_piix(bridge_dev);
            break;
        case PCI_IB_VIA:
            result = do_via_isabr(bridge_dev);
            break;
        case PCI_IB_AMD:
            result = do_amd_isabr(bridge_dev);
            break;
        case PCI_IB_SIS:
            result = do_sis_isabr(bridge_dev);
            break;
        default:
            panic("unknown ISA bridge type: %d", type);
        }
        return result;
    }

    if (unknown_bridge == -1) {
        if (debug) {
            printf("(warning) no ISA bridge found on bus %d\n", busind);
        }
        return 0;
    }
    
    if (debug) {
        printf("(warning) unsupported ISA bridge %04X:%04X for bus %d\n",
               pcidev[unknown_bridge].pd_vid,
               pcidev[unknown_bridge].pd_did, busind);
    }
    return 0;
}

static int
derive_irq(struct pcidev * dev, int pin)
{
	struct pcidev * parent_bridge;
	int slot;
	int bus_index;

	if (!dev) {
		return -1;
	}

	bus_index = get_busind(dev->pd_busnr);
	if (bus_index < 0 || bus_index >= MAX_PCIBUS) {
		return -1;
	}

	parent_bridge = &pcidev[pcibus[bus_index].pb_devind];
	if (!parent_bridge) {
		return -1;
	}

	slot = (dev->pd_func >> 3) & 0x1f;

	return acpi_get_irq(parent_bridge->pd_busnr,
			parent_bridge->pd_dev, (pin + slot) % 4);
}

static void
record_irq(int devind)
{
	int ilr, ipr, busnr, busind, cb_devind;

	ilr = __pci_attr_r8(devind, PCI_ILR);
	ipr = __pci_attr_r8(devind, PCI_IPR);

	if (ipr && machine.apic_enabled) {
		int irq = acpi_get_irq(pcidev[devind].pd_busnr,
				pcidev[devind].pd_dev, ipr - 1);

		if (irq < 0) {
			irq = derive_irq(&pcidev[devind], ipr - 1);
		}

		if (irq >= 0) {
			ilr = irq;
			__pci_attr_w8(devind, PCI_ILR, ilr);
			if (debug) {
				printf("PCI: ACPI IRQ %d for device %d.%d.%d INT%c\n",
						irq,
						pcidev[devind].pd_busnr,
						pcidev[devind].pd_dev,
						pcidev[devind].pd_func,
						'A' + ipr - 1);
			}
		} else if (debug) {
			printf("PCI: no ACPI IRQ routing for device %d.%d.%d INT%c\n",
					pcidev[devind].pd_busnr,
					pcidev[devind].pd_dev,
					pcidev[devind].pd_func,
					'A' + ipr - 1);
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
		return;
	}
	
	if (ilr != PCI_ILR_UNKNOWN && ipr) {
		if (debug) {
			printf("\tIRQ %d for INT%c\n", ilr, 'A' + ipr - 1);
		}
		return;
	}
	
	if (ilr != PCI_ILR_UNKNOWN) {
		printf("PCI: IRQ %d is assigned, but device %d.%d.%d does not need it\n",
			ilr, pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
			pcidev[devind].pd_func);
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	if (pcibus[busind].pb_type == PBT_CARDBUS) {
		cb_devind = pcibus[busind].pb_devind;
		ilr = pcidev[cb_devind].pd_ilr;
		if (ilr != PCI_ILR_UNKNOWN) {
			if (debug) {
				printf("assigning IRQ %d to Cardbus device\n", ilr);
			}
			__pci_attr_w8(devind, PCI_ILR, ilr);
			pcidev[devind].pd_ilr = ilr;
			return;
		}
	}
	
	if (debug) {
		printf("PCI: device %d.%d.%d uses INT%c but is not assigned any IRQ\n",
			pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
			pcidev[devind].pd_func, 'A' + ipr - 1);
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
		return record_io_bar(devind, reg, bar, bar_nr);
	} else {
		return record_memory_bar(devind, reg, bar, bar_nr, last);
	}
}

static int
record_io_bar(int devind, int reg, u32_t bar, int bar_nr)
{
	u32_t bar2;
	u16_t cmd;
	int dev_bar_nr;

	cmd = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, cmd & ~PCI_CR_IO_EN);

	__pci_attr_w32(devind, reg, 0xffffffff);
	bar2 = __pci_attr_r32(devind, reg);

	__pci_attr_w32(devind, reg, bar);
	__pci_attr_w16(devind, PCI_CR, cmd);

	bar &= PCI_BAR_IO_MASK;
	bar2 &= PCI_BAR_IO_MASK;
	bar2 = (~bar2 & 0xffff) + 1;

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

	return 1;
}

static int
record_memory_bar(int devind, int reg, u32_t bar, int bar_nr, int last)
{
	int type, dev_bar_nr, width, prefetch;
	u32_t bar2;
	u16_t cmd;

	width = 1;
	type = (bar & PCI_BAR_TYPE);

	switch (type) {
	case PCI_TYPE_32:
	case PCI_TYPE_32_1M:
		break;

	case PCI_TYPE_64:
		if (last) {
			printf("PCI: device %d.%d.%d BAR %d extends beyond designated area\n",
				pcidev[devind].pd_busnr,
				pcidev[devind].pd_dev,
				pcidev[devind].pd_func, bar_nr);
			return 1;
		}
		width = 2;

		bar2 = __pci_attr_r32(devind, reg + 4);
		if (bar2 != 0) {
			if (debug) {
				printf("\tbar_%d: (64-bit BAR with high bits set)\n", bar_nr);
			}
			return width;
		}
		break;

	default:
		if (debug) {
			printf("\tbar_%d: (unknown type %x)\n", bar_nr, type);
		}
		return 1;
	}

	cmd = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, cmd & ~PCI_CR_MEM_EN);

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
	bar2 = (~bar2) + 1;

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

static void
record_bars(int devind, int last_reg)
{
	int i, reg, width;

	if (last_reg < PCI_BAR) {
		return;
	}

	for (i = 0, reg = PCI_BAR; reg <= last_reg; i += width, reg += 4 * width)
	{
		width = record_bar(devind, i, reg == last_reg);
		if (width <= 0) {
			break;
		}
	}
}

static void
record_bars_normal(int devind)
{
	int i, j, clear_01, clear_23, pb_nr;

	record_bars(devind, PCI_BAR_6);

	if (pcidev[devind].pd_baseclass != PCI_BCR_MASS_STORAGE ||
		pcidev[devind].pd_subclass != PCI_MS_IDE)
	{
		return;
	}

	clear_01 = !(pcidev[devind].pd_infclass & PCI_IDE_PRI_NATIVE);
	clear_23 = !(pcidev[devind].pd_infclass & PCI_IDE_SEC_NATIVE);

	if (debug && clear_01)
	{
		printf("primary channel is not in native mode, clearing BARs 0 and 1\n");
	}
	if (debug && clear_23)
	{
		printf("secondary channel is not in native mode, clearing BARs 2 and 3\n");
	}

	j = 0;
	for (i = 0; i < pcidev[devind].pd_bar_nr; i++)
	{
		pb_nr = pcidev[devind].pd_bar[i].pb_nr;
		if ((pb_nr <= 1 && clear_01) || (pb_nr >= 2 && pb_nr <= 3 && clear_23))
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

static void
record_bars_bridge(int devind)
{
	u32_t base, limit, size;

	record_bars(devind, PCI_BAR_2);

	base = ((__pci_attr_r8(devind, PPB_IOBASE) & PPB_IOB_MASK) << 8) |
		(__pci_attr_r16(devind, PPB_IOBASEU16) << 16);
	limit = 0xff |
		((__pci_attr_r8(devind, PPB_IOLIMIT) & PPB_IOL_MASK) << 8) |
		((~PPB_IOL_MASK & 0xff) << 8) |
		(__pci_attr_r16(devind, PPB_IOLIMITU16) << 16);
	size = limit - base + 1;
	if (debug) {
		printf("\tI/O window: base 0x%x, limit 0x%x, size %d\n",
			base, limit, size);
	}

	base = (__pci_attr_r16(devind, PPB_MEMBASE) & PPB_MEMB_MASK) << 16;
	limit = 0xffff |
		((__pci_attr_r16(devind, PPB_MEMLIMIT) & PPB_MEML_MASK) << 16) |
		((~PPB_MEML_MASK & 0xffff) << 16);
	size = limit - base + 1;
	if (debug) {
		printf("\tMemory window: base 0x%x, limit 0x%x, size 0x%x\n",
			base, limit, size);
	}

	base = (__pci_attr_r16(devind, PPB_PFMEMBASE) & PPB_PFMEMB_MASK) << 16;
	limit = 0xffff |
		((__pci_attr_r16(devind, PPB_PFMEMLIMIT) & PPB_PFMEML_MASK) << 16) |
		((~PPB_PFMEML_MASK & 0xffff) << 16);
	size = limit - base + 1;
	if (debug) {
		printf("\tPrefetchable memory window: base 0x%x, limit 0x%x, size 0x%x\n",
			base, limit, size);
	}
}

static void
record_bars_cardbus(int devind)
{
	record_bars(devind, PCI_BAR);
	
	record_cardbus_window(devind, CBB_MEMBASE_0, CBB_MEMLIMIT_0, CBB_MEML_MASK, "Memory window 0");
	record_cardbus_window(devind, CBB_MEMBASE_1, CBB_MEMLIMIT_1, CBB_MEML_MASK, "Memory window 1");
	record_cardbus_window(devind, CBB_IOBASE_0, CBB_IOLIMIT_0, CBB_IOL_MASK, "I/O window 0");
	record_cardbus_window(devind, CBB_IOBASE_1, CBB_IOLIMIT_1, CBB_IOL_MASK, "I/O window 1");
}

static void
record_cardbus_window(int devind, int base_reg, int limit_reg, u32_t mask, const char* window_name)
{
	u32_t base = __pci_attr_r32(devind, base_reg);
	u32_t limit = __pci_attr_r32(devind, limit_reg) | (~mask & 0xffffffff);
	u32_t size = limit - base + 1;
	
	if (debug) {
		printf("\t%s: base 0x%x, limit 0x%x, size %u\n", window_name, base, limit, size);
	}
}

static void
complete_bars(void)
{
    int i, j, bar_nr, reg;
    u32_t memgap_low, memgap_high, iogap_low, iogap_high, io_high,
        base, size, v32, diff1, diff2;
    kinfo_t kinfo;

    if (OK != sys_getkinfo(&kinfo)) {
        panic("can't get kinfo");
    }

    memgap_low = kinfo.mem_high_phys;
    memgap_high = 0xfe000000;

    if (debug) {
        printf("complete_bars: initial gap: [0x%x .. 0x%x>\n",
            memgap_low, memgap_high);
    }

    for (i = 0; i < nr_pcidev; i++) {
        for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
            if (pcidev[i].pd_bar[j].pb_flags & PBF_IO) {
                continue;
            }
            if (pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE) {
                continue;
            }
            base = pcidev[i].pd_bar[j].pb_base;
            size = pcidev[i].pd_bar[j].pb_size;

            if (base >= memgap_high) {
                continue;
            }
            if (base + size <= memgap_low) {
                continue;
            }

            diff1 = base + size - memgap_low;
            diff2 = memgap_high - base;

            if (diff1 < diff2) {
                memgap_low = base + size;
            } else {
                memgap_high = base;
            }
        }
    }

    if (debug) {
        printf("complete_bars: intermediate gap: [0x%x .. 0x%x>\n",
            memgap_low, memgap_high);
    }

    if (memgap_high < memgap_low) {
        printf("PCI: bad memory gap: [0x%x .. 0x%x>\n",
            memgap_low, memgap_high);
        panic("bad memory gap");
    }

    iogap_high = 0x10000;
    iogap_low = 0x400;

    for (i = 0; i < nr_pcidev; i++) {
        for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
            if (!(pcidev[i].pd_bar[j].pb_flags & PBF_IO)) {
                continue;
            }
            if (pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE) {
                continue;
            }
            base = pcidev[i].pd_bar[j].pb_base;
            size = pcidev[i].pd_bar[j].pb_size;
            if (base >= iogap_high) {
                continue;
            }
            if (base + size <= iogap_low) {
                continue;
            }

            if (base + size - iogap_low < iogap_high - base) {
                iogap_low = base + size;
            } else {
                iogap_high = base;
            }
        }
    }

    if (iogap_high < iogap_low) {
        panic("iogap_high too low: %d", iogap_high);
    }

    if (debug) {
        printf("I/O range = [0x%x..0x%x>\n", iogap_low, iogap_high);
    }

    for (i = 0; i < nr_pcidev; i++) {
        for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
            if (pcidev[i].pd_bar[j].pb_flags & PBF_IO) {
                continue;
            }
            if (!(pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE)) {
                continue;
            }
            size = pcidev[i].pd_bar[j].pb_size;
            if (size < PAGE_SIZE) {
                size = PAGE_SIZE;
            }
            base = memgap_high - size;
            base &= ~(u32_t)(size - 1);
            if (base < memgap_low) {
                panic("memory base too low: %d", base);
            }
            memgap_high = base;
            bar_nr = pcidev[i].pd_bar[j].pb_nr;
            reg = PCI_BAR + 4 * bar_nr;
            v32 = __pci_attr_r32(i, reg);
            __pci_attr_w32(i, reg, v32 | base);
            if (debug) {
                printf(
                    "complete_bars: allocated 0x%x size %d to %d.%d.%d, bar_%d\n",
                    base, size, pcidev[i].pd_busnr,
                    pcidev[i].pd_dev, pcidev[i].pd_func,
                    bar_nr);
            }
            pcidev[i].pd_bar[j].pb_base = base;
            pcidev[i].pd_bar[j].pb_flags &= ~PBF_INCOMPLETE;
        }

        io_high = iogap_high;
        for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
            if (!(pcidev[i].pd_bar[j].pb_flags & PBF_IO)) {
                continue;
            }
            if (!(pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE)) {
                continue;
            }
            size = pcidev[i].pd_bar[j].pb_size;
            base = iogap_high - size;
            base &= ~(u32_t)(size - 1);

            base &= 0xfcff;

            if (base < iogap_low) {
                printf("I/O base too low: %d", base);
            }

            iogap_high = base;
            bar_nr = pcidev[i].pd_bar[j].pb_nr;
            reg = PCI_BAR + 4 * bar_nr;
            v32 = __pci_attr_r32(i, reg);
            __pci_attr_w32(i, reg, v32 | base);
            if (debug) {
                printf(
                    "complete_bars: allocated 0x%x size %d to %d.%d.%d, bar_%d\n",
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
            if (!(pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE)) {
                continue;
            }
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

	if (debug)
		printf("probe_bus(%d)\n", busind);
	
	if (nr_pcidev >= NR_PCIDEV)
		panic("too many PCI devices: %d", nr_pcidev);
	
	devind = nr_pcidev;
	busnr = pcibus[busind].pb_busnr;
	
	for (dev = 0; dev < 32; dev++) {
		for (func = 0; func < 8; func++) {
			if (nr_pcidev >= NR_PCIDEV)
				panic("too many PCI devices: %d", nr_pcidev);
				
			pcidev[devind].pd_busnr = busnr;
			pcidev[devind].pd_dev = dev;
			pcidev[devind].pd_func = func;

			pci_attr_wsts(devind, PSR_SSE|PSR_RMAS|PSR_RTAS);
			vid = __pci_attr_r16(devind, PCI_VID);
			did = __pci_attr_r16(devind, PCI_DID);
			headt = __pci_attr_r8(devind, PCI_HEADT);
			sts = pci_attr_rsts(devind);

			if (vid == NO_VID && did == NO_VID) {
				if (func == 0)
					break;
				continue;
			}

			if (sts & (PSR_SSE|PSR_RMAS|PSR_RTAS)) {
				static int warned = 0;
				if (!warned) {
					printf("PCI: ignoring bad value 0x%x in sts for QEMU\n",
						sts & (PSR_SSE|PSR_RMAS|PSR_RTAS));
					warned = 1;
				}
			}

			sub_vid = __pci_attr_r16(devind, PCI_SUBVID);
			sub_did = __pci_attr_r16(devind, PCI_SUBDID);

			if (debug) {
				dstr = _pci_dev_name(vid, did);
				if (dstr) {
					printf("%d.%lu.%lu: %s (%04X:%04X)\n",
						busnr, (unsigned long)dev,
						(unsigned long)func, dstr, vid, did);
				} else {
					printf("%d.%lu.%lu: Unknown device, vendor %04X (%s), device %04X\n",
						busnr, (unsigned long)dev,
						(unsigned long)func, vid,
						pci_vid_name(vid), did);
				}
				printf("Device index: %d\n", devind);
				printf("Subsystem: Vid 0x%x, did 0x%x\n", sub_vid, sub_did);
			}

			baseclass = __pci_attr_r8(devind, PCI_BCR);
			subclass = __pci_attr_r8(devind, PCI_SCR);
			infclass = __pci_attr_r8(devind, PCI_PIFR);
			
			s = pci_subclass_name(baseclass << 24 | subclass << 16);
			if (!s)
				s = pci_baseclass_name(baseclass << 24);
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
			
			pcidev[devind].pd_busnr = busnr;
			pcidev[devind].pd_dev = dev;
			pcidev[devind].pd_func = func;
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

			devind = nr_pcidev;

			if (func == 0 && !(headt & PHT_MULTIFUNC))
				break;
		}
	}
}


static u16_t
pcibr_std_rsts(int busind)
{
	if (busind < 0 || busind >= MAX_NR_PCIBUS || pcibus[busind].pb_devind == -1) {
		return 0;
	}
	
	int devind = pcibus[busind].pb_devind;
	return __pci_attr_r16(devind, PPB_SSTS);
}

static void
pcibr_std_wsts(int busind, u16_t value)
{
	if (busind < 0 || busind >= MAX_PCIBUS) {
		return;
	}
	
	int devind = pcibus[busind].pb_devind;
	
	if (devind < 0) {
		return;
	}
	
	__pci_attr_w16(devind, PPB_SSTS, value);
}

static u16_t
pcibr_cb_rsts(int busind)
{
    if (busind < 0 || busind >= MAX_PCIBUS || pcibus[busind].pb_devind == -1) {
        return 0xFFFF;
    }
    
    int devind = pcibus[busind].pb_devind;
    return __pci_attr_r16(devind, CBB_SSTS);
}

static void
pcibr_cb_wsts(int busind, u16_t value)
{
	if (busind < 0 || busind >= NR_PCIBUS || !pcibus[busind].pb_devind) {
		return;
	}
	
	int devind = pcibus[busind].pb_devind;
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
    if (pcibus == NULL || pcidev == NULL) {
        return;
    }

    for (int i = 0; i < nr_pcibus; i++) {
        if (!pcibus[i].pb_needinit) {
            continue;
        }

        int freebus = get_freebus();
        if (freebus < 0) {
            continue;
        }

        int devind = pcibus[i].pb_devind;
        if (devind < 0) {
            continue;
        }

        int prim_busnr = pcidev[devind].pd_busnr;
        if (prim_busnr != 0) {
            continue;
        }

        pcibus[i].pb_needinit = 0;
        pcibus[i].pb_busnr = freebus;

        __pci_attr_w8(devind, PPB_PRIMBN, prim_busnr);
        __pci_attr_w8(devind, PPB_SECBN, freebus);
        __pci_attr_w8(devind, PPB_SUBORDBN, freebus);
    }
}

static void
do_pcibridge(int busind)
{
	int devind, busnr;
	int ind, type;
	u16_t vid, did;
	u8_t sbusn, baseclass, subclass, infclass, headt;
	u32_t t3;

	if (busind < 0 || busind >= nr_pcibus)
		return;

	vid = did = 0;
	busnr = pcibus[busind].pb_busnr;
	
	for (devind = 0; devind < nr_pcidev; devind++)
	{
		if (pcidev[devind].pd_busnr != busnr)
			continue;

		vid = pcidev[devind].pd_vid;
		did = pcidev[devind].pd_did;

		headt = __pci_attr_r8(devind, PCI_HEADT);
		type = 0;
		
		if ((headt & PHT_MASK) == PHT_BRIDGE)
			type = PCI_PPB_STD;
		else if ((headt & PHT_MASK) == PHT_CARDBUS)
			type = PCI_PPB_CB;
		else
			continue;

		baseclass = __pci_attr_r8(devind, PCI_BCR);
		subclass = __pci_attr_r8(devind, PCI_SCR);
		infclass = __pci_attr_r8(devind, PCI_PIFR);
		t3 = ((baseclass << 16) | (subclass << 8) | infclass);
		
		if (type == PCI_PPB_STD &&
			t3 != PCI_T3_PCI2PCI &&
			t3 != PCI_T3_PCI2PCI_SUBTR)
		{
			printf("Unknown PCI class %02x/%02x/%02x for PCI-to-PCI bridge, device %04X:%04X\n",
				baseclass, subclass, infclass, vid, did);
			continue;
		}
		
		if (type == PCI_PPB_CB && t3 != PCI_T3_CARDBUS)
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
		
		ind = nr_pcibus;
		nr_pcibus++;
		
		pcibus[ind].pb_type = PBT_PCIBRIDGE;
		pcibus[ind].pb_needinit = 1;
		pcibus[ind].pb_isabridge_dev = -1;
		pcibus[ind].pb_isabridge_type = 0;
		pcibus[ind].pb_devind = devind;
		pcibus[ind].pb_busnr = sbusn;
		pcibus[ind].pb_rreg8 = pcibus[busind].pb_rreg8;
		pcibus[ind].pb_rreg16 = pcibus[busind].pb_rreg16;
		pcibus[ind].pb_rreg32 = pcibus[busind].pb_rreg32;
		pcibus[ind].pb_wreg8 = pcibus[busind].pb_wreg8;
		pcibus[ind].pb_wreg16 = pcibus[busind].pb_wreg16;
		pcibus[ind].pb_wreg32 = pcibus[busind].pb_wreg32;
		
		switch(type)
		{
		case PCI_PPB_STD:
			pcibus[ind].pb_rsts = pcibr_std_rsts;
			pcibus[ind].pb_wsts = pcibr_std_wsts;
			break;
		case PCI_PPB_CB:
			pcibus[ind].pb_type = PBT_CARDBUS;
			pcibus[ind].pb_rsts = pcibr_cb_rsts;
			pcibus[ind].pb_wsts = pcibr_cb_wsts;
			break;
		case PCI_AGPB_VIA:
			pcibus[ind].pb_rsts = pcibr_via_rsts;
			pcibus[ind].pb_wsts = pcibr_via_wsts;
			break;
		default:
			panic("unknown PCI-PCI bridge type: %d", type);
		}

		if (machine.apic_enabled)
		{
			acpi_map_bridge(pcidev[devind].pd_busnr,
					pcidev[devind].pd_dev, sbusn);
		}

		if (debug)
		{
			printf("bus(table) = %d, bus(sec) = %d, bus(subord) = %d\n",
				ind, sbusn, __pci_attr_r8(devind, PPB_SUBORDBN));
		}
		
		if (sbusn == 0)
		{
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
pci_intel_init(void)
{
    u16_t vid, did;
    int s, i, r, busind, busnr;
    const char *dstr;

    if (nr_pcibus >= NR_PCIBUS) {
        panic("too many PCI busses: %d", nr_pcibus);
    }

    vid = PCII_RREG16_(0, 0, 0, PCI_VID);
    did = PCII_RREG16_(0, 0, 0, PCI_DID);
    
    s = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (s != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", s);
    }

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
    if (!dstr) {
        dstr = "unknown device";
    }
    
    if (debug) {
        printf("pci_intel_init: %s (%04X:%04X)\n", dstr, vid, did);
    }

    probe_bus(busind);

    r = do_isabridge(busind);
    if (r != OK) {
        busnr = pcibus[busind].pb_busnr;
        
        for (i = 0; i < nr_pcidev; i++) {
            if (pcidev[i].pd_busnr == busnr) {
                pcidev[i].pd_inuse = 1;
            }
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
static int
visible(struct rs_pci *aclp, int devind)
{
	u16_t acl_sub_vid, acl_sub_did;
	int i;
	u32_t class_id;

	if (!aclp) {
		return TRUE;
	}

	for (i = 0; i < aclp->rsp_nr_device; i++) {
		acl_sub_vid = aclp->rsp_device[i].sub_vid;
		acl_sub_did = aclp->rsp_device[i].sub_did;
		
		if (aclp->rsp_device[i].vid == pcidev[devind].pd_vid &&
		    aclp->rsp_device[i].did == pcidev[devind].pd_did &&
		    (acl_sub_vid == NO_SUB_VID || acl_sub_vid == pcidev[devind].pd_sub_vid) &&
		    (acl_sub_did == NO_SUB_DID || acl_sub_did == pcidev[devind].pd_sub_did)) {
			return TRUE;
		}
	}

	if (aclp->rsp_nr_class == 0) {
		return FALSE;
	}

	class_id = (pcidev[devind].pd_baseclass << 16) |
		   (pcidev[devind].pd_subclass << 8) |
		   pcidev[devind].pd_infclass;

	for (i = 0; i < aclp->rsp_nr_class; i++) {
		if (aclp->rsp_class[i].pciclass == (class_id & aclp->rsp_class[i].mask)) {
			return TRUE;
		}
	}

	return FALSE;
}

/*===========================================================================*
 *				sef_cb_init_fresh			     *
 *===========================================================================*/
int
sef_cb_init(int type, sef_init_info_t *info)
{
    long v = 0;
    int r;
    struct rprocpub rprocpub[NR_BOOT_PROCS];
    int do_announce_driver;

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

    r = sys_safecopyfrom(RS_PROC_NR, info->rproctab_gid, 0,
                         (vir_bytes) rprocpub, sizeof(rprocpub));
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

    switch (type) {
    case SEF_INIT_FRESH:
    case SEF_INIT_RESTART:
        do_announce_driver = TRUE;
        break;
    case SEF_INIT_LU:
        do_announce_driver = FALSE;
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

    if (!rpub) {
        return EINVAL;
    }

    if (rpub->pci_acl.rsp_nr_device == 0 && rpub->pci_acl.rsp_nr_class == 0) {
        return OK;
    }

    for (i = 0; i < NR_DRIVERS; i++) {
        if (!pci_acl[i].inuse) {
            break;
        }
    }

    if (i >= NR_DRIVERS) {
        printf("PCI: map_service: table is full\n");
        return ENOMEM;
    }

    pci_acl[i].inuse = 1;
    pci_acl[i].acl = rpub->pci_acl;

    return OK;
}

/*===========================================================================*
 *				_pci_find_dev				     *
 *===========================================================================*/
int
_pci_find_dev(u8_t bus, u8_t dev, u8_t func, int *devindp)
{
	int devind;

	if (devindp == NULL)
		return 0;

	for (devind = 0; devind < nr_pcidev; devind++)
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
_pci_first_dev(struct rs_pci *aclp, int *devindp, u16_t *vidp, u16_t *didp)
{
	int devind;

	if (!aclp || !devindp || !vidp || !didp)
		return 0;

	for (devind = 0; devind < nr_pcidev; devind++)
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
	int devind;

	if (!aclp || !devindp || !vidp || !didp)
		return 0;

	for (devind = *devindp + 1; devind < nr_pcidev; devind++) {
		if (visible(aclp, devind))
			break;
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
int
_pci_grant_access(int devind, endpoint_t proc)
{
	int i, ilr;
	int r = OK;
	int final_result = OK;
	struct io_range ior;
	struct minix_mem_range mr;

	if (devind < 0 || proc < 0) {
		return EINVAL;
	}

	for (i = 0; i < pcidev[devind].pd_bar_nr; i++)
	{
		if (pcidev[devind].pd_bar[i].pb_flags & PBF_INCOMPLETE)
		{
			printf("pci_reserve_a: BAR %d is incomplete\n", i);
			continue;
		}

		if (pcidev[devind].pd_bar[i].pb_flags & PBF_IO)
		{
			ior.ior_base = pcidev[devind].pd_bar[i].pb_base;
			ior.ior_limit = ior.ior_base + pcidev[devind].pd_bar[i].pb_size - 1;

			if (debug) {
				printf("pci_reserve_a: for proc %d, adding I/O range [0x%x..0x%x]\n",
					proc, ior.ior_base, ior.ior_limit);
			}
			r = sys_privctl(proc, SYS_PRIV_ADD_IO, &ior);
		}
		else
		{
			mr.mr_base = pcidev[devind].pd_bar[i].pb_base;
			mr.mr_limit = mr.mr_base + pcidev[devind].pd_bar[i].pb_size - 1;

			r = sys_privctl(proc, SYS_PRIV_ADD_MEM, &mr);
		}

		if (r != OK)
		{
			printf("sys_privctl failed for proc %d: %d\n", proc, r);
			final_result = r;
		}
	}

	ilr = pcidev[devind].pd_ilr;
	if (ilr != PCI_ILR_UNKNOWN)
	{
		if (debug) {
			printf("pci_reserve_a: adding IRQ %d\n", ilr);
		}
		r = sys_privctl(proc, SYS_PRIV_ADD_IRQ, &ilr);
		if (r != OK)
		{
			printf("sys_privctl failed for proc %d: %d\n", proc, r);
			final_result = r;
		}
	}

	return final_result;
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
void
_pci_release(endpoint_t proc)
{
	if (proc == 0) {
		return;
	}

	for (int i = 0; i < nr_pcidev; i++)
	{
		if (pcidev[i].pd_inuse && pcidev[i].pd_proc == proc)
		{
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
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	
	if (vidp == NULL || didp == NULL)
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
    int busind = get_busind(busnr);
    if (busind < 0) {
        return;
    }
    
    if (probe_bus(busind) != 0) {
        return;
    }
    
    complete_bridges();
    complete_bars();
}

/*===========================================================================*
 *				_pci_slot_name				     *
 *===========================================================================*/
int
_pci_slot_name(int devind, char **cpp)
{
	static char label[16];
	char *p;
	char *end;

	if (devind < 0 || devind >= nr_pcidev || cpp == NULL)
		return EINVAL;

	p = label;
	end = label + sizeof(label);

	ntostr(0, &p, end);
	if (p >= end - 1) return EINVAL;
	*p++ = '.';

	ntostr(pcidev[devind].pd_busnr, &p, end);
	if (p >= end - 1) return EINVAL;
	*p++ = '.';

	ntostr(pcidev[devind].pd_dev, &p, end);
	if (p >= end - 1) return EINVAL;
	*p++ = '.';

	ntostr(pcidev[devind].pd_func, &p, end);
	if (p >= end) return EINVAL;

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
	
	if (pci_findproduct(product, sizeof(product), vid, did) != 0) {
		product[0] = '\0';
	}
	
	return product;
}

/*===========================================================================*
 *				_pci_get_bar				     *
 *===========================================================================*/
int
_pci_get_bar(int devind, int port, u32_t *base, u32_t *size, int *ioflag)
{
    int i, reg;
    
    if (devind < 0 || devind >= nr_pcidev || !base || !size || !ioflag)
        return EINVAL;

    for (i = 0; i < pcidev[devind].pd_bar_nr; i++)
    {
        reg = PCI_BAR + 4 * pcidev[devind].pd_bar[i].pb_nr;

        if (reg == port)
        {
            if (pcidev[devind].pd_bar[i].pb_flags & PBF_INCOMPLETE)
                return EINVAL;

            *base = pcidev[devind].pd_bar[i].pb_base;
            *size = pcidev[devind].pd_bar[i].pb_size;
            *ioflag = (pcidev[devind].pd_bar[i].pb_flags & PBF_IO) ? 1 : 0;
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
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > 255)
		return EINVAL;
	if (vp == NULL)
		return EINVAL;

	*vp = __pci_attr_r8(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_r16				     *
 *===========================================================================*/
int
_pci_attr_r16(int devind, int port, u16_t *vp)
{
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > 254)
		return EINVAL;
	if (vp == NULL)
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
	if (vp == NULL)
		return EINVAL;
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > 252)
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
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > 255)
		return EINVAL;

	__pci_attr_w8(devind, port, value);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w16				     *
 *===========================================================================*/
int
_pci_attr_w16(int devind, int port, u16_t value)
{
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > 254)
		return EINVAL;

	__pci_attr_w16(devind, port, value);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w32				     *
 *===========================================================================*/
int
_pci_attr_w32(int devind, int port, u32_t value)
{
	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}
	if (port < 0 || port > 252) {
		return EINVAL;
	}

	__pci_attr_w32(devind, port, value);
	return OK;
}
