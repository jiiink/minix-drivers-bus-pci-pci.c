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
#include <stdio.h>
#include <stdint.h>

static unsigned pci_inb(uint16_t port) {
    uint32_t value;
    int status;

    status = sys_inb(port, &value);
    if (status != OK) {
        fprintf(stderr, "PCI: warning, sys_inb failed: %d\n", status);
        return 0;
    }

    return (unsigned)(value & 0xFF);
}

#include <stdio.h>
#include <stdlib.h>

static unsigned pci_inw(u16_t port) {
	u32_t value;
	int s = sys_inw(port, &value);
	if (s != OK) {
		fprintf(stderr, "PCI: warning, sys_inw failed: %d\n", s);
		exit(EXIT_FAILURE);
	}
	return value;
}

#include <stdio.h>
#include <errno.h>

static unsigned pci_inl(u16_t port) {
    u32_t value;
    int s = sys_inl(port, &value);
    if (s != OK) {
        fprintf(stderr, "PCI: warning, sys_inl failed: %d\n", s);
        errno = s;
    }
    return value;
}

#include <errno.h>

static void pci_outb(u16_t port, u8_t value) {
	if (sys_outb(port, value) != OK) {
		perror("PCI: warning, sys_outb failed");
	}
}

#include <stdio.h>
#include <stdlib.h>

static void handle_sys_outw_error(int status) {
    fprintf(stderr, "PCI: warning, sys_outw failed: %d\n", status);
    exit(EXIT_FAILURE);
}

static void pci_outw(u16_t port, u16_t value) {
    int status = sys_outw(port, value);
    if (status != OK) {
        handle_sys_outw_error(status);
    }
}

#include <stdio.h>
#include <errno.h>

static void pci_outl(u16_t port, u32_t value) {
    if (sys_outl(port, value) != OK) {
        perror("PCI: sys_outl failed");
    }
}

#include <stdio.h>
#include <stdint.h>

#define OK 0
#define PCII_CONFADD 0x0CF8
#define PCII_UNSEL 0
typedef uint8_t u8_t;

static u8_t PCII_RREG8_(int busnr, int dev, int func, int port);
static int sys_outl(uint32_t port, uint32_t value);

u8_t pcii_rreg8(int busind, int devind, int port) {
    u8_t value = PCII_RREG8_(pcibus[busind].pb_busnr, pcidev[devind].pd_dev, pcidev[devind].pd_func, port);
    if (sys_outl(PCII_CONFADD, PCII_UNSEL) != OK) {
        fprintf(stderr, "PCI: error, sys_outl failed\n");
    }
    return value;
}

#include <stdio.h>

static u16_t pcii_rreg16(int busind, int devind, int port) {
    u16_t value = PCII_RREG16_(pcibus[busind].pb_busnr, 
                               pcidev[devind].pd_dev, 
                               pcidev[devind].pd_func, 
                               port);
    int status = sys_outl(PCII_CONFADD, PCII_UNSEL);

    if (status != OK) {
        fprintf(stderr, "PCI: warning, sys_outl failed: %d\n", status);
    }

    return value;
}

#include <stdio.h>

static u32_t pcii_rreg32(int busind, int devind, int port) {
    u32_t v = PCII_RREG32_(pcibus[busind].pb_busnr,
                           pcidev[devind].pd_dev,
                           pcidev[devind].pd_func, port);
    if (sys_outl(PCII_CONFADD, PCII_UNSEL) != OK) {
        fprintf(stderr, "PCI: warning, sys_outl failed\n");
    }
    return v;
}

#include <stdio.h>
#include <errno.h>

static void pcii_wreg8(int busind, int devind, int port, u8_t value) {
    int s;

    if (busind < 0 || devind < 0 || port < 0) {
        fprintf(stderr, "Error: Invalid parameters\n");
        return;
    }

    PCII_WREG8_(pcibus[busind].pb_busnr,
                pcidev[devind].pd_dev, 
                pcidev[devind].pd_func,
                port, 
                value);

    s = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (s != OK) {
        fprintf(stderr, "PCI: warning, sys_outl failed with error: %d\n", s);
    }
}

static void pcii_wreg16(int busind, int devind, int port, u16_t value) {
    if (busind < 0 || busind >= MAX_BUSES || devind < 0 || devind >= MAX_DEVICES) {
        fprintf(stderr, "PCI: Invalid bus or device index\n");
        return;
    }

    if (pcibus[busind].pb_busnr >= MAX_BUS_NUM || pcidev[devind].pd_dev >= MAX_DEV_NUM) {
        fprintf(stderr, "PCI: Bus or Device number out of bounds\n");
        return;
    }

    PCII_WREG16_(
        pcibus[busind].pb_busnr, 
        pcidev[devind].pd_dev, 
        pcidev[devind].pd_func, 
        port, 
        value
    );

    int s = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (s != OK) {
        fprintf(stderr, "PCI: Warning, sys_outl failed: %d\n", s);
    }
}

#include <stdio.h>

static void pcii_wreg32(int busind, int devind, int port, u32_t value) {
    int s;

    if(busind < 0 || devind < 0 || port < 0) {
        printf("PCI: error, invalid arguments\n");
        return;
    }

    PCII_WREG32_(pcibus[busind].pb_busnr,
                 pcidev[devind].pd_dev, 
                 pcidev[devind].pd_func,
                 port, value);

    if ((s = sys_outl(PCII_CONFADD, PCII_UNSEL)) != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", s);
    }
}

/*===========================================================================*
 *				ntostr					     *
 *===========================================================================*/
#include <limits.h>
#include <stdbool.h>

static bool ntostr(unsigned int n, char **str, const char *end) {
    char tmpstr[20];
    int i = 0;

    if (!str || !*str || !end) {
        return false;
    }

    if (n == 0) {
        tmpstr[i++] = '0';
    } else {
        while (n > 0 && i < sizeof(tmpstr) - 1) {
            tmpstr[i++] = '0' + (n % 10);
            n /= 10;
        }
    }

    if (i >= sizeof(tmpstr)) {
        return false;
    }

    while (i > 0 && *str < end) {
        *(*str)++ = tmpstr[--i];
    }

    if (*str > end) {
        return false;
    }

    if (*str == end) {
        (*str)[-1] = '\0';
    } else {
        **str = '\0';
    }

    return true;
}

/*===========================================================================*
 *				get_busind					     *
 *===========================================================================*/
#include <stdio.h>
#include <stdlib.h>

static int get_busind(int busnr) {
    for (int i = 0; i < nr_pcibus; i++) {
        if (pcibus[i].pb_busnr == busnr) {
            return i;
        }
    }
    fprintf(stderr, "Error: get_busind: can't find bus: %d\n", busnr);
    exit(EXIT_FAILURE);
}

/*===========================================================================*
 *			Unprotected helper functions			     *
 *===========================================================================*/
static uint8_t pci_attr_r8(int devind, int port) {
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    return pcibus[busind].pb_rreg8(busind, devind, port);
}

static u16_t pci_attr_r16(int devind, int port) {
    if (devind < 0 || devind >= MAX_DEVICES || port < 0 || port > MAX_PORT) {
        return 0; // or appropriate error value
    }
    
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    
    if (busind < 0 || busind >= MAX_BUSES) {
        return 0; // or appropriate error value
    }
    
    return pcibus[busind].pb_rreg16(busind, devind, port);
}

static u32_t pci_attr_r32(int devind, int port) {
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    if (busind < 0) {
        // Handle error: invalid bus index
        return 0; // Or an appropriate error code
    }
    return pcibus[busind].pb_rreg32(busind, devind, port);
}

