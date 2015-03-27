/* $Id$ */
/** @file
 * GIM - KVM, Internal header file.
 */

/*
 * Copyright (C) 2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___GIMKvmInternal_h
#define ___GIMKvmInternal_h

#include <VBox/vmm/gim.h>
#include <VBox/vmm/cpum.h>


/** @name KVM base features.
 * @{
 */
/** Old, deprecated clock source available. */
#define GIM_KVM_BASE_FEAT_CLOCK_OLD                RT_BIT(0)
/** No need for artifical delays on IO operations. */
#define GIM_KVM_BASE_FEAT_NOP_IO_DELAY             RT_BIT(1)
/** MMU op supported (deprecated, unused). */
#define GIM_KVM_BASE_FEAT_MMU_OP                   RT_BIT(2)
/** Clock source available. */
#define GIM_KVM_BASE_FEAT_CLOCK                    RT_BIT(3)
/** Asynchronous page faults supported. */
#define GIM_KVM_BASE_FEAT_ASYNC_PF                 RT_BIT(4)
/** Steal time (VCPU not executing guest code time in ns) available. */
#define GIM_KVM_BASE_FEAT_STEAL_TIME               RT_BIT(5)
/** Paravirtualized EOI (end-of-interrupt) supported. */
#define GIM_KVM_BASE_FEAT_PV_EOI                   RT_BIT(6)
/** Paravirtualized spinlock (unhalting VCPU) supported. */
#define GIM_KVM_BASE_FEAT_PV_UNHALT                RT_BIT(7)
/** The TSC is stable (fixed rate, monotonic). */
#define GIM_KVM_BASE_FEAT_TSC_STABLE               RT_BIT(24)
/** @}  */


/** @name KVM MSRs.
 * @{
 */
/** Start of range 0. */
#define MSR_GIM_KVM_RANGE0_START                   UINT32_C(0x11)
/** Old, deprecated wall clock. */
#define MSR_GIM_KVM_WALL_CLOCK_OLD                 UINT32_C(0x11)
/** Old, deprecated System time. */
#define MSR_GIM_KVM_SYSTEM_TIME_OLD                UINT32_C(0x12)
/** End of range 0. */
#define MSR_GIM_KVM_RANGE0_END                     MSR_GIM_KVM_SYSTEM_TIME_OLD

/** Start of range 1. */
#define MSR_GIM_KVM_RANGE1_START                   UINT32_C(0x4b564d00)
/** Wall clock. */
#define MSR_GIM_KVM_WALL_CLOCK                     UINT32_C(0x4b564d00)
/** System time. */
#define MSR_GIM_KVM_SYSTEM_TIME                    UINT32_C(0x4b564d01)
/** Asynchronous page fault. */
#define MSR_GIM_KVM_ASYNC_PF                       UINT32_C(0x4b564d02)
/** Steal time. */
#define MSR_GIM_KVM_STEAL_TIME                     UINT32_C(0x4b564d03)
/** Paravirtualized EOI (end-of-interrupt). */
#define MSR_GIM_KVM_EOI                            UINT32_C(0x4b564d04)
/** End of range 1. */
#define MSR_GIM_KVM_RANGE1_END                     MSR_GIM_KVM_EOI

AssertCompile(MSR_GIM_KVM_RANGE0_START <= MSR_GIM_KVM_RANGE0_END);
AssertCompile(MSR_GIM_KVM_RANGE1_START <= MSR_GIM_KVM_RANGE1_END);

/** KVM page size.  */
#define GIM_KVM_PAGE_SIZE                          0x1000

/**
 * MMIO2 region indices.
 */
/** The system time page(s) region. */
#define GIM_KVM_SYSTEM_TIME_PAGE_REGION_IDX        UINT8_C(0)
/** The steal time page(s) region. */
#define GIM_KVM_STEAL_TIME_PAGE_REGION_IDX         UINT8_C(1)
/** The maximum region index (must be <= UINT8_MAX). */
#define GIM_KVM_REGION_IDX_MAX                     GIM_KVM_STEAL_TIME_PAGE_REGION_IDX

