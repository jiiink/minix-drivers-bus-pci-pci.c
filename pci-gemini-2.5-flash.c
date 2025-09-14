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
	int status;

	status = sys_inb(port, &value);

	if (status != OK) {
		printf("PCI: warning, sys_inb failed for port %hu: %d\n", port, status);
	}
	return value;
}

static u32_t
pci_inw(u16_t port) {
	u32_t value = (u32_t)-1;
	int result = sys_inw(port, &value);
	if (result != OK) {
		printf("PCI: warning, sys_inw failed: %d\n", result);
	}
	return value;
}

static unsigned
pci_inl(u16_t port) {
    u32_t value;
    int status = sys_inl(port, &value);
    if (status != OK) {
        fprintf(stderr, "PCI: warning, sys_inl failed for port %u: %d\n", (unsigned)port, status);
        return 0;
    }
    return value;
}

static int
pci_outb(u16_t port, u8_t value) {
    int result = sys_outb(port, value);
    if (result != OK) {
        printf("PCI: WARNING: sys_outb failed with error %d for port 0x%04x, value 0x%02x\n", result, port, value);
    }
    return result;
}

static int
pci_outw(u16_t port, u16_t value) {
    return sys_outw(port, value);
}

static int
pci_outl(u16_t port, u32_t value) {
    int status = sys_outl(port, value);
    if (status != OK) {
        printf("PCI: warning, sys_outl failed: %d\n", status);
        return status;
    }
    return OK;
}

static u8_t
pcii_rreg8(int busind, int devind, int port)
{
	if (busind < 0 || (size_t)busind >= PCIBUS_COUNT) {
		printf("PCI: error, pcii_rreg8 received out-of-bounds bus index: %d (max %zu)\n", busind, PCIBUS_COUNT - 1);
		return 0;
	}

	if (devind < 0 || (size_t)devind >= PCIDEV_COUNT) {
		printf("PCI: error, pcii_rreg8 received out-of-bounds device index: %d (max %zu)\n", devind, PCIDEV_COUNT - 1);
		return 0;
	}

	u8_t v = PCII_RREG8_(pcibus[busind].pb_busnr,
		             pcidev[devind].pd_dev,
		             pcidev[devind].pd_func,
		             port);

	int s = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (OK != s) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}

	return v;
}

static u16_t
pcii_rreg16(int busind, int devind, int port)
{
    if (busind < 0 || busind >= PCI_MAX_BUS_INDEX) {
        printf("PCI: error, invalid bus index: %d\n", busind);
        return 0;
    }
    if (devind < 0 || devind >= PCI_MAX_DEV_INDEX) {
        printf("PCI: error, invalid device index: %d\n", devind);
        return 0;
    }

    u16_t val_read = PCII_RREG16_(
        pcibus[busind].pb_busnr,
        pcidev[devind].pd_dev,
        pcidev[devind].pd_func,
        port
    );
    
    int sys_outl_status = sys_outl(PCII_CONFADD, PCII_UNSEL);
    if (OK != sys_outl_status) {
        printf("PCI: warning, sys_outl failed: %d\n", sys_outl_status);
    }
    
    return val_read;
}

static u32_t
pcii_rreg32(int busind, int devind, int port)
{
	u32_t value = PCII_RREG32_(
		pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev,
		pcidev[devind].pd_func,
		port
	);

	int sys_status = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (OK != sys_status) {
		printf("PCI: warning, sys_outl(PCII_CONFADD, PCII_UNSEL) failed with status: %d\n", sys_status);
	}

	return value;
}

static void
pcii_wreg8(int busind, int devind, int port, u8_t value)
{
	int s;

	if (busind < 0 || busind >= PCIBUS_COUNT) {
		printf("PCI: warning, invalid bus index %d. Must be between 0 and %d.\n", busind, PCIBUS_COUNT - 1);
		return;
	}

	if (devind < 0 || devind >= PCIDEV_COUNT) {
		printf("PCI: warning, invalid device index %d. Must be between 0 and %d.\n", devind, PCIDEV_COUNT - 1);
		return;
	}

	PCII_WREG8_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port, value);

	if (OK != (s = sys_outl(PCII_CONFADD, PCII_UNSEL))) {
		printf("PCI: warning, sys_outl failed: %d\n", s);
	}
}

static void
pcii_wreg16(int busind, int devind, int port, u16_t value)
{
	int s;
	PCII_WREG16_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port, value);
	if (OK != (s=sys_outl(PCII_CONFADD, PCII_UNSEL)))
		printf("PCI: warning, sys_outl failed: %d\n", s);
}

static void
pcii_wreg32(int busind, int devind, int port, u32_t value)
{
	PCII_WREG32_(pcibus[busind].pb_busnr,
		pcidev[devind].pd_dev, pcidev[devind].pd_func,
		port, value);

	int status = sys_outl(PCII_CONFADD, PCII_UNSEL);
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
    int i = 0;

    do {
        tmpstr[i++] = '0' + (n % 10);
        n /= 10;
    } while (n > 0);

    while (i > 0) {
        if (*str == end) {
            break;
        }
        **str = tmpstr[--i];
        (*str)++;
    }

    if (*str == end) {
        (*str)[-1] = '\0';
    } else {
        **str = '\0';
    }
}

/*===========================================================================*
 *				get_busind					     *
 *===========================================================================*/
static int
get_busind(int busnr)
{
	for (int i = 0; i < nr_pcibus; i++)
	{
		if (pcibus[i].pb_busnr == busnr)
			return i;
	}
	return -1;
}

/*===========================================================================*
 *			Unprotected helper functions			     *
 *===========================================================================*/
static u8_t
__pci_attr_r8(int devind, int port)
{
    if (devind < 0 || devind >= MAX_PCI_DEVICES) {
        return 0xFF;
    }

    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);

    if (busind < 0 || busind >= MAX_PCI_BUSES) {
        return 0xFF;
    }

    if (pcibus[busind].pb_rreg8 == NULL) {
        return 0xFF;
    }

    return pcibus[busind].pb_rreg8(busind, devind, port);
}

static u16_t
__pci_attr_r16(int devind, int port)
{
    const u16_t PCI_READ_ERROR_VALUE = (u16_t)0xFFFF;

    if (devind < 0 || devind >= PCI_MAX_DEVICES) {
        return PCI_READ_ERROR_VALUE;
    }

    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);

    if (busind < 0 || busind >= PCI_MAX_BUSES) {
        return PCI_READ_ERROR_VALUE;
    }

    if (pcibus[busind].pb_rreg16 == NULL) {
        return PCI_READ_ERROR_VALUE;
    }

    return pcibus[busind].pb_rreg16(busind, devind, port);
}

static u32_t
__pci_attr_r32(int devind, int port)
{
    if (devind < 0 || devind >= MAX_PCI_DEVICES) {
        return PCI_READ_ERROR_VALUE;
    }

    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);

    if (busind < 0 || busind >= MAX_PCI_BUSES) {
        return PCI_READ_ERROR_VALUE;
    }

    if (pcibus[busind].pb_rreg32 == NULL) {
        return PCI_READ_ERROR_VALUE;
    }

    return pcibus[busind].pb_rreg32(busind, devind, port);
}