static void pci_attr_w8(int devind, int port, u8_t value) {
    int busind = get_busind(pcidev[devind].pd_busnr);
    if (busind >= 0) {
        pcibus[busind].pb_wreg8(busind, devind, port, value);
    }
}

static void pci_attr_write_16(int device_index, int port, uint16_t value) {
    int bus_number = pcidev[device_index].pd_busnr;
    int bus_index = get_busind(bus_number);
    
    if (bus_index >= 0 && bus_index < sizeof(pcibus) / sizeof(pcibus[0])) {
        pcibus[bus_index].pb_wreg16(bus_index, device_index, port, value);
    }
}

static void write_pci_attr_w32(int device_index, int port, uint32_t value) {
    int bus_number = pcidev[device_index].pd_busnr;
    int bus_index = get_busind(bus_number);
    if (bus_index < 0 || bus_index >= MAX_BUSES) {
        // Handle invalid bus index error
        return;
    }
    if (device_index < 0 || device_index >= MAX_DEVICES) {
        // Handle invalid device index error
        return;
    }
    if (port < 0 || port >= MAX_PORTS) {
        // Handle invalid port error
        return;
    }
    pcibus[bus_index].pb_wreg32(bus_index, device_index, port, value);
}

/*===========================================================================*
 *				helpers					     *
 *===========================================================================*/
u16_t pci_attr_rsts(int devind) {
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    return pcibus[busind].pb_rsts(busind);
}

#include <errno.h>

static void pci_attr_wsts(int devind, u16_t value) {
    if (devind < 0 || devind >= MAX_DEVICES) {
        perror("Invalid device index");
        return;
    }

    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);

    if (busind < 0 || busind >= MAX_BUSES) {
        perror("Invalid bus index");
        return;
    }

    if (pcibus[busind].pb_wsts) {
        pcibus[busind].pb_wsts(busind, value);
    } else {
        perror("Function pointer pb_wsts is NULL");
    }
}

#include <stdio.h>
#include <stdint.h>

#define OK 0
#define PCI_SR 0x06
#define PCII_CONFADD 0x0CF8
#define PCII_UNSEL 0xFFFFFFFF

typedef uint16_t u16_t;

typedef struct {
	int pb_busnr;
} pcibus_t;

extern pcibus_t pcibus[];
extern uint16_t PCII_RREG16_(int busnr, int dev, int func, int reg);
extern int sys_outl(uint32_t port, uint32_t value);

static u16_t pcii_rsts(int busind) {
	u16_t value = PCII_RREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR);
	if (sys_outl(PCII_CONFADD, PCII_UNSEL) != OK) {
		fprintf(stderr, "PCI: warning, sys_outl failed\n");
	}
	return value;
}

#include <stdio.h>
#include <errno.h>

static void pcii_wsts(int busind, u16_t value) {
    if (sys_outl(PCII_CONFADD, PCII_UNSEL) != OK) {
        perror("PCI: sys_outl failed");
        return;
    }
    PCII_WREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR, value);
}

static int is_duplicate(u8_t busnr, u8_t dev, u8_t func) {
    for (int i = 0; i < nr_pcidev; i++) {
        if (pcidev[i].pd_busnr == busnr && pcidev[i].pd_dev == dev && pcidev[i].pd_func == func) {
            return 1;
        }
    }
    return 0;
}

int get_freebus(void) {
    int freebus = 1;
    for (int i = 0; i < nr_pcibus; i++) {
        if (pcibus[i].pb_needinit || pcibus[i].pb_type == PBT_INTEL_HOST) {
            continue;
        }
        if (pcibus[i].pb_busnr <= freebus) {
            freebus = pcibus[i].pb_busnr + 1;
        }
    }
    return freebus;
}

#include <stdio.h>
#include <string.h>
#include <errno.h>

static const char *pci_vid_name(u16_t vid) {
    static char vendor[PCI_VENDORSTR_LEN];
    
    if (pci_findvendor(vendor, sizeof(vendor), vid) < 0) {
        return strerror(errno);  // Return error message on failure
    }

    return vendor;
}


static void print_hyper_cap(int devind, u8_t capptr) {
    u32_t v = __pci_attr_r32(devind, capptr);
    u16_t cmd = (v >> 16) & 0xffff;
    int capabilityType = (cmd & 0xE000) >> 13;

    printf("\n");
    printf("print_hyper_cap: @0x%x, off 0 (cap):", capptr);

    if (capabilityType == 0 || capabilityType == 1) {
        const char *typeStr = (capabilityType == 0) 
            ? "Slave or Primary Interface" 
            : "Host or Secondary Interface";
        printf("Capability Type: %s\n", typeStr);
    } else {
        printf(" Capability Type 0x%x", (cmd & 0xF800) >> 11);
    }
    
    cmd &= (capabilityType == 0 || capabilityType == 1) ? ~0xE000 : ~0xF800;
    
    if (cmd) {
        printf(" undecoded 0x%x\n", cmd);
    }
}

static void print_capabilities(int devind) {
    const struct {
        u8_t type;
        const char *description;
    } capability_types[] = {
        {1, "PCI Power Management"},
        {2, "AGP"},
        {3, "Vital Product Data"},
        {4, "Slot Identification"},
        {5, "Message Signaled Interrupts"},
        {6, "CompactPCI Hot Swap"},
        {8, "AMD HyperTransport"},
        {0xf, "Secure Device"},
        {0, "(unknown type)"}
    };

    u8_t status = __pci_attr_r16(devind, PCI_SR);
    if (!(status & PSR_CAPPTR)) return;

    u8_t capptr = __pci_attr_r8(devind, PCI_CAPPTR) & PCI_CP_MASK;
    while (capptr) {
        u8_t type = __pci_attr_r8(devind, capptr + CAP_TYPE);
        u8_t next = __pci_attr_r8(devind, capptr + CAP_NEXT) & PCI_CP_MASK;

        const char *str = "(unknown type)";
        for (size_t i = 0; i < sizeof(capability_types) / sizeof(capability_types[0]) - 1; ++i) {
            if (capability_types[i].type == type) {
                str = capability_types[i].description;
                break;
            }
        }

        printf(" @0x%x (0x%08x): capability type 0x%x: %s", capptr, __pci_attr_r32(devind, capptr), type, str);
        
        if (type == 0x08) {
            print_hyper_cap(devind, capptr);
        } else if (type == 0x0f) {
            u8_t subtype = __pci_attr_r8(devind, capptr + 2) & 0x07;
            const char *sub_str = (subtype == 0) ? "Device Exclusion Vector" : (subtype == 3) ? "IOMMU" : "(unknown type)";
            printf(", sub type 0%o: %s", subtype, sub_str);
        }

        printf("\n");
        capptr = next;
    }
}

/*===========================================================================*
 *				ISA Bridge Helpers			     *
 *===========================================================================*/
static void update_bridge4dev_io(int devind, u32_t io_base, u32_t io_size) {
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    int type = pcibus[busind].pb_type;

    if (type == PBT_INTEL_HOST || type == PBT_PCIBRIDGE) {
        if (type == PBT_PCIBRIDGE) {
            printf("update_bridge4dev_io: not implemented for PCI bridges\n");
        }
        return;
    }

    if (type != PBT_CARDBUS) {
        panic("update_bridge4dev_io: unexpected bus type: %d", type);
    }

    if (debug) {
        printf("update_bridge4dev_io: adding 0x%x at 0x%x\n", io_size, io_base);
    }

    int br_devind = pcibus[busind].pb_devind;
    __pci_attr_w32(br_devind, CBB_IOLIMIT_0, io_base + io_size - 1);
    __pci_attr_w32(br_devind, CBB_IOBASE_0, io_base);

    u16_t v16 = __pci_attr_r16(devind, PCI_CR);
    __pci_attr_w16(devind, PCI_CR, v16 | PCI_CR_IO_EN | PCI_CR_MAST_EN);
}

