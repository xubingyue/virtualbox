/* $Id$ */
/** @file
 * DevPCI - ICH9 southbridge PCI bus emulation device.
 *
 * @remarks We'll be slowly promoting the code in this file to common PCI bus
 *          code.   Function without 'static' and using 'devpci' as prefix is
 *          also used by DevPCI.cpp and have a prototype in DevPciInternal.h.
 *
 *          For the time being the DevPciMerge1.cpp.h file will remain separate,
 *          due to 5.1.  We can merge it into this one later in the dev cycle.
 */

/*
 * Copyright (C) 2010-2016 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */


/*********************************************************************************************************************************
*   Header Files                                                                                                                 *
*********************************************************************************************************************************/
#define LOG_GROUP LOG_GROUP_DEV_PCI
#define PDMPCIDEV_INCLUDE_PRIVATE  /* Hack to get pdmpcidevint.h included at the right point. */
#include <VBox/vmm/pdmpcidev.h>

#include <VBox/msi.h>
#include <VBox/vmm/pdmdev.h>
#include <VBox/vmm/mm.h>
#include <iprt/asm.h>
#include <iprt/assert.h>
#include <iprt/string.h>
#ifdef IN_RING3
# include <iprt/mem.h>
#endif

#include "PciInline.h"
#include "VBoxDD.h"
#include "MsiCommon.h"
#include "DevPciInternal.h"


/*********************************************************************************************************************************
*   Structures and Typedefs                                                                                                      *
*********************************************************************************************************************************/
/**
 * PCI configuration space address.
 */
typedef struct
{
    uint8_t  iBus;
    uint8_t  iDeviceFunc;
    uint16_t iRegister;
} PciAddress;


/*********************************************************************************************************************************
*   Defined Constants And Macros                                                                                                 *
*********************************************************************************************************************************/
/** @def VBOX_ICH9PCI_SAVED_STATE_VERSION
 * Saved state version of the ICH9 PCI bus device.
 */
#define VBOX_ICH9PCI_SAVED_STATE_VERSION_NOMSI 1
#define VBOX_ICH9PCI_SAVED_STATE_VERSION_MSI   2
#define VBOX_ICH9PCI_SAVED_STATE_VERSION_CURRENT VBOX_ICH9PCI_SAVED_STATE_VERSION_MSI

/** Invalid PCI region mapping address. */
#define INVALID_PCI_ADDRESS     UINT32_MAX


/*********************************************************************************************************************************
*   Internal Functions                                                                                                           *
*********************************************************************************************************************************/
/* Prototypes */
static void ich9pciSetIrqInternal(PDEVPCIROOT pPciRoot, uint8_t uDevFn, PPDMPCIDEV pPciDev,
                                  int iIrq, int iLevel, uint32_t uTagSrc);
#ifdef IN_RING3
static void ich9pcibridgeReset(PPDMDEVINS pDevIns);
DECLINLINE(PPDMPCIDEV) ich9pciFindBridge(PDEVPCIBUS pBus, uint8_t iBus);
static void ich9pciBiosInitDevice(PDEVPCIROOT pPciRoot, uint8_t uBus, uint8_t uDevFn);
#endif


// See 7.2.2. PCI Express Enhanced Configuration Mechanism for details of address
// mapping, we take n=6 approach
DECLINLINE(void) ich9pciPhysToPciAddr(PDEVPCIROOT pPciRoot, RTGCPHYS GCPhysAddr, PciAddress* pPciAddr)
{
    NOREF(pPciRoot);
    pPciAddr->iBus          = (GCPhysAddr >> 20) & ((1<<6)       - 1);
    pPciAddr->iDeviceFunc   = (GCPhysAddr >> 12) & ((1<<(5+3))   - 1); // 5 bits - device, 3 bits - function
    pPciAddr->iRegister     = (GCPhysAddr >>  0) & ((1<<(6+4+2)) - 1); // 6 bits - register, 4 bits - extended register, 2 bits -Byte Enable
}

DECLINLINE(void) ich9pciStateToPciAddr(PDEVPCIROOT pPciRoot, RTGCPHYS addr, PciAddress* pPciAddr)
{
    pPciAddr->iBus         = (pPciRoot->uConfigReg >> 16) & 0xff;
    pPciAddr->iDeviceFunc  = (pPciRoot->uConfigReg >> 8) & 0xff;
    pPciAddr->iRegister    = (pPciRoot->uConfigReg & 0xfc) | (addr & 3);
}

PDMBOTHCBDECL(void) ich9pciSetIrq(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, int iIrq, int iLevel, uint32_t uTagSrc)
{
    LogFlowFunc(("invoked by %p/%d: iIrq=%d iLevel=%d uTagSrc=%#x\n", pDevIns, pDevIns->iInstance, iIrq, iLevel, uTagSrc));
    ich9pciSetIrqInternal(PDMINS_2_DATA(pDevIns, PDEVPCIROOT), pPciDev->uDevFn, pPciDev, iIrq, iLevel, uTagSrc);
}

PDMBOTHCBDECL(void) ich9pcibridgeSetIrq(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, int iIrq, int iLevel, uint32_t uTagSrc)
{
    /*
     * The PCI-to-PCI bridge specification defines how the interrupt pins
     * are routed from the secondary to the primary bus (see chapter 9).
     * iIrq gives the interrupt pin the pci device asserted.
     * We change iIrq here according to the spec and call the SetIrq function
     * of our parent passing the device which asserted the interrupt instead of the device of the bridge.
     */
    PDEVPCIBUS    pBus          = PDMINS_2_DATA(pDevIns, PDEVPCIBUS);
    PPDMPCIDEV     pPciDevBus    = pPciDev;
    int            iIrqPinBridge = iIrq;
    uint8_t        uDevFnBridge  = 0;

    /* Walk the chain until we reach the host bus. */
    do
    {
        uDevFnBridge  = pBus->PciDev.uDevFn;
        iIrqPinBridge = ((pPciDevBus->uDevFn >> 3) + iIrqPinBridge) & 3;

        /* Get the parent. */
        pBus = pBus->PciDev.Int.s.CTX_SUFF(pBus);
        pPciDevBus = &pBus->PciDev;
    } while (pBus->iBus != 0);

    AssertMsgReturnVoid(pBus->iBus == 0, ("This is not the host pci bus iBus=%d\n", pBus->iBus));
    ich9pciSetIrqInternal(DEVPCIBUS_2_DEVPCIROOT(pBus), uDevFnBridge, pPciDev, iIrqPinBridge, iLevel, uTagSrc);
}


/**
 * Port I/O Handler for PCI address OUT operations.
 *
 * Emulates writes to Configuration Address Port at 0CF8h for
 * Configuration Mechanism #1.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     ICH9 device instance.
 * @param   pvUser      User argument - ignored.
 * @param   uPort       Port number used for the OUT operation.
 * @param   u32         The value to output.
 * @param   cb          The value size in bytes.
 */
PDMBOTHCBDECL(int) ich9pciIOPortAddressWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT Port, uint32_t u32, unsigned cb)
{
    LogFlow(("ich9pciIOPortAddressWrite: Port=%#x u32=%#x cb=%d\n", Port, u32, cb));
    RT_NOREF2(Port, pvUser);
    if (cb == 4)
    {
        PDEVPCIROOT pThis = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);

        /*
         * bits [1:0] are hard-wired, read-only and must return zeroes
         * when read.
         */
        u32 &= ~3;

        PCI_LOCK(pDevIns, VINF_IOM_R3_IOPORT_WRITE);
        pThis->uConfigReg = u32;
        PCI_UNLOCK(pDevIns);
    }

    return VINF_SUCCESS;
}


/**
 * Port I/O Handler for PCI address IN operations.
 *
 * Emulates reads from Configuration Address Port at 0CF8h for
 * Configuration Mechanism #1.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     ICH9 device instance.
 * @param   pvUser      User argument - ignored.
 * @param   uPort       Port number used for the IN operation.
 * @param   pu32        Where to store the result.
 * @param   cb          Number of bytes read.
 */
PDMBOTHCBDECL(int) ich9pciIOPortAddressRead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT Port, uint32_t *pu32, unsigned cb)
{
    RT_NOREF2(Port, pvUser);
    if (cb == 4)
    {
        PDEVPCIROOT pThis = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);

        PCI_LOCK(pDevIns, VINF_IOM_R3_IOPORT_READ);
        *pu32 = pThis->uConfigReg;
        PCI_UNLOCK(pDevIns);

        LogFlow(("ich9pciIOPortAddressRead: Port=%#x cb=%d -> %#x\n", Port, cb, *pu32));
        return VINF_SUCCESS;
    }

    Log(("ich9pciIOPortAddressRead: Port=%#x cb=%d VERR_IOM_IOPORT_UNUSED\n", Port, cb));
    return VERR_IOM_IOPORT_UNUSED;
}


/*
 * Perform configuration space write.
 */
static int ich9pciDataWriteAddr(PDEVPCIROOT pPciRoot, PciAddress* pAddr,
                                uint32_t val, int cb, int rcReschedule)
{
    int rc = VINF_SUCCESS;
#ifdef IN_RING3
    NOREF(rcReschedule);
#else
    RT_NOREF2(val, cb);
#endif

    if (pAddr->iBus != 0)       /* forward to subordinate bus */
    {
        if (pPciRoot->PciBus.cBridges)
        {
#ifdef IN_RING3 /** @todo do lookup in R0/RC too! */
            PPDMPCIDEV pBridgeDevice = ich9pciFindBridge(&pPciRoot->PciBus, pAddr->iBus);
            if (pBridgeDevice)
            {
                AssertPtr(pBridgeDevice->Int.s.pfnBridgeConfigWrite);
                pBridgeDevice->Int.s.pfnBridgeConfigWrite(pBridgeDevice->Int.s.CTX_SUFF(pDevIns), pAddr->iBus, pAddr->iDeviceFunc,
                                                          pAddr->iRegister, val, cb);
            }
#else
            rc = rcReschedule;
#endif
        }
    }
    else                    /* forward to directly connected device */
    {
        R3PTRTYPE(PDMPCIDEV *) pPciDev = pPciRoot->PciBus.apDevices[pAddr->iDeviceFunc];
        if (pPciDev)
        {
#ifdef IN_RING3
            pPciDev->Int.s.pfnConfigWrite(pPciDev->Int.s.CTX_SUFF(pDevIns), pPciDev, pAddr->iRegister, val, cb);
#else
            rc = rcReschedule;
#endif
        }
    }

    Log2(("ich9pciDataWriteAddr: %02x:%02x:%02x reg %x(%d) %x %Rrc\n",
          pAddr->iBus, pAddr->iDeviceFunc >> 3, pAddr->iDeviceFunc & 0x7, pAddr->iRegister,
          cb, val, rc));
    return rc;
}


/*
 * Decode value latched in Configuration Address Port and perform
 * requsted write to the target configuration space register.
 *
 * XXX: This code should be probably moved to its only caller
 * (ich9pciIOPortDataWrite) to avoid prolifiration of confusingly
 * similarly named functions.
 */
static int ich9pciDataWrite(PDEVPCIROOT pPciRoot, uint32_t addr, uint32_t val, int len)
{
    LogFlow(("ich9pciDataWrite: config=%08x val=%08x len=%d\n", pPciRoot->uConfigReg, val, len));

    /* Configuration space mapping enabled? */
    if (!(pPciRoot->uConfigReg & (1 << 31)))
        return VINF_SUCCESS;

    /* Decode target device and configuration space register */
    PciAddress aPciAddr;
    ich9pciStateToPciAddr(pPciRoot, addr, &aPciAddr);

    /* Perform configuration space write */
    return ich9pciDataWriteAddr(pPciRoot, &aPciAddr, val, len, VINF_IOM_R3_IOPORT_WRITE);
}


/**
 * Port I/O Handler for PCI data OUT operations.
 *
 * Emulates writes to Configuration Data Port at 0CFCh for
 * Configuration Mechanism #1.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     ICH9 device instance.
 * @param   pvUser      User argument - ignored.
 * @param   uPort       Port number used for the OUT operation.
 * @param   u32         The value to output.
 * @param   cb          The value size in bytes.
 */
PDMBOTHCBDECL(int) ich9pciIOPortDataWrite(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT Port, uint32_t u32, unsigned cb)
{
    LogFlow(("ich9pciIOPortDataWrite: Port=%#x u32=%#x cb=%d\n", Port, u32, cb));
    NOREF(pvUser);
    int rc = VINF_SUCCESS;
    if (!(Port % cb))
    {
        PDEVPCIROOT pThis = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);

        PCI_LOCK(pDevIns, VINF_IOM_R3_IOPORT_WRITE);
        rc = ich9pciDataWrite(pThis, Port, u32, cb);
        PCI_UNLOCK(pDevIns);
    }
    else
        AssertMsgFailed(("Unaligned write to port %#x u32=%#x cb=%d\n", Port, u32, cb));
    return rc;
}


static void ich9pciNoMem(void* ptr, int cb)
{
    for (int i = 0; i < cb; i++)
        ((uint8_t*)ptr)[i] = 0xff;
}


/*
 * Perform configuration space read.
 */
static int ich9pciDataReadAddr(PDEVPCIROOT pPciRoot, PciAddress* pPciAddr, int cb,
                               uint32_t *pu32, int rcReschedule)
{
    int rc = VINF_SUCCESS;
#ifdef IN_RING3
    NOREF(rcReschedule);
#endif

    if (pPciAddr->iBus != 0)    /* forward to subordinate bus */
    {
        if (pPciRoot->PciBus.cBridges)
        {
#ifdef IN_RING3 /** @todo do lookup in R0/RC too! */
            PPDMPCIDEV pBridgeDevice = ich9pciFindBridge(&pPciRoot->PciBus, pPciAddr->iBus);
            if (pBridgeDevice)
            {
                AssertPtr(pBridgeDevice->Int.s.pfnBridgeConfigRead);
                *pu32 = pBridgeDevice->Int.s.pfnBridgeConfigRead(pBridgeDevice->Int.s.CTX_SUFF(pDevIns), pPciAddr->iBus,
                                                                 pPciAddr->iDeviceFunc, pPciAddr->iRegister, cb);
            }
            else
                ich9pciNoMem(pu32, cb);
#else
            rc = rcReschedule;
#endif
        }
        else
            ich9pciNoMem(pu32, cb);
    }
    else                    /* forward to directly connected device */
    {
        R3PTRTYPE(PDMPCIDEV *) pPciDev = pPciRoot->PciBus.apDevices[pPciAddr->iDeviceFunc];
        if (pPciDev)
        {
#ifdef IN_RING3
            *pu32 = pPciDev->Int.s.pfnConfigRead(pPciDev->Int.s.CTX_SUFF(pDevIns), pPciDev, pPciAddr->iRegister, cb);
#else
            rc = rcReschedule;
#endif
        }
        else
            ich9pciNoMem(pu32, cb);
    }

    Log3(("ich9pciDataReadAddr: %02x:%02x:%02x reg %x(%d) gave %x %Rrc\n",
          pPciAddr->iBus, pPciAddr->iDeviceFunc >> 3, pPciAddr->iDeviceFunc & 0x7, pPciAddr->iRegister,
          cb, *pu32, rc));
    return rc;
}


/*
 * Decode value latched in Configuration Address Port and perform
 * requsted read from the target configuration space register.
 *
 * XXX: This code should be probably moved to its only caller
 * (ich9pciIOPortDataRead) to avoid prolifiration of confusingly
 * similarly named functions.
 */
static int ich9pciDataRead(PDEVPCIROOT pPciRoot, uint32_t addr, int cb, uint32_t *pu32)
{
    LogFlow(("ich9pciDataRead: config=%x cb=%d\n",  pPciRoot->uConfigReg, cb));

    *pu32 = 0xffffffff;

    /* Configuration space mapping enabled? */
    if (!(pPciRoot->uConfigReg & (1 << 31)))
        return VINF_SUCCESS;

    /* Decode target device and configuration space register */
    PciAddress aPciAddr;
    ich9pciStateToPciAddr(pPciRoot, addr, &aPciAddr);

    /* Perform configuration space read */
    return ich9pciDataReadAddr(pPciRoot, &aPciAddr, cb, pu32, VINF_IOM_R3_IOPORT_READ);
}


/**
 * Port I/O Handler for PCI data IN operations.
 *
 * Emulates reads from Configuration Data Port at 0CFCh for
 * Configuration Mechanism #1.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     ICH9 device instance.
 * @param   pvUser      User argument - ignored.
 * @param   uPort       Port number used for the IN operation.
 * @param   pu32        Where to store the result.
 * @param   cb          Number of bytes read.
 */