/**
 * KVM system-time structure (GIM_KVM_SYSTEM_TIME_FLAGS_XXX) flags.
 * See "Documentation/virtual/kvm/api.txt".
 */
/** The TSC is stable (monotonic). */
#define GIM_KVM_SYSTEM_TIME_FLAGS_TSC_STABLE       RT_BIT(0)
/** The guest VCPU has been paused by the hypervisor. */
#define GIM_KVM_SYSTEM_TIME_FLAGS_GUEST_PAUSED     RT_BIT(1)
/** */

/** @name KVM MSR - System time (MSR_GIM_KVM_SYSTEM_TIME and
 * MSR_GIM_KVM_SYSTEM_TIME_OLD).
 * @{
 */
/** The system-time enable bit. */
#define MSR_GIM_KVM_SYSTEM_TIME_ENABLE_BIT        RT_BIT_64(0)
/** Whether the system-time struct. is enabled or not. */
#define MSR_GIM_KVM_SYSTEM_TIME_IS_ENABLED(a)     RT_BOOL((a) & MSR_GIM_KVM_SYSTEM_TIME_ENABLE_BIT)
/** Guest-physical address of the system-time struct. */
#define MSR_GIM_KVM_SYSTEM_TIME_GUEST_GPA(a)      ((a) & ~MSR_GIM_KVM_SYSTEM_TIME_ENABLE_BIT)
/** @} */

/** @name KVM MSR - Wall clock (MSR_GIM_KVM_WALL_CLOCK and
 * MSR_GIM_KVM_WALL_CLOCK_OLD).
 * @{
 */
/** Guest-physical address of the wall-clock struct. */
#define MSR_GIM_KVM_WALL_CLOCK_GUEST_GPA(a)      (a)
/** @} */


/**
 * KVM per-VCPU system-time structure.
 */
typedef struct GIMKVMSYSTEMTIME
{
    /** Version (sequence number). */
    uint32_t        u32Version;
    /** Alignment padding. */
    uint32_t        u32Padding0;
    /** TSC time stamp.  */
    uint64_t        u64Tsc;
    /** System time in nanoseconds. */
    uint64_t        u64NanoTS;
    /** TSC to system time scale factor. */
    uint32_t        u32TscScale;
    /** TSC frequency shift.  */
    int8_t          i8TscShift;
    /** Clock source (GIM_KVM_SYSTEM_TIME_FLAGS_XXX) flags. */
    uint8_t         fFlags;
    /** Alignment padding. */
    uint8_t         abPadding0[2];
} GIMKVMSYSTEMTIME;
/** Pointer to KVM system-time struct. */
typedef GIMKVMSYSTEMTIME *PGIMKVMSYSTEMTIME;
/** Pointer to a const KVM system-time struct. */
typedef GIMKVMSYSTEMTIME const *PCGIMKVMSYSTEMTIME;
AssertCompileSize(GIMKVMSYSTEMTIME, 32);


/**
 * KVM per-VM wall-clock structure.
 */
typedef struct GIMKVMWALLCLOCK
{
    /** Version (sequence number). */
    uint32_t        u32Version;
    /** Number of seconds since boot. */
    uint32_t        u32Sec;
    /** Number of nanoseconds since boot. */
    uint32_t        u32Nano;
} GIMKVMWALLCLOCK;
/** Pointer to KVM wall-clock struct. */
typedef GIMKVMWALLCLOCK *PGIMKVMWALLCLOCK;
/** Pointer to a const KVM wall-clock struct. */
typedef GIMKVMWALLCLOCK const *PCGIMKVMWALLCLOCK;
AssertCompileSize(GIMKVMWALLCLOCK, 12);


/**
 * GIM KVMV VM instance data.
 * Changes to this must checked against the padding of the gim union in VM!
 */