#include <stdio.h>

static int do_piix(int devind) {
    int i, s, irqrc, irq;
    u32_t elcr1 = 0, elcr2 = 0, elcr;
    
    if ((s = sys_inb(PIIX_ELCR1, &elcr1)) != OK) {
        fprintf(stderr, "Error reading PIIX_ELCR1: %d\n", s);
        return s;
    }

    if ((s = sys_inb(PIIX_ELCR2, &elcr2)) != OK) {
        fprintf(stderr, "Error reading PIIX_ELCR2: %d\n", s);
        return s;
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
        
        if (!(elcr & (1 << irq))) {
            if (debug) {
                printf("(warning) IRQ %d is not level triggered\n", irq);
            }
        }

        irq_mode_pci(irq);
    }

    return 0;
}

static int do_amd_isabr(int devind) {
    int i, busnr, dev, func, xdevind, irq, edge;
    u8_t levmask;
    u16_t pciirq;

    if (nr_pcidev >= NR_PCIDEV) {
        panic("too many PCI devices: %d", nr_pcidev);
        return -1;
    }

    func = AMD_ISABR_FUNC;
    busnr = pcidev[devind].pd_busnr;
    dev = pcidev[devind].pd_dev;
    
    xdevind = nr_pcidev++;
    pcidev[xdevind] = (PCI_DEV){.pd_busnr = busnr, .pd_dev = dev, .pd_func = func, .pd_inuse = 1};
    
    levmask = __pci_attr_r8(xdevind, AMD_ISABR_PCIIRQ_LEV);
    pciirq = __pci_attr_r16(xdevind, AMD_ISABR_PCIIRQ_ROUTE);

    for (i = 0; i < 4; i++) {
        edge = (levmask >> i) & 1;
        irq = (pciirq >> (4 * i)) & 0xf;

        if (debug) {
            if (!irq) {
                printf("INT%c: disabled\n", 'A' + i);
            } else {
                printf("INT%c: %d\n", 'A' + i, irq);
                if (edge) {
                    printf("(warning) IRQ %d is not level triggered\n", irq);
                }
            }
        }

        if (irq) {
            irq_mode_pci(irq);
        }
    }

    nr_pcidev--;
    return 0;
}

int get_sis_isabr_irq(int devind, int *irqs) {
    for (int i = 0; i < 4; i++) {
        irqs[i] = __pci_attr_r8(devind, SIS_ISABR_IRQ_A + i);
        irqs[i] &= SIS_IRQ_MASK;
    }
    return 0;
}

int print_irqs_and_set_mode(int *irqs, int debug) {
    for (int i = 0; i < 4; i++) {
        if (irqs[i] == 0) {
            if (debug) {
                printf("INT%c: disabled\n", 'A' + i);
            }
        } else {
            if (debug) {
                printf("INT%c: %d\n", 'A' + i, irqs[i]);
            }
            irq_mode_pci(irqs[i]);
        }
    }
    return 0;
}

int do_sis_isabr(int devind, int debug) {
    int irqs[4] = {0};
    get_sis_isabr_irq(devind, irqs);
    return print_irqs_and_set_mode(irqs, debug);
}

static int do_via_isabr(int devind) {
    int irq, edge;
    u8_t irq_val;
    u8_t levmask = __pci_attr_r8(devind, VIA_ISABR_EL);

    for (int i = 0; i < 4; i++) {
        switch (i) {
            case 0:
                edge = (levmask & VIA_ISABR_EL_INTA);
                irq_val = __pci_attr_r8(devind, VIA_ISABR_IRQ_R2);
                irq = (irq_val >> 4) & 0xf;
                break;
            case 1:
                edge = (levmask & VIA_ISABR_EL_INTB);
                irq_val = __pci_attr_r8(devind, VIA_ISABR_IRQ_R2);
                irq = irq_val & 0xf;
                break;
            case 2:
                edge = (levmask & VIA_ISABR_EL_INTC);
                irq_val = __pci_attr_r8(devind, VIA_ISABR_IRQ_R3);
                irq = (irq_val >> 4) & 0xf;
                break;
            case 3:
                edge = (levmask & VIA_ISABR_EL_INTD);
                irq_val = __pci_attr_r8(devind, VIA_ISABR_IRQ_R1);
                irq = (irq_val >> 4) & 0xf;
                break;
            default:
                return -1; // Error handling for unexpected case
        }

        if (debug) {
            if (irq == 0) {
                printf("INT%c: disabled\n", 'A' + i);
            } else {
                printf("INT%c: %d\n", 'A' + i, irq);
                if (edge) {
                    printf("(warning) IRQ %d is not level triggered\n", irq);
                }
            }
        }

        if (irq) {
            irq_mode_pci(irq);
        }
    }
    return 0;
}

static int do_isabridge(int busind) {
    int i, bridge_dev = -1;
    u16_t vid = 0, did = 0;
    const char *dstr;
    int busnr = pcibus[busind].pb_busnr;

    for (i = 0; i < nr_pcidev; i++) {
        if (pcidev[i].pd_busnr != busnr)
            continue;

        u32_t t3 = (pcidev[i].pd_baseclass << 16) | (pcidev[i].pd_subclass << 8) | pcidev[i].pd_infclass;
        if (t3 == PCI_T3_ISA) {
            vid = pcidev[i].pd_vid;
            did = pcidev[i].pd_did;

            for (int j = 0; pci_isabridge[j].vid != 0; j++) {
                if (pci_isabridge[j].vid == vid && pci_isabridge[j].did == did) {
                    if (!pci_isabridge[j].checkclass || pcidev[i].pd_infclass == pcidev[i].pd_baseclass) {
                        bridge_dev = i;
                        break;
                    }
                }
            }
            if (bridge_dev != -1) break;
        }
    }

    if (bridge_dev != -1) {
        dstr = _pci_dev_name(vid, did);
        if (!dstr) dstr = "unknown device";

        if (debug) {
            printf("found ISA bridge (%04X:%04X) %s\n", vid, did, dstr);
        }
        pcibus[busind].pb_isabridge_dev = bridge_dev;
        int type = pci_isabridge[bridge_dev].type;
        pcibus[busind].pb_isabridge_type = type;

        switch (type) {
            case PCI_IB_PIIX:
                return do_piix(bridge_dev);
            case PCI_IB_VIA:
                return do_via_isabr(bridge_dev);
            case PCI_IB_AMD:
                return do_amd_isabr(bridge_dev);
            case PCI_IB_SIS:
                return do_sis_isabr(bridge_dev);
            default:
                panic("unknown ISA bridge type: %d", type);
        }
    } else {
        if (debug) {
            if (vid == 0 && did == 0) {
                printf("(warning) no ISA bridge found on bus %d\n", busind);
            } else {
                printf("(warning) unsupported ISA bridge %04X:%04X for bus %d\n", vid, did, busind);
            }
        }
    }
    return 0;
}