PDMBOTHCBDECL(int) ich9pciIOPortDataRead(PPDMDEVINS pDevIns, void *pvUser, RTIOPORT Port, uint32_t *pu32, unsigned cb)
{
    NOREF(pvUser);
    if (!(Port % cb))
    {
        PDEVPCIROOT pThis = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);

        PCI_LOCK(pDevIns, VINF_IOM_R3_IOPORT_READ);
        int rc = ich9pciDataRead(pThis, Port, cb, pu32);
        PCI_UNLOCK(pDevIns);

        LogFlow(("ich9pciIOPortDataRead: Port=%#x cb=%#x -> %#x (%Rrc)\n", Port, cb, *pu32, rc));
        return rc;
    }
    AssertMsgFailed(("Unaligned read from port %#x cb=%d\n", Port, cb));
    return VERR_IOM_IOPORT_UNUSED;
}


/* Compute mapping of PCI slot and IRQ number to APIC interrupt line */
DECLINLINE(int) ich9pciSlot2ApicIrq(uint8_t uSlot, int irq_num)
{
    return (irq_num + uSlot) & 7;
}

#ifdef IN_RING3

/* return the global irq number corresponding to a given device irq
   pin. We could also use the bus number to have a more precise
   mapping. This is the implementation note described in the PCI spec chapter 2.2.6 */
DECLINLINE(int) ich9pciSlotGetPirq(uint8_t uBus, uint8_t uDevFn, int iIrqNum)
{
    NOREF(uBus);
    int iSlotAddend = (uDevFn >> 3) - 1;
    return (iIrqNum + iSlotAddend) & 3;
}

/* irqs corresponding to PCI irqs A-D, must match pci_irq_list in rombios.c */
static const uint8_t aPciIrqs[4] = { 11, 10, 9, 5 };

#endif /* IN_RING3 */

/* Add one more level up request on APIC input line */
DECLINLINE(void) ich9pciApicLevelUp(PDEVPCIROOT pPciRoot, int irq_num)
{
    ASMAtomicIncU32(&pPciRoot->auPciApicIrqLevels[irq_num]);
}

/* Remove one level up request on APIC input line */
DECLINLINE(void) ich9pciApicLevelDown(PDEVPCIROOT pPciRoot, int irq_num)
{
    ASMAtomicDecU32(&pPciRoot->auPciApicIrqLevels[irq_num]);
}

static void ich9pciApicSetIrq(PDEVPCIBUS pBus, uint8_t uDevFn, PDMPCIDEV *pPciDev, int irq_num1, int iLevel,
                              uint32_t uTagSrc, int iForcedIrq)
{
    /* This is only allowed to be called with a pointer to the root bus. */
    AssertMsg(pBus->iBus == 0, ("iBus=%u\n", pBus->iBus));

    if (iForcedIrq == -1)
    {
        int apic_irq, apic_level;
        PDEVPCIROOT pPciRoot = DEVPCIBUS_2_DEVPCIROOT(pBus);
        int irq_num = ich9pciSlot2ApicIrq(uDevFn >> 3, irq_num1);

        if ((iLevel & PDM_IRQ_LEVEL_HIGH) == PDM_IRQ_LEVEL_HIGH)
            ich9pciApicLevelUp(pPciRoot, irq_num);
        else if ((iLevel & PDM_IRQ_LEVEL_HIGH) == PDM_IRQ_LEVEL_LOW)
            ich9pciApicLevelDown(pPciRoot, irq_num);

        apic_irq = irq_num + 0x10;
        apic_level = pPciRoot->auPciApicIrqLevels[irq_num] != 0;
        Log3(("ich9pciApicSetIrq: %s: irq_num1=%d level=%d apic_irq=%d apic_level=%d irq_num1=%d uTagSrc=%#x\n",
              R3STRING(pPciDev->pszNameR3), irq_num1, iLevel, apic_irq, apic_level, irq_num, uTagSrc));
        pBus->CTX_SUFF(pPciHlp)->pfnIoApicSetIrq(pBus->CTX_SUFF(pDevIns), apic_irq, apic_level, uTagSrc);

        if ((iLevel & PDM_IRQ_LEVEL_FLIP_FLOP) == PDM_IRQ_LEVEL_FLIP_FLOP)
        {
            /*
             *  we raised it few lines above, as PDM_IRQ_LEVEL_FLIP_FLOP has
             * PDM_IRQ_LEVEL_HIGH bit set
             */
            ich9pciApicLevelDown(pPciRoot, irq_num);
            pPciDev->Int.s.uIrqPinState = PDM_IRQ_LEVEL_LOW;
            apic_level = pPciRoot->auPciApicIrqLevels[irq_num] != 0;
            Log3(("ich9pciApicSetIrq: %s: irq_num1=%d level=%d apic_irq=%d apic_level=%d irq_num1=%d uTagSrc=%#x (flop)\n",
                  R3STRING(pPciDev->pszNameR3), irq_num1, iLevel, apic_irq, apic_level, irq_num, uTagSrc));
            pBus->CTX_SUFF(pPciHlp)->pfnIoApicSetIrq(pBus->CTX_SUFF(pDevIns), apic_irq, apic_level, uTagSrc);
        }
    } else {
        Log3(("ich9pciApicSetIrq: (forced) %s: irq_num1=%d level=%d acpi_irq=%d uTagSrc=%#x\n",
              R3STRING(pPciDev->pszNameR3), irq_num1, iLevel, iForcedIrq, uTagSrc));
        pBus->CTX_SUFF(pPciHlp)->pfnIoApicSetIrq(pBus->CTX_SUFF(pDevIns), iForcedIrq, iLevel, uTagSrc);
    }
}

static void ich9pciSetIrqInternal(PDEVPCIROOT pPciRoot, uint8_t uDevFn, PPDMPCIDEV pPciDev,
                                  int iIrq, int iLevel, uint32_t uTagSrc)
{
    /* If MSI or MSI-X is enabled, PCI INTx# signals are disabled regardless of the PCI command
     * register interrupt bit state.
     * PCI 3.0 (section 6.8) forbids MSI and MSI-X to be enabled at the same time and makes
     * that undefined behavior. We check for MSI first, then MSI-X.
     */
    if (MsiIsEnabled(pPciDev))
    {
        Assert(!MsixIsEnabled(pPciDev));    /* Not allowed -- see note above. */
        LogFlowFunc(("PCI Dev %p : MSI\n", pPciDev));
        PPDMDEVINS pDevIns = pPciRoot->PciBus.CTX_SUFF(pDevIns);
        MsiNotify(pDevIns, pPciRoot->PciBus.CTX_SUFF(pPciHlp), pPciDev, iIrq, iLevel, uTagSrc);
        return;
    }

    if (MsixIsEnabled(pPciDev))
    {
        LogFlowFunc(("PCI Dev %p : MSI-X\n", pPciDev));
        PPDMDEVINS pDevIns = pPciRoot->PciBus.CTX_SUFF(pDevIns);
        MsixNotify(pDevIns, pPciRoot->PciBus.CTX_SUFF(pPciHlp), pPciDev, iIrq, iLevel, uTagSrc);
        return;
    }

    PDEVPCIBUS     pBus      =     &pPciRoot->PciBus;
    const bool  fIsAcpiDevice = PCIDevGetDeviceId(pPciDev) == 0x7113;

    LogFlowFunc(("PCI Dev %p : IRQ\n", pPciDev));
    /* Check if the state changed. */
    if (pPciDev->Int.s.uIrqPinState != iLevel)
    {
        pPciDev->Int.s.uIrqPinState = (iLevel & PDM_IRQ_LEVEL_HIGH);

        /* Send interrupt to I/O APIC only now. */
        if (fIsAcpiDevice)
            /*
             * ACPI needs special treatment since SCI is hardwired and
             * should not be affected by PCI IRQ routing tables at the
             * same time SCI IRQ is shared in PCI sense hence this
             * kludge (i.e. we fetch the hardwired value from ACPIs
             * PCI device configuration space).
             */
            ich9pciApicSetIrq(pBus, uDevFn, pPciDev, -1, iLevel, uTagSrc, PCIDevGetInterruptLine(pPciDev));
        else
            ich9pciApicSetIrq(pBus, uDevFn, pPciDev, iIrq, iLevel, uTagSrc, -1);
    }
}


/**
 * Memory mapped I/O Handler for write operations.
 *
 * Emulates writes to configuration space.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   GCPhysAddr  Physical address (in GC) where the read starts.
 * @param   pv          Where to fetch the result.
 * @param   cb          Number of bytes to write.
 * @remarks Caller enters the device critical section.
 */
PDMBOTHCBDECL(int) ich9pciMcfgMMIOWrite(PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS GCPhysAddr, void const *pv, unsigned cb)
{
    PDEVPCIROOT pPciRoot = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);
    uint32_t u32 = 0;
    NOREF(pvUser);

    Log2(("ich9pciMcfgMMIOWrite: %RGp(%d) \n", GCPhysAddr, cb));

    PCI_LOCK(pDevIns, VINF_IOM_R3_MMIO_WRITE);

    /* Decode target device and configuration space register */
    PciAddress aDest;
    ich9pciPhysToPciAddr(pPciRoot, GCPhysAddr, &aDest);

    switch (cb)
    {
        case 1:
            u32 = *(uint8_t*)pv;
            break;
        case 2:
            u32 = *(uint16_t*)pv;
            break;
        case 4:
            u32 = *(uint32_t*)pv;
            break;
        default:
            Assert(false);
            break;
    }

    /* Perform configuration space write */
    int rc = ich9pciDataWriteAddr(pPciRoot, &aDest, u32, cb, VINF_IOM_R3_MMIO_WRITE);
    PCI_UNLOCK(pDevIns);

    return rc;
}


/**
 * Memory mapped I/O Handler for read operations.
 *
 * Emulates reads from configuration space.
 *
 * @returns VBox status code.
 *
 * @param   pDevIns     The device instance.
 * @param   pvUser      User argument.
 * @param   GCPhysAddr  Physical address (in GC) where the read starts.
 * @param   pv          Where to store the result.
 * @param   cb          Number of bytes read.
 * @remarks Caller enters the device critical section.
 */
PDMBOTHCBDECL(int) ich9pciMcfgMMIORead (PPDMDEVINS pDevIns, void *pvUser, RTGCPHYS GCPhysAddr, void *pv, unsigned cb)
{
    PDEVPCIROOT pPciRoot = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);
    uint32_t    rv;
    NOREF(pvUser);

    LogFlow(("ich9pciMcfgMMIORead: %RGp(%d) \n", GCPhysAddr, cb));

    PCI_LOCK(pDevIns, VINF_IOM_R3_MMIO_READ);

    /* Decode target device and configuration space register */
    PciAddress aDest;
    ich9pciPhysToPciAddr(pPciRoot, GCPhysAddr, &aDest);

    /* Perform configuration space read */
    int rc = ich9pciDataReadAddr(pPciRoot, &aDest, cb, &rv, VINF_IOM_R3_MMIO_READ);

    if (RT_SUCCESS(rc))
    {
        switch (cb)
        {
            case 1:
                *(uint8_t*)pv   = (uint8_t)rv;
                break;
            case 2:
                *(uint16_t*)pv  = (uint16_t)rv;
                break;
            case 4:
                *(uint32_t*)pv  = (uint32_t)rv;
                break;
            default:
                Assert(false);
                break;
        }
    }
    PCI_UNLOCK(pDevIns);

    return rc;
}

#ifdef IN_RING3

/*
 * Include code we share with the other PCI bus implementation.
 *
 * Note! No #ifdefs, use instant data booleans/flags/whatever.  Goal is to
 *       completely merge these files!  File #1 contains code we write, where
 *       as a possible file #2 contains external code if there's any left.
 */
# include "DevPciMerge1.cpp.h"


DECLINLINE(PPDMPCIDEV) ich9pciFindBridge(PDEVPCIBUS pBus, uint8_t iBus)
{
    /* Search for a fitting bridge. */
    for (uint32_t iBridge = 0; iBridge < pBus->cBridges; iBridge++)
    {
        /*
         * Examine secondary and subordinate bus number.
         * If the target bus is in the range we pass the request on to the bridge.
         */
        PPDMPCIDEV pBridge = pBus->papBridgesR3[iBridge];
        AssertMsg(pBridge && pciDevIsPci2PciBridge(pBridge),
                  ("Device is not a PCI bridge but on the list of PCI bridges\n"));
        uint32_t uSecondary   = PDMPciDevGetByte(pBridge, VBOX_PCI_SECONDARY_BUS);
        uint32_t uSubordinate = PDMPciDevGetByte(pBridge, VBOX_PCI_SUBORDINATE_BUS);
        Log3(("ich9pciFindBridge on bus %p, bridge %d: %d in %d..%d\n", pBus, iBridge, iBus, uSecondary, uSubordinate));
        if (iBus >= uSecondary && iBus <= uSubordinate)
            return pBridge;
    }

    /* Nothing found. */
    return NULL;
}

static uint32_t ich9pciGetCfg(PPDMPCIDEV pPciDev, int32_t iRegister, int cb)
{
    return pPciDev->Int.s.pfnConfigRead(pPciDev->Int.s.CTX_SUFF(pDevIns), pPciDev, iRegister, cb);
}

static uint8_t ich9pciGetByte(PPDMPCIDEV pPciDev, int32_t iRegister)
{
    return (uint8_t)ich9pciGetCfg(pPciDev, iRegister, 1);
}

static uint16_t ich9pciGetWord(PPDMPCIDEV pPciDev, int32_t iRegister)
{
    return (uint16_t)ich9pciGetCfg(pPciDev, iRegister, 2);
}

static uint32_t ich9pciGetDWord(PPDMPCIDEV pPciDev, int32_t iRegister)
{
    return (uint32_t)ich9pciGetCfg(pPciDev, iRegister, 4);
}

DECLINLINE(uint32_t) ich9pciGetRegionReg(int iRegion)
{
    return iRegion == VBOX_PCI_ROM_SLOT
         ?  VBOX_PCI_ROM_ADDRESS : (VBOX_PCI_BASE_ADDRESS_0 + iRegion * 4);
}


/* -=-=-=-=-=- PCI Bus Interface Methods (PDMPCIBUSREG) -=-=-=-=-=- */


static DECLCALLBACK(int) ich9pciRegisterMsi(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, PPDMMSIREG pMsiReg)
{
    NOREF(pDevIns);
    int rc;

    rc = MsiInit(pPciDev, pMsiReg);
    if (RT_FAILURE(rc))
        return rc;

    rc = MsixInit(pPciDev->Int.s.CTX_SUFF(pBus)->CTX_SUFF(pPciHlp), pPciDev, pMsiReg);
    if (RT_FAILURE(rc))
        return rc;

    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMPCIBUSREG,pfnIORegionRegisterR3}
 */
DECLCALLBACK(int) devpciR3CommonIORegionRegister(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, int iRegion, RTGCPHYS cbRegion,
                                                 PCIADDRESSSPACE enmType, PFNPCIIOREGIONMAP pfnCallback)
{
    NOREF(pDevIns);

    /*
     * Validate.
     */
    AssertMsgReturn(   enmType == (PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_BAR32)
                    || enmType == (PCI_ADDRESS_SPACE_MEM_PREFETCH | PCI_ADDRESS_SPACE_BAR32)
                    || enmType == (PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_BAR64)
                    || enmType == (PCI_ADDRESS_SPACE_MEM_PREFETCH | PCI_ADDRESS_SPACE_BAR64)
                    || enmType ==  PCI_ADDRESS_SPACE_IO
                    ,
                    ("Invalid enmType=%#x? Or was this a bitmask after all...\n", enmType),
                    VERR_INVALID_PARAMETER);
    AssertMsgReturn((unsigned)iRegion < VBOX_PCI_NUM_REGIONS,
                    ("Invalid iRegion=%d VBOX_PCI_NUM_REGIONS=%d\n", iRegion, VBOX_PCI_NUM_REGIONS),
                    VERR_INVALID_PARAMETER);
    int iLastSet = ASMBitLastSetU64(cbRegion);
    AssertMsgReturn(    iLastSet != 0
                    &&  RT_BIT_64(iLastSet - 1) == cbRegion,
                    ("Invalid cbRegion=%RGp iLastSet=%#x (not a power of 2 or 0)\n", cbRegion, iLastSet),
                    VERR_INVALID_PARAMETER);

    Log(("devpciR3CommonIORegionRegister: %s region %d size %RGp type %x\n",
         pPciDev->pszNameR3, iRegion, cbRegion, enmType));

    /* Make sure that we haven't marked this region as continuation of 64-bit region. */
    Assert(pPciDev->Int.s.aIORegions[iRegion].type != 0xff);

    /*
     * Register the I/O region.
     */
    PPCIIOREGION pRegion = &pPciDev->Int.s.aIORegions[iRegion];
    pRegion->addr        = INVALID_PCI_ADDRESS;
    pRegion->size        = cbRegion;
    pRegion->type        = enmType;
    pRegion->map_func    = pfnCallback;

    if ((enmType & PCI_ADDRESS_SPACE_BAR64) != 0)
    {
        /* VBOX_PCI_BASE_ADDRESS_5 and VBOX_PCI_ROM_ADDRESS are excluded. */
        AssertMsgReturn(iRegion < VBOX_PCI_NUM_REGIONS - 2,
                        ("Region %d cannot be 64-bit\n", iRegion),
                        VERR_INVALID_PARAMETER);
        /* Mark next region as continuation of this one. */
        pPciDev->Int.s.aIORegions[iRegion + 1].type = 0xff;
    }

    /* Set type in the PCI config space. */
    AssertCompile(PCI_ADDRESS_SPACE_MEM          == 0);
    AssertCompile(PCI_ADDRESS_SPACE_IO           == 1);
    AssertCompile(PCI_ADDRESS_SPACE_BAR64        == RT_BIT_32(2));
    AssertCompile(PCI_ADDRESS_SPACE_MEM_PREFETCH == RT_BIT_32(3));
    uint32_t u32Value   = (uint32_t)enmType & (PCI_ADDRESS_SPACE_IO | PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_MEM_PREFETCH);
    PCIDevSetDWord(pPciDev, ich9pciGetRegionReg(iRegion), u32Value);

    return VINF_SUCCESS;
}


