/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm/desc.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <linux/bitmap.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "SimpleSvm_internal.h"

// Context definitions moved to SimpleSvm_internal.h

// Global devirtualization flag
// Set to 1 when module unload is requested, checked in VMEXIT handlers
atomic_t sv_should_exit = ATOMIC_INIT(0);

// Global devirtualized CPU counter
// Incremented by each CPU after successful devirtualization
atomic_t sv_devirt_count = ATOMIC_INIT(0);

// Global shared data pointer for cleanup during devirtualization
PSHARED_VIRTUAL_PROCESSOR_DATA g_sharedVpData = NULL;

//Patch
static DEFINE_PER_CPU(PVIRTUAL_PROCESSOR_DATA, ssvm_vpdata) = NULL;

// Global init_mm pointer (found via kprobe at module init)
static struct mm_struct *g_init_mm = NULL;

/*! =========================================================================
    @brief      Find init_mm symbol using kprobe trick.
    
    @details    init_mm is not exported in newer kernels. We use kprobe to
                find kallsyms_lookup_name, then use it to find init_mm.

				IRQ can't be closedd here
    ========================================================================= */
int SvFindInitMm(void)
{
	typedef unsigned long (*kallsyms_lookup_name_t)(const char *name);
	kallsyms_lookup_name_t kallsyms_lookup_name_func;
	
	struct kprobe kp = {
		.symbol_name = "kallsyms_lookup_name"
	};
	
	if (register_kprobe(&kp) < 0) {
		pr_err("[SimpleSvm] FATAL: Cannot find kallsyms_lookup_name!\n");
		return -EFAULT;
	}
	
	kallsyms_lookup_name_func = (kallsyms_lookup_name_t)kp.addr;
	unregister_kprobe(&kp);
	
	// Now find init_mm
	g_init_mm = (struct mm_struct *)kallsyms_lookup_name_func("init_mm");
	if (g_init_mm == NULL) {
		pr_err("[SimpleSvm] FATAL: Cannot find init_mm symbol!\n");
		return -EFAULT;
	}
	
	pr_info("[SimpleSvm] Found init_mm at %px, pgd PA=0x%llx\n", 
		g_init_mm, virt_to_phys(g_init_mm->pgd));
	
	return 0;
}

extern void SvCaptureContext(PSSVM_CONTEXT ctx);

// Get segment limit using the x86 LSL (Load Segment Limit) instruction
static inline u32 GetSegmentLimit(u16 selector)
{
	u32 limit = 0;
	u8 success;

	asm volatile("lsl %2, %0\n\t"
		     "setz %1\n\t"
		     : "=r"(limit), "=r"(success)
		     : "r"((u32)selector));

	if (!success)
		return 0xFFFFFFFF; // Default to max limit on failure

	return limit;
}

/*! =========================================================================
    @brief      Returns attributes of a segment specified by the segment selector.
    ========================================================================= */
static UINT16 GetSegmentAccessRight(_In_ UINT16 SegmentSelector, _In_ unsigned long GdtBase,
				    _In_ UINT16 GdtrLimit)
{
	PSEGMENT_DESCRIPTOR descriptor;
	SEGMENT_ATTRIBUTE attribute;
	u16 index;

	// Check Table Indicator bit (must be GDT)
	if (SegmentSelector & 0x4) { // TI=1 -> LDT not supported
		attribute.AsUInt16 = 0;
		return attribute.AsUInt16;
	}

	index = SegmentSelector & ~RPL_MASK;

	// Validate bounds INCLUDING descriptor size
	if (GdtBase == 0 || index == 0 || index > GdtrLimit ||
	    (GdtrLimit - index) < sizeof(SEGMENT_DESCRIPTOR)) {
		attribute.AsUInt16 = 0;
		return attribute.AsUInt16;
	}

	// Get a segment descriptor corresponds to the specified segment selector.
	descriptor = (PSEGMENT_DESCRIPTOR)(GdtBase + index);

	// Extract all attribute fields in the segment descriptor
	attribute.Fields.Type = descriptor->Fields.Type;
	attribute.Fields.System = descriptor->Fields.System;
	attribute.Fields.Dpl = descriptor->Fields.Dpl;
	attribute.Fields.Present = descriptor->Fields.Present;
	attribute.Fields.Avl = descriptor->Fields.Avl;
	attribute.Fields.LongMode = descriptor->Fields.LongMode;
	attribute.Fields.DefaultBit = descriptor->Fields.DefaultBit;
	attribute.Fields.Granularity = descriptor->Fields.Granularity;
	attribute.Fields.Reserved1 = 0;

	return attribute.AsUInt16;
}

   // Per-CPU flag for robust presence check during concurrent install
static DEFINE_PER_CPU(bool, ssvm_virtualized) = false;