int derive_irq(struct pcidev *dev, int pin) {
    if (!dev || pin < 0) return -1;

    struct pcidev *parent_bridge;
    int slot;
    int bus_index = get_busind(dev->pd_busnr);

    if (bus_index < 0 || bus_index >= MAX_BUSES) return -1;

    parent_bridge = &pcidev[pcibus[bus_index].pb_devind];
    if (!parent_bridge) return -1;

    slot = (dev->pd_func >> 3) & 0x1f;
    
    return acpi_get_irq(parent_bridge->pd_busnr,
                        parent_bridge->pd_dev, 
                        (pin + slot) % 4);
}

static void record_irq(int devind) {
    int ilr = __pci_attr_r8(devind, PCI_ILR);
    int ipr = __pci_attr_r8(devind, PCI_IPR);
    int irq = -1;

    if (ipr && machine.apic_enabled) {
        irq = acpi_get_irq(pcidev[devind].pd_busnr, pcidev[devind].pd_dev, ipr - 1);
        if (irq < 0) {
            irq = derive_irq(&pcidev[devind], ipr - 1);
        }
    }

    if (irq >= 0) {
        ilr = irq;
        __pci_attr_w8(devind, PCI_ILR, ilr);
        if (debug) {
            printf("PCI: ACPI IRQ %d for device %d.%d.%d INT%c\n", irq, pcidev[devind].pd_busnr, pcidev[devind].pd_dev, pcidev[devind].pd_func, 'A' + ipr - 1);
        }
    } else if (ipr && debug) {
        printf("PCI: no ACPI IRQ routing for device %d.%d.%d INT%c\n", pcidev[devind].pd_busnr, pcidev[devind].pd_dev, pcidev[devind].pd_func, 'A' + ipr - 1);
    }

    if (ilr == 0 && first && debug) {
        first = 0;
        printf("PCI: strange, BIOS assigned IRQ0\n");
    }

    if (ilr == 0) {
        ilr = PCI_ILR_UNKNOWN;
    }

    pcidev[devind].pd_ilr = ilr;
    if (ilr == PCI_ILR_UNKNOWN && ipr) {
        handle_cardbus(devind, ipr);
    } else if (ilr != PCI_ILR_UNKNOWN) {
        if (debug) {
            printf("\tIRQ %d for INT%c\n", ilr, 'A' + ipr - 1);
        }
    } else {
        printf("PCI: IRQ %d is assigned, but device %d.%d.%d does not need it\n", ilr, pcidev[devind].pd_busnr, pcidev[devind].pd_dev, pcidev[devind].pd_func);
    }
}

static void handle_cardbus(int devind, int ipr) {
    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);
    if (pcibus[busind].pb_type == PBT_CARDBUS) {
        int cb_devind = pcibus[busind].pb_devind;
        int ilr = pcidev[cb_devind].pd_ilr;
        if (ilr != PCI_ILR_UNKNOWN) {
            if (debug) {
                printf("assigning IRQ %d to Cardbus device\n", ilr);
            }
            __pci_attr_w8(devind, PCI_ILR, ilr);
            pcidev[devind].pd_ilr = ilr;
        } else if (debug) {
            printf("PCI: device %d.%d.%d uses INT%c but is not assigned any IRQ\n", pcidev[devind].pd_busnr, pcidev[devind].pd_dev, pcidev[devind].pd_func, 'A' + ipr - 1);
        }
    }
}

/*===========================================================================*
 *				BAR helpers				     *
 *===========================================================================*/
static int record_bar(int devind, int bar_nr, int last) {
    int reg = PCI_BAR + 4 * bar_nr;
    u32_t bar = __pci_attr_r32(devind, reg);
    u16_t cmd;
    int width = 1; // Assume 32-bit BAR initially

    if (bar & PCI_BAR_IO) {
        // Handle I/O BAR
        handle_io_bar(devind, reg, bar, bar_nr);
    } else {
        // Handle memory BAR
        width += handle_memory_bar(devind, reg, bar, bar_nr, last, &cmd);
    }
    return width;
}

static void handle_io_bar(int devind, int reg, u32_t bar, int bar_nr) {
    u32_t bar2;
    u16_t cmd = __pci_attr_r16(devind, PCI_CR);
    __pci_attr_w16(devind, PCI_CR, cmd & ~PCI_CR_IO_EN);
    __pci_attr_w32(devind, reg, 0xffffffff);
    bar2 = __pci_attr_r32(devind, reg);
    __pci_attr_w32(devind, reg, bar);
    __pci_attr_w16(devind, PCI_CR, cmd);
    bar &= PCI_BAR_IO_MASK;
    bar2 = (~bar2 & 0xffff) + 1;
    
    if (debug) {
        printf("\tbar_%d: %d bytes at 0x%x I/O\n", bar_nr, bar2, bar);
    }

    int dev_bar_nr = pcidev[devind].pd_bar_nr++;
    pcidev[devind].pd_bar[dev_bar_nr].pb_flags = PBF_IO;
    pcidev[devind].pd_bar[dev_bar_nr].pb_base = bar;
    pcidev[devind].pd_bar[dev_bar_nr].pb_size = bar2;
    pcidev[devind].pd_bar[dev_bar_nr].pb_nr = bar_nr;

    if (bar == 0) {
        pcidev[devind].pd_bar[dev_bar_nr].pb_flags |= PBF_INCOMPLETE;
    }
}

static int handle_memory_bar(int devind, int reg, u32_t bar, int bar_nr, int last, u16_t *cmd) {
    int width = 0;
    int type = (bar & PCI_BAR_TYPE);
    u32_t bar2;
    
    switch (type) {
        case PCI_TYPE_32:
        case PCI_TYPE_32_1M:
            break;
        case PCI_TYPE_64:
            width = handle_64bit_bar(devind, reg, bar_nr, last);
            break;
        default:
            if (debug) {
                printf("\tbar_%d: (unknown type %x)\n", bar_nr, type);
            }
            return width;
    }

    *cmd = __pci_attr_r16(devind, PCI_CR);
    __pci_attr_w16(devind, PCI_CR, *cmd & ~PCI_CR_MEM_EN);
    __pci_attr_w32(devind, reg, 0xffffffff);
    bar2 = __pci_attr_r32(devind, reg);
    __pci_attr_w32(devind, reg, bar);
    __pci_attr_w16(devind, PCI_CR, *cmd);

    if (bar2 == 0) {
        return width;
    }

    int prefetch = !!(bar & PCI_BAR_PREFETCH);
    bar &= PCI_BAR_MEM_MASK;
    bar2 = (~bar2) + 1;

    if (debug) {
        printf("\tbar_%d: 0x%x bytes at 0x%x%s memory%s\n", bar_nr, bar2, bar,
               prefetch ? " prefetchable" : "",
               type == PCI_TYPE_64 ? ", 64-bit" : "");
    }

    int dev_bar_nr = pcidev[devind].pd_bar_nr++;
    pcidev[devind].pd_bar[dev_bar_nr].pb_flags = 0;
    pcidev[devind].pd_bar[dev_bar_nr].pb_base = bar;
    pcidev[devind].pd_bar[dev_bar_nr].pb_size = bar2;
    pcidev[devind].pd_bar[dev_bar_nr].pb_nr = bar_nr;

    if (bar == 0) {
        pcidev[devind].pd_bar[dev_bar_nr].pb_flags |= PBF_INCOMPLETE;
    }
    return width + 1;
}

static int handle_64bit_bar(int devind, int reg, int bar_nr, int last) {
    if (last) {
        printf("PCI: device %d.%d.%d BAR %d extends beyond designated area\n",
               pcidev[devind].pd_busnr,
               pcidev[devind].pd_dev,
               pcidev[devind].pd_func, bar_nr);
        return 0;
    }
    
    u32_t bar2 = __pci_attr_r32(devind, reg + 4);
    if (bar2 != 0) {
        if (debug) {
            printf("\tbar_%d: (64-bit BAR with high bits set)\n", bar_nr);
        }
        return 1;
    }
    return 0;
}