/**
 * @interface_method_impl{PDMPCIBUSREG,pfnSetConfigCallbacksR3}
 */
DECLCALLBACK(void) devpciR3CommonSetConfigCallbacks(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev,
                                                    PFNPCICONFIGREAD pfnRead, PPFNPCICONFIGREAD ppfnReadOld,
                                                    PFNPCICONFIGWRITE pfnWrite, PPFNPCICONFIGWRITE ppfnWriteOld)
{
    NOREF(pDevIns);

    if (ppfnReadOld)
        *ppfnReadOld = pPciDev->Int.s.pfnConfigRead;
    pPciDev->Int.s.pfnConfigRead  = pfnRead;

    if (ppfnWriteOld)
        *ppfnWriteOld = pPciDev->Int.s.pfnConfigWrite;
    pPciDev->Int.s.pfnConfigWrite = pfnWrite;
}


static int ich9pciR3CommonSaveExec(PDEVPCIBUS pBus, PSSMHANDLE pSSM)
{
    /*
     * Iterate thru all the devices.
     */
    for (uint32_t i = 0; i < RT_ELEMENTS(pBus->apDevices); i++)
    {
        PPDMPCIDEV pDev = pBus->apDevices[i];
        if (pDev)
        {
            /* Device position */
            SSMR3PutU32(pSSM, i);
            /* PCI config registers */
            SSMR3PutMem(pSSM, pDev->abConfig, sizeof(pDev->abConfig));

            /* Device flags */
            int rc = SSMR3PutU32(pSSM, pDev->Int.s.fFlags);
            if (RT_FAILURE(rc))
                return rc;

            /* IRQ pin state */
            rc = SSMR3PutS32(pSSM, pDev->Int.s.uIrqPinState);
            if (RT_FAILURE(rc))
                return rc;

            /* MSI info */
            rc = SSMR3PutU8(pSSM, pDev->Int.s.u8MsiCapOffset);
            if (RT_FAILURE(rc))
                return rc;
            rc = SSMR3PutU8(pSSM, pDev->Int.s.u8MsiCapSize);
            if (RT_FAILURE(rc))
                return rc;

            /* MSI-X info */
            rc = SSMR3PutU8(pSSM, pDev->Int.s.u8MsixCapOffset);
            if (RT_FAILURE(rc))
                return rc;
            rc = SSMR3PutU8(pSSM, pDev->Int.s.u8MsixCapSize);
            if (RT_FAILURE(rc))
                return rc;
            /* Save MSI-X page state */
            if (pDev->Int.s.u8MsixCapOffset != 0)
            {
                Assert(pDev->Int.s.pMsixPageR3 != NULL);
                SSMR3PutMem(pSSM, pDev->Int.s.pMsixPageR3, 0x1000);
                if (RT_FAILURE(rc))
                    return rc;
            }
        }
    }
    return SSMR3PutU32(pSSM, UINT32_MAX); /* terminator */
}

static DECLCALLBACK(int) ich9pciR3SaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PDEVPCIROOT pThis = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);

    /*
     * Bus state data.
     */
    SSMR3PutU32(pSSM, pThis->uConfigReg);

    /*
     * Save IRQ states.
     */
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->auPciApicIrqLevels); i++)
        SSMR3PutU32(pSSM, pThis->auPciApicIrqLevels[i]);

    SSMR3PutU32(pSSM, UINT32_MAX);  /* separator */

    return ich9pciR3CommonSaveExec(&pThis->PciBus, pSSM);
}


static DECLCALLBACK(int) ich9pcibridgeR3SaveExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM)
{
    PDEVPCIBUS pThis = PDMINS_2_DATA(pDevIns, PDEVPCIBUS);
    return ich9pciR3CommonSaveExec(pThis, pSSM);
}


static DECLCALLBACK(void) ich9pcibridgeConfigWrite(PPDMDEVINSR3 pDevIns, uint8_t iBus, uint8_t iDevice, uint32_t u32Address, uint32_t u32Value, unsigned cb)
{
    PDEVPCIBUS pBus = PDMINS_2_DATA(pDevIns, PDEVPCIBUS);

    LogFlowFunc((": pDevIns=%p iBus=%d iDevice=%d u32Address=%u u32Value=%u cb=%d\n", pDevIns, iBus, iDevice, u32Address, u32Value, cb));

    /* If the current bus is not the target bus search for the bus which contains the device. */
    if (iBus != PDMPciDevGetByte(&pBus->PciDev, VBOX_PCI_SECONDARY_BUS))
    {
        PPDMPCIDEV pBridgeDevice = ich9pciFindBridge(pBus, iBus);
        if (pBridgeDevice)
        {
            AssertPtr(pBridgeDevice->Int.s.pfnBridgeConfigWrite);
            pBridgeDevice->Int.s.pfnBridgeConfigWrite(pBridgeDevice->Int.s.CTX_SUFF(pDevIns), iBus, iDevice,
                                                      u32Address, u32Value, cb);
        }
    }
    else
    {
        /* This is the target bus, pass the write to the device. */
        PPDMPCIDEV pPciDev = pBus->apDevices[iDevice];
        if (pPciDev)
        {
            Log(("%s: %s: addr=%02x val=%08x len=%d\n", __FUNCTION__, pPciDev->pszNameR3, u32Address, u32Value, cb));
            pPciDev->Int.s.pfnConfigWrite(pPciDev->Int.s.CTX_SUFF(pDevIns), pPciDev, u32Address, u32Value, cb);
        }
    }
}

static DECLCALLBACK(uint32_t) ich9pcibridgeConfigRead(PPDMDEVINSR3 pDevIns, uint8_t iBus, uint8_t iDevice, uint32_t u32Address, unsigned cb)
{
    PDEVPCIBUS pBus = PDMINS_2_DATA(pDevIns, PDEVPCIBUS);
    uint32_t u32Value;

    LogFlowFunc((": pDevIns=%p iBus=%d iDevice=%d u32Address=%u cb=%d\n", pDevIns, iBus, iDevice, u32Address, cb));

    /* If the current bus is not the target bus search for the bus which contains the device. */
    if (iBus != PDMPciDevGetByte(&pBus->PciDev, VBOX_PCI_SECONDARY_BUS))
    {
        PPDMPCIDEV pBridgeDevice = ich9pciFindBridge(pBus, iBus);
        if (pBridgeDevice)
        {
            AssertPtr( pBridgeDevice->Int.s.pfnBridgeConfigRead);
            u32Value = pBridgeDevice->Int.s.pfnBridgeConfigRead(pBridgeDevice->Int.s.CTX_SUFF(pDevIns), iBus, iDevice,
                                                                u32Address, cb);
        }
        else
            ich9pciNoMem(&u32Value, 4);
    }
    else
    {
        /* This is the target bus, pass the read to the device. */
        PPDMPCIDEV pPciDev = pBus->apDevices[iDevice];
        if (pPciDev)
        {
            u32Value = pPciDev->Int.s.pfnConfigRead(pPciDev->Int.s.CTX_SUFF(pDevIns), pPciDev, u32Address, cb);
            Log(("%s: %s: u32Address=%02x u32Value=%08x cb=%d\n", __FUNCTION__, pPciDev->pszNameR3, u32Address, u32Value, cb));
        }
        else
            ich9pciNoMem(&u32Value, 4);
    }

    return u32Value;
}



/* -=-=-=-=-=- Saved State -=-=-=-=-=- */


/**
 * Common routine for restoring the config registers of a PCI device.
 *
 * @param   pDev                The PCI device.
 * @param   pbSrcConfig         The configuration register values to be loaded.
 * @param   fIsBridge           Whether this is a bridge device or not.
 */
void devpciR3CommonRestoreConfig(PPDMPCIDEV pDev, uint8_t const *pbSrcConfig, bool fIsBridge)
{
    /*
     * This table defines the fields for normal devices and bridge devices, and
     * the order in which they need to be restored.
     */
    static const struct PciField
    {
        uint8_t     off;
        uint8_t     cb;
        uint8_t     fWritable;
        uint8_t     fBridge;
        const char *pszName;
    } s_aFields[] =
    {
        /* off,cb,fW,fB, pszName */
        { 0x00, 2, 0, 3, "VENDOR_ID" },
        { 0x02, 2, 0, 3, "DEVICE_ID" },
        { 0x06, 2, 1, 3, "STATUS" },
        { 0x08, 1, 0, 3, "REVISION_ID" },
        { 0x09, 1, 0, 3, "CLASS_PROG" },
        { 0x0a, 1, 0, 3, "CLASS_SUB" },
        { 0x0b, 1, 0, 3, "CLASS_BASE" },
        { 0x0c, 1, 1, 3, "CACHE_LINE_SIZE" },
        { 0x0d, 1, 1, 3, "LATENCY_TIMER" },
        { 0x0e, 1, 0, 3, "HEADER_TYPE" },
        { 0x0f, 1, 1, 3, "BIST" },
        { 0x10, 4, 1, 3, "BASE_ADDRESS_0" },
        { 0x14, 4, 1, 3, "BASE_ADDRESS_1" },
        { 0x18, 4, 1, 1, "BASE_ADDRESS_2" },
        { 0x18, 1, 1, 2, "PRIMARY_BUS" },       // fWritable = ??
        { 0x19, 1, 1, 2, "SECONDARY_BUS" },     // fWritable = ??
        { 0x1a, 1, 1, 2, "SUBORDINATE_BUS" },   // fWritable = ??
        { 0x1b, 1, 1, 2, "SEC_LATENCY_TIMER" }, // fWritable = ??
        { 0x1c, 4, 1, 1, "BASE_ADDRESS_3" },
        { 0x1c, 1, 1, 2, "IO_BASE" },           // fWritable = ??
        { 0x1d, 1, 1, 2, "IO_LIMIT" },          // fWritable = ??
        { 0x1e, 2, 1, 2, "SEC_STATUS" },        // fWritable = ??
        { 0x20, 4, 1, 1, "BASE_ADDRESS_4" },
        { 0x20, 2, 1, 2, "MEMORY_BASE" },       // fWritable = ??
        { 0x22, 2, 1, 2, "MEMORY_LIMIT" },      // fWritable = ??
        { 0x24, 4, 1, 1, "BASE_ADDRESS_5" },
        { 0x24, 2, 1, 2, "PREF_MEMORY_BASE" },  // fWritable = ??
        { 0x26, 2, 1, 2, "PREF_MEMORY_LIMIT" }, // fWritable = ??
        { 0x28, 4, 1, 1, "CARDBUS_CIS" },       // fWritable = ??
        { 0x28, 4, 1, 2, "PREF_BASE_UPPER32" }, // fWritable = ??
        { 0x2c, 2, 0, 1, "SUBSYSTEM_VENDOR_ID" },// fWritable = !?
        { 0x2c, 4, 1, 2, "PREF_LIMIT_UPPER32" },// fWritable = ??
        { 0x2e, 2, 0, 1, "SUBSYSTEM_ID" },      // fWritable = !?
        { 0x30, 4, 1, 1, "ROM_ADDRESS" },       // fWritable = ?!
        { 0x30, 2, 1, 2, "IO_BASE_UPPER16" },   // fWritable = ?!
        { 0x32, 2, 1, 2, "IO_LIMIT_UPPER16" },  // fWritable = ?!
        { 0x34, 4, 0, 3, "CAPABILITY_LIST" },   // fWritable = !? cb=!?
        { 0x38, 4, 1, 1, "RESERVED_38" },       // ???
        { 0x38, 4, 1, 2, "ROM_ADDRESS_BR" },    // fWritable = !? cb=!? fBridge=!?
        { 0x3c, 1, 1, 3, "INTERRUPT_LINE" },    // fBridge=??
        { 0x3d, 1, 0, 3, "INTERRUPT_PIN" },     // fBridge=??
        { 0x3e, 1, 0, 1, "MIN_GNT" },
        { 0x3e, 2, 1, 2, "BRIDGE_CONTROL" },    // fWritable = !?
        { 0x3f, 1, 0, 1, "MAX_LAT" },
        /* The COMMAND register must come last as it requires the *ADDRESS*
           registers to be restored before we pretent to change it from 0 to
           whatever value the guest assigned it. */
        { 0x04, 2, 1, 3, "COMMAND" },
    };

#ifdef RT_STRICT
    /* Check that we've got full register coverage. */
    uint32_t bmDevice[0x40 / 32];
    uint32_t bmBridge[0x40 / 32];
    RT_ZERO(bmDevice);
    RT_ZERO(bmBridge);
    for (uint32_t i = 0; i < RT_ELEMENTS(s_aFields); i++)
    {
        uint8_t off = s_aFields[i].off;
        uint8_t cb  = s_aFields[i].cb;
        uint8_t f   = s_aFields[i].fBridge;
        while (cb-- > 0)
        {
            if (f & 1) AssertMsg(!ASMBitTest(bmDevice, off), ("%#x\n", off));
            if (f & 2) AssertMsg(!ASMBitTest(bmBridge, off), ("%#x\n", off));
            if (f & 1) ASMBitSet(bmDevice, off);
            if (f & 2) ASMBitSet(bmBridge, off);
            off++;
        }
    }
    for (uint32_t off = 0; off < 0x40; off++)
    {
        AssertMsg(ASMBitTest(bmDevice, off), ("%#x\n", off));
        AssertMsg(ASMBitTest(bmBridge, off), ("%#x\n", off));
    }
#endif

    /*
     * Loop thru the fields covering the 64 bytes of standard registers.
     */
    uint8_t const fBridge = fIsBridge ? 2 : 1;
    Assert(!pciDevIsPassthrough(pDev));
    uint8_t *pbDstConfig = &pDev->abConfig[0];

    for (uint32_t i = 0; i < RT_ELEMENTS(s_aFields); i++)
        if (s_aFields[i].fBridge & fBridge)
        {
            uint8_t const   off = s_aFields[i].off;
            uint8_t const   cb  = s_aFields[i].cb;
            uint32_t        u32Src;
            uint32_t        u32Dst;
            switch (cb)
            {
                case 1:
                    u32Src = pbSrcConfig[off];
                    u32Dst = pbDstConfig[off];
                    break;
                case 2:
                    u32Src = *(uint16_t const *)&pbSrcConfig[off];
                    u32Dst = *(uint16_t const *)&pbDstConfig[off];
                    break;
                case 4:
                    u32Src = *(uint32_t const *)&pbSrcConfig[off];
                    u32Dst = *(uint32_t const *)&pbDstConfig[off];
                    break;
                default:
                    AssertFailed();
                    continue;
            }

            if (    u32Src != u32Dst
                ||  off == VBOX_PCI_COMMAND)
            {
                if (u32Src != u32Dst)
                {
                    if (!s_aFields[i].fWritable)
                        LogRel(("PCI: %8s/%u: %2u-bit field %s: %x -> %x - !READ ONLY!\n",
                                pDev->pszNameR3, pDev->Int.s.CTX_SUFF(pDevIns)->iInstance, cb*8, s_aFields[i].pszName, u32Dst, u32Src));
                    else
                        LogRel(("PCI: %8s/%u: %2u-bit field %s: %x -> %x\n",
                                pDev->pszNameR3, pDev->Int.s.CTX_SUFF(pDevIns)->iInstance, cb*8, s_aFields[i].pszName, u32Dst, u32Src));
                }
                if (off == VBOX_PCI_COMMAND)
                    PCIDevSetCommand(pDev, 0); /* For remapping, see ich9pciR3CommonLoadExec. */
                pDev->Int.s.pfnConfigWrite(pDev->Int.s.CTX_SUFF(pDevIns), pDev, off, u32Src, cb);
            }
        }

    /*
     * The device dependent registers.
     *
     * We will not use ConfigWrite here as we have no clue about the size
     * of the registers, so the device is responsible for correctly
     * restoring functionality governed by these registers.
     */
    for (uint32_t off = 0x40; off < sizeof(pDev->abConfig); off++)
        if (pbDstConfig[off] != pbSrcConfig[off])
        {
            LogRel(("PCI: %8s/%u: register %02x: %02x -> %02x\n",
                    pDev->pszNameR3, pDev->Int.s.CTX_SUFF(pDevIns)->iInstance, off, pbDstConfig[off], pbSrcConfig[off])); /** @todo make this Log() later. */
            pbDstConfig[off] = pbSrcConfig[off];
        }
}