static bool __maybe_unused IsHypervisorInstalled(void)
{
	if (this_cpu_read(ssvm_virtualized)) {
		/* NO LOGGING HERE Guest runs this with IRQs disabled inside VM.
		 * pr_info → spinlock → deadlock, or console MMIO → Crash */
		return true;
	}

	// Fallback: Use CPUID presence check (may not work if intercept broken)
	unsigned int eax, ebx, ecx, edx;

	pr_info("[SimpleSvm] CPU%d: IsHypervisorInstalled - checking CPUID 0x%x\n",
		smp_processor_id(), CPUID_CHECK_PRESENCE);

	cpuid(CPUID_CHECK_PRESENCE, &eax, &ebx, &ecx, &edx);

	pr_info("[SimpleSvm] CPU%d: CPUID result - EAX=0x%x (expected 0x%x)\n", smp_processor_id(),
		eax, CPUID_CHECK_PRESENCE);

	// If CPUID returns our magic value, we're virtualized
	if (eax == CPUID_CHECK_PRESENCE) {
		// Update the flag for consistency
		this_cpu_write(ssvm_virtualized, true);
		pr_info("[SimpleSvm] CPU%d: IsHypervisorInstalled - CPUID check SUCCESS\n",
			smp_processor_id());
		return true;
	}

	pr_info("[SimpleSvm] CPU%d: IsHypervisorInstalled - NOT virtualized\n", smp_processor_id());
	return false;
}

/*! =========================================================================
    @brief      Virtualizes the current processor.
    ========================================================================= */