typedef struct GIMKVM
{
    /** @name MSRs. */
    /** Wall-clock MSR. */
    uint64_t                    u64WallClockMsr;
    /** @} */

    /** @name CPUID features. */
    /** Basic features. */
    uint32_t                    uBaseFeat;
    /** @} */
} GIMKVM;
/** Pointer to per-VM GIM KVM instance data. */
typedef GIMKVM *PGIMKVM;
/** Pointer to const per-VM GIM KVM instance data. */
typedef GIMKVM const *PCGIMKVM;

/**
 * GIM KVMV VCPU instance data.
 * Changes to this must checked against the padding of the gim union in VMCPU!
 */
typedef struct GIMKVMCPU
{
    /** System-time MSR. */
    uint64_t                    u64SystemTimeMsr;
    /** The guest-physical address of the system-time struct. */
    RTGCPHYS                    GCPhysSystemTime;
    /** The version (sequence number) of the system-time struct. */
    uint32_t                    u32SystemTimeVersion;
    /** The guest TSC value while enabling the system-time MSR. */
    uint64_t                    uTsc;
    /** The guest virtual time while enabling the system-time MSR. */
    uint64_t                    uVirtNanoTS;
} GIMKVMCPU;
/** Pointer to per-VCPU GIM KVM instance data. */
typedef GIMKVMCPU *PGIMKVMCPU;
/** Pointer to const per-VCPU GIM KVM instance data. */
typedef GIMKVMCPU const *PCGIMKVMCPU;


RT_C_DECLS_BEGIN

#ifdef IN_RING0
#if 0
VMMR0_INT_DECL(int)             gimR0KvmInitVM(PVM pVM);
VMMR0_INT_DECL(int)             gimR0KvmTermVM(PVM pVM);
VMMR0_INT_DECL(int)             gimR0KvmUpdateParavirtTsc(PVM pVM, uint64_t u64Offset);
#endif
#endif /* IN_RING0 */

#ifdef IN_RING3
VMMR3_INT_DECL(int)             gimR3KvmInit(PVM pVM);
VMMR3_INT_DECL(int)             gimR3KvmInitCompleted(PVM pVM);
VMMR3_INT_DECL(int)             gimR3KvmTerm(PVM pVM);
VMMR3_INT_DECL(void)            gimR3KvmRelocate(PVM pVM, RTGCINTPTR offDelta);
VMMR3_INT_DECL(void)            gimR3KvmReset(PVM pVM);
VMMR3_INT_DECL(int)             gimR3KvmSave(PVM pVM, PSSMHANDLE pSSM);
VMMR3_INT_DECL(int)             gimR3KvmLoad(PVM pVM, PSSMHANDLE pSSM, uint32_t uSSMVersion);

VMMR3_INT_DECL(int)             gimR3KvmDisableSystemTime(PVM pVM);
VMMR3_INT_DECL(int)             gimR3KvmEnableSystemTime(PVM pVM, PVMCPU pVCpu, PGIMKVMCPU pKvmCpu, uint8_t fFlags);
VMMR3_INT_DECL(int)             gimR3KvmEnableWallClock(PVM pVM, RTGCPHYS GCPhysSysTime, uint32_t uVersion);
#endif /* IN_RING3 */

VMM_INT_DECL(bool)              gimKvmIsParavirtTscEnabled(PVM pVM);
VMM_INT_DECL(bool)              gimKvmAreHypercallsEnabled(PVMCPU pVCpu);
VMM_INT_DECL(int)               gimKvmHypercall(PVMCPU pVCpu, PCPUMCTX pCtx);
VMM_INT_DECL(VBOXSTRICTRC)      gimKvmReadMsr(PVMCPU pVCpu, uint32_t idMsr, PCCPUMMSRRANGE pRange, uint64_t *puValue);
VMM_INT_DECL(VBOXSTRICTRC)      gimKvmWriteMsr(PVMCPU pVCpu, uint32_t idMsr, PCCPUMMSRRANGE pRange, uint64_t uRawValue);

RT_C_DECLS_END

#endif