/**
 * Common worker for ich9pciR3LoadExec and ich9pcibridgeR3LoadExec.
 *
 * @returns VBox status code.
 * @param   pBus                The bus which data is being loaded.
 * @param   pSSM                The saved state handle.
 * @param   uVersion            The data version.
 * @param   uPass               The pass.
 */
static DECLCALLBACK(int) ich9pciR3CommonLoadExec(PDEVPCIBUS pBus, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    uint32_t    u32;
    uint32_t    i;
    int         rc;

    Assert(uPass == SSM_PASS_FINAL); NOREF(uPass);
    if (uVersion != VBOX_ICH9PCI_SAVED_STATE_VERSION_CURRENT)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    /*
     * Iterate thru all the devices and write 0 to the COMMAND register so
     * that all the memory is unmapped before we start restoring the saved
     * mapping locations.
     *
     * The register value is restored afterwards so we can do proper
     * LogRels in devpciR3CommonRestoreConfig.
     */
    for (i = 0; i < RT_ELEMENTS(pBus->apDevices); i++)
    {
        PPDMPCIDEV pDev = pBus->apDevices[i];
        if (pDev)
        {
            uint16_t u16 = PCIDevGetCommand(pDev);
            pDev->Int.s.pfnConfigWrite(pDev->Int.s.CTX_SUFF(pDevIns), pDev, VBOX_PCI_COMMAND, 0, 2);
            PCIDevSetCommand(pDev, u16);
            Assert(PCIDevGetCommand(pDev) == u16);
        }
    }

    void *pvMsixPage = RTMemTmpAllocZ(0x1000);
    AssertReturn(pvMsixPage, VERR_NO_TMP_MEMORY);

    /*
     * Iterate all the devices.
     */
    for (i = 0;; i++)
    {
        PPDMPCIDEV  pDev;
        PDMPCIDEV   DevTmp;

        /* index / terminator */
        rc = SSMR3GetU32(pSSM, &u32);
        if (RT_FAILURE(rc))
            break;
        if (u32 == (uint32_t)~0)
            break;
        AssertMsgBreak(u32 < RT_ELEMENTS(pBus->apDevices) && u32 >= i, ("u32=%#x i=%#x\n", u32, i));

        /* skip forward to the device checking that no new devices are present. */
        for (; i < u32; i++)
        {
            pDev = pBus->apDevices[i];
            if (pDev)
            {
                LogRel(("PCI: New device in slot %#x, %s (vendor=%#06x device=%#06x)\n", i, pDev->pszNameR3,
                        PCIDevGetVendorId(pDev), PCIDevGetDeviceId(pDev)));
                if (SSMR3HandleGetAfter(pSSM) != SSMAFTER_DEBUG_IT)
                {
                    rc = SSMR3SetCfgError(pSSM, RT_SRC_POS, N_("New device in slot %#x, %s (vendor=%#06x device=%#06x)"),
                                          i, pDev->pszNameR3, PCIDevGetVendorId(pDev), PCIDevGetDeviceId(pDev));
                    break;
                }
            }
        }
        if (RT_FAILURE(rc))
            break;

        /* get the data */
        DevTmp.Int.s.fFlags = 0;
        DevTmp.Int.s.u8MsiCapOffset = 0;
        DevTmp.Int.s.u8MsiCapSize = 0;
        DevTmp.Int.s.u8MsixCapOffset = 0;
        DevTmp.Int.s.u8MsixCapSize = 0;
        DevTmp.Int.s.uIrqPinState = ~0; /* Invalid value in case we have an older saved state to force a state change in pciSetIrq. */
        SSMR3GetMem(pSSM, DevTmp.abConfig, sizeof(DevTmp.abConfig));

        SSMR3GetU32(pSSM, &DevTmp.Int.s.fFlags);
        SSMR3GetS32(pSSM, &DevTmp.Int.s.uIrqPinState);
        SSMR3GetU8(pSSM, &DevTmp.Int.s.u8MsiCapOffset);
        SSMR3GetU8(pSSM, &DevTmp.Int.s.u8MsiCapSize);
        SSMR3GetU8(pSSM, &DevTmp.Int.s.u8MsixCapOffset);
        rc = SSMR3GetU8(pSSM, &DevTmp.Int.s.u8MsixCapSize);
        if (RT_FAILURE(rc))
            break;

        /* Load MSI-X page state */
        if (DevTmp.Int.s.u8MsixCapOffset != 0)
        {
            Assert(pvMsixPage != NULL);
            rc = SSMR3GetMem(pSSM, pvMsixPage, 0x1000);
            if (RT_FAILURE(rc))
                break;
        }

        /* check that it's still around. */
        pDev = pBus->apDevices[i];
        if (!pDev)
        {
            LogRel(("PCI: Device in slot %#x has been removed! vendor=%#06x device=%#06x\n", i,
                    PCIDevGetVendorId(&DevTmp), PCIDevGetDeviceId(&DevTmp)));
            if (SSMR3HandleGetAfter(pSSM) != SSMAFTER_DEBUG_IT)
            {
                rc = SSMR3SetCfgError(pSSM, RT_SRC_POS, N_("Device in slot %#x has been removed! vendor=%#06x device=%#06x"),
                                      i, PCIDevGetVendorId(&DevTmp), PCIDevGetDeviceId(&DevTmp));
                break;
            }
            continue;
        }

        /* match the vendor id assuming that this will never be changed. */
        if (PCIDevGetVendorId(&DevTmp) != PCIDevGetVendorId(pDev))
        {
            rc = SSMR3SetCfgError(pSSM, RT_SRC_POS, N_("Device in slot %#x (%s) vendor id mismatch! saved=%.4Rhxs current=%.4Rhxs"),
                                  i, pDev->pszNameR3, PCIDevGetVendorId(&DevTmp), PCIDevGetVendorId(pDev));
            break;
        }

        /* commit the loaded device config. */
        Assert(!pciDevIsPassthrough(pDev));
        devpciR3CommonRestoreConfig(pDev, &DevTmp.abConfig[0], false ); /** @todo fix bridge fun! */

        pDev->Int.s.uIrqPinState = DevTmp.Int.s.uIrqPinState;
        pDev->Int.s.u8MsiCapOffset  = DevTmp.Int.s.u8MsiCapOffset;
        pDev->Int.s.u8MsiCapSize    = DevTmp.Int.s.u8MsiCapSize;
        pDev->Int.s.u8MsixCapOffset = DevTmp.Int.s.u8MsixCapOffset;
        pDev->Int.s.u8MsixCapSize   = DevTmp.Int.s.u8MsixCapSize;
        if (DevTmp.Int.s.u8MsixCapSize != 0)
        {
            Assert(pDev->Int.s.pMsixPageR3 != NULL);
            memcpy(pDev->Int.s.pMsixPageR3, pvMsixPage, 0x1000);
        }
    }

    RTMemTmpFree(pvMsixPage);

    return rc;
}

static DECLCALLBACK(int) ich9pciR3LoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PDEVPCIROOT pThis = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);
    PDEVPCIBUS     pBus  = &pThis->PciBus;
    uint32_t        u32;
    int             rc;

    /* We ignore this version as there's no saved state with it anyway */
    if (uVersion == VBOX_ICH9PCI_SAVED_STATE_VERSION_NOMSI)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    if (uVersion > VBOX_ICH9PCI_SAVED_STATE_VERSION_MSI)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;

    /*
     * Bus state data.
     */
    SSMR3GetU32(pSSM, &pThis->uConfigReg);

    /*
     * Load IRQ states.
     */
    for (unsigned i = 0; i < RT_ELEMENTS(pThis->auPciApicIrqLevels); i++)
        SSMR3GetU32(pSSM, (uint32_t*)&pThis->auPciApicIrqLevels[i]);

    /* separator */
    rc = SSMR3GetU32(pSSM, &u32);
    if (RT_FAILURE(rc))
        return rc;
    if (u32 != (uint32_t)~0)
        AssertMsgFailedReturn(("u32=%#x\n", u32), rc);

    return ich9pciR3CommonLoadExec(pBus, pSSM, uVersion, uPass);
}

static DECLCALLBACK(int) ich9pcibridgeR3LoadExec(PPDMDEVINS pDevIns, PSSMHANDLE pSSM, uint32_t uVersion, uint32_t uPass)
{
    PDEVPCIBUS pThis = PDMINS_2_DATA(pDevIns, PDEVPCIBUS);
    if (uVersion > VBOX_ICH9PCI_SAVED_STATE_VERSION_MSI)
        return VERR_SSM_UNSUPPORTED_DATA_UNIT_VERSION;
    return ich9pciR3CommonLoadExec(pThis, pSSM, uVersion, uPass);
}



/* -=-=-=-=-=- Fake PCI BIOS Init -=-=-=-=-=- */


/*
 * Perform imeediate read of configuration space register.
 * Cannot be rescheduled, as already in R3.
 */
static uint32_t ich9pciBiosInitReadConfig(PDEVPCIROOT pPciRoot, uint8_t uBus, uint8_t uDevFn, uint32_t addr, uint32_t len)
{
    PciAddress aPciAddr;
    aPciAddr.iBus = uBus;
    aPciAddr.iDeviceFunc = uDevFn;
    aPciAddr.iRegister = addr;

    uint32_t u32Val = 0;
    int rc = ich9pciDataReadAddr(pPciRoot, &aPciAddr, len, &u32Val, VERR_INTERNAL_ERROR);
    AssertRC(rc);

    return u32Val;
}


/*
 * Perform imeediate write to configuration space register.
 * Cannot be rescheduled, as already in R3.
 */
static void ich9pciBiosInitWriteConfig(PDEVPCIROOT pPciRoot, uint8_t uBus, uint8_t uDevFn, uint32_t addr, uint32_t val, uint32_t len)
{
    PciAddress aPciAddr;
    aPciAddr.iBus = uBus;
    aPciAddr.iDeviceFunc = uDevFn;
    aPciAddr.iRegister = addr;

    int rc = ich9pciDataWriteAddr(pPciRoot, &aPciAddr, val, len, VERR_INTERNAL_ERROR);
    AssertRC(rc);
}


static void ich9pciBiosInitSetRegionAddress(PDEVPCIROOT pPciRoot, uint8_t uBus, uint8_t uDevFn, int iRegion, uint64_t addr)
{
    uint32_t uReg = ich9pciGetRegionReg(iRegion);

    /* Read memory type first. */
    uint8_t uResourceType = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, uReg, 1);
    bool    f64Bit =    (uResourceType & ((uint8_t)(PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_IO)))
                     == PCI_ADDRESS_SPACE_BAR64;

    Log(("Set region address: %02x:%02x.%d region %d address=%RX64%s\n",
         uBus, uDevFn >> 3, uDevFn & 7, iRegion, addr, f64Bit ? " (64-bit)" : ""));

    /* Write address of the device. */
    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, uReg, (uint32_t)addr, 4);
    if (f64Bit)
        ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, uReg + 4, (uint32_t)(addr >> 32), 4);
}


static void ich9pciBiosInitBridge(PDEVPCIROOT pPciRoot, uint8_t uBus, uint8_t uDevFn)
{
    Log(("BIOS init bridge: %02x::%02x.%d\n", uBus, uDevFn >> 3, uDevFn & 7));

    /*
     * The I/O range for the bridge must be aligned to a 4KB boundary.
     * This does not change anything really as the access to the device is not going
     * through the bridge but we want to be compliant to the spec.
     */
    if ((pPciRoot->uPciBiosIo % 4096) != 0)
    {
        pPciRoot->uPciBiosIo = RT_ALIGN_32(pPciRoot->uPciBiosIo, 4*1024);
        Log(("%s: Aligned I/O start address. New address %#x\n", __FUNCTION__, pPciRoot->uPciBiosIo));
    }
    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_IO_BASE, (pPciRoot->uPciBiosIo >> 8) & 0xf0, 1);

    /* The MMIO range for the bridge must be aligned to a 1MB boundary. */
    if ((pPciRoot->uPciBiosMmio % (1024 * 1024)) != 0)
    {
        pPciRoot->uPciBiosMmio = RT_ALIGN_32(pPciRoot->uPciBiosMmio, 1024*1024);
        Log(("%s: Aligned MMIO start address. New address %#x\n", __FUNCTION__, pPciRoot->uPciBiosMmio));
    }
    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_MEMORY_BASE, (pPciRoot->uPciBiosMmio >> 16) & UINT32_C(0xffff0), 2);

    /* Save values to compare later to. */
    uint32_t u32IoAddressBase = pPciRoot->uPciBiosIo;
    uint32_t u32MMIOAddressBase = pPciRoot->uPciBiosMmio;
    uint8_t uBridgeBus = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_SECONDARY_BUS, 1);

    /* Init devices behind the bridge and possibly other bridges as well. */
    for (int iDev = 0; iDev <= 255; iDev++)
        ich9pciBiosInitDevice(pPciRoot, uBridgeBus, iDev);

    /*
     * Set I/O limit register. If there is no device with I/O space behind the bridge
     * we set a lower value than in the base register.
     * The result with a real bridge is that no I/O transactions are passed to the secondary
     * interface. Again this doesn't really matter here but we want to be compliant to the spec.
     */
    if ((u32IoAddressBase != pPciRoot->uPciBiosIo) && ((pPciRoot->uPciBiosIo % 4096) != 0))
    {
        /* The upper boundary must be one byte less than a 4KB boundary. */
        pPciRoot->uPciBiosIo = RT_ALIGN_32(pPciRoot->uPciBiosIo, 4*1024);
    }

    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_IO_LIMIT, ((pPciRoot->uPciBiosIo >> 8) & 0xf0) - 1, 1);

    /* Same with the MMIO limit register but with 1MB boundary here. */
    if ((u32MMIOAddressBase != pPciRoot->uPciBiosMmio) && ((pPciRoot->uPciBiosMmio % (1024 * 1024)) != 0))
    {
        /* The upper boundary must be one byte less than a 1MB boundary. */
        pPciRoot->uPciBiosMmio = RT_ALIGN_32(pPciRoot->uPciBiosMmio, 1024*1024);
    }
    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_MEMORY_LIMIT, ((pPciRoot->uPciBiosMmio >> 16) & UINT32_C(0xfff0)) - 1, 2);

    /*
     * Set the prefetch base and limit registers. We currently have no device with a prefetchable region
     * which may be behind a bridge. That's why it is unconditionally disabled here atm by writing a higher value into
     * the base register than in the limit register.
     */
    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_PREF_MEMORY_BASE, 0xfff0, 2);
    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_PREF_MEMORY_LIMIT, 0x0, 2);
    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_PREF_BASE_UPPER32, 0x00, 4);
    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_PREF_LIMIT_UPPER32, 0x00, 4);
}