static void record_bars(int devind, int last_reg) {
    int i = 0;
    for (int reg = PCI_BAR; reg <= last_reg; ) {
        int is_last = (reg == last_reg);
        int width = record_bar(devind, i, is_last);
        i += width;
        reg += 4 * width;
    }
}

static void record_bars_normal(int devind) {
    int clear_01 = 0, clear_23 = 0, j = 0;

    record_bars(devind, PCI_BAR_6);

    if (pcidev[devind].pd_baseclass == PCI_BCR_MASS_STORAGE &&
        pcidev[devind].pd_subclass == PCI_MS_IDE) {
        
        clear_01 = !(pcidev[devind].pd_infclass & PCI_IDE_PRI_NATIVE);
        clear_23 = !(pcidev[devind].pd_infclass & PCI_IDE_SEC_NATIVE);

        if (debug && clear_01) {
            printf("primary channel is not in native mode, clearing BARs 0 and 1\n");
        }
        if (debug && clear_23) {
            printf("secondary channel is not in native mode, clearing BARs 2 and 3\n");
        }

        for (int i = 0; i < pcidev[devind].pd_bar_nr; i++) {
            int pb_nr = pcidev[devind].pd_bar[i].pb_nr;

            if ((pb_nr == 0 || pb_nr == 1) && clear_01) {
                if (debug) printf("skipping bar %d\n", pb_nr);
                continue;
            }
            if ((pb_nr == 2 || pb_nr == 3) && clear_23) {
                if (debug) printf("skipping bar %d\n", pb_nr);
                continue;
            }
            if (i != j) {
                pcidev[devind].pd_bar[j] = pcidev[devind].pd_bar[i];
            }
            j++;
        }
        pcidev[devind].pd_bar_nr = j;
    }
}

static void record_bars_bridge(int devind) {
    const u32_t limitMask32 = 0xFF;
    const u32_t limitMask16 = 0xFFFF;
    
    record_bars(devind, PCI_BAR_2);

    // IO Window Calculation
    u32_t ioBase = ((__pci_attr_r8(devind, PPB_IOBASE) & PPB_IOB_MASK) << 8) | 
                   (__pci_attr_r16(devind, PPB_IOBASEU16) << 16);
    u32_t ioLimit = limitMask32 | 
                    ((__pci_attr_r8(devind, PPB_IOLIMIT) & PPB_IOL_MASK) << 8) | 
                    (__pci_attr_r16(devind, PPB_IOLIMITU16) << 16);
    u32_t ioSize = ioLimit - ioBase + 1;

    if (debug) {
        printf("\tI/O window: base 0x%x, limit 0x%x, size %d\n", ioBase, ioLimit, ioSize);
    }

    // Memory Window Calculation
    u32_t memBase = (__pci_attr_r16(devind, PPB_MEMBASE) & PPB_MEMB_MASK) << 16;
    u32_t memLimit = limitMask16 | 
                     ((__pci_attr_r16(devind, PPB_MEMLIMIT) & PPB_MEML_MASK) << 16);
    u32_t memSize = memLimit - memBase + 1;

    if (debug) {
        printf("\tMemory window: base 0x%x, limit 0x%x, size 0x%x\n", memBase, memLimit, memSize);
    }

    // Prefetchable Memory Window Calculation
    u32_t pfMemBase = (__pci_attr_r16(devind, PPB_PFMEMBASE) & PPB_PFMEMB_MASK) << 16;
    u32_t pfMemLimit = limitMask16 | 
                       ((__pci_attr_r16(devind, PPB_PFMEMLIMIT) & PPB_PFMEML_MASK) << 16);
    u32_t pfMemSize = pfMemLimit - pfMemBase + 1;

    if (debug) {
        printf("\tPrefetchable memory window: base 0x%x, limit 0x%x, size 0x%x\n", pfMemBase, pfMemLimit, pfMemSize);
    }
}

static void record_bars_cardbus(int devind) {
    u32_t base, limit, size;
    const int num_windows = 4;
    const u32_t mem_base_regs[] = {CBB_MEMBASE_0, CBB_MEMBASE_1, CBB_IOBASE_0, CBB_IOBASE_1};
    const u32_t mem_limit_regs[] = {CBB_MEMLIMIT_0, CBB_MEMLIMIT_1, CBB_IOLIMIT_0, CBB_IOLIMIT_1};
    const u32_t mem_masks[] = {CBB_MEML_MASK, CBB_MEML_MASK, CBB_IOL_MASK, CBB_IOL_MASK};
    const char* window_types[] = {"Memory", "Memory", "I/O", "I/O"};

    record_bars(devind, PCI_BAR);

    for (int i = 0; i < num_windows; i++) {
        base = __pci_attr_r32(devind, mem_base_regs[i]);
        limit = __pci_attr_r32(devind, mem_limit_regs[i]) | (~mem_masks[i] & 0xffffffff);
        size = limit - base + 1;

        if (debug) {
            printf("\t%s window %d: base 0x%x, limit 0x%x, size %d\n",
                   window_types[i], i % 2, base, limit, size);
        }
    }
}

#include <stdio.h>
#include <stdlib.h>
#include "sysdefs.h"
#include "pci.h"

static void complete_bars(void) {
    int i, j, bar_nr, reg;
    u32_t memgap_low, memgap_high, iogap_low, iogap_high, io_high, base, size, v32;
    kinfo_t kinfo;

    if (OK != sys_getkinfo(&kinfo)) {
        perror("Failed to get kernel info");
        exit(EXIT_FAILURE);
    }

    memgap_low = kinfo.mem_high_phys;
    memgap_high = 0xfe000000;

    iogap_high = 0x10000;
    iogap_low = 0x400;

    for (i = 0; i < nr_pcidev; i++) {
        for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
            if (!(pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE)) continue;

            base = pcidev[i].pd_bar[j].pb_base;
            size = pcidev[i].pd_bar[j].pb_size ? pcidev[i].pd_bar[j].pb_size : PAGE_SIZE;

            if (pcidev[i].pd_bar[j].pb_flags & PBF_IO) {
                if (base >= iogap_low && base + size < iogap_high) {
                    v32 = __pci_attr_r32(i, PCI_BAR + 4 * pcidev[i].pd_bar[j].pb_nr);
                    __pci_attr_w32(i, PCI_BAR + 4 * pcidev[i].pd_bar[j].pb_nr, v32 | (base & 0xfcff));
                    pcidev[i].pd_bar[j].pb_base = base & 0xfcff;
                    pcidev[i].pd_bar[j].pb_flags &= ~PBF_INCOMPLETE;

                    io_high = iogap_high;
                    iogap_high = base & 0xfcff;
                    if (iogap_high != io_high) {
                        update_bridge4dev_io(i, iogap_high, io_high - iogap_high);
                    }
                }
            } else {
                if (base >= memgap_low && base + size < memgap_high) {
                    base = memgap_high - size;
                    base &= ~(u32_t)(size - 1);
                    v32 = __pci_attr_r32(i, PCI_BAR + 4 * pcidev[i].pd_bar[j].pb_nr);
                    __pci_attr_w32(i, PCI_BAR + 4 * pcidev[i].pd_bar[j].pb_nr, v32 | base);
                    pcidev[i].pd_bar[j].pb_base = base;
                    pcidev[i].pd_bar[j].pb_flags &= ~PBF_INCOMPLETE;
                    memgap_high = base;
                }
            }
        }
    }

    for (i = 0; i < nr_pcidev; i++) {
        for (j = 0; j < pcidev[i].pd_bar_nr; j++) {
            if (pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE) {
                printf("Resources should be allocated for device %d\n", i);
            }
        }
    }
}