static int PrepareForVirtualization(_Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
					 _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
					 _In_ const SSVM_CONTEXT *ContextRecord)
{
	u64 guestVmcbPa, hostVmcbPa, hostStateAreaPa, pml4BasePa, msrpmPa;
	u64 pat, efer;
	u64 cr0, cr2, cr3, cr4;

	// Minimum 6 GDT entries (0x2F = 47 bytes) for CS/DS/ES/SS (0x10, 0x18, 0x20, 0x28)
	if (ContextRecord->GdtrBase == 0 || ContextRecord->GdtrLimit < 0x2F) {
		pr_err("[SimpleSvm] CPU%d: Invalid GDTR! addr=0x%llx size=0x%x\n",
		       smp_processor_id(), ContextRecord->GdtrBase, ContextRecord->GdtrLimit);
		return -EFAULT;
	}

	// Validate IDTR to prevent descriptor table corruption
	// Minimum IDT size: 256 entries = 0xFFF bytes (Linux standard)
	if (ContextRecord->IdtrBase == 0 || ContextRecord->IdtrLimit < 0xFFF) {
		pr_err("[SimpleSvm] CPU%d: Invalid IDTR! addr=0x%llx size=0x%x (minimum 0xFFF)\n",
		       smp_processor_id(), ContextRecord->IdtrBase, ContextRecord->IdtrLimit);
		return -EFAULT;
	}

	// Check if NPT and MSRPM were built successfully
	if (SharedVpData->npt_ctx.pml4_pa == 0 || SharedVpData->MsrPermissionsMap == NULL) {
		pr_err("[SimpleSvm] CPU%d: NPT or MSRPM not ready! pml4_pa=0x%llx "
		       "MsrPermissionsMap=%p\n",
		       smp_processor_id(), SharedVpData->npt_ctx.pml4_pa,
		       SharedVpData->MsrPermissionsMap);
		return -EFAULT;
	}

	guestVmcbPa = ssvm_virt_to_phys(&VpData->GuestVmcb);
	hostVmcbPa = ssvm_virt_to_phys(&VpData->HostVmcb);
	hostStateAreaPa = ssvm_virt_to_phys(&VpData->HostStateArea);
	pml4BasePa = SharedVpData->npt_ctx.pml4_pa;
	msrpmPa = ssvm_virt_to_phys(SharedVpData->MsrPermissionsMap);

	// Debug: Log physical addresses
	pr_info("[SimpleSvm] CPU%d: GuestVmcb VA=%px PA=0x%llx\n", smp_processor_id(),
		&VpData->GuestVmcb, guestVmcbPa);
	pr_info("[SimpleSvm] CPU%d: HostVmcb VA=%px PA=0x%llx\n", smp_processor_id(),
		&VpData->HostVmcb, hostVmcbPa);
	pr_info("[SimpleSvm] CPU%d: HostStateArea VA=%px PA=0x%llx\n", smp_processor_id(),
		&VpData->HostStateArea, hostStateAreaPa);

	// Verify VIRTUAL ADDRESS alignment (struct member alignment)
	if (!IS_ALIGNED((unsigned long)&VpData->GuestVmcb, PAGE_SIZE)) {
		pr_err("[SimpleSvm] CPU%d: FATAL - GuestVmcb VA not 4KB aligned! VA=%px\n",
		       smp_processor_id(), &VpData->GuestVmcb);
		return -EFAULT;
	}
	if (!IS_ALIGNED((unsigned long)&VpData->HostVmcb, PAGE_SIZE)) {
		pr_err("[SimpleSvm] CPU%d: FATAL - HostVmcb VA not 4KB aligned! VA=%px\n",
		       smp_processor_id(), &VpData->HostVmcb);
		return -EFAULT;
	}
	if (!IS_ALIGNED((unsigned long)&VpData->HostStateArea, PAGE_SIZE)) {
		pr_err("[SimpleSvm] CPU%d: FATAL - HostStateArea VA not 4KB aligned! VA=%px\n",
		       smp_processor_id(), &VpData->HostStateArea);
		return -EFAULT;
	}


	if (guestVmcbPa == 0 || hostVmcbPa == 0 || hostStateAreaPa == 0 || pml4BasePa == 0 ||
	    msrpmPa == 0) {
		pr_err("[SimpleSvm] CPU%d: FATAL - Critical physical address is NULL!\n",
		       smp_processor_id());
		return -EFAULT;
	}

	if (!IS_ALIGNED(guestVmcbPa, PAGE_SIZE) || !IS_ALIGNED(hostVmcbPa, PAGE_SIZE)) {
		pr_err("[SimpleSvm] CPU%d: FATAL - VMCB lacks 4KB alignment!\n",
		       smp_processor_id());
		return -EFAULT;
	}

	// Configure intercepts using corrected bit positions (SimpleSvm.hpp)
	// InterceptMisc1 bits are relative to base 96 (word 5 of intercept bitmap)

	// CPUID intercept (bit 18 of InterceptMisc1)
	VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_CPUID;

	// MSR intercept (bit 28 of InterceptMisc1) - enables MSRPM
	VpData->GuestVmcb.ControlArea.InterceptMisc1 |= SVM_INTERCEPT_MISC1_MSR_PROT;

	
	// VpData->GuestVmcb.ControlArea.InterceptMisc1 |= (1UL << 0); // INTR intercept DISABLED
	
	// VMRUN intercept (bit 0 of InterceptMisc2)
	VpData->GuestVmcb.ControlArea.InterceptMisc2 |= SVM_INTERCEPT_MISC2_VMRUN;

	
	// ASID=1 for all CPUs causes TLB conflicts in multi-core systems!
	// Each CPU needs its own ASID to prevent TLB entry collisions.
	
	VpData->GuestVmcb.ControlArea.GuestAsid = smp_processor_id() + 1;

	// TLB Control: 0 = no flush (rely on ASID isolation)
	// With per-CPU ASID, we don't need TLB flush on every VMRUN
	VpData->GuestVmcb.ControlArea.TlbControl = 0;

	// Force clean=0 for first VMRUN — CPU must load all VMCB fields.
	VpData->GuestVmcb.ControlArea.VmcbClean = 0;

    // AMD APM Vol.2 S15.x: Non-zero InterruptShadow can cause VMEXIT_INVALID 
	VpData->GuestVmcb.ControlArea.InterruptShadow = 0;

	// IOPM is not used: IOIO_PROT is not set in InterceptMisc1, so
	// the IOPM is never consulted by hardware. Set IopmBasePa to 0.
	VpData->GuestVmcb.ControlArea.IopmBasePa = 0;

	VpData->GuestVmcb.ControlArea.MsrpmBasePa = msrpmPa;

	// Save original LSTAR (syscall entry point) for potential hooking.
	// Only save once (first CPU); LSTAR is system-wide.
	if (READ_ONCE(SharedVpData->OriginalLstar) == 0) {
		u64 lstar;
		rdmsrl(0xC0000082, lstar); // MSR_LSTAR
		// Atomically set to avoid race conditions across CPUs
		if (cmpxchg(&SharedVpData->OriginalLstar, 0ULL, lstar) == 0) {
			// We won the race, ensure global visibility with memory barrier
			smp_mb();
		}
	}

	// Enable Nested Page Tables
	VpData->GuestVmcb.ControlArea.NpEnable |= SVM_NP_ENABLE_NP_ENABLE;
	VpData->GuestVmcb.ControlArea.NCr3 = pml4BasePa;

	// Setup the initial guest state
	VpData->GuestVmcb.StateSaveArea.GdtrBase = ContextRecord->GdtrBase;
	VpData->GuestVmcb.StateSaveArea.GdtrLimit = ContextRecord->GdtrLimit;

	// Log bare-metal GDTR limit once — expect 0x7F on standard Linux (16 GDT entries).
	if (SharedVpData->OriginalLstar != 0) // only log on first CPU (OriginalLstar already set)
		SvDebugPrint("Bare-metal GDTR.Limit = 0x%x (expect 0x7F)\n",
			     ContextRecord->GdtrLimit);
	VpData->GuestVmcb.StateSaveArea.IdtrBase = ContextRecord->IdtrBase;
	VpData->GuestVmcb.StateSaveArea.IdtrLimit = ContextRecord->IdtrLimit;

	VpData->GuestVmcb.StateSaveArea.CsLimit = GetSegmentLimit(ContextRecord->SegCs);
	VpData->GuestVmcb.StateSaveArea.DsLimit = GetSegmentLimit(ContextRecord->SegDs);
	VpData->GuestVmcb.StateSaveArea.EsLimit = GetSegmentLimit(ContextRecord->SegEs);
	VpData->GuestVmcb.StateSaveArea.SsLimit = GetSegmentLimit(ContextRecord->SegSs);
	VpData->GuestVmcb.StateSaveArea.CsSelector = ContextRecord->SegCs;
	VpData->GuestVmcb.StateSaveArea.DsSelector = ContextRecord->SegDs;
	VpData->GuestVmcb.StateSaveArea.EsSelector = ContextRecord->SegEs;
	VpData->GuestVmcb.StateSaveArea.SsSelector = ContextRecord->SegSs;

	// In 64-bit mode, CS/SS/DS/ES base must be 0
	// Segment base'i ignore ediyor zaten
	VpData->GuestVmcb.StateSaveArea.CsBase = 0;
	VpData->GuestVmcb.StateSaveArea.SsBase = 0;
	VpData->GuestVmcb.StateSaveArea.DsBase = 0;
	VpData->GuestVmcb.StateSaveArea.EsBase = 0;

	// CS must have valid attributes (Present=1, System=1 required for code segment)
	u16 cs_attrib = GetSegmentAccessRight(ContextRecord->SegCs, ContextRecord->GdtrBase,
					      ContextRecord->GdtrLimit);
	if (cs_attrib == 0) {
		pr_err("[SimpleSvm] Failed to get CS attributes for selector 0x%x\n",
		       ContextRecord->SegCs);
		return -EFAULT;
	}
	VpData->GuestVmcb.StateSaveArea.CsAttrib = cs_attrib;

	// SS must have valid attributes (stack segment required)
	u16 ss_attrib = GetSegmentAccessRight(ContextRecord->SegSs, ContextRecord->GdtrBase,
					      ContextRecord->GdtrLimit);
	if (ss_attrib == 0) {
		pr_err("[SimpleSvm] Failed to get SS attributes for selector 0x%x\n",
		       ContextRecord->SegSs);
		return -EFAULT;
	}
	VpData->GuestVmcb.StateSaveArea.SsAttrib = ss_attrib;

	// DS/ES can be 0 (NULL selector is valid in 64-bit mode)
	VpData->GuestVmcb.StateSaveArea.DsAttrib = GetSegmentAccessRight(
	    ContextRecord->SegDs, ContextRecord->GdtrBase, ContextRecord->GdtrLimit);
	VpData->GuestVmcb.StateSaveArea.EsAttrib = GetSegmentAccessRight(
	    ContextRecord->SegEs, ContextRecord->GdtrBase, ContextRecord->GdtrLimit);

	rdmsrl(MSR_EFER, efer);
	cr0 = read_cr0();
	cr2 = read_cr2();
	cr3 = __read_cr3();
	cr4 = native_read_cr4(); /* NOT __read_cr4() shadow may lack PKE! */
	rdmsrl(MSR_IA32_CR_PAT, pat);

	u64 original_efer = efer;
	// Don't mask use host EFER as-is, just ensure SVME is set
	efer |= EFER_SVME; // Ensure SVME is set

	if (original_efer != efer) {
		pr_info("[SimpleSvm] CPU%d: Guest EFER set to 0x%llx (host EFER with SVME)\n",
			smp_processor_id(), efer);
	}

	
	pr_info("[SimpleSvm] CPU%d: Guest CR4 = 0x%llx (unmasked, same as host)\n",
		smp_processor_id(), cr4);

	// EFER: Ensure SVME bit is set in guest state.
	VpData->GuestVmcb.StateSaveArea.Efer = efer | EFER_SVME;
	VpData->GuestVmcb.StateSaveArea.Cr0 = cr0;
	VpData->GuestVmcb.StateSaveArea.Cr2 = cr2;
	VpData->GuestVmcb.StateSaveArea.Cr3 = cr3;
	VpData->GuestVmcb.StateSaveArea.Cr4 = cr4;


	VpData->GuestVmcb.StateSaveArea.Rflags = ContextRecord->EFlags;

	VpData->GuestVmcb.StateSaveArea.Rsp = ContextRecord->Rsp;
	VpData->GuestVmcb.StateSaveArea.Rip = ContextRecord->Rip;
	VpData->GuestVmcb.StateSaveArea.GPat = pat;

	// NOTE: Initialize guest RAX from captured context!
	// Guest code may use RAX immediately after resume (marker check, CPUID).
	// If RAX is not initialized, guest starts with garbage value → crash!
	VpData->GuestVmcb.StateSaveArea.Rax = ContextRecord->Rax;

	// FIX: Set CPL from CS selector RPL (AMD APM §15.14, offset 0xCB)
	VpData->GuestVmcb.StateSaveArea.Cpl = ContextRecord->SegCs & 0x3;

	// Debug registers: safe defaults
	VpData->GuestVmcb.StateSaveArea.Dr6 = 0xFFFF0FF0ULL;
	VpData->GuestVmcb.StateSaveArea.Dr7 = 0x00000400ULL;


	// FIX: Switch CR3 to init_mm.pgd BEFORE VMRUN!
	//
	// Problem: VMRUN (not VMSAVE) captures the current CR3 into VM_HSAVE_PA.
	// At this point, CR3 belongs to the insmod process. When insmod exits after
	// module_init returns, exit_mm() frees those page tables. But every #VMEXIT
	// restores the host CR3 from VM_HSAVE_PA — which now points to FREED memory.
	// Result: Host dereferences freed page tables → #PF → #DF → TRIPLE FAULT.
	//
	// Fix: Switch to init_mm.pgd (kernel's permanent page table) before VMRUN.
	// init_mm maps all of kernel space (ffff...) and is never freed.
	// The hypervisor code (SvRunVmLoop, SvHandleVmExit, safe stack) is all
	// in kernel space, so init_mm is sufficient for host operation.
	//
	// NOTE: g_init_mm is found via kprobe at module_init (IRQ enabled context).

	if (g_init_mm == NULL) {
		/* NO LOGGING - IRQs disabled! Just fail silently. */
		return -EFAULT;
	}
	
	u64 init_pgd_pa = virt_to_phys(g_init_mm->pgd);
	/* NO LOGGING - IRQs disabled! pr_emerg → spinlock → deadlock */
	native_write_cr3(init_pgd_pa);

	/* NO LOGGING - IRQs disabled! */
	asm volatile("vmsave %0" : : "a"(hostVmcbPa) : "memory");

	// Now use VMSAVE to capture current state into GUEST VMCB as well
	// AMD APM Vol.2 §15.15: VMSAVE saves FS/GS/TR/LDTR (selector+base+limit+attrib)
	// plus SYSCALL MSRs (STAR/LSTAR/CSTAR/SFMASK/KernelGsBase) and SYSENTER MSRs.
	// This captures the complete segment state for the guest.
	asm volatile("vmsave %0" : : "a"(guestVmcbPa) : "memory");

	// Initialize HostStackLayout fields for VMEXIT handler access.
	// These are placed at the top of the safe stack and accessed by
	// SvHandleCpuid (SharedVpData), devirtualization path, etc.
	VpData->HostStackLayout.GuestVmcbPa = guestVmcbPa;
	VpData->HostStackLayout.HostVmcbPa = hostVmcbPa;
	VpData->HostStackLayout.Self = VpData;
	VpData->HostStackLayout.SharedVpData = SharedVpData;

	// Set an address of the host state area to VM_HSAVE_PA MSR.
	/* NO LOGGING - IRQs disabled! */
	wrmsrl(SVM_MSR_VM_HSAVE_PA, hostStateAreaPa);

	/* NO LOGGING - IRQs disabled! */
	return 0;
}

