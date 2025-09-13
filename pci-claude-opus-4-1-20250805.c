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
static unsigned pci_inb(u16_t port) {
    u32_t value = 0;
    int result = sys_inb(port, &value);
    
    if (result != OK) {
        printf("PCI: warning, sys_inb failed: %d\n", result);
        return 0;
    }
    
    return value;
}

static unsigned pci_inw(u16_t port) {
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
	int s = sys_inl(port, &value);
	if (s != OK) {
		printf("PCI: warning, sys_inl failed: %d\n", s);
	}
	return value;
}

static void pci_outb(u16_t port, u8_t value) {
    int result = sys_outb(port, value);
    if (result != OK) {
        printf("PCI: warning, sys_outb failed: %d\n", result);
    }
}

static void pci_outw(u16_t port, u16_t value) {
    int result = sys_outw(port, value);
    if (result != OK) {
        printf("PCI: warning, sys_outw failed: %d\n", result);
    }
}

static void pci_outl(u16_t port, u32_t value) {
    int result = sys_outl(port, value);
    if (result != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", result);
    }
}

static u8_t pcii_rreg8(int busind, int devind, int port)
{
    u8_t value;
    int status;

    value = PCII_RREG8_(pcibus[busind].pb_busnr,
                        pcidev[devind].pd_dev,
                        pcidev[devind].pd_func,
                        port);

    status = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (status != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", status);
    }

    return value;
}

static u16_t pcii_rreg16(int busind, int devind, int port)
{
    u16_t value;
    int result;

    value = PCII_RREG16_(pcibus[busind].pb_busnr,
                         pcidev[devind].pd_dev,
                         pcidev[devind].pd_func,
                         port);

    result = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (result != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", result);
    }

    return value;
}

static u32_t pcii_rreg32(int busind, int devind, int port)
{
    u32_t value;
    int status;

    value = PCII_RREG32_(pcibus[busind].pb_busnr,
                         pcidev[devind].pd_dev,
                         pcidev[devind].pd_func,
                         port);

    status = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (status != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", status);
    }

    return value;
}

static void
pcii_wreg8(int busind, int devind, int port, u8_t value)
{
	int result;
	
	PCII_WREG8_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port, value);
	
	result = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (result != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", result);
	}
}

static void
pcii_wreg16(int busind, int devind, int port, u16_t value)
{
	int s;
	
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
	int result;
	
	PCII_WREG32_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port, value);
	
	result = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (result != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", result);
	}
}

/*===========================================================================*
 *				ntostr					     *
 *===========================================================================*/
static void ntostr(unsigned int n, char **str, const char *end)
{
    char tmpstr[20];
    int i = 0;
    
    if (str == NULL || *str == NULL || end == NULL || *str >= end) {
        return;
    }
    
    if (n == 0) {
        tmpstr[0] = '0';
        i = 1;
    } else {
        while (n > 0 && i < sizeof(tmpstr)) {
            tmpstr[i] = '0' + (n % 10);
            n /= 10;
            i++;
        }
    }
    
    while (i > 0 && *str < end - 1) {
        i--;
        **str = tmpstr[i];
        (*str)++;
    }
    
    **str = '\0';
}

/*===========================================================================*
 *				get_busind					     *
 *===========================================================================*/
static int get_busind(int busnr)
{
	int i;

	for (i = 0; i < nr_pcibus; i++)
	{
		if (pcibus[i].pb_busnr == busnr)
			return i;
	}
	
	return -1;
}

/*===========================================================================*
 *			Unprotected helper functions			     *
 *===========================================================================*/
static u8_t __pci_attr_r8(int devind, int port)
{
    if (devind < 0 || devind >= MAX_PCIDEV) {
        return 0;
    }
    
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return 0;
    }
    
    if (pcibus[busind].pb_rreg8 == NULL) {
        return 0;
    }
    
    return pcibus[busind].pb_rreg8(busind, devind, port);
}

static u16_t __pci_attr_r16(int devind, int port)
{
    if (devind < 0 || devind >= MAX_PCI_DEVICES) {
        return 0;
    }
    
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (busind < 0 || busind >= MAX_PCI_BUSES) {
        return 0;
    }
    
    if (pcibus[busind].pb_rreg16 == NULL) {
        return 0;
    }
    
    return pcibus[busind].pb_rreg16(busind, devind, port);
}

static u32_t __pci_attr_r32(int devind, int port)
{
    if (devind < 0 || devind >= MAX_PCIDEV) {
        return 0;
    }
    
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return 0;
    }
    
    if (pcibus[busind].pb_rreg32 == NULL) {
        return 0;
    }
    
    return pcibus[busind].pb_rreg32(busind, devind, port);
}

static void __pci_attr_w8(int devind, int port, u8_t value)
{
    if (devind < 0 || devind >= MAX_PCI_DEVICES) {
        return;
    }
    
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (busind < 0 || busind >= MAX_PCI_BUSES) {
        return;
    }
    
    if (pcibus[busind].pb_wreg8 == NULL) {
        return;
    }
    
    pcibus[busind].pb_wreg8(busind, devind, port, value);
}

static void __pci_attr_w16(int devind, int port, u16_t value)
{
    if (devind < 0 || devind >= MAX_PCI_DEVICES) {
        return;
    }
    
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (busind < 0 || busind >= MAX_PCI_BUSES) {
        return;
    }
    
    if (pcibus[busind].pb_wreg16 != NULL) {
        pcibus[busind].pb_wreg16(busind, devind, port, value);
    }
}

static void __pci_attr_w32(int devind, int port, u32_t value)
{
    if (devind < 0 || devind >= MAX_PCIDEV) {
        return;
    }
    
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return;
    }
    
    if (pcibus[busind].pb_wreg32 != NULL) {
        pcibus[busind].pb_wreg32(busind, devind, port, value);
    }
}

/*===========================================================================*
 *				helpers					     *
 *===========================================================================*/
static u16_t
pci_attr_rsts(int devind)
{
	int busnr;
	int busind;

	if (devind < 0 || devind >= MAX_PCIDEV) {
		return 0;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);

	if (busind < 0 || busind >= MAX_PCIBUS) {
		return 0;
	}

	if (pcibus[busind].pb_rsts == NULL) {
		return 0;
	}

	return pcibus[busind].pb_rsts(busind);
}