/*===========================================================================*
 *				PCI Bridge Helpers			     *
 *===========================================================================*/
static void probe_bus(int busind) {
    uint32_t dev, func;
    u16_t vid, did, sts, sub_vid, sub_did;
    u8_t headt, baseclass, subclass, infclass;
    int devind, busnr;
    const char *s, *dstr;
    static int warned = 0;

    if (debug) printf("probe_bus(%d)\n", busind);
    if (nr_pcidev >= NR_PCIDEV) panic("too many PCI devices: %d", nr_pcidev);

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
                if (func == 0) break;
                else continue;
            }

            if ((sts & (PSR_SSE | PSR_RMAS | PSR_RTAS)) && !warned) {
                printf("PCI: ignoring bad value 0x%x in sts for QEMU\n", sts & (PSR_SSE | PSR_RMAS | PSR_RTAS));
                warned = 1;
            }

            sub_vid = __pci_attr_r16(devind, PCI_SUBVID);
            sub_did = __pci_attr_r16(devind, PCI_SUBDID);
            dstr = _pci_dev_name(vid, did);

            if (debug) {
                if (dstr) {
                    printf("%d.%lu.%lu: %s (%04X:%04X)\n", busnr, (unsigned long)dev, (unsigned long)func, dstr, vid, did);
                } else {
                    printf("%d.%lu.%lu: Unknown device, vendor %04X (%s), device %04X\n",
                           busnr, (unsigned long)dev, (unsigned long)func, vid, pci_vid_name(vid), did);
                }
                printf("Device index: %d\n", devind);
                printf("Subsystem: Vid 0x%x, did 0x%x\n", sub_vid, sub_did);
            }

            baseclass = __pci_attr_r8(devind, PCI_BCR);
            subclass = __pci_attr_r8(devind, PCI_SCR);
            infclass = __pci_attr_r8(devind, PCI_PIFR);
            s = pci_subclass_name((baseclass << 24) | (subclass << 16));
            if (!s) s = pci_baseclass_name(baseclass << 24);

            if (!s) s = "(unknown class)";
            if (debug) {
                printf("\tclass %s (%X/%X/%X)\n", s, baseclass, subclass, infclass);
            }

            if (is_duplicate(busnr, dev, func)) {
                printf("\tduplicate!\n");
                if (func == 0 && !(headt & PHT_MULTIFUNC)) break;
                else continue;
            }

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
                    printf("\t%d.%d.%d: unknown header type %d\n", busind, dev, func, headt & PHT_MASK);
                    break;
            }

            if (debug) print_capabilities(devind);
            if (nr_pcidev >= NR_PCIDEV) panic("too many PCI devices: %d", nr_pcidev);
            if (func == 0 && !(headt & PHT_MULTIFUNC)) break;
        }
    }
}


static u16_t pcibr_std_rsts(int busind) {
    if (busind < 0 || busind >= num_pcibus) {
        return 0; // or some error code if necessary
    }
    
    int devind = pcibus[busind].pb_devind;
    
    if (devind < 0) {
        return 0; // or some error code if necessary
    }
    
    return __pci_attr_r16(devind, PPB_SSTS);
}

#include <stdio.h>

#define PPB_SSTS 0x06

static void write_pci_status(int device_index, u16_t value);

static void pcibr_std_wsts(int busind, u16_t value) {
    if (busind < 0 || busind >= MAX_PCI_BUS) {
        fprintf(stderr, "Invalid bus index: %d\n", busind);
        return;
    }

    int devind = pcibus[busind].pb_devind;
    if (devind < 0) {
        fprintf(stderr, "Invalid device index for bus: %d\n", busind);
        return;
    }

    write_pci_status(devind, value);
}

void write_pci_status(int device_index, u16_t value) {
    __pci_attr_w16(device_index, PPB_SSTS, value);
}

u16_t pcibr_cb_rsts(int busind) {
    if (busind < 0 || busind >= MAX_PCIBUS_INDEX) {
        // Decide correct error handling strategy, e.g., return 0 or assert
        return 0;
    }
    int devind = pcibus[busind].pb_devind;
    return __pci_attr_r16(devind, CBB_SSTS);
}

void pcibr_cb_wsts(int busind, u16_t value) {
	if (busind < 0 || busind >= MAX_BUSES) {
		return; // Add an appropriate error handling mechanism or log
	}

	int devind = pcibus[busind].pb_devind;
	if (devind < 0) {
		return; // Add an appropriate error handling mechanism or log
	}

	__pci_attr_w16(devind, CBB_SSTS, value);
}

static uint16_t pcibr_via_rsts(int busind) {
    (void)busind; 
    return 0;
}



static void complete_bridges(void) {
    for (int i = 0; i < nr_pcibus; i++) {
        if (!pcibus[i].pb_needinit) continue;

        int freebus = get_freebus();
        int devind = pcibus[i].pb_devind;
        int prim_busnr = pcidev[devind].pd_busnr;

        if (prim_busnr != 0) {
            printf("complete_bridge: updating subordinate bus number not implemented\n");
        }

        pcibus[i].pb_needinit = 0;
        pcibus[i].pb_busnr = freebus;

        __pci_attr_w8(devind, PPB_PRIMBN, prim_busnr);
        __pci_attr_w8(devind, PPB_SECBN, freebus);
        __pci_attr_w8(devind, PPB_SUBORDBN, freebus);

        printf("devind = %d\n", devind);
        printf("prim_busnr = %d\n", prim_busnr);
        printf("CR = 0x%x\n", __pci_attr_r16(devind, PCI_CR));
        printf("SECBLT = 0x%x\n", __pci_attr_r8(devind, PPB_SECBLT));
        printf("BRIDGECTRL = 0x%x\n", __pci_attr_r16(devind, PPB_BRIDGECTRL));
    }
}