/*! =========================================================================
    @brief      Virtualize the current processor.
    ========================================================================= */
int __attribute__((no_stack_protector)) VirtualizeProcessor(_In_opt_ void * Context)
{

pr_info("[SimpleSvm] sizeof VIRTUAL_PROCESSOR_DATA = %zu\n", sizeof(VIRTUAL_PROCESSOR_DATA));
    pr_info("[SimpleSvm] offsetof HostStateArea = %zu\n", offsetof(VIRTUAL_PROCESSOR_DATA, HostStateArea));
	
int status = 0;
	PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData;
	PVIRTUAL_PROCESSOR_DATA vpData = NULL;
	SSVM_CONTEXT contextRecord;
	u64 guestVmcbPa, hostVmcbPa, hostStateAreaPa, pml4BasePa, msrpmPa;

	if (Context == NULL) {
		return -EFAULT;
	}

     // Cast Context to sharedVpData immediately to prevent NULL pointer dereference
	sharedVpData = (PSHARED_VIRTUAL_PROCESSOR_DATA)Context;

	// Allocate per processor data with physically contiguous memory.
	// __get_free_pages for VMCB/HSAVE — hardware structures
	// MUST be physically contiguous.
	vpData =
	    (PVIRTUAL_PROCESSOR_DATA)SvAllocateContiguousMemory(sizeof(VIRTUAL_PROCESSOR_DATA));
	if (vpData == NULL) {
		SvDebugPrint("Insufficient memory.\n");
		status = -ENOMEM;
		goto Exit;
	}

	// Zero the entire structure to prevent VMCB garbage data!
	// __get_free_pages with __GFP_ZERO only zeros at page granularity.
	// VMCB structures MUST be fully zeroed or AMD hardware will fail
	memset(vpData, 0, sizeof(VIRTUAL_PROCESSOR_DATA));

	memset(&vpData->GuestVmcb, 0, sizeof(VMCB));
	memset(&vpData->HostVmcb, 0, sizeof(VMCB));
	memset(&vpData->HostStateArea, 0, PAGE_SIZE);

	pr_info("[SimpleSvm] CPU%d: VpData allocated at VA=%px size=%zu\n", smp_processor_id(),
		vpData, sizeof(VIRTUAL_PROCESSOR_DATA));


	guestVmcbPa = ssvm_virt_to_phys(&vpData->GuestVmcb);
	hostVmcbPa = ssvm_virt_to_phys(&vpData->HostVmcb);
	hostStateAreaPa = ssvm_virt_to_phys(&vpData->HostStateArea);
	pml4BasePa = sharedVpData->npt_ctx.pml4_pa;
	msrpmPa = ssvm_virt_to_phys(sharedVpData->MsrPermissionsMap);

	// Debug: Log physical addresses
	pr_info("[SimpleSvm] CPU%d: GuestVmcb VA=%px PA=0x%llx\n", smp_processor_id(),
		&vpData->GuestVmcb, guestVmcbPa);
	pr_info("[SimpleSvm] CPU%d: HostVmcb VA=%px PA=0x%llx\n", smp_processor_id(),
		&vpData->HostVmcb, hostVmcbPa);
	pr_info("[SimpleSvm] CPU%d: HostStateArea VA=%px PA=0x%llx\n", smp_processor_id(),
		&vpData->HostStateArea, hostStateAreaPa);

	// Clear aligned pointers since we're not using them
	vpData->GuestVmcbPtr = NULL;
	vpData->HostVmcbPtr = NULL;
	vpData->HostStateAreaPtr = NULL;

	// Disable preemption BEFORE CaptureContext()!
	preempt_disable();
	local_irq_disable();

	// Capture segments, GDTR, IDTR, callee-saved regs, RFLAGS
	// Experimental: Use DR1 as a per-CPU, compiler-safe marker
	asm volatile("mov %0, %%db1" ::"r"(0ULL));
	SvCaptureContext(&contextRecord);

	asm volatile("lea 1f(%%rip), %%rax\n\t"
		     "mov %%rax, %0\n\t"
		     "mov %%rsp, %1\n\t"
		     "mov %%rbp, %2\n\t"
		     "jmp 2f\n\t"
		     "1:\n\t"
		     /* GUEST RESUME POINT - Test CPUID intercept */
		     "mov $0x42424242, %%eax\n\t" /* CPUID_CHECK_PRESENCE */
		     "mov $0x42424242, %%ecx\n\t"
		     "cpuid\n\t"
		     /* CPUID done, continue to marker check */
		     "jmp 3f\n\t"
		     "2:\n\t"
		     : "=m"(contextRecord.Rip), "=m"(contextRecord.Rsp), "=m"(contextRecord.Rbp)
		     :
		     : "rax", "rcx", "rdx", "rbx", "memory", "cc");

	/* End of HOST PATH */
	asm volatile("3:\n\t" : : : "memory");

	/* Both host (after VMRUN loop) and guest (after devirt) arrive here */
	u64 dr1_marker;
	asm volatile("mov %%db1, %0" : "=r"(dr1_marker));

	if (dr1_marker != 0) {
		/* GUEST PATH */
		asm volatile("mov %0, %%db1" ::"r"(0ULL)); // Clean up DR1
		local_irq_enable();
		preempt_enable();
		return 0;
	}

	/* HOST PATH: First execution, prepare for virtualization */
	asm volatile("mov %0, %%db1" ::"r"(0xDEAD1337BEEF0001ULL));

	sharedVpData = (PSHARED_VIRTUAL_PROCESSOR_DATA)Context;

	SvDebugPrint("Attempting to virtualize the processor.\n");
		
	// Check for AMD CPU
	unsigned int eax, ebx, ecx, edx;
	cpuid(0, &eax, &ebx, &ecx, &edx);
	if (!(ebx == 0x68747541 && edx == 0x69746E65 && ecx == 0x444D4163)) {
		// Not "AuthenticAMD"
		pr_err("[SimpleSvm] CPU is not AMD - SVM not supported\n");
		this_cpu_write(ssvm_virtualized, false);
		SvFreeContiguousMemory(vpData, sizeof(VIRTUAL_PROCESSOR_DATA));
		local_irq_enable();
		preempt_enable();
		return -EFAULT;
	}

	// Check SVM support (CPUID.80000001h:ECX.SVM[bit 2])
	cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
	if (!(ecx & (1 << 2))) {
		pr_err("[SimpleSvm] SVM not supported by this CPU\n");
		this_cpu_write(ssvm_virtualized, false);
		SvFreeContiguousMemory(vpData, sizeof(VIRTUAL_PROCESSOR_DATA));
		local_irq_enable();
		preempt_enable();
		return -EFAULT;
	}

	// Check if SVM is disabled in BIOS
	u64 vm_cr;
	rdmsrl(SVM_MSR_VM_CR, vm_cr);
	if (vm_cr & (1 << 4)) { // SVMDIS bit
		pr_err("[SimpleSvm] SVM disabled by BIOS\n");
		this_cpu_write(ssvm_virtualized, false);
		SvFreeContiguousMemory(vpData, sizeof(VIRTUAL_PROCESSOR_DATA));
		local_irq_enable();
		preempt_enable();
		return -EFAULT;
	}

	// Enable SVM by setting EFER.SVME
	u64 efer_original;
	rdmsrl(MSR_EFER, efer_original);
	wrmsrl(MSR_EFER, efer_original | EFER_SVME);

	pr_info("[SimpleSvm] CPU%d: Calling PrepareForVirtualization\n", smp_processor_id());

	// Set up VMCB
	status = PrepareForVirtualization(vpData, sharedVpData, &contextRecord);

	if (status == 0) {
		pr_emerg("[SimpleSvm] CPU%d: PrepareForVirtualization SUCCESS\n",
			 smp_processor_id());

		/* DEBUG: Validate VMCB fields before VMRUN */
		pr_emerg("[SimpleSvm] CPU%d: VMCB Validation:\n", smp_processor_id());
		pr_emerg("  GuestVmcb PA: 0x%llx\n", ssvm_virt_to_phys(&vpData->GuestVmcb));
		pr_emerg("  HostVmcb PA: 0x%llx\n", ssvm_virt_to_phys(&vpData->HostVmcb));
		pr_emerg("  ASID: %u (must be >0)\n", vpData->GuestVmcb.ControlArea.GuestAsid);
		pr_emerg("  EFER: 0x%llx (SVME bit 12 = %d)\n",
			 vpData->GuestVmcb.StateSaveArea.Efer,
			 !!(vpData->GuestVmcb.StateSaveArea.Efer & EFER_SVME));
		pr_emerg("  CR0: 0x%llx\n", vpData->GuestVmcb.StateSaveArea.Cr0);
		pr_emerg("  CR3: 0x%llx\n", vpData->GuestVmcb.StateSaveArea.Cr3);
		pr_emerg("  CR4: 0x%llx\n", vpData->GuestVmcb.StateSaveArea.Cr4);
		pr_emerg("  NP_ENABLE: 0x%llx\n", (u64)vpData->GuestVmcb.ControlArea.NpEnable);
		pr_emerg("  NCR3: 0x%llx\n", vpData->GuestVmcb.ControlArea.NCr3);
		pr_emerg("  RIP: 0x%llx\n", vpData->GuestVmcb.StateSaveArea.Rip);
		pr_emerg("  RSP: 0x%llx\n", vpData->GuestVmcb.StateSaveArea.Rsp);
		pr_emerg("  RFLAGS: 0x%llx\n", vpData->GuestVmcb.StateSaveArea.Rflags);
		pr_emerg(
		    "  InterceptMisc1: 0x%x (CPUID=%d)\n",
		    vpData->GuestVmcb.ControlArea.InterceptMisc1,
		    !!(vpData->GuestVmcb.ControlArea.InterceptMisc1 & SVM_INTERCEPT_MISC1_CPUID));
		pr_emerg(
		    "  InterceptMisc2: 0x%x (VMRUN=%d)\n",
		    vpData->GuestVmcb.ControlArea.InterceptMisc2,
		    !!(vpData->GuestVmcb.ControlArea.InterceptMisc2 & SVM_INTERCEPT_MISC2_VMRUN));
		pr_emerg("  SafeStack: 0x%llx - 0x%llx\n", (u64)vpData->HostStackLimit,
			 (u64)(vpData->HostStackLimit + SVM_KERNEL_STACK_SIZE));

		/* Check host EFER too */
		u64 host_efer;
		rdmsrl(MSR_EFER, host_efer);
		pr_emerg("  Host EFER: 0x%llx (SVME bit 12 = %d)\n", host_efer,
			 !!(host_efer & EFER_SVME));

		pr_emerg("[SimpleSvm] CPU%d: About to enter VMRUN loop (last log before "
			 "IRQ disable)\n",
			 smp_processor_id());
	}

	if (status != 0) {
		wrmsrl(MSR_EFER, efer_original);

		SvFreeContiguousMemory(vpData, sizeof(VIRTUAL_PROCESSOR_DATA));
		local_irq_enable();
		preempt_enable();
		return status;
	}

	// Enter the VMRUN loop.
	// Must use SvCallOnStack to run on HostStackLimit!
	// Guest resumes from CaptureContext on the ORIGINAL kernel stack.
	// If hypervisor also runs on that stack, guest overwrites its frames
	// -> VMEXIT reads garbage from stack -> crash.
	// SvCallOnStack switches RSP to HostStackLimit (separate 16KB area)
	// then calls SvRunVmLoop. Guest can't touch this stack.
	extern void SvCallOnStack(void *safe_stack, PVIRTUAL_PROCESSOR_DATA vpData,
				  PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData,
				  PSSVM_CONTEXT contextRecord);
	SvCallOnStack((void *)(vpData->HostStackLimit + SVM_KERNEL_STACK_SIZE), vpData,
		      sharedVpData, &contextRecord);

	/* If SvRunVmLoop returns (VMRUN failed), re-enable interrupts 
	assembly only does stgi. We must enable IF here to prevent spinlock deadlock. */

	local_irq_enable();
	preempt_enable();

	pr_emerg("[SimpleSvm] CPU%d: RETURNED from SvRunVmLoop - SHOULD NEVER REACH\n",
		 smp_processor_id());

	/* Print debug flags to see where execution stopped */
	SvPrintDebugFlags();

	// NOTE: preempt_enable() is NOT called here because we never return!
	// The guest will handle preempt_count via the else block below.

	// SHOULD NEVER REACH HERE - host never returns from SvCallOnStack
	pr_err("[SimpleSvm] CPU%d: FATAL - Returned from SvCallOnStack!\n", smp_processor_id());
	preempt_enable();
	SvFreeContiguousMemory(vpData, sizeof(VIRTUAL_PROCESSOR_DATA));
	return -EFAULT;

Exit:
	if (vpData != NULL) {
		SvFreeContiguousMemory(vpData, sizeof(VIRTUAL_PROCESSOR_DATA));
	}
	return status;
}