static void ich9pciBiosInitDevice(PDEVPCIROOT pPciRoot, uint8_t uBus, uint8_t uDevFn)
{
    uint16_t uDevClass, uVendor, uDevice;
    uint8_t uCmd;

    uDevClass  = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_CLASS_DEVICE, 2);
    uVendor    = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_VENDOR_ID, 2);
    uDevice    = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_DEVICE_ID, 2);

    /* If device is present */
    if (uVendor == 0xffff)
        return;

    Log(("BIOS init device: %02x:%02x.%d\n", uBus, uDevFn >> 3, uDevFn & 7));

    switch (uDevClass)
    {
        case 0x0101:
            /* IDE controller */
            ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, 0x40, 0x8000, 2); /* enable IDE0 */
            ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, 0x42, 0x8000, 2); /* enable IDE1 */
            goto default_map;
            break;
        case 0x0300:
            /* VGA controller */

            /* NB: Default Bochs VGA LFB address is 0xE0000000. Old guest
             * software may break if the framebuffer isn't mapped there.
             */

            /*
             * Legacy VGA I/O ports are implicitly decoded by a VGA class device. But
             * only the framebuffer (i.e., a memory region) is explicitly registered via
             * ich9pciSetRegionAddress, so don't forget to enable I/O decoding.
             */
            uCmd = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_COMMAND, 1);
            ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_COMMAND,
                                       uCmd | PCI_COMMAND_IOACCESS,
                                       1);
            goto default_map;
            break;
        case 0x0604:
            /* PCI-to-PCI bridge. */
            AssertMsg(pPciRoot->uPciBiosBus < 255, ("Too many bridges on the bus\n"));
            ich9pciBiosInitBridge(pPciRoot, uBus, uDevFn);
            break;
        default:
        default_map:
        {
            /* default memory mappings */
            bool fActiveMemRegion = false;
            bool fActiveIORegion = false;
            /*
             * We ignore ROM region here.
             */
            for (int iRegion = 0; iRegion < VBOX_PCI_NUM_REGIONS - 1; iRegion++)
            {
                uint32_t u32Address = ich9pciGetRegionReg(iRegion);

                /* Calculate size - we write all 1s into the BAR, and then evaluate which bits
                   are cleared. */
                uint8_t u8ResourceType = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, u32Address, 1);

                bool f64Bit =    (u8ResourceType & ((uint8_t)(PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_IO)))
                              == PCI_ADDRESS_SPACE_BAR64;
                bool fIsPio = ((u8ResourceType & PCI_COMMAND_IOACCESS) == PCI_COMMAND_IOACCESS);
                uint64_t cbRegSize64 = 0;

                if (f64Bit)
                {
                    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, u32Address,   UINT32_C(0xffffffff), 4);
                    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, u32Address+4, UINT32_C(0xffffffff), 4);
                    cbRegSize64  =            ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, u32Address,   4);
                    cbRegSize64 |= ((uint64_t)ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, u32Address+4, 4) << 32);
                    cbRegSize64 &= ~UINT64_C(0x0f);
                    cbRegSize64 = (~cbRegSize64) + 1;

                    /* No 64-bit PIO regions possible. */
#ifndef DEBUG_bird /* EFI triggers this for DevAHCI. */
                    AssertMsg((u8ResourceType & PCI_COMMAND_IOACCESS) == 0, ("type=%#x rgn=%d\n", u8ResourceType, iRegion));
#endif
                }
                else
                {
                    uint32_t cbRegSize32;
                    ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, u32Address, UINT32_C(0xffffffff), 4);
                    cbRegSize32 = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, u32Address, 4);

                    /* Clear resource information depending on resource type. */
                    if (fIsPio) /* PIO */
                        cbRegSize32 &= ~UINT32_C(0x01);
                    else        /* MMIO */
                        cbRegSize32 &= ~UINT32_C(0x0f);

                    /*
                     * Invert all bits and add 1 to get size of the region.
                     * (From PCI implementation note)
                     */
                    if (fIsPio && (cbRegSize32 & UINT32_C(0xffff0000)) == 0)
                        cbRegSize32 = (~(cbRegSize32 | UINT32_C(0xffff0000))) + 1;
                    else
                        cbRegSize32 = (~cbRegSize32) + 1;

                    cbRegSize64 = cbRegSize32;
                }
                Log2(("%s: Size of region %u for device %d on bus %d is %lld\n", __FUNCTION__, iRegion, uDevFn, uBus, cbRegSize64));

                if (cbRegSize64)
                {
                    /* Try 32-bit base first. */
                    uint32_t* paddr = fIsPio ? &pPciRoot->uPciBiosIo : &pPciRoot->uPciBiosMmio;
                    uint64_t  uNew = *paddr;
                    /* Align starting address to region size. */
                    uNew = (uNew + cbRegSize64 - 1) & ~(cbRegSize64 - 1);
                    if (fIsPio)
                        uNew &= UINT32_C(0xffff);
                    /* Unconditionally exclude I/O-APIC/HPET/ROM. Pessimistic, but better than causing a mess. */
                    if (   !uNew
                        || (uNew <= UINT32_C(0xffffffff) && uNew + cbRegSize64 - 1 >= UINT32_C(0xfec00000))
                        || uNew >= _4G)
                    {
                        if (f64Bit)
                        {
                            /* Map a 64-bit region above 4GB. */
                            Assert(!fIsPio);
                            uNew = pPciRoot->uPciBiosMmio64;
                            /* Align starting address to region size. */
                            uNew = (uNew + cbRegSize64 - 1) & ~(cbRegSize64 - 1);
                            LogFunc(("Start address of 64-bit MMIO region %u/%u is %#llx\n", iRegion, iRegion + 1, uNew));
                            ich9pciBiosInitSetRegionAddress(pPciRoot, uBus, uDevFn, iRegion, uNew);
                            fActiveMemRegion = true;
                            pPciRoot->uPciBiosMmio64 = uNew + cbRegSize64;
                            Log2Func(("New 64-bit address is %#llx\n", pPciRoot->uPciBiosMmio64));
                        }
                        else
                        {
                            LogRel(("PCI: no space left for BAR%u of device %u/%u/%u (vendor=%#06x device=%#06x)\n",
                                    iRegion, uBus, uDevFn >> 3, uDevFn & 7, uVendor, uDevice)); /** @todo make this a VM start failure later. */
                            /* Undo the mapping mess caused by the size probing. */
                            ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, u32Address, UINT32_C(0), 4);
                        }
                    }
                    else
                    {
                        LogFunc(("Start address of %s region %u is %#x\n", (fIsPio ? "I/O" : "MMIO"), iRegion, uNew));
                        ich9pciBiosInitSetRegionAddress(pPciRoot, uBus, uDevFn, iRegion, uNew);
                        if (fIsPio)
                            fActiveIORegion = true;
                        else
                            fActiveMemRegion = true;
                        *paddr = uNew + cbRegSize64;
                        Log2Func(("New 32-bit address is %#x\n", *paddr));
                    }

                    if (f64Bit)
                        iRegion++; /* skip next region */
                }
            }

            /* Update the command word appropriately. */
            uCmd = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_COMMAND, 2);
            if (fActiveMemRegion)
                uCmd |= PCI_COMMAND_MEMACCESS; /* Enable MMIO access. */
            if (fActiveIORegion)
                uCmd |= PCI_COMMAND_IOACCESS; /* Enable I/O space access. */
            ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_COMMAND, uCmd, 2);
            break;
        }
    }

    /* map the interrupt */
    uint32_t iPin = ich9pciBiosInitReadConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_INTERRUPT_PIN, 1);
    if (iPin != 0)
    {
        iPin--;

        if (uBus != 0)
        {
            /* Find bus this device attached to. */
            PDEVPCIBUS pBus = &pPciRoot->PciBus;
            while (1)
            {
                PPDMPCIDEV pBridge = ich9pciFindBridge(pBus, uBus);
                if (!pBridge)
                {
                    Assert(false);
                    break;
                }
                if (uBus == PDMPciDevGetByte(pBridge, VBOX_PCI_SECONDARY_BUS))
                {
                    /* OK, found bus this device attached to. */
                    break;
                }
                pBus = PDMINS_2_DATA(pBridge->Int.s.CTX_SUFF(pDevIns), PDEVPCIBUS);
            }

            /* We need to go up to the host bus to see which irq pin this
             * device will use there. See logic in ich9pcibridgeSetIrq().
             */
            while (pBus->iBus != 0)
            {
                /* Get the pin the device would assert on the bridge. */
                iPin = ((pBus->PciDev.uDevFn >> 3) + iPin) & 3;
                pBus = pBus->PciDev.Int.s.pBusR3;
            };
        }

        int iIrq = aPciIrqs[ich9pciSlotGetPirq(uBus, uDevFn, iPin)];
        Log(("Using pin %d and IRQ %d for device %02x:%02x.%d\n",
             iPin, iIrq, uBus, uDevFn>>3, uDevFn&7));
        ich9pciBiosInitWriteConfig(pPciRoot, uBus, uDevFn, VBOX_PCI_INTERRUPT_LINE, iIrq, 1);
    }
}

/**
 * Initializes bridges registers used for routing.
 *
 * @returns nothing.
 * @param   pPciRoot         Global device instance data used to generate unique bus numbers.
 * @param   pBus             The PCI bus to initialize.
 * @param   uBusPrimary      The primary bus number the bus is connected to.
 * @param   uBusSecondary    The secondary bus number, i.e. the bus number behind the bridge.
 */
static void ich9pciBiosInitBridgeTopology(PDEVPCIROOT pPciRoot, PDEVPCIBUS pBus, unsigned uBusPrimary, unsigned uBusSecondary)
{
    PPDMPCIDEV pBridgeDev = &pBus->PciDev;

    /* Set only if we are not on the root bus, it has no primary bus attached. */
    if (uBusSecondary != 0)
    {
        PCIDevSetByte(pBridgeDev, VBOX_PCI_PRIMARY_BUS, uBusPrimary);
        PCIDevSetByte(pBridgeDev, VBOX_PCI_SECONDARY_BUS, uBusSecondary);
    }

    for (uint32_t iBridge = 0; iBridge < pBus->cBridges; iBridge++)
    {
        PPDMPCIDEV pBridge = pBus->papBridgesR3[iBridge];
        AssertMsg(pBridge && pciDevIsPci2PciBridge(pBridge),
                  ("Device is not a PCI bridge but on the list of PCI bridges\n"));
        PDEVPCIBUS pChildBus = PDMINS_2_DATA(pBridge->Int.s.CTX_SUFF(pDevIns), PDEVPCIBUS);
        pPciRoot->uPciBiosBus++;
        ich9pciBiosInitBridgeTopology(pPciRoot, pChildBus, uBusSecondary, pPciRoot->uPciBiosBus);
    }
    PCIDevSetByte(pBridgeDev, VBOX_PCI_SUBORDINATE_BUS, pPciRoot->uPciBiosBus);
    Log2(("ich9pciBiosInitBridgeTopology: for bus %p: primary=%d secondary=%d subordinate=%d\n",
          pBus,
          PDMPciDevGetByte(pBridgeDev, VBOX_PCI_PRIMARY_BUS),
          PDMPciDevGetByte(pBridgeDev, VBOX_PCI_SECONDARY_BUS),
          PDMPciDevGetByte(pBridgeDev, VBOX_PCI_SUBORDINATE_BUS)
          ));
}


/**
 * @interface_method_impl{PDMPCIBUSREG,pfnFakePCIBIOSR3}
 */
static DECLCALLBACK(int) ich9pciFakePCIBIOS(PPDMDEVINS pDevIns)
{
    PDEVPCIROOT     pPciRoot   = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);
    PVM             pVM        = PDMDevHlpGetVM(pDevIns);
    uint32_t const  cbBelow4GB = MMR3PhysGetRamSizeBelow4GB(pVM);
    uint64_t const  cbAbove4GB = MMR3PhysGetRamSizeAbove4GB(pVM);

    /*
     * Set the start addresses.
     */
    pPciRoot->uPciBiosBus    = 0;
    pPciRoot->uPciBiosIo     = 0xd000;
    pPciRoot->uPciBiosMmio   = cbBelow4GB;
    pPciRoot->uPciBiosMmio64 = cbAbove4GB + _4G;

    /* NB: Assume that if PCI controller MMIO range is enabled, it is at the bottom of the memory hole. */
    if (pPciRoot->u64PciConfigMMioAddress)
    {
        AssertRelease(pPciRoot->u64PciConfigMMioAddress >= cbBelow4GB);
        pPciRoot->uPciBiosMmio = pPciRoot->u64PciConfigMMioAddress + pPciRoot->u64PciConfigMMioLength;
    }
    Log(("cbBelow4GB: %#RX32, uPciBiosMmio: %#RX64, cbAbove4GB: %#RX64, uPciBiosMmio64=%#RX64\n",
         cbBelow4GB, pPciRoot->uPciBiosMmio, cbAbove4GB, pPciRoot->uPciBiosMmio64));

    /*
     * Assign bridge topology, for further routing to work.
     */
    PDEVPCIBUS pBus = &pPciRoot->PciBus;
    ich9pciBiosInitBridgeTopology(pPciRoot, pBus, 0, 0);

    /*
     * Init the devices.
     */
    for (uint32_t i = 0; i < 256; i++)
        ich9pciBiosInitDevice(pPciRoot, 0, i);

    return VINF_SUCCESS;
}


/* -=-=-=-=-=- PCI Config Space -=-=-=-=-=- */


/**
 * @callback_method_impl{PFNPCICONFIGREAD, Default config space read callback.}
 */
DECLCALLBACK(uint32_t) devpciR3CommonDefaultConfigRead(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev, uint32_t uAddress, unsigned cb)
{
    NOREF(pDevIns);

    uint32_t uValue;
    if (uAddress + cb <= 256)
    {
        switch (cb)
        {
            case 1:
                uValue = PDMPciDevGetByte(pPciDev,  uAddress);
                break;
            case 2:
                uValue = PDMPciDevGetWord(pPciDev,  uAddress);
                break;
            case 4:
                uValue = PDMPciDevGetDWord(pPciDev, uAddress);
                break;
            default:
                AssertFailed();
                uValue = 0;
                break;
        }

#ifdef LOG_ENABLED
        if (   pciDevIsMsiCapable(pPciDev)
            && uAddress - (uint32_t)pPciDev->Int.s.u8MsiCapOffset < (uint32_t)pPciDev->Int.s.u8MsiCapSize )
            Log2(("devpciR3CommonDefaultConfigRead: MSI CAP: %#x LB %u -> %#x\n", uAddress - (uint32_t)pPciDev->Int.s.u8MsiCapOffset, cb, uValue));
        else if (   pciDevIsMsixCapable(pPciDev)
                 && uAddress - (uint32_t)pPciDev->Int.s.u8MsixCapOffset < (uint32_t)pPciDev->Int.s.u8MsixCapSize)
            Log2(("devpciR3CommonDefaultConfigRead: MSI-X CAP: %#x LB %u -> %#x\n", uAddress - (uint32_t)pPciDev->Int.s.u8MsiCapOffset, cb, uValue));
#endif
    }
    else
    {
        if (uAddress + cb < 4096)
            LogRel(("PCI: %8s/%u: Read from extended register %d fallen back to generic code\n",
                    pPciDev->pszNameR3, pPciDev->Int.s.CTX_SUFF(pDevIns)->iInstance, uAddress));
        else
            AssertFailed();
        uValue = 0;
    }
    return uValue;
}


/**
 * Worker for ich9pciResetDevice and devpciR3UpdateMappings that unmaps a region.
 *
 * @returns VBox status code.
 * @param   pDev                The PCI device.
 * @param   iRegion             The region to unmap.
 */
static int ich9pciUnmapRegion(PPDMPCIDEV pDev, int iRegion)
{
    PCIIORegion *pRegion = &pDev->Int.s.aIORegions[iRegion];
    AssertReturn(pRegion->size != 0, VINF_SUCCESS);

    int rc;
    if (pRegion->addr == INVALID_PCI_ADDRESS)
        rc = VINF_SUCCESS;
    else
    {
        if (pRegion->type & PCI_ADDRESS_SPACE_IO)
        {
            /* Port IO */
            rc = PDMDevHlpIOPortDeregister(pDev->Int.s.pDevInsR3, pRegion->addr, pRegion->size);
            AssertRC(rc);
        }
        else
        {
            PDEVPCIBUS pBus       = pDev->Int.s.CTX_SUFF(pBus);
            RTGCPHYS   GCPhysBase = pRegion->addr;
            if (pBus->pPciHlpR3->pfnIsMMIOExBase(pBus->pDevInsR3, pDev->Int.s.pDevInsR3, GCPhysBase))
            {
                /* unmap it. */
                rc = pRegion->map_func(pDev->Int.s.pDevInsR3, pDev, iRegion,
                                       NIL_RTGCPHYS, pRegion->size, (PCIADDRESSSPACE)(pRegion->type));
                AssertRC(rc);
                rc = PDMDevHlpMMIOExUnmap(pDev->Int.s.pDevInsR3, pDev, iRegion, GCPhysBase);
            }
            else
                rc = PDMDevHlpMMIODeregister(pDev->Int.s.pDevInsR3, GCPhysBase, pRegion->size);
            AssertRC(rc);
        }
        pRegion->addr = INVALID_PCI_ADDRESS;
    }
    return rc;
}