static void do_pcibridge(int busind) {
    int devind, busnr;
    int ind, type;
    u16_t vid, did;
    u8_t sbusn, baseclass, subclass, infclass, headt;
    u32_t t3;

    busnr = pcibus[busind].pb_busnr;

    for (devind = 0; devind < nr_pcidev; devind++) {
        if (pcidev[devind].pd_busnr != busnr) {
            continue;
        }

        vid = pcidev[devind].pd_vid;
        did = pcidev[devind].pd_did;

        headt = __pci_attr_r8(devind, PCI_HEADT);
        type = 0;

        switch (headt & PHT_MASK) {
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
        t3 = ((baseclass << 16) | (subclass << 8) | infclass);

        if ((type == PCI_PPB_STD && t3 != PCI_T3_PCI2PCI && t3 != PCI_T3_PCI2PCI_SUBTR) ||
            (type == PCI_PPB_CB && t3 != PCI_T3_CARDBUS)) {
            printf("Unknown PCI class %02x/%02x/%02x for bridge, device %04X:%04X\n",
                   baseclass, subclass, infclass, vid, did);
            continue;
        }

        if (debug) {
            printf("%u.%u.%u: PCI-to-PCI bridge: %04X:%04X\n",
                   pcidev[devind].pd_busnr, pcidev[devind].pd_dev, pcidev[devind].pd_func, vid, did);
        }

        sbusn = __pci_attr_r8(devind, PPB_SECBN);

        if (nr_pcibus >= NR_PCIBUS) {
            panic("too many PCI busses: %d", nr_pcibus);
        }

        ind = nr_pcibus++;
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

        switch (type) {
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

        if (machine.apic_enabled) {
            acpi_map_bridge(pcidev[devind].pd_busnr, pcidev[devind].pd_dev, sbusn);
        }

        if (debug) {
            printf("bus(table) = %d, bus(sec) = %d, bus(subord) = %d\n",
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
static void pci_intel_init(void) {
    u16_t vid, did;
    int s, r;
    int busind;
    const char *dstr;

    const u32_t bus = 0;
    const u32_t dev = 0;
    const u32_t func = 0;

    if ((vid = PCII_RREG16_(bus, dev, func, PCI_VID)) == 0xFFFF &&
        (did = PCII_RREG16_(bus, dev, func, PCI_DID)) == 0xFFFF) {
        if ((s = sys_outl(PCII_CONFADD, PCII_UNSEL)) != OK)
            printf("PCI: warning, sys_outl failed: %d\n", s);
        return;
    }

    if (nr_pcibus >= NR_PCIBUS)
        panic("too many PCI busses: %d", nr_pcibus);

    busind = nr_pcibus++;
    struct pcibus_t *current_bus = &pcibus[busind];
    current_bus->pb_type = PBT_INTEL_HOST;
    current_bus->pb_needinit = 0;
    current_bus->pb_isabridge_dev = -1;
    current_bus->pb_isabridge_type = 0;
    current_bus->pb_devind = -1;
    current_bus->pb_busnr = 0;
    current_bus->pb_rreg8 = pcii_rreg8;
    current_bus->pb_rreg16 = pcii_rreg16;
    current_bus->pb_rreg32 = pcii_rreg32;
    current_bus->pb_wreg8 = pcii_wreg8;
    current_bus->pb_wreg16 = pcii_wreg16;
    current_bus->pb_wreg32 = pcii_wreg32;
    current_bus->pb_rsts = pcii_rsts;
    current_bus->pb_wsts = pcii_wsts;

    dstr = _pci_dev_name(vid, did);
    if (!dstr) dstr = "unknown device";

    if (debug) {
        printf("pci_intel_init: %s (%04X:%04X)\n", dstr, vid, did);
    }

    probe_bus(busind);

    if ((r = do_isabridge(busind)) != OK) {
        int busnr = current_bus->pb_busnr;

        for (int i = 0; i < nr_pcidev; i++) {
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
static int visible(struct rs_pci *aclp, int devind) {
    u16_t acl_sub_vid, acl_sub_did;
    u32_t class_id;
    
    if (!aclp) return 1; // TRUE
    
    for (int i = 0; i < aclp->rsp_nr_device; i++) {
        acl_sub_vid = aclp->rsp_device[i].sub_vid;
        acl_sub_did = aclp->rsp_device[i].sub_did;
        
        if (aclp->rsp_device[i].vid == pcidev[devind].pd_vid &&
            aclp->rsp_device[i].did == pcidev[devind].pd_did &&
            (acl_sub_vid == NO_SUB_VID || acl_sub_vid == pcidev[devind].pd_sub_vid) &&
            (acl_sub_did == NO_SUB_DID || acl_sub_did == pcidev[devind].pd_sub_did)) {
            return 1; // TRUE
        }
    }
    
    if (aclp->rsp_nr_class == 0) return 0; // FALSE
    
    class_id = (pcidev[devind].pd_baseclass << 16) |
               (pcidev[devind].pd_subclass << 8) |
               pcidev[devind].pd_infclass;
    
    for (int i = 0; i < aclp->rsp_nr_class; i++) {
        if ((class_id & aclp->rsp_class[i].mask) == aclp->rsp_class[i].pciclass) {
            return 1; // TRUE
        }
    }
    
    return 0; // FALSE
}

/*===========================================================================*
 *				sef_cb_init_fresh			     *
 *===========================================================================*/
int sef_cb_init(int type, sef_init_info_t *info) {
    int do_announce_driver = -1;
    long debug_value = 0;
    struct rprocpub rprocpub[NR_BOOT_PROCS];

    env_parse("pci_debug", "d", 0, &debug_value, 0, 1);
    debug = debug_value;

    if (sys_getmachine(&machine) != OK) {
        fprintf(stderr, "PCI: no machine\n");
        return ENODEV;
    }

    if (machine.apic_enabled && acpi_init() != OK) {
        panic("PCI: Cannot use APIC mode without ACPI!\n");
    }

    pci_intel_init();

    if (sys_safecopyfrom(RS_PROC_NR, info->rproctab_gid, 0, (vir_bytes)rprocpub, sizeof(rprocpub)) != OK) {
        panic("sys_safecopyfrom failed\n");
    }

    for (int i = 0; i < NR_BOOT_PROCS; i++) {
        if (rprocpub[i].in_use) {
            if (map_service(&rprocpub[i]) != OK) {
                panic("unable to map service\n");
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
            panic("Unknown type of restart\n");
    }

    if (do_announce_driver) {
        chardriver_announce();
    }

    return OK;
}

/*===========================================================================*
 *		               map_service                                   *
 *===========================================================================*/
#include <stdio.h>
#include <errno.h>
#include <stdbool.h>

#define NR_DRIVERS 100
#define OK 0

struct pci_acl_entry {
    bool inuse;
    struct {
        unsigned int rsp_nr_device;
        unsigned int rsp_nr_class;
    } acl;
};

struct pci_acl_entry pci_acl[NR_DRIVERS];

struct rprocpub {
    struct {
        unsigned int rsp_nr_device;
        unsigned int rsp_nr_class;
    } pci_acl;
};

int map_service(const struct rprocpub *rpub) {
    if (rpub->pci_acl.rsp_nr_device == 0 && rpub->pci_acl.rsp_nr_class == 0) {
        return OK;
    }

    for (int i = 0; i < NR_DRIVERS; i++) {
        if (!pci_acl[i].inuse) {
            pci_acl[i].inuse = true;
            pci_acl[i].acl = rpub->pci_acl;
            return OK;
        }
    }
    
    fprintf(stderr, "PCI: map_service: table is full\n");
    return ENOMEM;
}

/*===========================================================================*
 *				_pci_find_dev				     *
 *===========================================================================*/
int _pci_find_dev(u8_t bus, u8_t dev, u8_t func, int *devindp) {
    if (devindp == NULL) {
        return 0; // Return early if the pointer is NULL to avoid dereferencing it
    }

    for (int devind = 0; devind < nr_pcidev; devind++) {
        if (pcidev[devind].pd_busnr == bus &&
            pcidev[devind].pd_dev == dev &&
            pcidev[devind].pd_func == func) {
            *devindp = devind;
            return 1;
        }
    }
    return 0;
}

/*===========================================================================*
 *				_pci_first_dev				     *
 *===========================================================================*/
int _pci_first_dev(struct rs_pci *aclp, int *devindp, u16_t *vidp, u16_t *didp) {
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
int _pci_next_dev(struct rs_pci *aclp, int *devindp, u16_t *vidp, u16_t *didp) {
    if (!aclp || !devindp || !vidp || !didp) return 0;

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
int _pci_grant_access(int devind, endpoint_t proc) {
    int i, ilr;
    int result = OK;
    struct io_range ior;
    struct minix_mem_range mr;

    for (i = 0; i < pcidev[devind].pd_bar_nr; i++) {
        if (pcidev[devind].pd_bar[i].pb_flags & PBF_INCOMPLETE) {
            printf("pci_reserve_a: BAR %d is incomplete\n", i);
            continue;
        }

        void *range = (pcidev[devind].pd_bar[i].pb_flags & PBF_IO) ? (void *)&ior : (void *)&mr;
        int sys_priv_type = (pcidev[devind].pd_bar[i].pb_flags & PBF_IO) ? SYS_PRIV_ADD_IO : SYS_PRIV_ADD_MEM;
        unsigned long base = pcidev[devind].pd_bar[i].pb_base;
        unsigned long limit = base + pcidev[devind].pd_bar[i].pb_size - 1;

        if (sys_priv_type == SYS_PRIV_ADD_IO) {
            ior.ior_base = base;
            ior.ior_limit = limit;
            if (debug) printf("pci_reserve_a: for proc %d, adding I/O range [0x%lx..0x%lx]\n", proc, ior.ior_base, ior.ior_limit);
        } else {
            mr.mr_base = base;
            mr.mr_limit = limit;
        }

        result = sys_privctl(proc, sys_priv_type, range);
        if (result != OK) {
            printf("sys_privctl failed for proc %d: %d\n", proc, result);
        }
    }

    ilr = pcidev[devind].pd_ilr;
    if (ilr != PCI_ILR_UNKNOWN) {
        if (debug) printf("pci_reserve_a: adding IRQ %d\n", ilr);
        result = sys_privctl(proc, SYS_PRIV_ADD_IRQ, &ilr);
        if (result != OK) {
            printf("sys_privctl failed for proc %d: %d\n", proc, result);
        }
    }

    return result;
}

/*===========================================================================*
 *				_pci_reserve				     *
 *===========================================================================*/
int _pci_reserve(int devind, endpoint_t proc, struct rs_pci *aclp) {
    if (devind < 0 || devind >= nr_pcidev) {
        fprintf(stderr, "Error: bad devind: %d\n", devind);
        return EINVAL;
    }

    if (!visible(aclp, devind)) {
        fprintf(stderr, "Error: %u is not allowed to reserve %d\n", proc, devind);
        return EPERM;
    }

    if (pcidev[devind].pd_inuse) {
        if (pcidev[devind].pd_proc != proc) {
            return EBUSY;
        }
    }

    pcidev[devind].pd_inuse = 1;
    pcidev[devind].pd_proc = proc;

    return _pci_grant_access(devind, proc);
}

/*===========================================================================*
 *				_pci_release				     *
 *===========================================================================*/
void _pci_release(endpoint_t proc) {
    for (int i = 0; i < nr_pcidev; i++) {
        if (pcidev[i].pd_inuse && pcidev[i].pd_proc == proc) {
            pcidev[i].pd_inuse = 0;
        }
    }
}

/*===========================================================================*
 *				_pci_ids				     *
 *===========================================================================*/
int _pci_ids(int devind, u16_t *vidp, u16_t *didp) {
    if (vidp == NULL || didp == NULL || devind < 0 || devind >= nr_pcidev) {
        return EINVAL;
    }

    *vidp = pcidev[devind].pd_vid;
    *didp = pcidev[devind].pd_did;
    return OK;
}

/*===========================================================================*
 *				_pci_rescan_bus				     *
 *===========================================================================*/
void _pci_rescan_bus(uint8_t busnr) {
    int busind = get_busind(busnr);
    if (busind < 0) {
        // Handle error, potentially logging or returning
        return;
    }
    if (probe_bus(busind) != 0) {
        // Handle error during probe
        return;
    }

    if (complete_bridges() != 0) {
        // Handle error during bridge completion
        return;
    }

    if (complete_bars() != 0) {
        // Handle error during bar completion
        return;
    }
}

/*===========================================================================*
 *				_pci_slot_name				     *
 *===========================================================================*/
int _pci_slot_name(int devind, char **cpp) {
    static char label[16];
    
    if (devind < 0 || devind >= nr_pcidev || cpp == NULL) {
        return EINVAL;
    }

    snprintf(label, sizeof(label), "0.%d.%d.%d", 
             pcidev[devind].pd_busnr, 
             pcidev[devind].pd_dev, 
             pcidev[devind].pd_func);

    *cpp = label;
    return OK;
}

/*===========================================================================*
 *				_pci_dev_name				     *
 *===========================================================================*/
#define PRODUCT_STR_MAX_LEN 128

const char *_pci_dev_name(uint16_t vid, uint16_t did) {
    static char product[PRODUCT_STR_MAX_LEN];
    if (!pci_findproduct(product, sizeof(product), vid, did)) {
        return "Unknown Device";
    }
    return product;
}

/*===========================================================================*
 *				_pci_get_bar				     *
 *===========================================================================*/
int _pci_get_bar(int devind, int port, u32_t *base, u32_t *size, int *ioflag) {
    if (devind < 0 || devind >= nr_pcidev || !base || !size || !ioflag) {
        return EINVAL;
    }

    for (int i = 0; i < pcidev[devind].pd_bar_nr; i++) {
        int reg = PCI_BAR + 4 * pcidev[devind].pd_bar[i].pb_nr;

        if (reg == port) {
            if (pcidev[devind].pd_bar[i].pb_flags & PBF_INCOMPLETE) {
                return EINVAL;
            }

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
int _pci_attr_r8(int devind, int port, u8_t *vp) {
    if (!vp || devind < 0 || devind >= nr_pcidev || port < 0 || port >= 256) {
        return EINVAL;
    }
    
    *vp = __pci_attr_r8(devind, port);
    return OK;
}

/*===========================================================================*
 *				_pci_attr_r16				     *
 *===========================================================================*/
#define MIN_PORT 0
#define MAX_PORT 254

int _pci_attr_r16(int devind, int port, u16_t *vp) {
    if (vp == NULL) {
        return EINVAL;
    }
    if (devind < 0 || devind >= nr_pcidev || port < MIN_PORT || port > MAX_PORT) {
        return EINVAL;
    }

    *vp = __pci_attr_r16(devind, port);
    return OK;
}

/*===========================================================================*
 *				_pci_attr_r32				     *
 *===========================================================================*/
int _pci_attr_r32(int devind, int port, u32_t *vp) {
    if (devind < 0 || devind >= nr_pcidev || port < 0 || port > 252 || vp == NULL) {
        return EINVAL;
    }

    *vp = __pci_attr_r32(devind, port);
    return OK;
}

/*===========================================================================*
 *				_pci_attr_w8				     *
 *===========================================================================*/
int _pci_attr_w8(int devind, int port, u8_t value) {
    if (devind < 0 || devind >= nr_pcidev || port < 0 || port >= 256) {
        return EINVAL;
    }

    __pci_attr_w8(devind, port, value);
    return OK;
}

/*===========================================================================*
 *				_pci_attr_w16				     *
 *===========================================================================*/
#include <stdbool.h>

int _pci_attr_w16(int devind, int port, u16_t value) {
    if (!is_valid_device(devind) || !is_valid_port(port)) {
        return EINVAL;
    }

    __pci_attr_w16(devind, port, value);
    return OK;
}

bool is_valid_device(int devind) {
    return devind >= 0 && devind < nr_pcidev;
}

bool is_valid_port(int port) {
    return port >= 0 && port <= 254; // 256-2 is equivalent to 254
}

/*===========================================================================*
 *				_pci_attr_w32				     *
 *===========================================================================*/
#include <errno.h> // For EINVAL

int _pci_attr_w32(int devind, int port, u32_t value) {
    if (devind < 0 || devind >= nr_pcidev || port < 0 || port > 252) {
        return EINVAL;
    }

    __pci_attr_w32(devind, port, value);
    return 0; // Use standard convention for success
}