static void
pci_attr_w8(int devind, int port, u8_t value)
{
	int busnr;
	int busind;

	if (devind < 0 || devind >= MAX_PCI_DEVICES) {
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	busind = get_busind(busnr);

	if (busind < 0 || busind >= MAX_PCI_BUSES) {
		return;
	}

	if (pcibus[busind].pb_wreg8 == NULL) {
		return;
	}

	pcibus[busind].pb_wreg8(busind, devind, port, value);
}

#include <stdio.h>

static void
__pci_attr_w16(int devind, int port, u16_t value)
{
	if (devind < 0 || devind >= MAX_PCI_DEVICES) {
		fprintf(stderr, "ERROR: __pci_attr_w16: Invalid device index %d\n", devind);
		return;
	}

	int busnr = pcidev[devind].pd_busnr;
	int busind = get_busind(busnr);

	if (busind < 0 || busind >= MAX_PCI_BUSES) {
		fprintf(stderr, "ERROR: __pci_attr_w16: Invalid bus index %d for bus number %d\n", busind, busnr);
		return;
	}

	if (pcibus[busind].pb_wreg16 == NULL) {
		fprintf(stderr, "ERROR: __pci_attr_w16: Bus %d (index %d) has a NULL write register function pointer.\n", busnr, busind);
		return;
	}

	pcibus[busind].pb_wreg16(busind, devind, port, value);
}

#ifndef u32_t
typedef unsigned int u32_t;
#endif

#define MAX_PCI_DEVICES 256
#define MAX_PCI_BUSES   8

typedef struct {
    int pd_busnr;
} PciDevice;

typedef struct {
    void (*pb_wreg32)(int bus_index, int device_index, int port_offset, u32_t value);
} PciBus;

extern PciDevice pcidev[MAX_PCI_DEVICES];
extern PciBus pcibus[MAX_PCI_BUSES];
extern int get_busind(int busnr);

static void
__pci_attr_w32(int devind, int port, u32_t value)
{
    if (devind < 0 || devind >= MAX_PCI_DEVICES) {
        return;
    }

    int busnr = pcidev[devind].pd_busnr;
    int busind = get_busind(busnr);

    if (busind < 0 || busind >= MAX_PCI_BUSES) {
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
	if (devind < 0 || devind >= PCI_MAX_DEVICES) {
		return 0;
	}

	int busnr = pcidev[devind].pd_busnr;
	int busind = get_busind(busnr);

	if (busind < 0 || busind >= PCI_MAX_BUSES) {
		return 0;
	}

	if (pcibus[busind].pb_rsts == NULL) {
		return 0;
	}

	return pcibus[busind].pb_rsts(busind);
}

static void
pci_attr_wsts(int devind, u16_t value)
{
	if (devind < 0 || devind >= PCI_MAX_DEVICES) {
		return;
	}

	int busnr = pcidev[devind].pd_busnr;
	int busind = get_busind(busnr);

	if (busind < 0 || busind >= PCI_MAX_BUSES) {
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
    u16_t status_register_value;
    int sys_call_result;

    status_register_value = PCII_RREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR);

    sys_call_result = sys_outl(PCII_CONFADD, PCII_UNSEL);

    if (OK != sys_call_result)
        printf("PCI: warning, sys_outl failed: %d\n", sys_call_result);

    return status_register_value;
}

static void
pcii_wsts(int busind, u16_t value)
{
	if (busind < 0 || busind >= PCI_MAX_BUSES) {
		printf("PCI: warning, pcii_wsts called with invalid bus index: %d\n", busind);
		return;
	}

	PCII_WREG16_(pcibus[busind].pb_busnr, 0, 0, PCI_SR, value);

	int status_code = sys_outl(PCII_CONFADD, PCII_UNSEL);
	if (OK != status_code) {
		printf("PCI: warning, sys_outl failed: %d\n", status_code);
	}
}

static int
is_duplicate(const u8_t busnr, const u8_t dev, const u8_t func)
{
	size_t i;

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

	for (int i = 0; i < nr_pcibus; ++i)
	{
		if (pcibus[i].pb_needinit || pcibus[i].pb_type == PBT_INTEL_HOST) {
			continue;
		}

		if (pcibus[i].pb_busnr <= freebus) {
			freebus = pcibus[i].pb_busnr + 1;
		}
	}
	return freebus;
}

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const char * const UNKNOWN_VENDOR_NAME = "<Unknown Vendor>";
static const char * const ALLOCATION_ERROR_NAME = "<Allocation Error>";

static const char *
pci_vid_name(u16_t vid)
{
    char buffer[PCI_VENDORSTR_LEN];
    buffer[0] = '\0';

    pci_findvendor(buffer, sizeof(buffer), vid);

    if (buffer[0] == '\0') {
        return UNKNOWN_VENDOR_NAME;
    }

    char *vendor_name = strdup(buffer);

    if (vendor_name == NULL) {
        return ALLOCATION_ERROR_NAME;
    }

    return vendor_name;
}


#include <stdio.h>

// Assuming u8_t, u16_t, u32_t are defined elsewhere (e.g., <stdint.h> or a custom header)
// For this context, we'll assume they are available.
// typedef unsigned char u8_t;
// typedef unsigned short u16_t;
// typedef unsigned int u32_t;

// Assuming __pci_attr_r32 is an external function
extern u32_t __pci_attr_r32(int devind, u8_t capptr);

// Constants for HyperTransport Capability Register 0 (Cap ID, Next Ptr, HT Cap Type)
// The HyperTransport Capability Type is located in bits 16-31 of the DWord.
#define HT_CAP_REGISTER0_TYPE_FIELD_SHIFT 16
#define HT_CAP_REGISTER0_TYPE_FIELD_MASK  0xFFFFU

// Constants for decoding the 16-bit HyperTransport Capability Type field itself.
// These masks and shifts are relative to the 16-bit HT Capability Type value.
// Type Field (3-bit): Bits 15:13 of the 16-bit HT Capability Type.
#define HT_TYPE_FIELD_3BIT_SHIFT    13
#define HT_TYPE_FIELD_3BIT_MASK     (0x7U << HT_TYPE_FIELD_3BIT_SHIFT) // 0xE000U

// Type Field (5-bit): Bits 15:11 of the 16-bit HT Capability Type.
#define HT_TYPE_FIELD_5BIT_SHIFT    11
#define HT_TYPE_FIELD_5BIT_MASK     (0x1FU << HT_TYPE_FIELD_5BIT_SHIFT) // 0xF800U

static void
print_hyper_cap(int devind, u8_t capptr)
{
    u32_t capability_dword_0;
    u16_t ht_capability_type_value;
    unsigned int decoded_type_3bit;
    unsigned int decoded_type_5bit;

    printf("\n");
    capability_dword_0 = __pci_attr_r32(devind, capptr);
    printf("print_hyper_cap: @0x%x, off 0 (cap):", capptr);

    // Extract the 16-bit HyperTransport Capability Type field from the DWord.
    ht_capability_type_value = (u16_t)((capability_dword_0 >> HT_CAP_REGISTER0_TYPE_FIELD_SHIFT) & HT_CAP_REGISTER0_TYPE_FIELD_MASK);

    // Attempt to decode a 3-bit type field.
    decoded_type_3bit = (ht_capability_type_value & HT_TYPE_FIELD_3BIT_MASK) >> HT_TYPE_FIELD_3BIT_SHIFT;

    if (decoded_type_3bit == 0 || decoded_type_3bit == 1)
    {
        printf("Capability Type: %s\n",
               decoded_type_3bit == 0 ? "Slave or Primary Interface" :
                                        "Host or Secondary Interface");
        // Clear the bits that were just decoded to track undecoded parts.
        ht_capability_type_value &= ~HT_TYPE_FIELD_3BIT_MASK;
    }
    else
    {
        // If the 3-bit type is not 0 or 1, attempt to decode a 5-bit type field.
        decoded_type_5bit = (ht_capability_type_value & HT_TYPE_FIELD_5BIT_MASK) >> HT_TYPE_FIELD_5BIT_SHIFT;
        printf(" Capability Type 0x%x", decoded_type_5bit);
        // Clear the bits that were just decoded.
        ht_capability_type_value &= ~HT_TYPE_FIELD_5BIT_MASK;
    }

    // Report any remaining bits in the HT Capability Type field as undecoded.
    if (ht_capability_type_value != 0)
    {
        printf(" undecoded 0x%x\n", ht_capability_type_value);
    }
}

static const u8_t PCI_CAP_ID_PM = 0x01;
static const u8_t PCI_CAP_ID_AGP = 0x02;
static const u8_t PCI_CAP_ID_VPD = 0x03;
static const u8_t PCI_CAP_ID_SLOTID = 0x04;
static const u8_t PCI_CAP_ID_MSI = 0x05;
static const u8_t PCI_CAP_ID_CPCI_HS = 0x06;
static const u8_t PCI_CAP_ID_HT = 0x08;
static const u8_t PCI_CAP_ID_SDA = 0x0F;

static const u8_t PCI_SDA_CAP_SUBTYPE_OFFSET = 2;
static const u8_t PCI_SDA_CAP_SUBTYPE_MASK = 0x07;

static const u8_t PCI_SDA_SUBTYPE_DEV_EXCL = 0x00;
static const u8_t PCI_SDA_SUBTYPE_IOMMU = 0x03;

static const char* get_pci_cap_type_string(u8_t type)
{
    switch (type)
    {
        case PCI_CAP_ID_PM: return "PCI Power Management";
        case PCI_CAP_ID_AGP: return "AGP";
        case PCI_CAP_ID_VPD: return "Vital Product Data";
        case PCI_CAP_ID_SLOTID: return "Slot Identification";
        case PCI_CAP_ID_MSI: return "Message Signaled Interrupts";
        case PCI_CAP_ID_CPCI_HS: return "CompactPCI Hot Swap";
        case PCI_CAP_ID_HT: return "AMD HyperTransport";
        case PCI_CAP_ID_SDA: return "Secure Device";
        default:   return "(unknown type)";
    }
}

static const char* get_pci_sda_subtype_string(u8_t subtype)
{
    switch (subtype)
    {
        case PCI_SDA_SUBTYPE_DEV_EXCL: return "Device Exclusion Vector";
        case PCI_SDA_SUBTYPE_IOMMU: return "IOMMU";
        default:   return "(unknown sub-type)";
    }
}

static void
print_capabilities(int devind)
{
	u8_t status_reg_value, capability_pointer, capability_type, next_capability_pointer, sda_subtype;
	const char *type_str, *subtype_str;

	status_reg_value = __pci_attr_r16(devind, PCI_SR);
	if (!(status_reg_value & PSR_CAPPTR))
		return;

	capability_pointer = (__pci_attr_r8(devind, PCI_CAPPTR) & PCI_CP_MASK);
	while (capability_pointer != 0)
	{
		capability_type = __pci_attr_r8(devind, capability_pointer + CAP_TYPE);
		next_capability_pointer = (__pci_attr_r8(devind, capability_pointer + CAP_NEXT) & PCI_CP_MASK);

		type_str = get_pci_cap_type_string(capability_type);

		printf(" @0x%x (0x%08x): capability type 0x%x: %s",
			   capability_pointer, __pci_attr_r32(devind, capability_pointer), capability_type, type_str);
		if (capability_type == PCI_CAP_ID_HT)
			print_hyper_cap(devind, capability_pointer);
		else if (capability_type == PCI_CAP_ID_SDA)
		{
			sda_subtype = (__pci_attr_r8(devind, capability_pointer + PCI_SDA_CAP_SUBTYPE_OFFSET) & PCI_SDA_CAP_SUBTYPE_MASK);
			subtype_str = get_pci_sda_subtype_string(sda_subtype);
			printf(", sub type 0%o: %s", sda_subtype, subtype_str);
		}
		printf("\n");
		capability_pointer = next_capability_pointer;
	}
}

/*===========================================================================*
 *				ISA Bridge Helpers			     *
 *===========================================================================*/
static void
update_bridge4dev_io(int devind, u32_t io_base, u32_t io_size)
{
	int busnr;
	int busind;
	int type;
	int br_devind;
	u16_t pci_cr_val;

	if (devind < 0) {
		panic("update_bridge4dev_io: invalid device index %d", devind);
	}

	if (io_size == 0) {
		if (debug) {
			printf("update_bridge4dev_io: zero io_size provided for devind %d, returning.\n", devind);
		}
		return;
	}

	busnr = pcidev[devind].pd_busnr;
	if (busnr < 0) {
		panic("update_bridge4dev_io: device %d has invalid bus number %d", devind, busnr);
	}

	busind = get_busind(busnr);
	if (busind < 0) {
		panic("update_bridge4dev_io: failed to get valid bus index for busnr %d (returned %d)", busnr, busind);
	}

	type = pcibus[busind].pb_type;

	if (type == PBT_INTEL_HOST) {
		if (debug) {
			printf("update_bridge4dev_io: bus %d is INTEL_HOST, nothing to do.\n", busnr);
		}
		return;
	}

	if (type == PBT_PCIBRIDGE) {
		printf("update_bridge4dev_io: not implemented for PCI bridges (bus %d).\n", busnr);
		return;
	}

	if (type != PBT_CARDBUS) {
		panic("update_bridge4dev_io: unsupported bus type %d for bus %d (devind %d)", type, busnr, devind);
	}

	if (debug) {
		printf("update_bridge4dev_io: devind %d (bus %d, type CARDBUS): adding I/O range 0x%x at 0x%x.\n",
			devind, busnr, io_size, io_base);
	}

	br_devind = pcibus[busind].pb_devind;
	if (br_devind < 0) {
		panic("update_bridge4dev_io: bus %d has invalid bridge device index %d", busnr, br_devind);
	}

	__pci_attr_w32(br_devind, CBB_IOBASE_0, io_base);
	__pci_attr_w32(br_devind, CBB_IOLIMIT_0, io_base + io_size - 1);

	pci_cr_val = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, pci_cr_val | PCI_CR_IO_EN | PCI_CR_MAST_EN);
}

static int
do_piix(int devind)
{
    int i;
    int status;
    u8_t elcr1_val, elcr2_val;
    u16_t elcr_full;

    if ((status = sys_inb(PIIX_ELCR1, &elcr1_val)) != OK) {
        printf("Warning, sys_inb failed for PIIX_ELCR1: %d\n", status);
        return status;
    }

    if ((status = sys_inb(PIIX_ELCR2, &elcr2_val)) != OK) {
        printf("Warning, sys_inb failed for PIIX_ELCR2: %d\n", status);
        return status;
    }

    elcr_full = (u16_t)elcr1_val | ((u16_t)elcr2_val << 8);

    for (i = 0; i < 4; ++i) {
        u8_t irq_routing_control = __pci_attr_r8(devind, PIIX_PIRQRCA + i);

        if (irq_routing_control & PIIX_IRQ_DI) {
            if (debug) {
                printf("INT%c: disabled\n", 'A' + i);
            }
        } else {
            int irq_num = irq_routing_control & PIIX_IRQ_MASK;

            if (debug) {
                printf("INT%c: %d\n", 'A' + i, irq_num);
            }

            if (!((elcr_full >> irq_num) & 1)) {
                if (debug) {
                    printf("(warning) IRQ %d is not level triggered in ELCR\n", irq_num);
                }
            }

            irq_mode_pci(irq_num);
        }
    }

    return OK;
}

static int
do_amd_isabr(int devind)
{
	int i;
	int busnr;
	int dev;
	int func;
	int xdevind;
	int irq_val;
	int edge;
	u8_t levmask;
	u16_t pciirq_route;
	int result = 0;
	int pcidev_added = 0;

	func = AMD_ISABR_FUNC;
	busnr = pcidev[devind].pd_busnr;
	dev = pcidev[devind].pd_dev;

	if (nr_pcidev >= NR_PCIDEV) {
		result = -1;
		goto end;
	}

	xdevind = nr_pcidev;
	pcidev[xdevind].pd_busnr = busnr;
	pcidev[xdevind].pd_dev = dev;
	pcidev[xdevind].pd_func = func;
	pcidev[xdevind].pd_inuse = 1;

	nr_pcidev++;
	pcidev_added = 1;

	levmask = __pci_attr_r8(xdevind, AMD_ISABR_PCIIRQ_LEV);
	pciirq_route = __pci_attr_r16(xdevind, AMD_ISABR_PCIIRQ_ROUTE);

	for (i = 0; i < 4; i++) {
		edge = (levmask >> i) & 1;
		irq_val = (pciirq_route >> (4 * i)) & 0xf;

		if (irq_val == 0) {
			if (debug) {
				printf("INT%c: disabled\n", 'A' + i);
			}
		} else {
			if (debug) {
				printf("INT%c: %d\n", 'A' + i, irq_val);
			}
			if (edge && debug) {
				printf("(warning) IRQ %d is not level triggered\n", irq_val);
			}
			irq_mode_pci(irq_val);
		}
	}

end:
	if (pcidev_added) {
		nr_pcidev--;
	}
	return result;
}

static int
do_sis_isabr(int devind)
{
	int i, irq;

	for (i= 0; i<4; i++)
	{
		irq= __pci_attr_r8(devind, SIS_ISABR_IRQ_A+i);
		if (irq & SIS_IRQ_DISABLED)
		{
			if (debug)
				printf("INT%c: disabled\n", 'A'+i);
		}
		else
		{
			irq &= SIS_IRQ_MASK;
			if (debug)
				printf("INT%c: %d\n", 'A'+i, irq);
			irq_mode_pci(irq);
		}
	}
	return 0;
}

static int
do_via_isabr(int devind)
{
	const u8_t edge_level_masks[] = {
		VIA_ISABR_EL_INTA,
		VIA_ISABR_EL_INTB,
		VIA_ISABR_EL_INTC,
		VIA_ISABR_EL_INTD
	};

	const u8_t irq_reg_offsets[] = {
		VIA_ISABR_IRQ_R2,
		VIA_ISABR_IRQ_R2,
		VIA_ISABR_IRQ_R3,
		VIA_ISABR_IRQ_R1
	};

	const int irq_shifts[] = {
		4,
		0,
		4,
		4
	};

	const int IRQ_NIBBLE_MASK = 0xF;

	u8_t levmask = __pci_attr_r8(devind, VIA_ISABR_EL);

	for (int i = 0; i < 4; i++)
	{
		int edge = (levmask & edge_level_masks[i]);
		int irq_val = __pci_attr_r8(devind, irq_reg_offsets[i]);
		irq_val = (irq_val >> irq_shifts[i]) & IRQ_NIBBLE_MASK;

		if (irq_val == 0)
		{
			if (debug)
				printf("INT%c: disabled\n", 'A' + i);
		}
		else
		{
			if (debug)
				printf("INT%c: %d\n", 'A' + i, irq_val);

			if (edge && debug)
			{
				printf("(warning) IRQ %d is not level triggered\n", irq_val);
			}
			irq_mode_pci(irq_val);
		}
	}
	return 0;
}

static int
do_isabridge(int busind)
{
    int i;
    int current_busnr = pcibus[busind].pb_busnr;

    int found_known_bridge_idx = -1;
    int found_unknown_bridge_idx = -1;
    int matched_pci_isabridge_entry_idx = -1;

    u16_t bridge_vid = 0;
    u16_t bridge_did = 0;

    for (i = 0; i < nr_pcidev; i++)
    {
        if (pcidev[i].pd_busnr != current_busnr)
        {
            continue;
        }

        u32_t device_class_id = ((u32_t)pcidev[i].pd_baseclass << 16) |
                                ((u32_t)pcidev[i].pd_subclass << 8) |
                                pcidev[i].pd_infclass;

        if (device_class_id == PCI_T3_ISA && found_unknown_bridge_idx == -1)
        {
            found_unknown_bridge_idx = i;
        }

        for (int j = 0; pci_isabridge[j].vid != 0; j++)
        {
            if (pci_isabridge[j].vid == pcidev[i].pd_vid &&
                pci_isabridge[j].did == pcidev[i].pd_did)
            {
                if (pci_isabridge[j].checkclass && device_class_id != PCI_T3_ISA)
                {
                    continue;
                }

                found_known_bridge_idx = i;
                matched_pci_isabridge_entry_idx = j;
                bridge_vid = pcidev[i].pd_vid;
                bridge_did = pcidev[i].pd_did;
                goto found_bridge_processing;
            }
        }
    }

found_bridge_processing:;

    if (found_known_bridge_idx != -1)
    {
        int bridge_dev_idx = found_known_bridge_idx;
        int bridge_type = pci_isabridge[matched_pci_isabridge_entry_idx].type;

        pcibus[busind].pb_isabridge_dev = bridge_dev_idx;
        pcibus[busind].pb_isabridge_type = bridge_type;

        if (debug)
        {
            const char *dstr = _pci_dev_name(bridge_vid, bridge_did);
            if (!dstr)
            {
                dstr = "unknown device";
            }
            printf("found ISA bridge (%04X:%04X) %s\n", bridge_vid, bridge_did, dstr);
        }

        int r = 0;
        switch(bridge_type)
        {
            case PCI_IB_PIIX:
                r = do_piix(bridge_dev_idx);
                break;
            case PCI_IB_VIA:
                r = do_via_isabr(bridge_dev_idx);
                break;
            case PCI_IB_AMD:
                r = do_amd_isabr(bridge_dev_idx);
                break;
            case PCI_IB_SIS:
                r = do_sis_isabr(bridge_dev_idx);
                break;
            default:
                panic("unknown ISA bridge type: %d", bridge_type);
        }
        return r;
    }

    if (found_unknown_bridge_idx != -1)
    {
        if (debug)
        {
            printf("(warning) unsupported ISA bridge %04X:%04X for bus %d\n",
                   pcidev[found_unknown_bridge_idx].pd_vid,
                   pcidev[found_unknown_bridge_idx].pd_did,
                   busind);
        }
        return 0;
    }

    if (debug)
    {
        printf("(warning) no ISA bridge found on bus %d\n", busind);
    }
    return 0;
}

static int
derive_irq(struct pcidev * dev, int pin)
{
    static const int PCI_FUNC_SLOT_SHIFT = 3;
    static const int PCI_SLOT_MASK = 0x1f;
    static const int PCI_IRQ_PINS_COUNT = 4;

    if (dev == NULL) {
        return -1;
    }

    int bus_idx = get_busind(dev->pd_busnr);
    if (bus_idx < 0) {
        return -1;
    }

    int parent_dev_idx = pcibus[bus_idx].pb_devind;
    if (parent_dev_idx < 0) {
        return -1;
    }

    struct pcidev * parent_bridge = &pcidev[parent_dev_idx];

    int slot = ((dev->pd_func) >> PCI_FUNC_SLOT_SHIFT) & PCI_SLOT_MASK;

    int mangled_pin = (pin + slot) % PCI_IRQ_PINS_COUNT;

    return acpi_get_irq(parent_bridge->pd_busnr,
                        parent_bridge->pd_dev,
                        mangled_pin);
}

static inline void print_pci_dev_location(int busnr, int dev, int func) {
    printf("%d.%d.%d", busnr, dev, func);
}

static void
record_irq(int devind)
{
    int ilr, ipr;
    struct pci_dev_info *dev_ptr = &pcidev[devind];

    ilr = __pci_attr_r8(devind, PCI_ILR);
    ipr = __pci_attr_r8(devind, PCI_IPR);

    char int_pin_char = '\0';
    if (ipr != 0) {
        int_pin_char = 'A' + ipr - 1;
    }

    if (ipr != 0 && machine.apic_enabled) {
        int irq_candidate = acpi_get_irq(dev_ptr->pd_busnr, dev_ptr->pd_dev, ipr - 1);

        if (irq_candidate < 0) {
            irq_candidate = derive_irq(dev_ptr, ipr - 1);
        }

        if (irq_candidate >= 0) {
            ilr = irq_candidate;
            __pci_attr_w8(devind, PCI_ILR, ilr);
            if (debug) {
                printf("PCI: ACPI/Derived IRQ %d for device ", ilr);
                print_pci_dev_location(dev_ptr->pd_busnr, dev_ptr->pd_dev, dev_ptr->pd_func);
                printf(" INT%c\n", int_pin_char);
            }
        } else if (debug) {
            printf("PCI: no ACPI/derived IRQ routing for device ");
            print_pci_dev_location(dev_ptr->pd_busnr, dev_ptr->pd_dev, dev_ptr->pd_func);
            printf(" INT%c\n", int_pin_char);
        }
    }

    if (ilr == 0) {
        static int first_irq0_warning = 1;
        if (ipr != 0 && first_irq0_warning && debug) {
            first_irq0_warning = 0;
            printf("PCI: strange, BIOS assigned IRQ0 (for device ");
            print_pci_dev_location(dev_ptr->pd_busnr, dev_ptr->pd_dev, dev_ptr->pd_func);
            printf(" INT%c)\n", int_pin_char);
        }
        ilr = PCI_ILR_UNKNOWN;
    }

    dev_ptr->pd_ilr = ilr;

    if (ilr == PCI_ILR_UNKNOWN && ipr != 0) {
        int busnr = dev_ptr->pd_busnr;
        int busind = get_busind(busnr);

        if (busind >= 0 && pcibus[busind].pb_type == PBT_CARDBUS) {
            int cb_devind = pcibus[busind].pb_devind;
            int cardbus_bridge_ilr = pcidev[cb_devind].pd_ilr;

            if (cardbus_bridge_ilr != PCI_ILR_UNKNOWN) {
                if (debug) {
                    printf("PCI: Assigning IRQ %d to Cardbus device ", cardbus_bridge_ilr);
                    print_pci_dev_location(dev_ptr->pd_busnr, dev_ptr->pd_dev, dev_ptr->pd_func);
                    printf(" INT%c (from bridge)\n", int_pin_char);
                }
                __pci_attr_w8(devind, PCI_ILR, cardbus_bridge_ilr);
                dev_ptr->pd_ilr = cardbus_bridge_ilr;
                return;
            }
        }
    }

    if (ilr != PCI_ILR_UNKNOWN && ipr != 0) {
        if (debug) {
            printf("\tIRQ %d for INT%c (device ", ilr, int_pin_char);
            print_pci_dev_location(dev_ptr->pd_busnr, dev_ptr->pd_dev, dev_ptr->pd_func);
            printf(")\n");
        }
    } else if (ilr != PCI_ILR_UNKNOWN && ipr == 0) {
        printf("PCI: IRQ %d is assigned, but device ", ilr);
        print_pci_dev_location(dev_ptr->pd_busnr, dev_ptr->pd_dev, dev_ptr->pd_func);
        printf(" does not need it\n");
    } else if (ilr == PCI_ILR_UNKNOWN && ipr != 0) {
        if (debug) {
            printf("PCI: device ");
            print_pci_dev_location(dev_ptr->pd_busnr, dev_ptr->pd_dev, dev_ptr->pd_func);
            printf(" uses INT%c but is not assigned any IRQ\n", int_pin_char);
        }
    }
}

/*===========================================================================*
 *				BAR helpers				     *
 *===========================================================================*/
static u32_t pci_probe_bar_size(int devind, int reg, u32_t original_bar_val, u16_t cr_disable_mask)
{
	u16_t cmd;
	u32_t probed_size;

	cmd = __pci_attr_r16(devind, PCI_CR);
	__pci_attr_w16(devind, PCI_CR, cmd & ~cr_disable_mask);
	__pci_attr_w32(devind, reg, 0xFFFFFFFF);
	probed_size = __pci_attr_r32(devind, reg);
	__pci_attr_w32(devind, reg, original_bar_val);
	__pci_attr_w16(devind, PCI_CR, cmd);

	return probed_size;
}

static int
record_bar(int devind, int bar_nr, int last)
{
	int reg = PCI_BAR + 4 * bar_nr;
	u32_t bar_val = __pci_attr_r32(devind, reg);
	
	u32_t bar_base_addr = 0;
	u32_t bar_size = 0;
	u32_t bar_flags = 0;
	int width = 1; // Default to 32-bit BAR (1 DWORD)
	int record_this_bar = 1; // Flag to indicate if BAR should be recorded in pcidev[]

	if (bar_val & PCI_BAR_IO)
	{
		// I/O BAR
		u32_t probed_val = pci_probe_bar_size(devind, reg, bar_val, PCI_CR_IO_EN);

		bar_base_addr = bar_val & PCI_BAR_IO_MASK;
		bar_size = (~probed_val & PCI_BAR_IO_MASK) + 1;
		bar_flags = PBF_IO;

		if (debug)
		{
			printf("\tbar_%d: %d bytes at 0x%x I/O\n", bar_nr, bar_size, bar_base_addr);
		}
	}
	else
	{
		// Memory BAR
		u32_t type = (bar_val & PCI_BAR_TYPE);

		switch (type)
		{
			case PCI_TYPE_32:
			case PCI_TYPE_32_1M:
				// 32-bit Memory BAR, width is 1 (default)
				break;

			case PCI_TYPE_64:
				// 64-bit Memory BAR occupies two consecutive DWORDs
				width = 2; 
				if (last)
				{
					printf("PCI: device %d.%d.%d BAR %d extends beyond designated area\n",
						pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
						pcidev[devind].pd_func, bar_nr);
					record_this_bar = 0; // Invalid configuration, skip recording
				}
				else
				{
					u32_t upper_bar_val = __pci_attr_r32(devind, reg + 4);
					if (upper_bar_val != 0)
					{
						if (debug)
						{
							printf("\tbar_%d: (64-bit BAR with high bits set, inaccessible)\n", bar_nr);
						}
						record_this_bar = 0; // Inaccessible memory, skip recording
					}
				}
				break;

			default:
				// Unknown BAR type, ignore
				if (debug)
				{
					printf("\tbar_%d: (unknown type %x, ignoring)\n", bar_nr, type);
				}
				record_this_bar = 0; // Skip recording
				break;
		}

		if (record_this_bar)
		{
			u32_t probed_val = pci_probe_bar_size(devind, reg, bar_val, PCI_CR_MEM_EN);

			if (probed_val == 0)
			{
				if (debug)
				{
					printf("\tbar_%d: (memory BAR not implemented, ignoring)\n", bar_nr);
				}
				record_this_bar = 0; // Register is not implemented
			}
			else
			{
				u32_t prefetch = !!(bar_val & PCI_BAR_PREFETCH);
				bar_base_addr = bar_val & PCI_BAR_MEM_MASK;
				bar_size = (~probed_val) + 1;

				if (prefetch)
				{
					bar_flags |= PBF_PREFETCH;
				}

				if (debug)
				{
					printf("\tbar_%d: 0x%x bytes at 0x%x%s memory%s\n",
						bar_nr, bar_size, bar_base_addr,
						prefetch ? " prefetchable" : "",
						type == PCI_TYPE_64 ? ", 64-bit" : "");
				}
			}
		}
	}

	// Record BAR information if it was deemed valid and usable
	if (record_this_bar)
	{
		int dev_bar_nr = pcidev[devind].pd_bar_nr;
		if (dev_bar_nr < PCI_MAX_BARS)
		{
			pcidev[devind].pd_bar[dev_bar_nr].pb_flags = bar_flags;
			pcidev[devind].pd_bar[dev_bar_nr].pb_base = bar_base_addr;
			pcidev[devind].pd_bar[dev_bar_nr].pb_size = bar_size;
			pcidev[devind].pd_bar[dev_bar_nr].pb_nr = bar_nr;
			if (bar_base_addr == 0)
			{
				pcidev[devind].pd_bar[dev_bar_nr].pb_flags |= PBF_INCOMPLETE;
			}
			pcidev[devind].pd_bar_nr++; // Increment only after successful storage
		}
		else
		{
			// Log an error if the device has more BARs than we can store
			printf("PCI: device %d.%d.%d has too many BARs (max %d), ignoring BAR %d\n",
				pcidev[devind].pd_busnr, pcidev[devind].pd_dev,
				pcidev[devind].pd_func, PCI_MAX_BARS, bar_nr);
		}
	}

	return width;
}

#define PCI_REG_SIZE_BYTES 4

static void
record_bars(int devind, int last_reg)
{
    int bar_offset_in_dwords = 0;
    int current_reg_addr = PCI_BAR;

    while (current_reg_addr <= last_reg)
    {
        int bar_width_in_dwords;

        bar_width_in_dwords = record_bar(devind, bar_offset_in_dwords, current_reg_addr == last_reg);

        if (bar_width_in_dwords <= 0)
        {
            break;
        }

        if (bar_width_in_dwords > 2)
        {
            break;
        }

        current_reg_addr += bar_width_in_dwords * PCI_REG_SIZE_BYTES;
        bar_offset_in_dwords += bar_width_in_dwords;
    }
}

static void
record_bars_normal(int devind)
{
	record_bars(devind, PCI_BAR_6);

	if (pcidev[devind].pd_baseclass != PCI_BCR_MASS_STORAGE ||
	    pcidev[devind].pd_subclass != PCI_MS_IDE)
	{
		return;
	}

	int clear_primary_channel_bars = 0;
	int clear_secondary_channel_bars = 0;

	if (!(pcidev[devind].pd_infclass & PCI_IDE_PRI_NATIVE))
	{
		if (debug)
		{
			printf("Primary IDE channel not in native mode, clearing BARs 0 and 1\n");
		}
		clear_primary_channel_bars = 1;
	}

	if (!(pcidev[devind].pd_infclass & PCI_IDE_SEC_NATIVE))
	{
		if (debug)
		{
			printf("Secondary IDE channel not in native mode, clearing BARs 2 and 3\n");
		}
		clear_secondary_channel_bars = 1;
	}

	int current_bar_idx = 0;
	for (int i = 0; i < pcidev[devind].pd_bar_nr; i++)
	{
		int pb_nr = pcidev[devind].pd_bar[i].pb_nr;
		int skip_bar = 0;

		if ((pb_nr == 0 || pb_nr == 1) && clear_primary_channel_bars)
		{
			skip_bar = 1;
		}
		else if ((pb_nr == 2 || pb_nr == 3) && clear_secondary_channel_bars)
		{
			skip_bar = 1;
		}

		if (skip_bar)
		{
			if (debug)
			{
				printf("Skipping BAR %d for device %d\n", pb_nr, devind);
			}
		}
		else
		{
			pcidev[devind].pd_bar[current_bar_idx] = pcidev[devind].pd_bar[i];
			current_bar_idx++;
		}
	}
	pcidev[devind].pd_bar_nr = current_bar_idx;
}

static void
record_bars_bridge(int devind)
{
    u32_t base, limit, size;

    record_bars(devind, PCI_BAR_2);

    u32_t io_base_reg_low = __pci_attr_r8(devind, PPB_IOBASE);
    u32_t io_base_reg_high = __pci_attr_r16(devind, PPB_IOBASEU16);
    u32_t io_limit_reg_low = __pci_attr_r8(devind, PPB_IOLIMIT);
    u32_t io_limit_reg_high = __pci_attr_r16(devind, PPB_IOLIMITU16);

    base = (io_base_reg_high << 16) | ((io_base_reg_low & PPB_IOB_MASK) << 8);
    limit = (io_limit_reg_high << 16) | ((io_limit_reg_low & PPB_IOL_MASK) << 8) | 0xFFF;
    size = limit - base + 1;
    if (debug)
    {
        printf("\tI/O window: base 0x%x, limit 0x%x, size 0x%x\n", base, limit, size);
    }

    u32_t mem_base_reg = __pci_attr_r16(devind, PPB_MEMBASE);
    u32_t mem_limit_reg = __pci_attr_r16(devind, PPB_MEMLIMIT);

    base = (mem_base_reg & PPB_MEMB_MASK) << 16;
    limit = ((mem_limit_reg & PPB_MEML_MASK) << 16) | 0xFFFF;
    size = limit - base + 1;
    if (debug)
    {
        printf("\tMemory window: base 0x%x, limit 0x%x, size 0x%x\n", base, limit, size);
    }

    u32_t pfm_base_reg = __pci_attr_r16(devind, PPB_PFMEMBASE);
    u32_t pfm_limit_reg = __pci_attr_r16(devind, PPB_PFMEMLIMIT);

    base = (pfm_base_reg & PPB_PFMEMB_MASK) << 16;
    limit = ((pfm_limit_reg & PPB_PFMEML_MASK) << 16) | 0xFFFF;
    size = limit - base + 1;
    if (debug)
    {
        printf("\tPrefetchable memory window: base 0x%x, limit 0x%x, size 0x%x\n", base, limit, size);
    }
}

static void
record_window_info(int devind, u32_t base_reg_offset, u32_t limit_reg_offset, u32_t limit_mask, const char *window_description)
{
	u32_t base = __pci_attr_r32(devind, base_reg_offset);
	u32_t limit = __pci_attr_r32(devind, limit_reg_offset) |
		(~limit_mask & 0xffffffff);
	u32_t size = limit - base + 1;

	if (debug)
	{
		printf("\t%s: base 0x%x, limit 0x%x, size %d\n",
			window_description, base, limit, size);
	}
}

static void
record_bars_cardbus(int devind)
{
	record_bars(devind, PCI_BAR);

	record_window_info(devind, CBB_MEMBASE_0, CBB_MEMLIMIT_0, CBB_MEML_MASK, "Memory window 0");
	record_window_info(devind, CBB_MEMBASE_1, CBB_MEMLIMIT_1, CBB_MEML_MASK, "Memory window 1");
	record_window_info(devind, CBB_IOBASE_0, CBB_IOLIMIT_0, CBB_IOL_MASK, "I/O window 0");
	record_window_info(devind, CBB_IOBASE_1, CBB_IOLIMIT_1, CBB_IOL_MASK, "I/O window 1");
}

static void print_debug_gap(const char *prefix, u32_t low, u32_t high);
static void update_resource_gap(u32_t *gap_low_ptr, u32_t *gap_high_ptr, u32_t bar_base, u32_t bar_size);
static void allocate_pci_bar_resource(int dev_idx, int bar_idx, u32_t *gap_low_ptr, u32_t *gap_high_ptr, bool is_io);

#define MEM_GAP_INITIAL_HIGH 0xfe000000UL
#define IO_GAP_INITIAL_HIGH  0x10000UL
#define IO_GAP_INITIAL_LOW   0x400UL
#define PCI_BAR_REGISTER_SIZE 4
#define PCI_IO_ISA_COMPAT_MASK 0xfcffUL

static void print_debug_gap(const char *prefix, u32_t low, u32_t high)
{
	if (debug)
	{
		printf("%s: [0x%x .. 0x%x>\n", prefix, low, high);
	}
}

static void update_resource_gap(u32_t *gap_low_ptr, u32_t *gap_high_ptr, u32_t bar_base, u32_t bar_size)
{
	u32_t current_low = *gap_low_ptr;
	u32_t current_high = *gap_high_ptr;
	u32_t bar_end = bar_base + bar_size;

	if (bar_end <= current_low || bar_base >= current_high)
	{
		return;
	}

	u32_t overlap_low_extent = bar_end - current_low;
	u32_t overlap_high_extent = current_high - bar_base;

	if (overlap_low_extent < overlap_high_extent)
	{
		*gap_low_ptr = bar_end;
	}
	else
	{
		*gap_high_ptr = bar_base;
	}
}

static void allocate_pci_bar_resource(int dev_idx, int bar_idx, u32_t *gap_low_ptr, u32_t *gap_high_ptr, bool is_io)
{
	pci_bar_t *bar = &pcidev[dev_idx].pd_bar[bar_idx];
	u32_t size = bar->pb_size;
	u32_t base;
	int bar_reg_nr = bar->pb_nr;
	u32_t current_bar_reg_value;
	int reg_offset = PCI_BAR + PCI_BAR_REGISTER_SIZE * bar_reg_nr;

	if (!is_io && size < PAGE_SIZE)
	{
		size = PAGE_SIZE;
	}

	base = *gap_high_ptr - size;
	base &= ~(size - 1);

	if (is_io)
	{
		base &= PCI_IO_ISA_COMPAT_MASK;

		if (base < *gap_low_ptr)
		{
			printf("PCI: I/O base 0x%x for device %u (BAR %u) too low, below gap low 0x%x\n",
				base, dev_idx, bar_idx, *gap_low_ptr);
		}
	}
	else
	{
		if (base < *gap_low_ptr)
		{
			panic("PCI: Memory BAR allocation failure: base 0x%x for device %u (BAR %u) below gap low 0x%x",
				base, dev_idx, bar_idx, *gap_low_ptr);
		}
	}

	*gap_high_ptr = base;

	current_bar_reg_value = __pci_attr_r32(dev_idx, reg_offset);
	__pci_attr_w32(dev_idx, reg_offset, current_bar_reg_value | base);

	if (debug)
	{
		printf("complete_bars: allocated 0x%x size %u to %u.%u.%u, bar_%d (%s)\n",
			base, size, pcidev[dev_idx].pd_busnr,
			pcidev[dev_idx].pd_dev, pcidev[dev_idx].pd_func,
			bar_reg_nr, is_io ? "I/O" : "Memory");
	}

	bar->pb_base = base;
	bar->pb_flags &= ~PBF_INCOMPLETE;
}


static void
complete_bars(void)
{
	kinfo_t kinfo;

	if (OK != sys_getkinfo(&kinfo))
	{
		panic("can't get kinfo");
	}

	u32_t memgap_low = kinfo.mem_high_phys;
	u32_t memgap_high = MEM_GAP_INITIAL_HIGH;

	print_debug_gap("complete_bars: initial mem gap", memgap_low, memgap_high);

	for (int i = 0; i < nr_pcidev; i++)
	{
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++)
		{
			pci_bar_t *bar = &pcidev[i].pd_bar[j];
			if ((bar->pb_flags & PBF_IO) || (bar->pb_flags & PBF_INCOMPLETE))
			{
				continue;
			}
			update_resource_gap(&memgap_low, &memgap_high, bar->pb_base, bar->pb_size);
		}
	}

	print_debug_gap("complete_bars: intermediate mem gap", memgap_low, memgap_high);

	if (memgap_high < memgap_low)
	{
		printf("PCI: bad memory gap: [0x%x .. 0x%x>\n", memgap_low, memgap_high);
		panic("memory gap inversion detected after processing existing BARs");
	}

	u32_t iogap_low = IO_GAP_INITIAL_LOW;
	u32_t iogap_high = IO_GAP_INITIAL_HIGH;

	for (int i = 0; i < nr_pcidev; i++)
	{
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++)
		{
			pci_bar_t *bar = &pcidev[i].pd_bar[j];
			if (!(bar->pb_flags & PBF_IO) || (bar->pb_flags & PBF_INCOMPLETE))
			{
				continue;
			}
			update_resource_gap(&iogap_low, &iogap_high, bar->pb_base, bar->pb_size);
		}
	}

	if (iogap_high < iogap_low)
	{
		panic("I/O gap inversion detected after processing existing BARs: [0x%x .. 0x%x>", iogap_low, iogap_high);
	}
	print_debug_gap("complete_bars: I/O range", iogap_low, iogap_high);

	for (int i = 0; i < nr_pcidev; i++)
	{
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++)
		{
			pci_bar_t *bar = &pcidev[i].pd_bar[j];
			if ((bar->pb_flags & PBF_IO) || !(bar->pb_flags & PBF_INCOMPLETE))
			{
				continue;
			}
			allocate_pci_bar_resource(i, j, &memgap_low, &memgap_high, false);
		}
	}

	for (int i = 0; i < nr_pcidev; i++)
	{
		u32_t io_high_before_device_bars = iogap_high;

		for (int j = 0; j < pcidev[i].pd_bar_nr; j++)
		{
			pci_bar_t *bar = &pcidev[i].pd_bar[j];
			if (!(bar->pb_flags & PBF_IO) || !(bar->pb_flags & PBF_INCOMPLETE))
			{
				continue;
			}
			allocate_pci_bar_resource(i, j, &iogap_low, &iogap_high, true);
		}

		if (iogap_high != io_high_before_device_bars)
		{
			update_bridge4dev_io(i, iogap_high, io_high_before_device_bars - iogap_high);
		}
	}

	for (int i = 0; i < nr_pcidev; i++)
	{
		for (int j = 0; j < pcidev[i].pd_bar_nr; j++)
		{
			if (pcidev[i].pd_bar[j].pb_flags & PBF_INCOMPLETE)
			{
				printf("PCI: Should allocate resources for device %u (BAR %u), but failed.\n", i, j);
			}
		}
	}
	return;
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
	int busnr;
	const char *s, *dstr;
	_Bool is_multifunction_device_at_dev_slot = 0;
	static int warned_qemu_sts = 0;

	if (debug)
		printf("probe_bus(%d)\n", busind);

	busnr = pcibus[busind].pb_busnr;

	for (dev = 0; dev < 32; dev++)
	{
		is_multifunction_device_at_dev_slot = 0; // Reset for each new device slot

		for (func = 0; func < 8; func++)
		{
			// The index for the device we are currently trying to probe/add.
			// This will only be committed (and nr_pcidev incremented) if valid.
			int current_device_idx = nr_pcidev;

			// Check for available space before attempting to probe/add a new device.
			if (current_device_idx >= NR_PCIDEV) {
				panic("Too many PCI devices: %d (max %d), cannot add more.", nr_pcidev, NR_PCIDEV);
			}

			// Set the bus, device, and function for the current slot in the global pcidev array.
			// This is necessary as __pci_attr_rXX functions likely use these fields
			// to form the PCI configuration space address.
			pcidev[current_device_idx].pd_busnr = busnr;
			pcidev[current_device_idx].pd_dev = dev;
			pcidev[current_device_idx].pd_func = func;

			// Clear status bits before reading to detect new errors.
			pci_attr_wsts(current_device_idx, PSR_SSE | PSR_RMAS | PSR_RTAS);

			vid = __pci_attr_r16(current_device_idx, PCI_VID);
			did = __pci_attr_r16(current_device_idx, PCI_DID);
			headt = __pci_attr_r8(current_device_idx, PCI_HEADT);
			sts = pci_attr_rsts(current_device_idx);

			// If Vendor ID and Device ID are both NO_VID, the device does not exist.
			if (vid == NO_VID && did == NO_VID)
			{
				if (func == 0)
					break; /* No device at this bus/dev slot, stop probing functions. */
				// For func > 0, we continue to check other functions in case of a sparse multifunction device.
				continue;
			}

			// If func 0, determine if the device slot is for a multifunction device.
			if (func == 0) {
				is_multifunction_device_at_dev_slot = (headt & PHT_MULTIFUNC);
			}

			// Warn about specific status bits if set (often seen with QEMU).
			if (sts & (PSR_SSE | PSR_RMAS | PSR_RTAS))
			{
				if (!warned_qemu_sts) {
					printf("PCI: ignoring bad value 0x%x in sts for QEMU (first warning)\n",
						sts & (PSR_SSE | PSR_RMAS | PSR_RTAS));
					warned_qemu_sts = 1;
				}
			}

			// Read subsystem IDs.
			sub_vid = __pci_attr_r16(current_device_idx, PCI_SUBVID);
			sub_did = __pci_attr_r16(current_device_idx, PCI_SUBDID);

			// Debug print device names.
			dstr = _pci_dev_name(vid, did);
			if (debug)
			{
				if (dstr)
				{
					printf("%d.%lu.%lu: %s (%04X:%04X)\n",
						busnr, (unsigned long)dev, (unsigned long)func, dstr, vid, did);
				}
				else
				{
					printf("%d.%lu.%lu: Unknown device, vendor %04X (%s), device %04X\n",
						busnr, (unsigned long)dev, (unsigned long)func, vid, pci_vid_name(vid), did);
				}
				printf("Device index: %d\n", current_device_idx);
				printf("Subsystem: Vid 0x%x, did 0x%x\n", sub_vid, sub_did);
			}

			// Read class codes.
			baseclass = __pci_attr_r8(current_device_idx, PCI_BCR);
			subclass = __pci_attr_r8(current_device_idx, PCI_SCR);
			infclass = __pci_attr_r8(current_device_idx, PCI_PIFR);

			// Determine class name, falling back from subclass to baseclass to unknown.
			s = pci_subclass_name((uint32_t)baseclass << 24 | (uint32_t)subclass << 16);
			if (!s) {
				s = pci_baseclass_name((uint32_t)baseclass << 24);
			}
			if (!s) {
				s = "(unknown class)";
			}

			if (debug)
			{
				printf("\tclass %s (%X/%X/%X)\n", s, baseclass, subclass, infclass);
			}

			// Check if this device (bus, dev, func) is a duplicate.
			if (is_duplicate(busnr, dev, func))
			{
				if (debug) printf("\tduplicate!\n");
				// If func 0 and not a multifunction device, we are done with this device slot.
				if (func == 0 && !is_multifunction_device_at_dev_slot)
					break;
				continue; // Skip this duplicate and check the next function.
			}

			// A valid, non-duplicate device has been found. Populate its details.
			pcidev[current_device_idx].pd_baseclass = baseclass;
			pcidev[current_device_idx].pd_subclass = subclass;
			pcidev[current_device_idx].pd_infclass = infclass;
			pcidev[current_device_idx].pd_vid = vid;
			pcidev[current_device_idx].pd_did = did;
			pcidev[current_device_idx].pd_sub_vid = sub_vid;
			pcidev[current_device_idx].pd_sub_did = sub_did;
			pcidev[current_device_idx].pd_inuse = 0;
			pcidev[current_device_idx].pd_bar_nr = 0; // Initialize BAR count

			record_irq(current_device_idx);

			// Process BARs based on header type.
			switch (headt & PHT_MASK)
			{
			case PHT_NORMAL:
				record_bars_normal(current_device_idx);
				break;
			case PHT_BRIDGE:
				record_bars_bridge(current_device_idx);
				break;
			case PHT_CARDBUS:
				record_bars_cardbus(current_device_idx);
				break;
			default:
				printf("\t%d.%d.%d: unknown header type %d\n",
					busnr, dev, func, headt & PHT_MASK);
				break;
			}

			if (debug)
				print_capabilities(current_device_idx);

			// Commit the device by incrementing the global count.
			// This makes current_device_idx available for the next device.
			nr_pcidev++;

			// If it was function 0 and the header indicates it's not a multifunction device,
			// there are no more functions to probe for this device slot.
			if (func == 0 && !is_multifunction_device_at_dev_slot)
				break;
		}
	}
}