/*! =========================================================================
    @brief      De-virtualize the current processor if virtualized.
    ========================================================================= */
int
DevirtualizeProcessor(_In_opt_ void * Context)
{
	unsigned int eax, ebx, ecx, edx;
	UINT64 high, low;
	PVIRTUAL_PROCESSOR_DATA vpData;
	PSHARED_VIRTUAL_PROCESSOR_DATA *sharedVpDataPtr;

	if (Context == NULL) {
		goto Exit;
	}

	// Ask SimpleSVM hypervisor to deactivate itself through back-door
	// Use inline assembly for exact __cpuidex mapping:
	eax = CPUID_UNLOAD_SIMPLE_SVM;
	ecx = CPUID_UNLOAD_SIMPLE_SVM;
	asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(eax), "c"(ecx));

	if (ecx != 0x5353564D) // 'SSVM'
	{
		goto Exit;
	}

	SvDebugPrint("The processor has been de-virtualized.\n");

	// Get an address of per processor data
	high = edx;
	low = eax;
	vpData = (PVIRTUAL_PROCESSOR_DATA)((high << 32) | low);

	// Validate reconstructed VpData pointer before dereference
	// Prevent arbitrary kernel pointer deref
	if (!vpData || !virt_addr_valid(vpData) || !PAGE_ALIGNED(vpData))
		goto Exit;

	// Safe dereference with fault handling
	void *temp_self;
	if (copy_from_kernel_nofault(&temp_self, &vpData->HostStackLayout.Self, sizeof(void *)) !=
	    0) {
		pr_err("[SimpleSvm] VpData Self field inaccessible: 0x%px\n", vpData);
		goto Exit;
	}

	if (temp_self != vpData) {
		pr_err("[SimpleSvm] VpData Self mismatch: expected 0x%px, got 0x%px\n", vpData,
		       temp_self);
		goto Exit;
	}

	// Save an address of shared data, then free per processor data.
	sharedVpDataPtr = (PSHARED_VIRTUAL_PROCESSOR_DATA *)Context;
	*sharedVpDataPtr = vpData->HostStackLayout.SharedVpData;
	this_cpu_write(ssvm_virtualized, false); // Clear per-CPU virtualized flag
	SvFreeContiguousMemory(vpData, sizeof(VIRTUAL_PROCESSOR_DATA));