/**
 * Worker for devpciR3IsConfigByteWritable that update BAR and ROM mappings.
 *
 * @param   pDev                The PCI device to update the mappings for.
 */
static void devpciR3UpdateMappings(PPDMPCIDEV pPciDev)
{
    uint16_t const u16Cmd = ich9pciGetWord(pPciDev, VBOX_PCI_COMMAND);
    for (unsigned iRegion = 0; iRegion < VBOX_PCI_NUM_REGIONS; iRegion++)
    {
        PCIIORegion   *pRegion  = &pPciDev->Int.s.aIORegions[iRegion];
        uint64_t const cbRegion = pRegion->size;
        if (cbRegion != 0)
        {
            uint32_t const offCfgReg = ich9pciGetRegionReg(iRegion);
            bool const     f64Bit    =    (pRegion->type & ((uint8_t)(PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_IO)))
                                       == PCI_ADDRESS_SPACE_BAR64;
            uint64_t       uNew      = INVALID_PCI_ADDRESS;

            /*
             * Port I/O region. Check if mapped and within 1..65535 range.
             */
            if (pRegion->type & PCI_ADDRESS_SPACE_IO)
            {
                if (u16Cmd & PCI_COMMAND_IOACCESS)
                {
                    uint32_t uIoBase = ich9pciGetDWord(pPciDev, offCfgReg);
                    uIoBase &= ~(uint32_t)(cbRegion - 1);

                    uint64_t uLast = cbRegion - 1 + uIoBase;
                    if (   uLast < _64K
                        && uIoBase < uLast
                        && uIoBase > 0)
                        uNew = uIoBase;
                }
            }
            /*
             * MMIO or ROM.  Check ROM enable bit and range.
             *
             * Note! We exclude the I/O-APIC/HPET/ROM area at the end of the first 4GB to
             *       prevent the (fake) PCI BIOS and others from making a mess.  Pure paranoia.
             */
            else if (u16Cmd & PCI_COMMAND_MEMACCESS)
            {
                uint64_t uMemBase = ich9pciGetDWord(pPciDev, offCfgReg);
                if (f64Bit)
                {
                    Assert(iRegion < VBOX_PCI_ROM_SLOT);
                    uMemBase |= (uint64_t)ich9pciGetDWord(pPciDev, offCfgReg + 4) << 32;
                }
                if (   iRegion != PCI_ROM_SLOT
                    || (uMemBase & RT_BIT_32(0))) /* ROM enable bit. */
                {
                    uMemBase &= ~(cbRegion - 1);

                    uint64_t uLast = uMemBase + cbRegion - 1;
                    if (   uMemBase < uLast
                        && uMemBase > 0
                        && !(   uNew  <= UINT32_C(0xffffffff)
                             && uLast >= UINT32_C(0xfec00000)) )
                        uNew = uMemBase;
                }
            }

            /*
             * Do real unmapping and/or mapping if the address change.
             */
            if (uNew != pRegion->addr)
            {
                LogRel2(("PCI: config dev %u/%u (%s) BAR%i: %#RX64 -> %#RX64 (LB %RX64 (%RU64))\n",
                         pPciDev->uDevFn >> VBOX_PCI_DEVFN_DEV_SHIFT, pPciDev->uDevFn & VBOX_PCI_DEVFN_FUN_MASK,
                         pPciDev->pszNameR3, iRegion, pRegion->addr, uNew, cbRegion, cbRegion));

                ich9pciUnmapRegion(pPciDev, iRegion);
                pRegion->addr = uNew;
                if (uNew != INVALID_PCI_ADDRESS)
                {
                    int rc = pRegion->map_func(pPciDev->Int.s.pDevInsR3, pPciDev, iRegion, uNew, cbRegion,
                                               (PCIADDRESSSPACE)(pRegion->type));
                    AssertRC(rc);
                }
            }

            if (f64Bit)
                iRegion++;
        }
        /* else: size == 0: unused region */
    }
}


/**
 * Worker for devpciR3CommonDefaultConfigWrite that write a byte to a BAR.
 *
 * @param   pPciDev             The PCI device.
 * @param   iRegion             The region.
 * @param   off                 The BAR offset.
 * @param   bVal                The byte to write.
 */
DECLINLINE(void) devpciR3WriteBarByte(PPDMPCIDEV pPciDev, uint32_t iRegion, uint32_t off, uint8_t bVal)
{
    PCIIORegion *pRegion = &pPciDev->Int.s.aIORegions[iRegion];
    Log3(("devpciR3WriteBarByte: region=%d off=%d val=%#x size=%#llx\n", iRegion, off, bVal, pRegion->size));
    Assert(off <= 3);

    /* Check if we're writing to upper part of 64-bit BAR. */
    if (pRegion->type == 0xff)
    {
        AssertLogRelReturnVoid(iRegion > 0 && iRegion < VBOX_PCI_ROM_SLOT);
        pRegion--;
        iRegion--;
        off += 4;
        Assert(pRegion->type & PCI_ADDRESS_SPACE_BAR64);
    }

    /* Ignore zero sized regions (they don't exist). */
    if (pRegion->size != 0)
    {
        uint32_t uAddr = ich9pciGetRegionReg(iRegion) + off;
        Assert((pRegion->size & (pRegion->size - 1)) == 0); /* Region size must be power of two. */
        uint8_t bMask = ( (pRegion->size - 1) >> (off * 8) ) & 0xff;
        if (off == 0)
            bMask |= (pRegion->type & PCI_ADDRESS_SPACE_IO)
                   ? (1 << 2) - 1 /* 2 lowest bits for IO region */ :
                     (1 << 4) - 1 /* 4 lowest bits for memory region, also ROM enable bit for ROM region */;

        uint8_t bOld = PDMPciDevGetByte(pPciDev, uAddr) & bMask;
        bVal = (bOld & bMask) | (bVal & ~bMask);

        Log3(("devpciR3WriteBarByte: %x changed to  %x\n", bOld, bVal));

        PCIDevSetByte(pPciDev, uAddr, bVal);
    }
}


/**
 * Checks if the given configuration byte is writable.
 *
 * @returns true if writable, false if not
 * @param   uAddress            The config space byte byte.
 * @param   bHeaderType         The device header byte.
 */
DECLINLINE(bool) devpciR3IsConfigByteWritable(uint32_t uAddress, uint8_t bHeaderType)
{
    switch (bHeaderType)
    {
        case 0x00: /* normal device */
        case 0x80: /* multi-function device */
            switch (uAddress)
            {
                /* Read-only registers. */
                case VBOX_PCI_VENDOR_ID:
                case VBOX_PCI_VENDOR_ID+1:
                case VBOX_PCI_DEVICE_ID:
                case VBOX_PCI_DEVICE_ID+1:
                case VBOX_PCI_REVISION_ID:
                case VBOX_PCI_CLASS_PROG:
                case VBOX_PCI_CLASS_SUB:
                case VBOX_PCI_CLASS_BASE:
                case VBOX_PCI_HEADER_TYPE:
                case VBOX_PCI_SUBSYSTEM_VENDOR_ID:
                case VBOX_PCI_SUBSYSTEM_VENDOR_ID+1:
                case VBOX_PCI_SUBSYSTEM_ID:
                case VBOX_PCI_SUBSYSTEM_ID+1:
                case VBOX_PCI_ROM_ADDRESS:
                case VBOX_PCI_ROM_ADDRESS+1:
                case VBOX_PCI_ROM_ADDRESS+2:
                case VBOX_PCI_ROM_ADDRESS+3:
                case VBOX_PCI_CAPABILITY_LIST:
                case VBOX_PCI_INTERRUPT_PIN:
                    return false;
                /* Other registers can be written. */
                default:
                    return true;
            }
            break;
        case 0x01: /* PCI-PCI bridge */
            switch (uAddress)
            {
                /* Read-only registers. */
                case VBOX_PCI_VENDOR_ID:
                case VBOX_PCI_VENDOR_ID+1:
                case VBOX_PCI_DEVICE_ID:
                case VBOX_PCI_DEVICE_ID+1:
                case VBOX_PCI_REVISION_ID:
                case VBOX_PCI_CLASS_PROG:
                case VBOX_PCI_CLASS_SUB:
                case VBOX_PCI_CLASS_BASE:
                case VBOX_PCI_HEADER_TYPE:
                case VBOX_PCI_ROM_ADDRESS_BR:
                case VBOX_PCI_ROM_ADDRESS_BR+1:
                case VBOX_PCI_ROM_ADDRESS_BR+2:
                case VBOX_PCI_ROM_ADDRESS_BR+3:
                case VBOX_PCI_INTERRUPT_PIN:
                    return false;
                /* Other registers can be written. */
                default:
                    return true;
            }
            break;
        default:
            AssertMsgFailed(("Unknown header type %#x\n", bHeaderType));
            return false;
    }
}


/**
 * @callback_method_impl{PFNPCICONFIGWRITE,
 *      Default config space write callback.}
 *
 * See paragraph 7.5 of PCI Express specification (p. 349) for
 * definition of registers and their writability policy.
 */
DECLCALLBACK(void) devpciR3CommonDefaultConfigWrite(PPDMDEVINS pDevIns, PPDMPCIDEV pPciDev,
                                                    uint32_t uAddress, uint32_t u32Value, unsigned cb)
{
    NOREF(pDevIns);
    Assert(cb <= 4);

    if (uAddress + cb <= 256)
    {
        /*
         * MSI and MSI-X capabilites needs to be handled separately.
         */
        if (   pciDevIsMsiCapable(pPciDev)
            && uAddress - (uint32_t)pPciDev->Int.s.u8MsiCapOffset < (uint32_t)pPciDev->Int.s.u8MsiCapSize)
            MsiPciConfigWrite(pPciDev->Int.s.CTX_SUFF(pBus)->CTX_SUFF(pDevIns),
                              pPciDev->Int.s.CTX_SUFF(pBus)->CTX_SUFF(pPciHlp),
                              pPciDev, uAddress, u32Value, cb);
        else if (   pciDevIsMsixCapable(pPciDev)
                 && uAddress - (uint32_t)pPciDev->Int.s.u8MsixCapOffset < (uint32_t)pPciDev->Int.s.u8MsixCapSize)
            MsixPciConfigWrite(pPciDev->Int.s.CTX_SUFF(pBus)->CTX_SUFF(pDevIns),
                               pPciDev->Int.s.CTX_SUFF(pBus)->CTX_SUFF(pPciHlp),
                               pPciDev, uAddress, u32Value, cb);
        else
        {
            /*
             * Handle the writes byte-by-byte to catch all possible cases.
             *
             * Note! Real hardware may not necessarily handle non-dword writes like
             *       we do here and even produce erratic behavior.  We don't (yet)
             *       try emulate that.
             */
            uint8_t const   bHeaderType     = ich9pciGetByte(pPciDev, VBOX_PCI_HEADER_TYPE);
            bool const      fP2PBridge      = bHeaderType == 0x01; /* PCI-PCI bridge */
            bool            fUpdateMappings = false;
            while (cb-- > 0)
            {
                bool    fWritable = devpciR3IsConfigByteWritable(uAddress, bHeaderType);
                uint8_t bVal = (uint8_t)u32Value;
                bool    fRom = false;
                switch (uAddress)
                {
                    case VBOX_PCI_COMMAND: /* Command register, bits 0-7. */
                        if (fWritable)
                        {
                            PCIDevSetByte(pPciDev, uAddress, bVal);
                            fUpdateMappings = true; /** @todo r=bird: Probably not necessary to update mappings on VBOX_PCI_COMMAND changes for bridges? */
                        }
                        break;

                    case VBOX_PCI_COMMAND+1: /* Command register, bits 8-15. */
                        if (fWritable)
                        {
                            /* don't change reserved bits (11-15) */
                            bVal &= ~UINT8_C(0xf8);
                            PCIDevSetByte(pPciDev, uAddress, bVal);
                            fUpdateMappings = true;
                        }
                        break;

                    case VBOX_PCI_STATUS:  /* Status register, bits 0-7. */
                        /* don't change read-only bits => actually all lower bits are read-only */
                        bVal &= ~UINT8_C(0xff);
                        /* status register, low part: clear bits by writing a '1' to the corresponding bit */
                        pPciDev->abConfig[uAddress] &= ~bVal;
                        break;

                    case VBOX_PCI_STATUS+1:  /* Status register, bits 8-15. */
                        /* don't change read-only bits */
                        bVal &= ~UINT8_C(0x06);
                        /* status register, high part: clear bits by writing a '1' to the corresponding bit */
                        pPciDev->abConfig[uAddress] &= ~bVal;
                        break;

                    case VBOX_PCI_ROM_ADDRESS:    case VBOX_PCI_ROM_ADDRESS   +1: case VBOX_PCI_ROM_ADDRESS   +2: case VBOX_PCI_ROM_ADDRESS   +3:
                        fRom = true;
                    case VBOX_PCI_BASE_ADDRESS_0: case VBOX_PCI_BASE_ADDRESS_0+1: case VBOX_PCI_BASE_ADDRESS_0+2: case VBOX_PCI_BASE_ADDRESS_0+3:
                    case VBOX_PCI_BASE_ADDRESS_1: case VBOX_PCI_BASE_ADDRESS_1+1: case VBOX_PCI_BASE_ADDRESS_1+2: case VBOX_PCI_BASE_ADDRESS_1+3:
                    case VBOX_PCI_BASE_ADDRESS_2: case VBOX_PCI_BASE_ADDRESS_2+1: case VBOX_PCI_BASE_ADDRESS_2+2: case VBOX_PCI_BASE_ADDRESS_2+3:
                    case VBOX_PCI_BASE_ADDRESS_3: case VBOX_PCI_BASE_ADDRESS_3+1: case VBOX_PCI_BASE_ADDRESS_3+2: case VBOX_PCI_BASE_ADDRESS_3+3:
                    case VBOX_PCI_BASE_ADDRESS_4: case VBOX_PCI_BASE_ADDRESS_4+1: case VBOX_PCI_BASE_ADDRESS_4+2: case VBOX_PCI_BASE_ADDRESS_4+3:
                    case VBOX_PCI_BASE_ADDRESS_5: case VBOX_PCI_BASE_ADDRESS_5+1: case VBOX_PCI_BASE_ADDRESS_5+2: case VBOX_PCI_BASE_ADDRESS_5+3:
                        /* We check that, as same PCI register numbers as BARs may mean different registers for bridges */
                        if (!fP2PBridge)
                        {
                            uint32_t iRegion = fRom ? VBOX_PCI_ROM_SLOT : (uAddress - VBOX_PCI_BASE_ADDRESS_0) >> 2;
                            devpciR3WriteBarByte(pPciDev, iRegion, uAddress & 0x3, bVal);
                            fUpdateMappings = true;
                            break;
                        }
                        /* fall thru (bridge) */
                    default:
                        if (fWritable)
                            PCIDevSetByte(pPciDev, uAddress, bVal);
                        break;
                }
                uAddress++;
                u32Value >>= 8;
            }

            /*
             * Update the region mappings if anything changed related to them (command, BARs, ROM).
             */
            if (fUpdateMappings)
                devpciR3UpdateMappings(pPciDev);
        }
    }
    else if (uAddress + cb <= 4096)
        LogRel(("PCI: %8s/%u: Write to extended register %d fallen back to generic code\n",
                pPciDev->pszNameR3, pPciDev->Int.s.CTX_SUFF(pDevIns)->iInstance, uAddress));
    else
        AssertMsgFailed(("Write after end of PCI config space\n"));
}


/* -=-=-=-=-=- Debug Info Handlers -=-=-=-=-=- */

/**
 * Indents an info line.
 * @param   pHlp                The info helper.
 * @param   iIndentLvl          The desired indentation level.
 */
static void devpciR3InfoIndent(PCDBGFINFOHLP pHlp, unsigned iIndentLvl)
{
    for (unsigned i = 0; i < iIndentLvl; i++)
        pHlp->pfnPrintf(pHlp, "    ");
}


/**
 * Recursive worker for devpciR3InfoPci.
 *
 * @param   pBus                The bus to show info for.
 * @param   pHlp                The info helpers.
 * @param   iIndentLvl          The indentation level.
 * @param   fRegisters          Whether to show device registers or not.
 */