static u16_t
pcibr_std_rsts(int busind)
{
    // Validate bus index to prevent out-of-bounds access.
    // PCI_MAX_BUSES_COUNT is assumed to be a globally defined constant
    // (e.g., a macro or a const size_t variable) representing the maximum
    // valid index for the 'pcibus' array.
    if (busind < 0 || busind >= PCI_MAX_BUSES_COUNT) {
        // Return a sentinel value (0xFFFF, all bits set) to indicate an error.
        // This is a common pattern for u16_t return types when an error occurs
        // and a valid status value is unlikely to be all ones.
        return (u16_t)0xFFFF;
    }

    // Safely retrieve the device index from the global 'pcibus' array.
    // 'pcibus' is assumed to be a globally accessible array of structures,
    // and 'pb_devind' is a member of that structure.
    int devind = pcibus[busind].pb_devind;

    // Read the 16-bit PCI attribute using the device index and the
    // PCI Primary Bus Status register offset (PPB_SSTS).
    // The function '__pci_attr_r16' is assumed to handle its own internal
    // error conditions, potentially returning 0xFFFF or logging if it fails
    // to read the attribute.
    return __pci_attr_r16(devind, PPB_SSTS);
}

static int
pcibr_std_wsts(int busind, u16_t value)
{
	if (busind < 0 || busind >= PCI_BUS_COUNT) {
		return -1;
	}

	int devind = pcibus[busind].pb_devind;
	__pci_attr_w16(devind, PPB_SSTS, value);

	return 0;
}