Exit:
	return 0;
}

/*! =========================================================================
    @brief          Build the MSR permissions map (MSRPM).
    ========================================================================= */
void BuildMsrPermissionsMap(_Inout_ void * MsrPermissionsMap)
{
	unsigned long offsetFrom2ndBase, offset;
	unsigned long *bitmap = (unsigned long *)MsrPermissionsMap;

	// Clear all bits
	bitmap_zero(bitmap, SVM_MSR_PERMISSIONS_MAP_SIZE * 8);

	// Compute an offset for MSR_EFER in bits (write access).
	offsetFrom2ndBase = (MSR_EFER - 0xc0000000) * 2;
	offset = (0x800 * 8) + offsetFrom2ndBase;

	// Set the MSB bit indicating write accesses should be intercepted.
	set_bit(offset + 1, bitmap);

	//
	// Intercept LSTAR (0xC0000082) writes.
	// Read:  Allowed directly (fixes KASLR bypass prevention issue).
	// Write: Intercepted and routed to VMCB StateSaveArea.
	//
	offsetFrom2ndBase = (0xC0000082 - 0xc0000000) * 2;
	offset = (0x800 * 8) + offsetFrom2ndBase;
	set_bit(offset + 1, bitmap); // bit 1 = intercept WRMSR
}