static void devpciR3InfoPciBus(PDEVPCIBUS pBus, PCDBGFINFOHLP pHlp, unsigned iIndentLvl, bool fRegisters)
{
/** @todo r=bird: Not sure if we should actually do PCI config space
 *                callbacks from this code!! */

    for (uint32_t iDev = 0; iDev < RT_ELEMENTS(pBus->apDevices); iDev++)
    {
        PPDMPCIDEV pPciDev = pBus->apDevices[iDev];
        if (pPciDev != NULL)
        {
            devpciR3InfoIndent(pHlp, iIndentLvl);

            /*
             * For passthrough devices MSI/MSI-X mostly reflects the way interrupts delivered to the guest,
             * as host driver handles real devices interrupts.
             */
            pHlp->pfnPrintf(pHlp, "%02x:%02x:%02x %s%s: %04x-%04x %s%s%s",
                            pBus->iBus, (iDev >> 3) & 0xff, iDev & 0x7,
                            pPciDev->pszNameR3,
                            pciDevIsPassthrough(pPciDev) ? " (PASSTHROUGH)" : "",
                            ich9pciGetWord(pPciDev, VBOX_PCI_VENDOR_ID), ich9pciGetWord(pPciDev, VBOX_PCI_DEVICE_ID),
                            pBus->fTypeIch9 ? "ICH9" : pBus->fTypePiix3 ? "PIIX3" : "?type?",
                            pciDevIsMsiCapable(pPciDev)  ? " MSI" : "",
                            pciDevIsMsixCapable(pPciDev) ? " MSI-X" : ""
                            );
            if (ich9pciGetByte(pPciDev, VBOX_PCI_INTERRUPT_PIN) != 0)
            {
                pHlp->pfnPrintf(pHlp, " IRQ%d", ich9pciGetByte(pPciDev, VBOX_PCI_INTERRUPT_LINE));
                pHlp->pfnPrintf(pHlp, " (INTA#->IRQ%d)", 0x10 + ich9pciSlot2ApicIrq(iDev >> 3, 0));
            }
            pHlp->pfnPrintf(pHlp, "\n");

            if (pciDevIsMsiCapable(pPciDev) || pciDevIsMsixCapable(pPciDev))
            {
                devpciR3InfoIndent(pHlp, iIndentLvl + 2);

                if (pciDevIsMsiCapable(pPciDev))
                    pHlp->pfnPrintf(pHlp, "MSI:%s ", MsiIsEnabled(pPciDev) ? "on" : "off");

                if (pciDevIsMsixCapable(pPciDev))
                    pHlp->pfnPrintf(pHlp, "MSI-X:%s ", MsixIsEnabled(pPciDev) ? "on" : "off");

                pHlp->pfnPrintf(pHlp, "\n");
            }

            uint16_t iCmd = ich9pciGetWord(pPciDev, VBOX_PCI_COMMAND);
            if ((iCmd & (VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY)) != 0)
            {
                for (unsigned iRegion = 0; iRegion < VBOX_PCI_NUM_REGIONS; iRegion++)
                {
                    PCIIORegion const *pRegion  = &pPciDev->Int.s.aIORegions[iRegion];
                    uint64_t const     cbRegion = pRegion->size;

                    if (cbRegion == 0)
                        continue;

                    uint32_t uAddr = ich9pciGetDWord(pPciDev, ich9pciGetRegionReg(iRegion));
                    const char * pszDesc;
                    char szDescBuf[128];

                    bool f64Bit =    (pRegion->type & ((uint8_t)(PCI_ADDRESS_SPACE_BAR64 | PCI_ADDRESS_SPACE_IO)))
                                  == PCI_ADDRESS_SPACE_BAR64;
                    if (pRegion->type & PCI_ADDRESS_SPACE_IO)
                    {
                        pszDesc = "IO";
                        uAddr &= ~0x3;
                    }
                    else
                    {
                        RTStrPrintf(szDescBuf, sizeof(szDescBuf), "MMIO%s%s",
                                    f64Bit ? "64" : "32",
                                    pRegion->type & PCI_ADDRESS_SPACE_MEM_PREFETCH ? " PREFETCH" : "");
                        pszDesc = szDescBuf;
                        uAddr &= ~0xf;
                    }

                    devpciR3InfoIndent(pHlp, iIndentLvl + 2);
                    pHlp->pfnPrintf(pHlp, "%s region #%u: ", pszDesc, iRegion);
                    if (f64Bit)
                    {
                        uint32_t u32High = ich9pciGetDWord(pPciDev, ich9pciGetRegionReg(iRegion+1));
                        uint64_t u64Addr = RT_MAKE_U64(uAddr, u32High);
                        pHlp->pfnPrintf(pHlp, "%RX64..%RX64\n", u64Addr, u64Addr + cbRegion);
                        iRegion++;
                    }
                    else
                        pHlp->pfnPrintf(pHlp, "%x..%x\n", uAddr, uAddr + (uint32_t)cbRegion);
                }
            }

            devpciR3InfoIndent(pHlp, iIndentLvl + 2);
            uint16_t iStatus = ich9pciGetWord(pPciDev, VBOX_PCI_STATUS);
            pHlp->pfnPrintf(pHlp, "Command: %04X, Status: %04x\n", iCmd, iStatus);
            devpciR3InfoIndent(pHlp, iIndentLvl + 2);
            pHlp->pfnPrintf(pHlp, "Bus master: %s\n", iCmd & VBOX_PCI_COMMAND_MASTER ? "Yes" : "No");

            if (fRegisters)
            {
                devpciR3InfoIndent(pHlp, iIndentLvl + 2);
                pHlp->pfnPrintf(pHlp, "PCI registers:\n");
                for (unsigned iReg = 0; iReg < 0x100; )
                {
                    unsigned iPerLine = 0x10;
                    Assert(0x100 % iPerLine == 0);
                    devpciR3InfoIndent(pHlp, iIndentLvl + 3);

                    while (iPerLine-- > 0)
                        pHlp->pfnPrintf(pHlp, "%02x ", ich9pciGetByte(pPciDev, iReg++));
                    pHlp->pfnPrintf(pHlp, "\n");
                }
            }
        }
    }

    if (pBus->cBridges > 0)
    {
        devpciR3InfoIndent(pHlp, iIndentLvl);
        pHlp->pfnPrintf(pHlp, "Registered %d bridges, subordinate buses info follows\n", pBus->cBridges);
        for (uint32_t iBridge = 0; iBridge < pBus->cBridges; iBridge++)
        {
            PDEVPCIBUS pBusSub = PDMINS_2_DATA(pBus->papBridgesR3[iBridge]->Int.s.CTX_SUFF(pDevIns), PDEVPCIBUS);
            devpciR3InfoPciBus(pBusSub, pHlp, iIndentLvl + 1, fRegisters);
        }
    }
}


/**
 * @callback_method_impl{FNDBGFHANDLERDEV, 'pci'}
 */
DECLCALLBACK(void) devpciR3InfoPci(PPDMDEVINS pDevIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    PDEVPCIBUS pBus = DEVINS_2_DEVPCIBUS(pDevIns);

    if (pszArgs == NULL || !*pszArgs || !strcmp(pszArgs, "basic"))
        devpciR3InfoPciBus(pBus, pHlp, 0 /*iIndentLvl*/, false /*fRegisters*/);
    else if (!strcmp(pszArgs, "verbose"))
        devpciR3InfoPciBus(pBus, pHlp, 0 /*iIndentLvl*/, true /*fRegisters*/);
    else
        pHlp->pfnPrintf(pHlp, "Invalid argument. Recognized arguments are 'basic', 'verbose'.\n");
}


/**
 * @callback_method_impl{FNDBGFHANDLERDEV, 'pciirq'}
 */
DECLCALLBACK(void) devpciR3InfoPciIrq(PPDMDEVINS pDevIns, PCDBGFINFOHLP pHlp, const char *pszArgs)
{
    PDEVPCIROOT pPciRoot = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);
    NOREF(pszArgs);

    pHlp->pfnPrintf(pHlp, "PCI I/O APIC IRQ levels:\n");
    for (int i = 0; i < DEVPCI_APIC_IRQ_PINS; ++i)
        pHlp->pfnPrintf(pHlp, "  IRQ%02d: %u\n", 0x10 + i, pPciRoot->auPciApicIrqLevels[i]);
}



static DECLCALLBACK(int) ich9pciConstruct(PPDMDEVINS pDevIns, int iInstance, PCFGMNODE  pCfg)
{
    RT_NOREF1(iInstance);
    Assert(iInstance == 0);
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);

    /*
     * Validate and read configuration.
     */
    if (!CFGMR3AreValuesValid(pCfg,
                              "IOAPIC\0"
                              "GCEnabled\0"
                              "R0Enabled\0"
                              "McfgBase\0"
                              "McfgLength\0"
                              ))
        return VERR_PDM_DEVINS_UNKNOWN_CFG_VALUES;

    /* query whether we got an IOAPIC */
    bool fUseIoApic;
    int rc = CFGMR3QueryBoolDef(pCfg, "IOAPIC", &fUseIoApic, false);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to query boolean value \"IOAPIC\""));

    /* check if RC code is enabled. */
    bool fGCEnabled;
    rc = CFGMR3QueryBoolDef(pCfg, "GCEnabled", &fGCEnabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to query boolean value \"GCEnabled\""));
    /* check if R0 code is enabled. */
    bool fR0Enabled;
    rc = CFGMR3QueryBoolDef(pCfg, "R0Enabled", &fR0Enabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to query boolean value \"R0Enabled\""));

    Log(("PCI: fUseIoApic=%RTbool fGCEnabled=%RTbool fR0Enabled=%RTbool\n", fUseIoApic, fGCEnabled, fR0Enabled));

    /*
     * Init data.
     */
    PDEVPCIROOT pPciRoot = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);
    PDEVPCIBUS     pBus     = &pPciRoot->PciBus;
    /* Zero out everything */
    memset(pPciRoot, 0, sizeof(*pPciRoot));
    /* And fill values */
    if (!fUseIoApic)
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Must use IO-APIC with ICH9 chipset"));
    rc = CFGMR3QueryU64Def(pCfg, "McfgBase", &pPciRoot->u64PciConfigMMioAddress, 0);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to read \"McfgBase\""));
    rc = CFGMR3QueryU64Def(pCfg, "McfgLength", &pPciRoot->u64PciConfigMMioLength, 0);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to read \"McfgLength\""));

    pPciRoot->fUseIoApic = fUseIoApic;
    pPciRoot->pDevInsR3 = pDevIns;
    pPciRoot->pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
    pPciRoot->pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);

    pPciRoot->PciBus.fTypePiix3  = false;
    pPciRoot->PciBus.fTypeIch9   = true;
    pPciRoot->PciBus.fPureBridge = false;
    pPciRoot->PciBus.pDevInsR3 = pDevIns;
    pPciRoot->PciBus.pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
    pPciRoot->PciBus.pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);
    pPciRoot->PciBus.papBridgesR3 = (PPDMPCIDEV *)PDMDevHlpMMHeapAllocZ(pDevIns, sizeof(PPDMPCIDEV) * RT_ELEMENTS(pPciRoot->PciBus.apDevices));
    AssertLogRelReturn(pPciRoot->PciBus.papBridgesR3, VERR_NO_MEMORY);

    /*
     * Register bus
     */
    PDMPCIBUSREG PciBusReg;
    PciBusReg.u32Version              = PDM_PCIBUSREG_VERSION;
    PciBusReg.pfnRegisterR3           = pciR3MergedRegister;
    PciBusReg.pfnRegisterMsiR3        = ich9pciRegisterMsi;
    PciBusReg.pfnIORegionRegisterR3   = devpciR3CommonIORegionRegister;
    PciBusReg.pfnSetConfigCallbacksR3 = devpciR3CommonSetConfigCallbacks;
    PciBusReg.pfnSetIrqR3             = ich9pciSetIrq;
    PciBusReg.pfnFakePCIBIOSR3        = ich9pciFakePCIBIOS;
    PciBusReg.pszSetIrqRC             = fGCEnabled ? "ich9pciSetIrq" : NULL;
    PciBusReg.pszSetIrqR0             = fR0Enabled ? "ich9pciSetIrq" : NULL;
    rc = PDMDevHlpPCIBusRegister(pDevIns, &PciBusReg, &pBus->pPciHlpR3);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Failed to register ourselves as a PCI Bus"));
    if (pBus->pPciHlpR3->u32Version != PDM_PCIHLPR3_VERSION)
        return PDMDevHlpVMSetError(pDevIns, VERR_VERSION_MISMATCH, RT_SRC_POS,
                                   N_("PCI helper version mismatch; got %#x expected %#x"),
                                   pBus->pPciHlpR3->u32Version, PDM_PCIHLPR3_VERSION);

    pBus->pPciHlpRC = pBus->pPciHlpR3->pfnGetRCHelpers(pDevIns);
    pBus->pPciHlpR0 = pBus->pPciHlpR3->pfnGetR0Helpers(pDevIns);

    /*
     * Fill in PCI configs and add them to the bus.
     */
    /** @todo Disabled for now because this causes error messages with Linux guests.
     *         The guest loads the x38_edac device which tries to map a memory region
     *         using an address given at place 0x48 - 0x4f in the PCi config space.
     *         This fails. because we don't register such a region.
     */
#if 0
    /* Host bridge device */
    PCIDevSetVendorId(  &pBus->PciDev, 0x8086); /* Intel */
    PCIDevSetDeviceId(  &pBus->PciDev, 0x29e0); /* Desktop */
    PCIDevSetRevisionId(&pBus->PciDev,   0x01); /* rev. 01 */
    PCIDevSetClassBase( &pBus->PciDev,   0x06); /* bridge */
    PCIDevSetClassSub(  &pBus->PciDev,   0x00); /* Host/PCI bridge */
    PCIDevSetClassProg( &pBus->PciDev,   0x00); /* Host/PCI bridge */
    PCIDevSetHeaderType(&pBus->PciDev,   0x00); /* bridge */
    PCIDevSetWord(&pBus->PciDev,  VBOX_PCI_SEC_STATUS, 0x0280);  /* secondary status */

    pBus->PciDev.pDevIns               = pDevIns;
    /* We register Host<->PCI controller on the bus */
    ich9pciRegisterInternal(pBus, 0, &pBus->PciDev, "dram");
#endif

    /*
     * Register I/O ports and save state.
     */
    rc = PDMDevHlpIOPortRegister(pDevIns, 0x0cf8, 1, NULL, ich9pciIOPortAddressWrite, ich9pciIOPortAddressRead, NULL, NULL, "ICH9 (PCI)");
    if (RT_FAILURE(rc))
        return rc;
    rc = PDMDevHlpIOPortRegister(pDevIns, 0x0cfc, 4, NULL, ich9pciIOPortDataWrite, ich9pciIOPortDataRead, NULL, NULL, "ICH9 (PCI)");
    if (RT_FAILURE(rc))
        return rc;
    if (fGCEnabled)
    {
        rc = PDMDevHlpIOPortRegisterRC(pDevIns, 0x0cf8, 1, NIL_RTGCPTR, "ich9pciIOPortAddressWrite", "ich9pciIOPortAddressRead", NULL, NULL, "ICH9 (PCI)");
        if (RT_FAILURE(rc))
            return rc;
        rc = PDMDevHlpIOPortRegisterRC(pDevIns, 0x0cfc, 4, NIL_RTGCPTR, "ich9pciIOPortDataWrite", "ich9pciIOPortDataRead", NULL, NULL, "ICH9 (PCI)");
        if (RT_FAILURE(rc))
            return rc;
    }
    if (fR0Enabled)
    {
        rc = PDMDevHlpIOPortRegisterR0(pDevIns, 0x0cf8, 1, NIL_RTR0PTR, "ich9pciIOPortAddressWrite", "ich9pciIOPortAddressRead", NULL, NULL, "ICH9 (PCI)");
        if (RT_FAILURE(rc))
            return rc;
        rc = PDMDevHlpIOPortRegisterR0(pDevIns, 0x0cfc, 4, NIL_RTR0PTR, "ich9pciIOPortDataWrite", "ich9pciIOPortDataRead", NULL, NULL, "ICH9 (PCI)");
        if (RT_FAILURE(rc))
            return rc;
    }

    if (pPciRoot->u64PciConfigMMioAddress != 0)
    {
        rc = PDMDevHlpMMIORegister(pDevIns, pPciRoot->u64PciConfigMMioAddress, pPciRoot->u64PciConfigMMioLength, NULL /*pvUser*/,
                                   IOMMMIO_FLAGS_READ_PASSTHRU | IOMMMIO_FLAGS_WRITE_PASSTHRU,
                                   ich9pciMcfgMMIOWrite, ich9pciMcfgMMIORead, "MCFG ranges");
        AssertMsgRCReturn(rc, ("rc=%Rrc %#llx/%#llx\n", rc,  pPciRoot->u64PciConfigMMioAddress, pPciRoot->u64PciConfigMMioLength), rc);

        if (fGCEnabled)
        {
            rc = PDMDevHlpMMIORegisterRC(pDevIns, pPciRoot->u64PciConfigMMioAddress, pPciRoot->u64PciConfigMMioLength,
                                         NIL_RTRCPTR /*pvUser*/, "ich9pciMcfgMMIOWrite", "ich9pciMcfgMMIORead");
            AssertRCReturn(rc, rc);
        }


        if (fR0Enabled)
        {
            rc = PDMDevHlpMMIORegisterR0(pDevIns, pPciRoot->u64PciConfigMMioAddress, pPciRoot->u64PciConfigMMioLength,
                                         NIL_RTR0PTR /*pvUser*/, "ich9pciMcfgMMIOWrite", "ich9pciMcfgMMIORead");
            AssertRCReturn(rc, rc);
        }
    }

    rc = PDMDevHlpSSMRegisterEx(pDevIns, VBOX_ICH9PCI_SAVED_STATE_VERSION_CURRENT,
                                sizeof(*pBus) + 16*128, "pgm",
                                NULL, NULL, NULL,
                                NULL, ich9pciR3SaveExec, NULL,
                                NULL, ich9pciR3LoadExec, NULL);
    if (RT_FAILURE(rc))
        return rc;


    /** @todo other chipset devices shall be registered too */

    PDMDevHlpDBGFInfoRegister(pDevIns, "pci",
                              "Display PCI bus status. Recognizes 'basic' or 'verbose' as arguments, defaults to 'basic'.",
                              devpciR3InfoPci);
    PDMDevHlpDBGFInfoRegister(pDevIns, "pciirq", "Display PCI IRQ state. (no arguments)", devpciR3InfoPciIrq);

    return VINF_SUCCESS;
}