static void pci_attr_wsts(int devind, u16_t value)
{
    if (devind < 0 || devind >= MAX_PCIDEV) {
        return;
    }
    
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return;
    }
    
    if (pcibus[busind].pb_wsts != NULL) {
        pcibus[busind].pb_wsts(busind, value);
    }
}

static u16_t pcii_rsts(int busind)
{
    u16_t status_value;
    int result;

    if (busind < 0 || busind >= MAX_PCIBUS) {
        return 0;
    }

    status_value = PCII_RREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR);
    
    result = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (result != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", result);
    }
    
    return status_value;
}

static void pcii_wsts(int busind, u16_t value)
{
    int result;
    
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return;
    }
    
    PCII_WREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR, value);
    
    result = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (result != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", result);
    }
}

static int is_duplicate(u8_t busnr, u8_t dev, u8_t func)
{
    if (pcidev == NULL || nr_pcidev < 0) {
        return 0;
    }
    
    for (int i = 0; i < nr_pcidev; i++) {
        if (pcidev[i].pd_busnr == busnr &&
            pcidev[i].pd_dev == dev &&
            pcidev[i].pd_func == func) {
            return 1;
        }
    }
    
    return 0;
}

static int get_freebus(void)
{
    int freebus = 1;
    
    for (int i = 0; i < nr_pcibus; i++)
    {
        if (pcibus[i].pb_needinit || pcibus[i].pb_type == PBT_INTEL_HOST)
        {
            continue;
        }
        
        if (pcibus[i].pb_busnr >= freebus)
        {
            freebus = pcibus[i].pb_busnr + 1;
        }
    }
    
    return freebus;
}

static const char *
pci_vid_name(u16_t vid)
{
	static char vendor[PCI_VENDORSTR_LEN];
	pci_findvendor(vendor, sizeof(vendor), vid);
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
		const char *type_str = (type0 == 0) ? 
			"Slave or Primary Interface" : 
			"Host or Secondary Interface";
		printf("Capability Type: %s\n", type_str);
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
		
		str = get_capability_name(type);
		
		printf(" @0x%x (0x%08x): capability type 0x%x: %s",
			capptr, __pci_attr_r32(devind, capptr), type, str);
		
		if (type == 0x08)
		{
			print_hyper_cap(devind, capptr);
		}
		else if (type == 0x0f)
		{
			print_secure_device_subtype(devind, capptr);
		}
		
		printf("\n");
		capptr = next;
	}
}

static const char *
get_capability_name(u8_t type)
{
	switch (type)
	{
	case 1:
		return "PCI Power Management";
	case 2:
		return "AGP";
	case 3:
		return "Vital Product Data";
	case 4:
		return "Slot Identification";
	case 5:
		return "Message Signaled Interrupts";
	case 6:
		return "CompactPCI Hot Swap";
	case 8:
		return "AMD HyperTransport";
	case 0xf:
		return "Secure Device";
	default:
		return "(unknown type)";
	}
}

static void
print_secure_device_subtype(int devind, u8_t capptr)
{
	u8_t subtype;
	const char *str;
	
	subtype = (__pci_attr_r8(devind, capptr + 2) & 0x07);
	
	switch (subtype)
	{
	case 0:
		str = "Device Exclusion Vector";
		break;
	case 3:
		str = "IOMMU";
		break;
	default:
		str = "(unknown type)";
		break;
	}
	
	printf(", sub type 0%o: %s", subtype, str);
}

/*===========================================================================*
 *				ISA Bridge Helpers			     *
 *===========================================================================*/