static u16_t
pcibr_cb_rsts(int busind)
{
	return __pci_attr_r16(pcibus[busind].pb_devind, CBB_SSTS);
}

static void
pcibr_cb_wsts(int busind, u16_t value)
{
    if (busind < 0 || busind > PCI_MAX_BUS_INDEX) {
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
	int i, freebus, devind, prim_busnr;

	for (i = 0; i < nr_pcibus; i++)
	{
		if (!pcibus[i].pb_needinit)
			continue;

		freebus = get_freebus();

		devind = pcibus[i].pb_devind;
		prim_busnr = pcidev[devind].pd_busnr;

		if (prim_busnr != 0)
		{
			fprintf(stderr, "WARNING: complete_bridge: Device %d (bus index %d) has primary bus number %d. Full handling for updating primary/subordinate bus numbers not fully implemented for this scenario.\n",
			        devind, i, prim_busnr);
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
	int devind;
	int busnr = pcibus[busind].pb_busnr;
	u16_t vid, did;
	u8_t headt, sbusn, baseclass, subclass, infclass;
	u32_t t3;
	int type;
	int new_bus_idx;

	for (devind = 0; devind < nr_pcidev; devind++)
	{
		if (pcidev[devind].pd_busnr != busnr)
		{
			continue;
		}

		vid = pcidev[devind].pd_vid;
		did = pcidev[devind].pd_did;
		
		headt = __pci_attr_r8(devind, PCI_HEADT);
		type = 0; // Initialize type for clear state

		if ((headt & PHT_MASK) == PHT_BRIDGE)
			type = PCI_PPB_STD;
		else if ((headt & PHT_MASK) == PHT_CARDBUS)
			type = PCI_PPB_CB;
		else
		{
			continue;	/* Not a bridge */
		}

		baseclass = __pci_attr_r8(devind, PCI_BCR);
		subclass = __pci_attr_r8(devind, PCI_SCR);
		infclass = __pci_attr_r8(devind, PCI_PIFR);
		t3 = ((u32_t)baseclass << 16) | ((u32_t)subclass << 8) | infclass;

		if (type == PCI_PPB_STD)
		{
			if (t3 != PCI_T3_PCI2PCI && t3 != PCI_T3_PCI2PCI_SUBTR)
			{
				printf(
"Unknown PCI class %02x/%02x/%02x for PCI-to-PCI bridge, device %04X:%04X\n",
					baseclass, subclass, infclass,
					vid, did);
				continue;
			}
		}
		else if (type == PCI_PPB_CB)
		{
			if (t3 != PCI_T3_CARDBUS)
			{
				printf(
"Unknown PCI class %02x/%02x/%02x for Cardbus bridge, device %04X:%04X\n",
					baseclass, subclass, infclass,
					vid, did);
				continue;
			}
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
			printf("Secondary bus number not initialized for device %04X:%04X at %u.%u.%u\n",
				vid, did, pcidev[devind].pd_busnr, pcidev[devind].pd_dev, pcidev[devind].pd_func);
			continue;
		}

		if (nr_pcibus >= NR_PCIBUS)
		{
			panic("too many PCI busses: %d, limit %d", nr_pcibus, NR_PCIBUS);
		}
		
		new_bus_idx = nr_pcibus;
		nr_pcibus++;

		pcibus[new_bus_idx].pb_type = PBT_PCIBRIDGE;
		pcibus[new_bus_idx].pb_needinit = 0;
		pcibus[new_bus_idx].pb_isabridge_dev = -1;
		pcibus[new_bus_idx].pb_isabridge_type = 0;
		pcibus[new_bus_idx].pb_devind = devind;
		pcibus[new_bus_idx].pb_busnr = sbusn;
		pcibus[new_bus_idx].pb_rreg8 = pcibus[busind].pb_rreg8;
		pcibus[new_bus_idx].pb_rreg16 = pcibus[busind].pb_rreg16;
		pcibus[new_bus_idx].pb_rreg32 = pcibus[busind].pb_rreg32;
		pcibus[new_bus_idx].pb_wreg8 = pcibus[busind].pb_wreg8;
		pcibus[new_bus_idx].pb_wreg16 = pcibus[busind].pb_wreg16;
		pcibus[new_bus_idx].pb_wreg32 = pcibus[busind].pb_wreg32;
		
		switch(type)
		{
		case PCI_PPB_STD:
			pcibus[new_bus_idx].pb_rsts = pcibr_std_rsts;
			pcibus[new_bus_idx].pb_wsts = pcibr_std_wsts;
			break;
		case PCI_PPB_CB:
			pcibus[new_bus_idx].pb_type = PBT_CARDBUS;
			pcibus[new_bus_idx].pb_rsts = pcibr_cb_rsts;
			pcibus[new_bus_idx].pb_wsts = pcibr_cb_wsts;
			break;
		default:
			// Based on prior logic, 'type' should only be PCI_PPB_STD or PCI_PPB_CB.
			// Any other value indicates an internal logic error.
			panic("unknown or unreachable PCI-PCI bridge type: %d", type);
		}

		if (machine.apic_enabled)
			acpi_map_bridge(pcidev[devind].pd_busnr,
					pcidev[devind].pd_dev, sbusn);

		if (debug)
		{
			printf(
			"bus(table) = %d, bus(sec) = %d, bus(subord) = %d\n",
				new_bus_idx, sbusn, __pci_attr_r8(devind, PPB_SUBORDBN));
		}
		
		probe_bus(new_bus_idx);

		do_pcibridge(new_bus_idx);
	}
}

/*===========================================================================*
 *				pci_intel_init				     *
 *===========================================================================*/
static void init_pci_bus_struct(int bus_index) {
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
pci_intel_init(void)
{
	const u32_t bus = 0;
	const u32_t dev = 0;
	const u32_t func = 0;
	u16_t vid, did;
	int sys_outl_ret;
	int bus_index;
	const char *device_name;
	int isa_bridge_result;

	vid = PCII_RREG16_(bus, dev, func, PCI_VID);
	did = PCII_RREG16_(bus, dev, func, PCI_DID);

	if (OK != (sys_outl_ret = sys_outl(PCII_CONFADD, PCII_UNSEL))) {
		printf("PCI: warning, sys_outl failed: %d\n", sys_outl_ret);
	}

	if (nr_pcibus >= NR_PCIBUS) {
		panic("too many PCI busses: %d", nr_pcibus);
	}

	bus_index = nr_pcibus;
	nr_pcibus++;

	init_pci_bus_struct(bus_index);

	device_name = _pci_dev_name(vid, did);
	if (!device_name) {
		device_name = "unknown device";
	}

	if (debug) {
		printf("pci_intel_init: %s (%04X:%04X)\n",
			device_name, vid, did);
	}

	probe_bus(bus_index);

	isa_bridge_result = do_isabridge(bus_index);
	if (isa_bridge_result != OK) {
		const int current_bus_number = pcibus[bus_index].pb_busnr;

		for (int device_idx = 0; device_idx < nr_pcidev; device_idx++) {
			if (pcidev[device_idx].pd_busnr == current_bus_number) {
				pcidev[device_idx].pd_inuse = 1;
			}
		}
		return;
	}

	do_pcibridge(bus_index);

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
visible(const struct rs_pci *aclp, int devind)
{
	if (aclp == NULL) {
		return 1;
	}

	const struct pci_device *current_pci_dev = &pcidev[devind];

	for (int i = 0; i < aclp->rsp_nr_device; i++) {
		const struct rs_device_acl *acl_dev = &aclp->rsp_device[i];

		if (acl_dev->vid == current_pci_dev->pd_vid &&
		    acl_dev->did == current_pci_dev->pd_did) {
			int sub_vid_match = (acl_dev->sub_vid == NO_SUB_VID ||
			                       acl_dev->sub_vid == current_pci_dev->pd_sub_vid);

			int sub_did_match = (acl_dev->sub_did == NO_SUB_DID ||
			                       acl_dev->sub_did == current_pci_dev->pd_sub_did);

			if (sub_vid_match && sub_did_match) {
				return 1;
			}
		}
	}

	if (aclp->rsp_nr_class == 0) {
		return 0;
	}

	u32_t class_id = ((u32_t)current_pci_dev->pd_baseclass << 16) |
	                 ((u32_t)current_pci_dev->pd_subclass << 8) |
	                 current_pci_dev->pd_infclass;

	for (int i = 0; i < aclp->rsp_nr_class; i++) {
		const struct rs_class_acl *acl_class = &aclp->rsp_class[i];

		if (acl_class->pciclass == (class_id & acl_class->mask)) {
			return 1;
		}
	}

	return 0;
}

/*===========================================================================*
 *				sef_cb_init_fresh			     *
 *===========================================================================*/
int
sef_cb_init(int type, sef_init_info_t *info)
{
	int r;
	struct rprocpub rprocpub[NR_BOOT_PROCS];
	int do_announce_driver;

	long pci_debug_val = 0;
	env_parse("pci_debug", "d", 0, &pci_debug_val, 0, 1);
	debug = pci_debug_val;

	if (sys_getmachine(&machine) != OK) {
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

	do_announce_driver = FALSE;
	switch (type) {
	case SEF_INIT_FRESH:
	case SEF_INIT_RESTART:
		do_announce_driver = TRUE;
		break;
	case SEF_INIT_LU:
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

    if (rpub == NULL) {
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
	if (devindp == NULL) {
		return -1;
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
	int devind;

	if (devindp == (void *)0 || vidp == (void *)0 || didp == (void *)0)
	{
		return 0;
	}

	for (devind = 0; devind < nr_pcidev; devind++)
	{
		if (!visible(aclp, devind))
			continue;
		break;
	}

	if (devind >= nr_pcidev)
	{
		return 0;
	}

	*devindp = devind;
	*vidp = pcidev[devind].pd_vid;
	*didp = pcidev[devind].pd_did;

	return 1;
}

/*===========================================================================*
 *				_pci_next_dev				     *
 *===========================================================================*/
int
_pci_next_dev(const struct rs_pci *aclp, int *devindp, u16_t *vidp, u16_t *didp)
{
    if (devindp == NULL || vidp == NULL || didp == NULL) {
        return 0;
    }

    int current_search_start_index = *devindp;

    for (int devind = current_search_start_index + 1; devind < nr_pcidev; ++devind) {
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
#define _PCI_CALL_SYS_PRIVCTL(proc_arg, cmd_arg, data_arg, type_str) \
    do { \
        r = sys_privctl(proc_arg, cmd_arg, data_arg); \
        if (r != OK) { \
            printf("sys_privctl failed for proc %d, type %s: %d\n", \
                proc_arg, type_str, r); \
        } \
    } while (0)

int
_pci_grant_access(int devind, endpoint_t proc)
{
    int i;
    int r = OK;
    struct pci_device *dev_ptr;

    // Assuming pcidev is a globally accessible array.
    // No bounds check on devind can be added without potentially
    // altering external functionality (return value for invalid devind)
    // or introducing new global dependencies (e.g., nr_pci_devs).
    // The original code implies devind is always valid.
    dev_ptr = &pcidev[devind];

    for (i = 0; i < dev_ptr->pd_bar_nr; i++)
    {
        if (dev_ptr->pd_bar[i].pb_flags & PBF_INCOMPLETE)
        {
            printf("pci_reserve_a: BAR %d is incomplete\n", i);
            continue;
        }

        if (dev_ptr->pd_bar[i].pb_flags & PBF_IO)
        {
            struct io_range ior;
            ior.ior_base = dev_ptr->pd_bar[i].pb_base;
            ior.ior_limit = ior.ior_base + dev_ptr->pd_bar[i].pb_size - 1;

            if (debug) {
                printf(
                    "pci_reserve_a: for proc %d, adding I/O range [0x%x..0x%x]\n",
                    proc, ior.ior_base, ior.ior_limit);
            }
            _PCI_CALL_SYS_PRIVCTL(proc, SYS_PRIV_ADD_IO, &ior, "I/O range");
        }
        else // Memory range
        {
            struct minix_mem_range mr;
            mr.mr_base = dev_ptr->pd_bar[i].pb_base;
            mr.mr_limit = mr.mr_base + dev_ptr->pd_bar[i].pb_size - 1;

            _PCI_CALL_SYS_PRIVCTL(proc, SYS_PRIV_ADD_MEM, &mr, "memory range");
        }
    }

    // Handle Interrupt Line Register (ILR)
    if (dev_ptr->pd_ilr != PCI_ILR_UNKNOWN)
    {
        int ilr_val = dev_ptr->pd_ilr; // Use a local copy as per original logic
        if (debug) printf("pci_reserve_a: adding IRQ %d\n", ilr_val);
        _PCI_CALL_SYS_PRIVCTL(proc, SYS_PRIV_ADD_IRQ, &ilr_val, "IRQ");
    }

    // The function returns the result of the last sys_privctl call,
    // or OK if all calls succeeded. This preserves the original
    // error propagation behavior.
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
void
_pci_release(endpoint_t proc)
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

	complete_bridges();

	complete_bars();
}

/*===========================================================================*
 *				_pci_slot_name				     *
 *===========================================================================*/
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>

#ifndef OK
#define OK 0
#endif

struct PCIDevice {
    unsigned int pd_busnr;
    unsigned int pd_dev;
    unsigned int pd_func;
};

extern struct PCIDevice *pcidev;
extern int nr_pcidev;

#define PCI_SLOT_NAME_MAX_LEN 15

int
_pci_slot_name(int devind, char **cpp)
{
    char *buffer = NULL;
    int domain_nb = 0;

    if (cpp == NULL) {
        return EINVAL;
    }

    if (devind < 0 || devind >= nr_pcidev) {
        return EINVAL;
    }

    buffer = (char *)malloc(PCI_SLOT_NAME_MAX_LEN);
    if (buffer == NULL) {
        return ENOMEM;
    }

    int chars_written = snprintf(buffer, PCI_SLOT_NAME_MAX_LEN, "%u.%u.%u.%u",
                                 domain_nb,
                                 pcidev[devind].pd_busnr,
                                 pcidev[devind].pd_dev,
                                 pcidev[devind].pd_func);

    if (chars_written < 0 || chars_written >= PCI_SLOT_NAME_MAX_LEN) {
        free(buffer);
        return EINVAL;
    }

    *cpp = buffer;
    return OK;
}

/*===========================================================================*
 *				_pci_dev_name				     *
 *===========================================================================*/
const char *
_pci_dev_name(u16_t vid, u16_t did)
{
	_Thread_local static char product[PCI_PRODUCTSTR_LEN];
	product[0] = '\0';
	pci_findproduct(product, sizeof(product), vid, did);
	product[PCI_PRODUCTSTR_LEN - 1] = '\0';
	return product;
}

/*===========================================================================*
 *				_pci_get_bar				     *
 *===========================================================================*/
int
_pci_get_bar(int devind, int target_bar_offset, u32_t *base, u32_t *size,
	int *ioflag)
{
	if (base == NULL || size == NULL || ioflag == NULL) {
		return EINVAL;
	}

	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}

	const pci_device_t *device = &pcidev[devind];

	for (int i = 0; i < device->pd_bar_nr; i++) {
		const pci_bar_t *current_bar = &device->pd_bar[i];

		int current_bar_register_offset = PCI_BAR + 4 * current_bar->pb_nr;

		if (current_bar_register_offset == target_bar_offset) {
			if (current_bar->pb_flags & PBF_INCOMPLETE) {
				return EINVAL;
			}

			*base = current_bar->pb_base;
			*size = current_bar->pb_size;
			*ioflag = !!(current_bar->pb_flags & PBF_IO);
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
	if (vp == NULL) {
		return EINVAL;
	}
	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}
	if (port < 0 || port > 256-1) {
		return EINVAL;
	}

	*vp = __pci_attr_r8(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_r16				     *
 *===========================================================================*/
int
_pci_attr_r16(int devind, int port, u16_t *vp)
{
	if (vp == NULL) {
		return EINVAL;
	}

	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}

	if (port < 0 || port > (PCI_CONFIG_SPACE_SIZE_BYTES - sizeof(u16_t))) {
		return EINVAL;
	}

	*vp = __pci_attr_r16(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_r32				     *
 *===========================================================================*/
int
_pci_attr_r32(int devind, int port, u32_t *vp)
{
	if (vp == NULL) {
		return EINVAL;
	}
	if (devind < 0 || devind >= nr_pcidev) {
		return EINVAL;
	}
	if (port < 0 || port > 256 - 4) {
		return EINVAL;
	}

	*vp = __pci_attr_r32(devind, port);
	return OK;
}

/*===========================================================================*
 *				_pci_attr_w8				     *
 *===========================================================================*/
#define PCI_PORT_COUNT 256

int
_pci_attr_w8(int devind, int port, u8_t value)
{
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port >= PCI_PORT_COUNT)
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
	if (port < 0 || port > 254 || (port % 2 != 0))
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
	if (devind < 0 || devind >= nr_pcidev)
		return EINVAL;
	if (port < 0 || port > PCI_PORT_MAX_WRITE_ADDR_U32)
		return EINVAL;

	__pci_attr_w32(devind, port, value);
	return OK;
}