static void ich9pciResetDevice(PPDMPCIDEV pDev)
{
    /* Clear regions */
    for (int iRegion = 0; iRegion < VBOX_PCI_NUM_REGIONS; iRegion++)
    {
        PCIIORegion* pRegion = &pDev->Int.s.aIORegions[iRegion];
        if (pRegion->size == 0)
            continue;

        ich9pciUnmapRegion(pDev, iRegion);
    }

    if (pciDevIsPassthrough(pDev))
    {
        // no reset handler - we can do what we need in PDM reset handler
        /// @todo is it correct?
    }
    else
    {
        PCIDevSetCommand(pDev,
                         PCIDevGetCommand(pDev)
                         &
                         ~(VBOX_PCI_COMMAND_IO | VBOX_PCI_COMMAND_MEMORY |
                           VBOX_PCI_COMMAND_MASTER | VBOX_PCI_COMMAND_SPECIAL |
                           VBOX_PCI_COMMAND_PARITY | VBOX_PCI_COMMAND_SERR |
                           VBOX_PCI_COMMAND_FAST_BACK | VBOX_PCI_COMMAND_INTX_DISABLE));

        /* Bridge device reset handlers processed later */
        if (!pciDevIsPci2PciBridge(pDev))
        {
            PCIDevSetByte(pDev, VBOX_PCI_CACHE_LINE_SIZE, 0x0);
            PCIDevSetInterruptLine(pDev, 0x0);
        }

        /* Reset MSI message control. */
        if (pciDevIsMsiCapable(pDev))
        {
            /* Extracted from MsiPciConfigWrite(). */
            pDev->abConfig[pDev->Int.s.u8MsiCapOffset + VBOX_MSI_CAP_MESSAGE_CONTROL] &= 0x8e;
        }

        /* Reset MSI-X message control. */
        if (pciDevIsMsixCapable(pDev))
        {
            /* Extracted from MsixPciConfigWrite(); no side effects. */
            pDev->abConfig[pDev->Int.s.u8MsixCapOffset + VBOX_MSIX_CAP_MESSAGE_CONTROL + 1] &= 0x3f;
        }
    }
}


/**
 * @copydoc FNPDMDEVRESET
 */
static DECLCALLBACK(void) ich9pciReset(PPDMDEVINS pDevIns)
{
    PDEVPCIROOT pPciRoot = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);
    PDEVPCIBUS     pBus     = &pPciRoot->PciBus;

    /* PCI-specific reset for each device. */
    for (uint32_t i = 0; i < RT_ELEMENTS(pBus->apDevices); i++)
    {
        if (pBus->apDevices[i])
            ich9pciResetDevice(pBus->apDevices[i]);
    }

    for (uint32_t iBridge = 0; iBridge < pBus->cBridges; iBridge++)
    {
        if (pBus->papBridgesR3[iBridge])
            ich9pcibridgeReset(pBus->papBridgesR3[iBridge]->Int.s.CTX_SUFF(pDevIns));
    }

    ich9pciFakePCIBIOS(pDevIns);
}


/**
 * @interface_method_impl{PDMDEVREG,pfnRelocate}
 */
DECLCALLBACK(void) devpciR3BusRelocate(PPDMDEVINS pDevIns, RTGCINTPTR offDelta)
{
    PDEVPCIBUS pBus = PDMINS_2_DATA(pDevIns, PDEVPCIBUS);

    pBus->pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);
    pBus->pPciHlpRC = pBus->pPciHlpR3->pfnGetRCHelpers(pDevIns);

    /* Relocate RC pointers for the attached pci devices. */
    for (uint32_t i = 0; i < RT_ELEMENTS(pBus->apDevices); i++)
    {
        PPDMPCIDEV pDev = pBus->apDevices[i];
        if (pDev)
        {
            pDev->Int.s.pBusRC += offDelta;
            if (pDev->Int.s.pMsixPageRC)
                pDev->Int.s.pMsixPageRC += offDelta;
        }
    }
}


/**
 * @interface_method_impl{PDMDEVREG,pfnRelocate}
 */
DECLCALLBACK(void) devpciR3RootRelocate(PPDMDEVINS pDevIns, RTGCINTPTR offDelta)
{
    PDEVPCIROOT pPciRoot = PDMINS_2_DATA(pDevIns, PDEVPCIROOT);
    pPciRoot->pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);

    AssertCompileMemberOffset(DEVPCIROOT, PciBus, 0);
    devpciR3BusRelocate(pDevIns, offDelta);
}


/**
 * @interface_method_impl{PDMDEVREG,pfnConstruct}
 */
static DECLCALLBACK(int)   ich9pcibridgeConstruct(PPDMDEVINS pDevIns,
                                                  int        iInstance,
                                                  PCFGMNODE  pCfg)
{
    PDMDEV_CHECK_VERSIONS_RETURN(pDevIns);

    /*
     * Validate and read configuration.
     */
    if (!CFGMR3AreValuesValid(pCfg, "GCEnabled\0" "R0Enabled\0"))
        return VERR_PDM_DEVINS_UNKNOWN_CFG_VALUES;

    /* check if RC code is enabled. */
    bool fGCEnabled;
    int rc = CFGMR3QueryBoolDef(pCfg, "GCEnabled", &fGCEnabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to query boolean value \"GCEnabled\""));

    /* check if R0 code is enabled. */
    bool fR0Enabled;
    rc = CFGMR3QueryBoolDef(pCfg, "R0Enabled", &fR0Enabled, true);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Configuration error: Failed to query boolean value \"R0Enabled\""));
    Log(("PCI: fGCEnabled=%RTbool fR0Enabled=%RTbool\n", fGCEnabled, fR0Enabled));

    /*
     * Init data and register the PCI bus.
     */
    PDEVPCIBUS pBus = PDMINS_2_DATA(pDevIns, PDEVPCIBUS);
    pBus->fTypePiix3  = false;
    pBus->fTypeIch9   = true;
    pBus->fPureBridge = true;
    pBus->pDevInsR3 = pDevIns;
    pBus->pDevInsR0 = PDMDEVINS_2_R0PTR(pDevIns);
    pBus->pDevInsRC = PDMDEVINS_2_RCPTR(pDevIns);
    pBus->papBridgesR3 = (PPDMPCIDEV *)PDMDevHlpMMHeapAllocZ(pDevIns, sizeof(PPDMPCIDEV) * RT_ELEMENTS(pBus->apDevices));
    AssertLogRelReturn(pBus->papBridgesR3, VERR_NO_MEMORY);

    PDMPCIBUSREG PciBusReg;
    PciBusReg.u32Version              = PDM_PCIBUSREG_VERSION;
    PciBusReg.pfnRegisterR3           = pcibridgeR3MergedRegisterDevice;
    PciBusReg.pfnRegisterMsiR3        = ich9pciRegisterMsi;
    PciBusReg.pfnIORegionRegisterR3   = devpciR3CommonIORegionRegister;
    PciBusReg.pfnSetConfigCallbacksR3 = devpciR3CommonSetConfigCallbacks;
    PciBusReg.pfnSetIrqR3             = ich9pcibridgeSetIrq;
    PciBusReg.pfnFakePCIBIOSR3        = NULL; /* Only needed for the first bus. */
    PciBusReg.pszSetIrqRC             = fGCEnabled ? "ich9pcibridgeSetIrq" : NULL;
    PciBusReg.pszSetIrqR0             = fR0Enabled ? "ich9pcibridgeSetIrq" : NULL;
    rc = PDMDevHlpPCIBusRegister(pDevIns, &PciBusReg, &pBus->pPciHlpR3);
    if (RT_FAILURE(rc))
        return PDMDEV_SET_ERROR(pDevIns, rc,
                                N_("Failed to register ourselves as a PCI Bus"));
    if (pBus->pPciHlpR3->u32Version != PDM_PCIHLPR3_VERSION)
        return PDMDevHlpVMSetError(pDevIns, VERR_VERSION_MISMATCH, RT_SRC_POS,
                                   N_("PCI helper version mismatch; got %#x expected %#x"),
                                   pBus->pPciHlpR3->u32Version, PDM_PCIHLPR3_VERSION);

    pBus->pPciHlpRC = pBus->pPciHlpR3->pfnGetRCHelpers(pDevIns);
    pBus->pPciHlpR0 = pBus->pPciHlpR3->pfnGetR0Helpers(pDevIns);

    /* Disable default device locking. */
    rc = PDMDevHlpSetDeviceCritSect(pDevIns, PDMDevHlpCritSectGetNop(pDevIns));
    AssertRCReturn(rc, rc);

    /*
     * Fill in PCI configs and add them to the bus.
     */
    PCIDevSetVendorId(  &pBus->PciDev, 0x8086); /* Intel */
    PCIDevSetDeviceId(  &pBus->PciDev, 0x2448); /* 82801 Mobile PCI bridge. */
    PCIDevSetRevisionId(&pBus->PciDev,   0xf2);
    PCIDevSetClassSub(  &pBus->PciDev,   0x04); /* pci2pci */
    PCIDevSetClassBase( &pBus->PciDev,   0x06); /* PCI_bridge */
    PCIDevSetClassProg( &pBus->PciDev,   0x01); /* Supports subtractive decoding. */
    PCIDevSetHeaderType(&pBus->PciDev,   0x01); /* Single function device which adheres to the PCI-to-PCI bridge spec. */
    PCIDevSetCommand(   &pBus->PciDev,   0x00);
    PCIDevSetStatus(    &pBus->PciDev,   0x20); /* 66MHz Capable. */
    PCIDevSetInterruptLine(&pBus->PciDev, 0x00); /* This device does not assert interrupts. */

    /*
     * This device does not generate interrupts. Interrupt delivery from
     * devices attached to the bus is unaffected.
     */
    PCIDevSetInterruptPin (&pBus->PciDev, 0x00);

    /*
     * Register this PCI bridge. The called function will take care on which bus we will get registered.
     */
    rc = PDMDevHlpPCIRegisterEx(pDevIns, &pBus->PciDev, PDMPCIDEVREG_CFG_PRIMARY, PDMPCIDEVREG_F_PCI_BRIDGE,
                                PDMPCIDEVREG_DEV_NO_FIRST_UNUSED, PDMPCIDEVREG_FUN_NO_FIRST_UNUSED, "ich9pcibridge");
    if (RT_FAILURE(rc))
        return rc;
    pBus->PciDev.Int.s.pfnBridgeConfigRead  = ich9pcibridgeConfigRead;
    pBus->PciDev.Int.s.pfnBridgeConfigWrite = ich9pcibridgeConfigWrite;

    /*
     * The iBus property doesn't really represent the bus number
     * because the guest and the BIOS can choose different bus numbers
     * for them.
     * The bus number is mainly for the setIrq function to indicate
     * when the host bus is reached which will have iBus = 0.
     * That's why the + 1.
     */
    pBus->iBus = iInstance + 1;

    /*
     * Register SSM handlers. We use the same saved state version as for the host bridge
     * to make changes easier.
     */
    rc = PDMDevHlpSSMRegisterEx(pDevIns, VBOX_ICH9PCI_SAVED_STATE_VERSION_CURRENT,
                                sizeof(*pBus) + 16*128,
                                "pgm" /* before */,
                                NULL, NULL, NULL,
                                NULL, ich9pcibridgeR3SaveExec, NULL,
                                NULL, ich9pcibridgeR3LoadExec, NULL);
    if (RT_FAILURE(rc))
        return rc;


    return VINF_SUCCESS;
}

/**
 * @copydoc FNPDMDEVRESET
 */
static void ich9pcibridgeReset(PPDMDEVINS pDevIns)
{
    PDEVPCIBUS pBus = PDMINS_2_DATA(pDevIns, PDEVPCIBUS);

    /* Reset config space to default values. */
    PCIDevSetByte(&pBus->PciDev, VBOX_PCI_PRIMARY_BUS, 0);
    PCIDevSetByte(&pBus->PciDev, VBOX_PCI_SECONDARY_BUS, 0);
    PCIDevSetByte(&pBus->PciDev, VBOX_PCI_SUBORDINATE_BUS, 0);

    /* PCI-specific reset for each device. */
    for (uint32_t i = 0; i < RT_ELEMENTS(pBus->apDevices); i++)
    {
        if (pBus->apDevices[i])
            ich9pciResetDevice(pBus->apDevices[i]);
    }
}



/**
 * The PCI bus device registration structure.
 */
const PDMDEVREG g_DevicePciIch9 =
{
    /* u32Version */
    PDM_DEVREG_VERSION,
    /* szName */
    "ich9pci",
    /* szRCMod */
    "VBoxDDRC.rc",
    /* szR0Mod */
    "VBoxDDR0.r0",
    /* pszDescription */
    "ICH9 PCI bridge",
    /* fFlags */
    PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_RC | PDM_DEVREG_FLAGS_R0,
    /* fClass */
    PDM_DEVREG_CLASS_BUS_PCI | PDM_DEVREG_CLASS_BUS_ISA,
    /* cMaxInstances */
    1,
    /* cbInstance */
    sizeof(DEVPCIROOT),
    /* pfnConstruct */
    ich9pciConstruct,
    /* pfnDestruct */
    NULL,
    /* pfnRelocate */
    devpciR3RootRelocate,
    /* pfnMemSetup */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    ich9pciReset,
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnQueryInterface */
    NULL,
    /* pfnInitComplete */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32VersionEnd */
    PDM_DEVREG_VERSION
};

/**
 * The device registration structure
 * for the PCI-to-PCI bridge.
 */
const PDMDEVREG g_DevicePciIch9Bridge =
{
    /* u32Version */
    PDM_DEVREG_VERSION,
    /* szName */
    "ich9pcibridge",
    /* szRCMod */
    "VBoxDDRC.rc",
    /* szR0Mod */
    "VBoxDDR0.r0",
    /* pszDescription */
    "ICH9 PCI to PCI bridge",
    /* fFlags */
    PDM_DEVREG_FLAGS_DEFAULT_BITS | PDM_DEVREG_FLAGS_RC | PDM_DEVREG_FLAGS_R0,
    /* fClass */
    PDM_DEVREG_CLASS_BUS_PCI,
    /* cMaxInstances */
    ~0U,
    /* cbInstance */
    sizeof(DEVPCIBUS),
    /* pfnConstruct */
    ich9pcibridgeConstruct,
    /* pfnDestruct */
    NULL,
    /* pfnRelocate */
    devpciR3BusRelocate,
    /* pfnMemSetup */
    NULL,
    /* pfnPowerOn */
    NULL,
    /* pfnReset */
    NULL, /* Must be NULL, to make sure only bus driver handles reset */
    /* pfnSuspend */
    NULL,
    /* pfnResume */
    NULL,
    /* pfnAttach */
    NULL,
    /* pfnDetach */
    NULL,
    /* pfnQueryInterface */
    NULL,
    /* pfnInitComplete */
    NULL,
    /* pfnPowerOff */
    NULL,
    /* pfnSoftReset */
    NULL,
    /* u32VersionEnd */
    PDM_DEVREG_VERSION
};

#endif /* IN_RING3 */