static void
update_bridge4dev_io(int devind, u32_t io_base, u32_t io_size)
{
	int busnr, busind, type, br_devind;
	u16_t v16;

	if (devind < 0 || io_size == 0) {
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);
	if (busind < 0) {
		return;
	}

	type = pcibus[busind].pb_type;
	
	switch (type) {
	case PBT_INTEL_HOST:
		return;
	case PBT_PCIBRIDGE:
		printf("update_bridge4dev_io: not implemented for PCI bridges\n");
		return;
	case PBT_CARDBUS:
		break;
	default:
		panic("update_bridge4dev_io: strange bus type: %d", type);
		return;
	}

	if (debug) {
		printf("update_bridge4dev_io: adding 0x%x at 0x%x\n",
			io_size, io_base);
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

static int do_piix(int devind)
{
	u32_t elcr1 = 0;
	u32_t elcr2 = 0;
	u32_t elcr;
	int status;

#if DEBUG
	printf("in piix\n");
#endif

	status = sys_inb(PIIX_ELCR1, &elcr1);
	if (status != OK) {
		printf("Warning, sys_inb failed: %d\n", status);
		return -1;
	}

	status = sys_inb(PIIX_ELCR2, &elcr2);
	if (status != OK) {
		printf("Warning, sys_inb failed: %d\n", status);
		return -1;
	}

	elcr = elcr1 | (elcr2 << 8);

	for (int i = 0; i < 4; i++) {
		process_piix_interrupt(devind, i, elcr);
	}

	return 0;
}

static void process_piix_interrupt(int devind, int index, u32_t elcr)
{
	int irqrc = __pci_attr_r8(devind, PIIX_PIRQRCA + index);
	
	if (irqrc & PIIX_IRQ_DI) {
		if (debug) {
			printf("INT%c: disabled\n", 'A' + index);
		}
		return;
	}

	int irq = irqrc & PIIX_IRQ_MASK;
	
	if (debug) {
		printf("INT%c: %d\n", 'A' + index, irq);
		if (!(elcr & (1 << irq))) {
			printf("(warning) IRQ %d is not level triggered\n", irq);
		}
	}
	
	irq_mode_pci(irq);
}

static int
do_amd_isabr(int devind)
{
	int i, busnr, dev, func, xdevind, irq, edge;
	u8_t levmask;
	u16_t pciirq;

	if (devind < 0 || devind >= nr_pcidev)
		return -1;

	func = AMD_ISABR_FUNC;
	busnr = pcidev[devind].pd_busnr;
	dev = pcidev[devind].pd_dev;

	if (nr_pcidev >= NR_PCIDEV)
		panic("too many PCI devices: %d", nr_pcidev);
	
	xdevind = nr_pcidev;
	pcidev[xdevind].pd_busnr = busnr;
	pcidev[xdevind].pd_dev = dev;
	pcidev[xdevind].pd_func = func;
	pcidev[xdevind].pd_inuse = 1;
	nr_pcidev++;

	levmask = __pci_attr_r8(xdevind, AMD_ISABR_PCIIRQ_LEV);
	pciirq = __pci_attr_r16(xdevind, AMD_ISABR_PCIIRQ_ROUTE);
	
	for (i = 0; i < 4; i++)
	{
		edge = (levmask >> i) & 1;
		irq = (pciirq >> (4 * i)) & 0xf;
		
		if (!irq)
		{
			if (debug)
				printf("INT%c: disabled\n", 'A' + i);
			continue;
		}
		
		if (debug)
		{
			printf("INT%c: %d\n", 'A' + i, irq);
			if (edge)
				printf("(warning) IRQ %d is not level triggered\n", irq);
		}
		
		irq_mode_pci(irq);
	}
	
	nr_pcidev--;
	return 0;
}

static int do_sis_isabr(int devind)
{
    for (int i = 0; i < 4; i++)
    {
        int irq = __pci_attr_r8(devind, SIS_ISABR_IRQ_A + i);
        
        if (irq & SIS_IRQ_DISABLED)
        {
            if (debug)
            {
                printf("INT%c: disabled\n", 'A' + i);
            }
        }
        else
        {
            irq &= SIS_IRQ_MASK;
            if (debug)
            {
                printf("INT%c: %d\n", 'A' + i, irq);
            }
            irq_mode_pci(irq);
        }
    }
    return 0;
}

static int do_via_isabr(int devind)
{
    static const u8_t edge_masks[] = {
        VIA_ISABR_EL_INTA,
        VIA_ISABR_EL_INTB,
        VIA_ISABR_EL_INTC,
        VIA_ISABR_EL_INTD
    };
    
    static const u8_t irq_registers[] = {
        VIA_ISABR_IRQ_R2,
        VIA_ISABR_IRQ_R2,
        VIA_ISABR_IRQ_R3,
        VIA_ISABR_IRQ_R1
    };
    
    static const u8_t irq_shifts[] = { 4, 0, 4, 4 };
    
    u8_t levmask = __pci_attr_r8(devind, VIA_ISABR_EL);
    
    for (int i = 0; i < 4; i++)
    {
        int edge = (levmask & edge_masks[i]) != 0;
        u8_t irq_raw = __pci_attr_r8(devind, irq_registers[i]);
        int irq = (irq_raw >> irq_shifts[i]) & 0xf;
        
        if (irq == 0)
        {
            if (debug)
                printf("INT%c: disabled\n", 'A' + i);
        }
        else
        {
            if (debug)
            {
                printf("INT%c: %d\n", 'A' + i, irq);
                if (edge)
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
	int i, r, type, busnr, unknown_bridge, bridge_dev;
	u16_t vid, did;
	u32_t t3;
	const char *dstr;

	unknown_bridge = -1;
	bridge_dev = -1;
	busnr = pcibus[busind].pb_busnr;
	
	for (i = 0; i < nr_pcidev; i++)
	{
		if (pcidev[i].pd_busnr != busnr)
			continue;
			
		t3 = ((pcidev[i].pd_baseclass << 16) |
			(pcidev[i].pd_subclass << 8) | pcidev[i].pd_infclass);
			
		if (t3 == PCI_T3_ISA)
		{
			unknown_bridge = i;
		}

		vid = pcidev[i].pd_vid;
		did = pcidev[i].pd_did;
		
		int j;
		for (j = 0; pci_isabridge[j].vid != 0; j++)
		{
			if (pci_isabridge[j].vid != vid)
				continue;
			if (pci_isabridge[j].did != did)
				continue;
			if (pci_isabridge[j].checkclass && unknown_bridge != i)
				continue;
			break;
		}
		
		if (pci_isabridge[j].vid)
		{
			bridge_dev = i;
			type = pci_isabridge[j].type;
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
		{
			printf("found ISA bridge (%04X:%04X) %s\n",
				vid, did, dstr);
		}
		
		pcibus[busind].pb_isabridge_dev = bridge_dev;
		pcibus[busind].pb_isabridge_type = type;
		
		switch(type)
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

	if (unknown_bridge == -1)
	{
		if (debug)
		{
			printf("(warning) no ISA bridge found on bus %d\n",
				busind);
		}
		return 0;
	}
	
	if (debug)
	{
		printf(
		"(warning) unsupported ISA bridge %04X:%04X for bus %d\n",
			pcidev[unknown_bridge].pd_vid,
			pcidev[unknown_bridge].pd_did, busind);
	}
	return 0;
}

static int derive_irq(struct pcidev *dev, int pin)
{
    if (dev == NULL) {
        return -1;
    }
    
    int bus_index = get_busind(dev->pd_busnr);
    if (bus_index < 0 || pcibus == NULL) {
        return -1;
    }
    
    int parent_devind = pcibus[bus_index].pb_devind;
    if (parent_devind < 0 || pcidev == NULL) {
        return -1;
    }
    
    struct pcidev *parent_bridge = &pcidev[parent_devind];
    if (parent_bridge == NULL) {
        return -1;
    }
    
    int slot = (dev->pd_func >> 3) & 0x1f;
    int adjusted_pin = (pin + slot) % 4;
    
    return acpi_get_irq(parent_bridge->pd_busnr, parent_bridge->pd_dev, adjusted_pin);
}

static void record_irq(int devind) {
    int ilr = __pci_attr_r8(devind, PCI_ILR);
    int ipr = __pci_attr_r8(devind, PCI_IPR);
    
    if (ipr && machine.apic_enabled) {
        handle_apic_irq(devind, &ilr, ipr);
    }
    
    if (ilr == 0) {
        handle_zero_irq(&ilr, ipr);
    }
    
    pcidev[devind].pd_ilr = ilr;
    
    if (ilr == PCI_ILR_UNKNOWN && ipr) {
        handle_unknown_irq_with_ipr(devind, ipr);
    } else if (ilr != PCI_ILR_UNKNOWN) {
        handle_known_irq(devind, ilr, ipr);
    }
}

static void handle_apic_irq(int devind, int *ilr, int ipr) {
    int irq = acpi_get_irq(pcidev[devind].pd_busnr,
                          pcidev[devind].pd_dev, ipr - 1);
    
    if (irq < 0) {
        irq = derive_irq(&pcidev[devind], ipr - 1);
    }
    
    if (irq >= 0) {
        *ilr = irq;
        __pci_attr_w8(devind, PCI_ILR, *ilr);
        if (debug) {
            printf("PCI: ACPI IRQ %d for device %d.%d.%d INT%c\n",
                   irq, pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
                   pcidev[devind].pd_func, 'A' + ipr - 1);
        }
    } else if (debug) {
        printf("PCI: no ACPI IRQ routing for device %d.%d.%d INT%c\n",
               pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
               pcidev[devind].pd_func, 'A' + ipr - 1);
    }
}

static void handle_zero_irq(int *ilr, int ipr) {
    static int first = 1;
    if (ipr && first && debug) {
        first = 0;
        printf("PCI: strange, BIOS assigned IRQ0\n");
    }
    *ilr = PCI_ILR_UNKNOWN;
}

static void handle_unknown_irq_with_ipr(int devind, int ipr) {
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (pcibus[busind].pb_type == PBT_CARDBUS) {
        handle_cardbus_irq(devind, busind);
        return;
    }
    
    if (debug) {
        printf("PCI: device %d.%d.%d uses INT%c but is not assigned any IRQ\n",
               pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
               pcidev[devind].pd_func, 'A' + ipr - 1);
    }
}

static void handle_cardbus_irq(int devind, int busind) {
    int cb_devind = pcibus[busind].pb_devind;
    int ilr = pcidev[cb_devind].pd_ilr;
    
    if (ilr != PCI_ILR_UNKNOWN) {
        if (debug) {
            printf("assigning IRQ %d to Cardbus device\n", ilr);
        }
        __pci_attr_w8(devind, PCI_ILR, ilr);
        pcidev[devind].pd_ilr = ilr;
    }
}

static void handle_known_irq(int devind, int ilr, int ipr) {
    if (ipr) {
        if (debug) {
            printf("\tIRQ %d for INT%c\n", ilr, 'A' + ipr - 1);
        }
    } else {
        printf("PCI: IRQ %d is assigned, but device %d.%d.%d does not need it\n",
               ilr, pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
               pcidev[devind].pd_func);
    }
}

/*===========================================================================*
 *				BAR helpers				     *
 *===========================================================================*/
static int record_io_bar(int devind, int bar_nr, u32_t bar)
{
    u16_t cmd;
    u32_t bar2;
    int dev_bar_nr;

    cmd = __pci_attr_r16(devind, PCI_CR);
    __pci_attr_w16(devind, PCI_CR, cmd & ~PCI_CR_IO_EN);

    __pci_attr_w32(devind, PCI_BAR + 4 * bar_nr, 0xffffffff);
    bar2 = __pci_attr_r32(devind, PCI_BAR + 4 * bar_nr);

    __pci_attr_w32(devind, PCI_BAR + 4 * bar_nr, bar);
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

static int record_mem_bar(int devind, int bar_nr, u32_t bar, int last)
{
    u16_t cmd;
    u32_t bar2;
    int dev_bar_nr, prefetch, type, width = 1;

    type = bar & PCI_BAR_TYPE;

    if (type == PCI_TYPE_64) {
        if (last) {
            printf("PCI: device %d.%d.%d BAR %d extends beyond designated area\n",
                   pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
                   pcidev[devind].pd_func, bar_nr);
            return 1;
        }
        width = 2;
        bar2 = __pci_attr_r32(devind, PCI_BAR + 4 * (bar_nr + 1));
        if (bar2 != 0) {
            if (debug) {
                printf("\tbar_%d: (64-bit BAR with high bits set)\n", bar_nr);
            }
            return width;
        }
    } else if (type != PCI_TYPE_32 && type != PCI_TYPE_32_1M) {
        if (debug) {
            printf("\tbar_%d: (unknown type %x)\n", bar_nr, type);
        }
        return 1;
    }

    cmd = __pci_attr_r16(devind, PCI_CR);
    __pci_attr_w16(devind, PCI_CR, cmd & ~PCI_CR_MEM_EN);

    __pci_attr_w32(devind, PCI_BAR + 4 * bar_nr, 0xffffffff);
    bar2 = __pci_attr_r32(devind, PCI_BAR + 4 * bar_nr);

    __pci_attr_w32(devind, PCI_BAR + 4 * bar_nr, bar);
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

static int record_bar(int devind, int bar_nr, int last)
{
    u32_t bar;
    int reg;

    reg = PCI_BAR + 4 * bar_nr;
    bar = __pci_attr_r32(devind, reg);

    if (bar & PCI_BAR_IO) {
        return record_io_bar(devind, bar_nr, bar);
    }
    return record_mem_bar(devind, bar_nr, bar, last);
}

static void record_bars(int devind, int last_reg)
{
    int bar_index = 0;
    int current_reg = PCI_BAR;
    
    while (current_reg <= last_reg)
    {
        int is_last = (current_reg == last_reg);
        int width = record_bar(devind, bar_index, is_last);
        
        if (width <= 0)
        {
            break;
        }
        
        bar_index += width;
        current_reg += 4 * width;
    }
}

static void record_bars_normal(int devind)
{
	record_bars(devind, PCI_BAR_6);

	if (pcidev[devind].pd_baseclass != PCI_BCR_MASS_STORAGE ||
	    pcidev[devind].pd_subclass != PCI_MS_IDE) {
		return;
	}

	handle_ide_compatibility_mode(devind);
}

static void handle_ide_compatibility_mode(int devind)
{
	int clear_01 = should_clear_primary_bars(devind);
	int clear_23 = should_clear_secondary_bars(devind);

	if (!clear_01 && !clear_23) {
		return;
	}

	compact_bar_array(devind, clear_01, clear_23);
}

static int should_clear_primary_bars(int devind)
{
	if (pcidev[devind].pd_infclass & PCI_IDE_PRI_NATIVE) {
		return 0;
	}

	if (debug) {
		printf("primary channel is not in native mode, clearing BARs 0 and 1\n");
	}
	return 1;
}

static int should_clear_secondary_bars(int devind)
{
	if (pcidev[devind].pd_infclass & PCI_IDE_SEC_NATIVE) {
		return 0;
	}

	if (debug) {
		printf("secondary channel is not in native mode, clearing BARs 2 and 3\n");
	}
	return 1;
}

static void compact_bar_array(int devind, int clear_01, int clear_23)
{
	int src_idx, dst_idx = 0;

	for (src_idx = 0; src_idx < pcidev[devind].pd_bar_nr; src_idx++) {
		int pb_nr = pcidev[devind].pd_bar[src_idx].pb_nr;

		if (should_skip_bar(pb_nr, clear_01, clear_23)) {
			if (debug) {
				printf("skipping bar %d\n", pb_nr);
			}
			continue;
		}

		if (src_idx != dst_idx) {
			pcidev[devind].pd_bar[dst_idx] = pcidev[devind].pd_bar[src_idx];
		}
		dst_idx++;
	}

	pcidev[devind].pd_bar_nr = dst_idx;
}

static int should_skip_bar(int pb_nr, int clear_01, int clear_23)
{
	if ((pb_nr == 0 || pb_nr == 1) && clear_01) {
		return 1;
	}
	if ((pb_nr == 2 || pb_nr == 3) && clear_23) {
		return 1;
	}
	return 0;
}

static void record_bars_bridge(int devind) {
    record_bars(devind, PCI_BAR_2);
    
    log_io_window(devind);
    log_memory_window(devind);
    log_prefetchable_memory_window(devind);
}

static void log_io_window(int devind) {
    if (!debug) {
        return;
    }
    
    u32_t base = calculate_io_base(devind);
    u32_t limit = calculate_io_limit(devind);
    u32_t size = limit - base + 1;
    
    printf("\tI/O window: base 0x%x, limit 0x%x, size %d\n",
           base, limit, size);
}

static void log_memory_window(int devind) {
    if (!debug) {
        return;
    }
    
    u32_t base = calculate_memory_base(devind);
    u32_t limit = calculate_memory_limit(devind);
    u32_t size = limit - base + 1;
    
    printf("\tMemory window: base 0x%x, limit 0x%x, size 0x%x\n",
           base, limit, size);
}

static void log_prefetchable_memory_window(int devind) {
    if (!debug) {
        return;
    }
    
    u32_t base = calculate_prefetchable_base(devind);
    u32_t limit = calculate_prefetchable_limit(devind);
    u32_t size = limit - base + 1;
    
    printf("\tPrefetchable memory window: base 0x%x, limit 0x%x, size 0x%x\n",
           base, limit, size);
}

static u32_t calculate_io_base(int devind) {
    u8_t io_base = __pci_attr_r8(devind, PPB_IOBASE);
    u16_t io_base_upper = __pci_attr_r16(devind, PPB_IOBASEU16);
    
    return ((io_base & PPB_IOB_MASK) << 8) | (io_base_upper << 16);
}

static u32_t calculate_io_limit(int devind) {
    u8_t io_limit = __pci_attr_r8(devind, PPB_IOLIMIT);
    u16_t io_limit_upper = __pci_attr_r16(devind, PPB_IOLIMITU16);
    
    return 0xff | 
           ((io_limit & PPB_IOL_MASK) << 8) |
           ((~PPB_IOL_MASK & 0xff) << 8) |
           (io_limit_upper << 16);
}

static u32_t calculate_memory_base(int devind) {
    u16_t mem_base = __pci_attr_r16(devind, PPB_MEMBASE);
    return (mem_base & PPB_MEMB_MASK) << 16;
}

static u32_t calculate_memory_limit(int devind) {
    u16_t mem_limit = __pci_attr_r16(devind, PPB_MEMLIMIT);
    return 0xffff |
           ((mem_limit & PPB_MEML_MASK) << 16) |
           ((~PPB_MEML_MASK & 0xffff) << 16);
}

static u32_t calculate_prefetchable_base(int devind) {
    u16_t pfmem_base = __pci_attr_r16(devind, PPB_PFMEMBASE);
    return (pfmem_base & PPB_PFMEMB_MASK) << 16;
}

static u32_t calculate_prefetchable_limit(int devind) {
    u16_t pfmem_limit = __pci_attr_r16(devind, PPB_PFMEMLIMIT);
    return 0xffff |
           ((pfmem_limit & PPB_PFMEML_MASK) << 16) |
           ((~PPB_PFMEML_MASK & 0xffff) << 16);
}

static void record_bars_cardbus(int devind)
{
    static const struct {
        int base_reg;
        int limit_reg;
        u32_t mask;
        const char *type;
        int index;
    } windows[] = {
        {CBB_MEMBASE_0, CBB_MEMLIMIT_0, CBB_MEML_MASK, "Memory", 0},
        {CBB_MEMBASE_1, CBB_MEMLIMIT_1, CBB_MEML_MASK, "Memory", 1},
        {CBB_IOBASE_0, CBB_IOLIMIT_0, CBB_IOL_MASK, "I/O", 0},
        {CBB_IOBASE_1, CBB_IOLIMIT_1, CBB_IOL_MASK, "I/O", 1}
    };
    
    record_bars(devind, PCI_BAR);
    
    if (!debug) {
        return;
    }
    
    for (size_t i = 0; i < sizeof(windows) / sizeof(windows[0]); i++) {
        u32_t base = __pci_attr_r32(devind, windows[i].base_reg);
        u32_t limit = __pci_attr_r32(devind, windows[i].limit_reg) | (~windows[i].mask & 0xffffffff);
        u32_t size = limit - base + 1;
        
        printf("\t%s window %d: base 0x%x, limit 0x%x, size %d\n",
               windows[i].type, windows[i].index, base, limit, size);
    }
}

static void
complete_bars(void)
{
	kinfo_t kinfo;
	if(OK != sys_getkinfo(&kinfo))
		panic("can't get kinfo");

	u32_t memgap_low = kinfo.mem_high_phys;
	u32_t memgap_high = 0xfe000000;

	if (debug)
		printf("complete_bars: initial gap: [0x%x .. 0x%x>\n",
			memgap_low, memgap_high);

	for (int i = 0; i < nr_pcidev; i++) {
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++) {
			if ((pcidev[i].pd_bar[j].pb_flags & (PBF_IO | PBF_INCOMPLETE)) != 0)
				continue;

			u32_t base = pcidev[i].pd_bar[j].pb_base;
			u32_t size = pcidev[i].pd_bar[j].pb_size;

			if (base >= memgap_high || base + size <= memgap_low)
				continue;

			u32_t diff1 = base + size - memgap_low;
			u32_t diff2 = memgap_high - base;

			if (diff1 < diff2)
				memgap_low = base + size;
			else
				memgap_high = base;
		}
	}

	if (debug)
		printf("complete_bars: intermediate gap: [0x%x .. 0x%x>\n",
			memgap_low, memgap_high);

	if (memgap_high < memgap_low) {
		printf("PCI: bad memory gap: [0x%x .. 0x%x>\n",
			memgap_low, memgap_high);
		panic(NULL);
	}

	u32_t iogap_low = 0x400;
	u32_t iogap_high = 0x10000;

	for (int i = 0; i < nr_pcidev; i++) {
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++) {
			if ((pcidev[i].pd_bar[j].pb_flags & PBF_IO) == 0)
				continue;
			if (pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE)
				continue;

			u32_t base = pcidev[i].pd_bar[j].pb_base;
			u32_t size = pcidev[i].pd_bar[j].pb_size;

			if (base >= iogap_high || base + size <= iogap_low)
				continue;

			if (base + size - iogap_low < iogap_high - base)
				iogap_low = base + size;
			else
				iogap_high = base;
		}
	}

	if (iogap_high < iogap_low) {
		if (debug)
			printf("iogap_high too low, should panic\n");
		else
			panic("iogap_high too low: %d", iogap_high);
	}

	if (debug)
		printf("I/O range = [0x%x..0x%x>\n", iogap_low, iogap_high);

	for (int i = 0; i < nr_pcidev; i++) {
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++) {
			if ((pcidev[i].pd_bar[j].pb_flags & (PBF_IO | PBF_INCOMPLETE)) != PBF_INCOMPLETE)
				continue;

			u32_t size = pcidev[i].pd_bar[j].pb_size;
			if (size < PAGE_SIZE)
				size = PAGE_SIZE;

			u32_t base = memgap_high - size;
			base &= ~(u32_t)(size - 1);

			if (base < memgap_low)
				panic("memory base too low: %d", base);

			memgap_high = base;
			int bar_nr = pcidev[i].pd_bar[j].pb_nr;
			int reg = PCI_BAR + 4 * bar_nr;
			u32_t v32 = __pci_attr_r32(i, reg);
			__pci_attr_w32(i, reg, v32 | base);

			if (debug)
				printf(
		"complete_bars: allocated 0x%x size %d to %d.%d.%d, bar_%d\n",
					base, size, pcidev[i].pd_busnr,
					pcidev[i].pd_dev, pcidev[i].pd_func,
					bar_nr);

			pcidev[i].pd_bar[j].pb_base = base;
			pcidev[i].pd_bar[j].pb_flags &= ~PBF_INCOMPLETE;
		}

		u32_t io_high = iogap_high;
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++) {
			if ((pcidev[i].pd_bar[j].pb_flags & (PBF_IO | PBF_INCOMPLETE)) != (PBF_IO | PBF_INCOMPLETE))
				continue;

			u32_t size = pcidev[i].pd_bar[j].pb_size;
			u32_t base = iogap_high - size;
			base &= ~(u32_t)(size - 1);
			base &= 0xfcff;

			if (base < iogap_low)
				printf("I/O base too low: %d", base);

			iogap_high = base;
			int bar_nr = pcidev[i].pd_bar[j].pb_nr;
			int reg = PCI_BAR + 4 * bar_nr;
			u32_t v32 = __pci_attr_r32(i, reg);
			__pci_attr_w32(i, reg, v32 | base);

			if (debug)
				printf(
		"complete_bars: allocated 0x%x size %d to %d.%d.%d, bar_%d\n",
					base, size, pcidev[i].pd_busnr,
					pcidev[i].pd_dev, pcidev[i].pd_func,
					bar_nr);

			pcidev[i].pd_bar[j].pb_base = base;
			pcidev[i].pd_bar[j].pb_flags &= ~PBF_INCOMPLETE;
		}

		if (iogap_high != io_high)
			update_bridge4dev_io(i, iogap_high, io_high - iogap_high);
	}

	for (int i = 0; i < nr_pcidev; i++) {
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++) {
			if (pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE)
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

	busnr = pcibus[busind].pb_busnr;
	
	for (dev = 0; dev < 32; dev++) {
		for (func = 0; func < 8; func++) {
			devind = nr_pcidev;
			
			pcidev[devind].pd_busnr = busnr;
			pcidev[devind].pd_dev = dev;
			pcidev[devind].pd_func = func;

			pci_attr_wsts(devind, PSR_SSE | PSR_RMAS | PSR_RTAS);
			vid = __pci_attr_r16(devind, PCI_VID);
			did = __pci_attr_r16(devind, PCI_DID);
			headt = __pci_attr_r8(devind, PCI_HEADT);
			sts = pci_attr_rsts(devind);

			if (vid == NO_VID && did == NO_VID) {
				if (func == 0)
					break;
				continue;
			}

			if (sts & (PSR_SSE | PSR_RMAS | PSR_RTAS)) {
				static int warned = 0;
				if (!warned) {
					printf("PCI: ignoring bad value 0x%x in sts for QEMU\n",
						sts & (PSR_SSE | PSR_RMAS | PSR_RTAS));
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
			
			if (debug) {
				s = pci_subclass_name(baseclass << 24 | subclass << 16);
				if (!s)
					s = pci_baseclass_name(baseclass << 24);
				if (!s)
					s = "(unknown class)";
				printf("\tclass %s (%X/%X/%X)\n", s,
					baseclass, subclass, infclass);
			}

			if (is_duplicate(busnr, dev, func)) {
				printf("\tduplicate!\n");
				if (func == 0 && !(headt & PHT_MULTIFUNC))
					break;
				continue;
			}

			if (nr_pcidev >= NR_PCIDEV)
				panic("too many PCI devices: %d", nr_pcidev);

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

			nr_pcidev++;

			if (func == 0 && !(headt & PHT_MULTIFUNC))
				break;
		}
	}
}


static u16_t pcibr_std_rsts(int busind)
{
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return 0;
    }
    
    int devind = pcibus[busind].pb_devind;
    if (devind < 0) {
        return 0;
    }
    
    return __pci_attr_r16(devind, PPB_SSTS);
}

static void pcibr_std_wsts(int busind, u16_t value)
{
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return;
    }
    
    int devind = pcibus[busind].pb_devind;
    __pci_attr_w16(devind, PPB_SSTS, value);
}

static u16_t pcibr_cb_rsts(int busind)
{
	int devind = pcibus[busind].pb_devind;
	return __pci_attr_r16(devind, CBB_SSTS);
}

static void pcibr_cb_wsts(int busind, u16_t value)
{
    if (busind < 0 || busind >= MAX_PCIBUS) {
        return;
    }
    
    int devind = pcibus[busind].pb_devind;
    if (devind < 0) {
        return;
    }
    
    __pci_attr_w16(devind, CBB_SSTS, value);
}

static u16_t pcibr_via_rsts(int busind)
{
	(void)busind;
	return 0;
}

static void pcibr_via_wsts(int busind, u16_t value)
{
    (void)busind;
    (void)value;
}

static void complete_bridges(void)
{
    for (int i = 0; i < nr_pcibus; i++)
    {
        if (!pcibus[i].pb_needinit)
            continue;

        int freebus = get_freebus();
        int devind = pcibus[i].pb_devind;
        int prim_busnr = pcidev[devind].pd_busnr;

        if (prim_busnr != 0)
        {
            fprintf(stderr, "complete_bridge: updating subordinate bus number not implemented\n");
            continue;
        }

        pcibus[i].pb_needinit = 0;
        pcibus[i].pb_busnr = freebus;

        __pci_attr_w8(devind, PPB_PRIMBN, prim_busnr);
        __pci_attr_w8(devind, PPB_SECBN, freebus);
        __pci_attr_w8(devind, PPB_SUBORDBN, freebus);

#ifdef DEBUG
        printf("Allocated bus %d for bus %d (devind=%d, prim_busnr=%d)\n", 
               freebus, i, devind, prim_busnr);
        printf("CR=0x%x SECBLT=0x%x BRIDGECTRL=0x%x\n",
               __pci_attr_r16(devind, PCI_CR),
               __pci_attr_r8(devind, PPB_SECBLT),
               __pci_attr_r16(devind, PPB_BRIDGECTRL));
#endif
    }
}

static void
do_pcibridge(int busind)
{
	int devind, busnr, ind, type;
	u16_t vid, did;
	u8_t sbusn, baseclass, subclass, infclass, headt;
	u32_t t3;

	busnr = pcibus[busind].pb_busnr;
	
	for (devind = 0; devind < nr_pcidev; devind++)
	{
		if (pcidev[devind].pd_busnr != busnr)
			continue;

		vid = pcidev[devind].pd_vid;
		did = pcidev[devind].pd_did;
		
		headt = __pci_attr_r8(devind, PCI_HEADT);
		
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
			printf(
				"Unknown PCI class %02x/%02x/%02x for PCI-to-PCI bridge, device %04X:%04X\n",
				baseclass, subclass, infclass, vid, did);
			continue;
		}
		
		if (type == PCI_PPB_CB && t3 != PCI_T3_CARDBUS)
		{
			printf(
				"Unknown PCI class %02x/%02x/%02x for Cardbus bridge, device %04X:%04X\n",
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
		
		if (sbusn == 0)
		{
			printf("Secondary bus number not initialized\n");
			continue;
		}

		if (nr_pcibus >= NR_PCIBUS)
			panic("too many PCI busses: %d", nr_pcibus);
			
		ind = nr_pcibus;
		nr_pcibus++;
		
		pcibus[ind].pb_type = PBT_PCIBRIDGE;
		pcibus[ind].pb_needinit = 0;
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
			acpi_map_bridge(pcidev[devind].pd_busnr,
					pcidev[devind].pd_dev, sbusn);

		if (debug)
		{
			printf(
				"bus(table) = %d, bus(sec) = %d, bus(subord) = %d\n",
				ind, sbusn, __pci_attr_r8(devind, PPB_SUBORDBN));
		}

		probe_bus(ind);
		do_pcibridge(ind);
	}
}

/*===========================================================================*
 *				pci_intel_init				     *
 *===========================================================================*/
static void pci_intel_init(void)
{
	u32_t bus = 0;
	u32_t dev = 0;
	u32_t func = 0;
	u16_t vid;
	u16_t did;
	int s;
	int busind;
	const char *dstr;

	vid = PCII_RREG16_(bus, dev, func, PCI_VID);
	did = PCII_RREG16_(bus, dev, func, PCI_DID);
	
	s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (s != OK) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}

	if (nr_pcibus >= NR_PCIBUS) {
		panic("too many PCI busses: %d", nr_pcibus);
	}
	
	busind = nr_pcibus;
	nr_pcibus++;
	
	initialize_pcibus_entry(busind);
	
	dstr = _pci_dev_name(vid, did);
	if (dstr == NULL) {
		dstr = "unknown device";
	}
	
	if (debug) {
		printf("pci_intel_init: %s (%04X:%04X)\n", dstr, vid, did);
	}

	probe_bus(busind);

	if (do_isabridge(busind) != OK) {
		disable_bus_devices(busind);
		return;
	}

	do_pcibridge(busind);
	complete_bridges();
	complete_bars();
}

static void initialize_pcibus_entry(int busind)
{
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
}

static void disable_bus_devices(int busind)
{
	int busnr = pcibus[busind].pb_busnr;
	int i;
	
	for (i = 0; i < nr_pcidev; i++) {
		if (pcidev[i].pd_busnr == busnr) {
			pcidev[i].pd_inuse = 1;
		}
	}
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
	if (!aclp) {
		return TRUE;
	}

	if (devind < 0 || devind >= MAX_PCI_DEVICES) {
		return FALSE;
	}

	struct pci_device *dev = &pcidev[devind];
	
	for (int i = 0; i < aclp->rsp_nr_device; i++) {
		struct rs_pci_device *acl_dev = &aclp->rsp_device[i];
		
		if (acl_dev->vid != dev->pd_vid || acl_dev->did != dev->pd_did) {
			continue;
		}
		
		if (acl_dev->sub_vid != NO_SUB_VID && acl_dev->sub_vid != dev->pd_sub_vid) {
			continue;
		}
		
		if (acl_dev->sub_did != NO_SUB_DID && acl_dev->sub_did != dev->pd_sub_did) {
			continue;
		}
		
		return TRUE;
	}

	if (aclp->rsp_nr_class == 0) {
		return FALSE;
	}

	u32_t class_id = (dev->pd_baseclass << 16) |
	                 (dev->pd_subclass << 8) |
	                 dev->pd_infclass;

	for (int i = 0; i < aclp->rsp_nr_class; i++) {
		struct rs_pci_class *acl_class = &aclp->rsp_class[i];
		
		if (acl_class->pciclass == (class_id & acl_class->mask)) {
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
	int i, r;
	struct rprocpub rprocpub[NR_BOOT_PROCS];

	env_parse("pci_debug", "d", 0, &v, 0, 1);
	debug = v;

	r = sys_getmachine(&machine);
	if (r != OK) {
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

	for (i = 0; i < NR_BOOT_PROCS; i++) {
		if (!rprocpub[i].in_use) {
			continue;
		}
		r = map_service(&rprocpub[i]);
		if (r != OK) {
			panic("unable to map service: %d", r);
		}
	}

	if (type == SEF_INIT_FRESH || type == SEF_INIT_RESTART) {
		chardriver_announce();
	} else if (type != SEF_INIT_LU) {
		panic("Unknown type of restart");
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

	if (rpub == NULL) {
		return EINVAL;
	}

	if (rpub->pci_acl.rsp_nr_device == 0 && 
	    rpub->pci_acl.rsp_nr_class == 0) {
		return OK;
	}

	for (i = 0; i < NR_DRIVERS; i++) {
		if (!pci_acl[i].inuse) {
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
_pci_find_dev(u8_t bus, u8_t dev, u8_t func, int *devindp)
{
	if (devindp == NULL) {
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
int _pci_first_dev(struct rs_pci *aclp, int *devindp, u16_t *vidp, u16_t *didp)
{
	if (!aclp || !devindp || !vidp || !didp) {
		return 0;
	}

	for (int devind = 0; devind < nr_pcidev; devind++) {
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
 *				_pci_next_dev				     *
 *===========================================================================*/
int _pci_next_dev(struct rs_pci *aclp, int *devindp, u16_t *vidp, u16_t *didp)
{
	if (aclp == NULL || devindp == NULL || vidp == NULL || didp == NULL) {
		return 0;
	}

	int devind = *devindp + 1;
	
	while (devind < nr_pcidev && !visible(aclp, devind)) {
		devind++;
	}
	
	if (devind >= nr_pcidev) {
		return 0;
	}
	
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
	struct io_range ior;
	struct minix_mem_range mr;

	if (devind < 0 || devind >= MAX_PCI_DEVICES) {
		return EINVAL;
	}

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
			r = sys_privctl(proc, SYS_PRIV_ADD_IO, &ior);
		} else {
			mr.mr_base = pcidev[devind].pd_bar[i].pb_base;
			mr.mr_limit = mr.mr_base +
				pcidev[devind].pd_bar[i].pb_size - 1;

			r = sys_privctl(proc, SYS_PRIV_ADD_MEM, &mr);
		}

		if (r != OK) {
			printf("sys_privctl failed for proc %d: %d\n", proc, r);
			return r;
		}
	}

	ilr = pcidev[devind].pd_ilr;
	if (ilr != PCI_ILR_UNKNOWN) {
		if (debug) {
			printf("pci_reserve_a: adding IRQ %d\n", ilr);
		}
		r = sys_privctl(proc, SYS_PRIV_ADD_IRQ, &ilr);
		if (r != OK) {
			printf("sys_privctl failed for proc %d: %d\n", proc, r);
			return r;
		}
	}

	return r;
}

/*===========================================================================*
 *				_pci_reserve				     *
 *===========================================================================*/
int
_pci_reserve(int devind, endpoint_t proc, struct rs_pci *aclp)
{
	if (devind < 0 || devind >= nr_pcidev)
	{
		printf("pci_reserve_a: bad devind: %d\n", devind);
		return EINVAL;
	}
	
	if (!visible(aclp, devind))
	{
		printf("pci_reserve_a: %u is not allowed to reserve %d\n",
			proc, devind);
		return EPERM;
	}

	if (pcidev[devind].pd_inuse && pcidev[devind].pd_proc != proc)
	{
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

	for (i = 0; i < nr_pcidev; i++)
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
int _pci_ids(int devind, u16_t *vidp, u16_t *didp)
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
    int busind = get_busind(busnr);
    if (busind < 0) {
        return;
    }
    
    probe_bus(busind);
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
	int domain = 0;

	if (cpp == NULL) {
		return EINVAL;
	}

	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}

	p = label;
	end = label + sizeof(label);

	if (ntostr(domain, &p, end) != OK) {
		return EINVAL;
	}
	if (p >= end - 1) {
		return EINVAL;
	}
	*p++ = '.';

	if (ntostr(pcidev[devind].pd_busnr, &p, end) != OK) {
		return EINVAL;
	}
	if (p >= end - 1) {
		return EINVAL;
	}
	*p++ = '.';

	if (ntostr(pcidev[devind].pd_dev, &p, end) != OK) {
		return EINVAL;
	}
	if (p >= end - 1) {
		return EINVAL;
	}
	*p++ = '.';

	if (ntostr(pcidev[devind].pd_func, &p, end) != OK) {
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
_pci_get_bar(int devind, int port, u32_t *base, u32_t *size,
	int *ioflag)
{
	int i;
	int reg;
	struct pci_bar *bar;

	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;

	if (base == NULL || size == NULL || ioflag == NULL)
		return EINVAL;

	for (i = 0; i < pcidev[devind].pd_bar_nr; i++)
	{
		bar = &pcidev[devind].pd_bar[i];
		reg = PCI_BAR + (4 * bar->pb_nr);

		if (reg == port)
		{
			if (bar->pb_flags & PBF_INCOMPLETE)
				return EINVAL;

			*base = bar->pb_base;
			*size = bar->pb_size;
			*ioflag = (bar->pb_flags & PBF_IO) ? 1 : 0;
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
	if (vp == NULL)
		return EINVAL;
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > 254)
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
int _pci_attr_w8(int devind, int port, u8_t value)
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
int _pci_attr_w16(int devind, int port, u16_t value)
{
	if (devind < 0 || devind >= nr_pcidev || port < 0 || port > 254)
		return EINVAL;

	__pci_attr_w16(devind, port, value);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w32				     *
 *===========================================================================*/
int _pci_attr_w32(int devind, int port, u32_t value)
{
	const int MIN_DEVIND = 0;
	const int MIN_PORT = 0;
	const int MAX_PORT = 252;

	if (devind < MIN_DEVIND || devind >= nr_pcidev) {
		return EINVAL;
	}
	
	if (port < MIN_PORT || port > MAX_PORT) {
		return EINVAL;
	}

	__pci_attr_w32(devind, port, value);
	return OK;
}
