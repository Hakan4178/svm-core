/* SPDX-License-Identifier: GPL-2.0-only */

#include <asm/irqflags.h>
#include <asm/msr.h>
#include <asm/processor.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/smp.h>
#include <linux/workqueue.h>

#include "SimpleSvm_internal.h"

/* ═══ VMEXIT Diagnostic Counters ═══
 * Read via: cat /sys/module/simple_svm/parameters/vmexit_*
 * Non-atomic (approximate) — good enough for storm detection.
 */
static unsigned long vmexit_cpuid_count;
module_param(vmexit_cpuid_count, ulong, 0444);
MODULE_PARM_DESC(vmexit_cpuid_count, "Total VMEXIT_CPUID count");

static unsigned long vmexit_msr_count;
module_param(vmexit_msr_count, ulong, 0444);
MODULE_PARM_DESC(vmexit_msr_count, "Total VMEXIT_MSR count");

static unsigned long vmexit_intr_count;
module_param(vmexit_intr_count, ulong, 0444);
MODULE_PARM_DESC(vmexit_intr_count, "Total VMEXIT_INTR count (should be 0)");

static unsigned long vmexit_nmi_count;
module_param(vmexit_nmi_count, ulong, 0444);
MODULE_PARM_DESC(vmexit_nmi_count, "Total VMEXIT_NMI count (should be 0)");

static unsigned long vmexit_npf_count;
module_param(vmexit_npf_count, ulong, 0444);
MODULE_PARM_DESC(vmexit_npf_count, "Total VMEXIT_NPF count");

static unsigned long vmexit_vmrun_count;
module_param(vmexit_vmrun_count, ulong, 0444);
MODULE_PARM_DESC(vmexit_vmrun_count, "Total VMEXIT_VMRUN count");

static unsigned long vmexit_other_count;
module_param(vmexit_other_count, ulong, 0444);
MODULE_PARM_DESC(vmexit_other_count, "Total unhandled VMEXIT count");

static unsigned long vmexit_total_count;
module_param(vmexit_total_count, ulong, 0444);
MODULE_PARM_DESC(vmexit_total_count, "Total VMEXIT count all types");

/* ═══ Periodic Counter Dump Timer ═══
 * Runs every 1 second as guest workqueue (process context, IRQs enabled).
 */
static unsigned long vmexit_triple_fault_count = 0;


/* ═══ Early VMEXIT Ring Buffer Logger ═══
 *
 * Problem: pr_info in IF=0 atomic context can deadlock on console_lock.
 * _printk_deferred solves this but is not exported to modules.
 *
 * Solution: Record VMEXIT data to a per-CPU ring buffer (no locks,
 * no I/O, safe in atomic context). When buffer fills, schedule
 * work to a dedicated workqueue. Workqueue runs in process context
 * (IF=1, preemptible) where pr_info is completely safe. Mutex
 * serializes dumps across CPUs to prevent console lock contention.
 *
 * Flow: VMEXIT (IF=0) → write buf[idx] → idx++ → if full, queue_work()
 *       Workqueue (process context, IF=1) → mutex_lock → pr_info → mutex_unlock
 */
#define VMEXIT_LOG_SIZE 3 /* per-CPU entries (3 × 12 cores = 36 total logs) */

/* Global workqueue for VMEXIT log dumps (initialized in ssvm_main.c) */
struct workqueue_struct *vmexit_log_wq;

struct vmexit_log_entry {
	u64 exit_code;
	u64 rip;
	u64 rsp;
	u64 exit_info1;
	u64 exit_info2;
	u64 cr3;
};

/* Work struct with embedded CPU ID to handle workqueue migration */
struct vmexit_log_work_data {
	struct work_struct work;
	int cpu;
};

static DEFINE_PER_CPU(struct vmexit_log_entry[VMEXIT_LOG_SIZE], vmexit_log_buf);
static DEFINE_PER_CPU(unsigned int, vmexit_log_idx);
static DEFINE_PER_CPU(struct vmexit_log_work_data, vmexit_log_work);
static DEFINE_PER_CPU(bool, vmexit_log_work_init);
static DEFINE_PER_CPU(bool, vmexit_log_work_queued); /* Prevent duplicate queue_work */

/* Serialize dump across all CPUs to prevent console lock contention */
static DEFINE_MUTEX(vmexit_dump_mutex);

static void vmexit_log_dump_work(struct work_struct *work)
{
	struct vmexit_log_work_data *data = container_of(work, struct vmexit_log_work_data, work);
	int cpu = data->cpu;  /* Use embedded CPU ID, not smp_processor_id() */
	struct vmexit_log_entry *buf;
	unsigned int count;
	unsigned int i;

	/* This guarantees we see all buffer writes. */
	smp_rmb();

	buf = per_cpu_ptr(vmexit_log_buf, cpu);
	count = *per_cpu_ptr(&vmexit_log_idx, cpu);

	if (count > VMEXIT_LOG_SIZE)
		count = VMEXIT_LOG_SIZE;

	/* Serialize dump to prevent 12 CPUs fighting for console_lock */
	mutex_lock(&vmexit_dump_mutex);

	pr_info("[SimpleSvm] === VMEXIT LOG CPU%d (%u entries) ===\n", cpu, count);
	for (i = 0; i < count; i++) {
		pr_info("[SimpleSvm]   #%03u code=0x%-4llx RIP=0x%016llx "
			"RSP=0x%016llx info1=0x%llx info2=0x%llx CR3=0x%llx\n",
			i, buf[i].exit_code, buf[i].rip, buf[i].rsp,
			buf[i].exit_info1, buf[i].exit_info2, buf[i].cr3);
	}
	pr_info("[SimpleSvm] === END VMEXIT LOG CPU%d ===\n", cpu);

	mutex_unlock(&vmexit_dump_mutex);
	
	smp_wmb();
	*per_cpu_ptr(&vmexit_log_work_queued, cpu) = false;
}

/* Record one VMEXIT — lock-free, no I/O, safe in IF=0 context.
 * When buffer fills, schedule dump to workqueue (process context). */
static __always_inline void vmexit_log_record(PVIRTUAL_PROCESSOR_DATA VpData)
{
	unsigned int idx = __this_cpu_read(vmexit_log_idx);

	if (idx >= VMEXIT_LOG_SIZE)
		return; /* already full — hot path continues silently */

	/* Write entry to buffer */
	struct vmexit_log_entry *buf = this_cpu_ptr(vmexit_log_buf);
	buf[idx].exit_code  = VpData->GuestVmcb.ControlArea.ExitCode;
	buf[idx].rip        = VpData->GuestVmcb.StateSaveArea.Rip;
	buf[idx].rsp        = VpData->GuestVmcb.StateSaveArea.Rsp;
	buf[idx].exit_info1 = VpData->GuestVmcb.ControlArea.ExitInfo1;
	buf[idx].exit_info2 = VpData->GuestVmcb.ControlArea.ExitInfo2;
	buf[idx].cr3        = VpData->GuestVmcb.StateSaveArea.Cr3;
	
	/* Memory barrier to ensure buffer writes are visible
	 * to other CPUs before we increment idx and potentially queue work.
	 * Without this, the workqueue (running on another CPU) could read
	 * stale/garbage data from the buffer due to store buffer delays.
	 * 
	 * smp_wmb() ensures all preceding stores are globally visible
	 * before any subsequent stores (idx increment). */
	smp_wmb();

	/* Buffer just filled (idx == VMEXIT_LOG_SIZE-1, this is the last entry)
	 * Schedule dump to workqueue (process context).
	 * 
	 * Check flag and queue work BEFORE incrementing idx!
	 * If we increment first, then check flag and return, idx will be
	 * VMEXIT_LOG_SIZE but work was never queued → logs lost forever.
	 */
	if (idx + 1 == VMEXIT_LOG_SIZE) {
		struct vmexit_log_work_data *data = this_cpu_ptr(&vmexit_log_work);
		struct workqueue_struct *wq;
		
		if (!__this_cpu_read(vmexit_log_work_queued)) {
			if (!__this_cpu_read(vmexit_log_work_init)) {
				INIT_WORK(&data->work, vmexit_log_dump_work);
				data->cpu = smp_processor_id();  /* Embed CPU ID */
				__this_cpu_write(vmexit_log_work_init, true);
			}
			
			wq = READ_ONCE(vmexit_log_wq);
			
			/* Set queued flag BEFORE queue_work to prevent race.
			 * Memory barrier ensures flag write is visible before work execution. */
			if (wq) {
				__this_cpu_write(vmexit_log_work_queued, true);
				smp_wmb();
				queue_work(wq, &data->work);
			}
		}
	}

	/* Increment index atomically - AFTER work queueing decision.
	 * This ensures idx reaches VMEXIT_LOG_SIZE only after work is queued
	 * (or intentionally skipped if already queued). */
	__this_cpu_inc(vmexit_log_idx);
}

/*! =========================================================================
    @brief          Injects #GP with 0 of error code.
    ========================================================================= */
static void SvInjectGeneralProtectionException(_Inout_ PVIRTUAL_PROCESSOR_DATA VpData)
{
	EVENTINJ event;

	// Inject #GP(vector = 13, type = 3 = exception) with a valid error code.
	// An error code is always zero.
	event.AsUInt64 = 0;
	event.Fields.Vector = 13;
	event.Fields.Type = 3;
	event.Fields.ErrorCodeValid = 1;
	event.Fields.Valid = 1;
	VpData->GuestVmcb.ControlArea.EventInj = event.AsUInt64;
}

/*! =========================================================================
    @brief          Injects #PF.
    ========================================================================= */
static __maybe_unused void SvInjectPageFaultException(_Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
						      _In_ UINT64 ErrorCode,
						      _In_ UINT64 FaultAddress)
{
	EVENTINJ event;

	// Inject #PF (vector = 14, type = 3 = exception) with the provided error code.
	event.AsUInt64 = 0;
	event.Fields.Vector = 14;
	event.Fields.Type = 3;
	event.Fields.ErrorCodeValid = 1;
	event.Fields.ErrorCode = (UINT32)ErrorCode;
	event.Fields.Valid = 1;

	VpData->GuestVmcb.ControlArea.EventInj = event.AsUInt64;
	VpData->GuestVmcb.StateSaveArea.Cr2 = FaultAddress;
}

//deleted 0x01

/*! =========================================================================
    @brief          Handles #VMEXIT due to execution of the CPUID instructions.
    ========================================================================= */
static void SvHandleCpuid(_Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
			  _Inout_ PGUEST_CONTEXT GuestContext)
{
	unsigned int eax, ebx, ecx, edx;
	unsigned int leaf, subLeaf;
	SEGMENT_ATTRIBUTE attribute;
	PSHARED_VIRTUAL_PROCESSOR_DATA shared = VpData->HostStackLayout.SharedVpData;

	if (unlikely(atomic_read(&sv_should_exit))) {
		GuestContext->ExitVm = TRUE;
		return; // Skip CPUID processing, devirtualize immediately
	}

	// Execute CPUID as requested (bare-metal result).
	leaf = (unsigned int)VpData->GuestVmcb.StateSaveArea.Rax;
	subLeaf = (unsigned int)GuestContext->VpRegs->Rcx;
	cpuid_count(leaf, subLeaf, &eax, &ebx, &ecx, &edx);

	switch (leaf) {
	case CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS:
		
		ecx &= ~CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT;
		break;

	case CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS_EX:
		
		ecx &= ~CPUID_FN8000_0001_ECX_SVM;
		break;

//deleted 0x02

	case CPUID_REGISTER_GAME_CR3:			// 0x69696969
		if (subLeaf == CPUID_REGISTER_GAME_CR3) // Double-check leaf
		{
			// First-caller-wins: atomic compare-and-swap (P2.1)
			u64 guest_cr3 = VpData->GuestVmcb.StateSaveArea.Cr3;
			u64 old_cr3 = cmpxchg64(&shared->TargetCr3, 0, guest_cr3);

			if (old_cr3 == 0) {
				// We won the race and registered successfully
				WRITE_ONCE(shared->StealthEnabled, true);
				eax = 1;
			} else if (old_cr3 == guest_cr3) {
				// Same process re-registering (e.g. after fork) — allow.
				eax = 1;
			}
		}
		break;

	case CPUID_SET_TARGET_PID: // 0x1337
		// Only the registered game process can set its PID.
		{
			u64 target_cr3 = READ_ONCE(shared->TargetCr3);
			if (target_cr3 != 0 && VpData->GuestVmcb.StateSaveArea.Cr3 == target_cr3) {
				UINT64 pid = GuestContext->VpRegs->Rdx & 0xFFFFFFFF;
				if (pid != 0 && pid < 0x400000) // Sanitize: valid PID range
				{
					shared->TargetPid = pid;
					eax = 1;
				}
			}
		}
		break;

	case CPUID_STORE_GAME_DATA: // 0x336933
		// Only the registered game process can store data.
		{
			u64 target_cr3 = READ_ONCE(shared->TargetCr3);
			if (target_cr3 != 0 && VpData->GuestVmcb.StateSaveArea.Cr3 == target_cr3) {
				shared->GameData = GuestContext->VpRegs->Rcx;
				eax = 1;
			}
		}
		break;

	case CPUID_CHECK_PRESENCE: // 0x42424242
		// Internal presence check to verify we successfully virtualized
		if (subLeaf == CPUID_CHECK_PRESENCE) {
			eax = CPUID_CHECK_PRESENCE;
		}
		break;

	case CPUID_UNLOAD_SIMPLE_SVM: // 0x41414141
		if (subLeaf == CPUID_UNLOAD_SIMPLE_SVM) {
			// UNLOAD remains ring-0 ONLY — never allow userspace to devirtualize.
			attribute.AsUInt16 = VpData->GuestVmcb.StateSaveArea.SsAttrib;
			if (attribute.Fields.Dpl == DPL_SYSTEM) {
				// FIX: VMCB PA bounds check (max 64TB physical address limit for
				// typical x86_64)
				if (VpData->HostStackLayout.GuestVmcbPa >= (1ULL << 46)) {
					
					SvInjectGeneralProtectionException(VpData);
					return; // Abort unload
				}

				// FIX: Basic RSP guard check for unloaded guest
				if (VpData->GuestVmcb.StateSaveArea.Rsp < 0xFFFF800000000000ULL ||
				    (VpData->GuestVmcb.StateSaveArea.Rsp & 0xFFF) <=
					0x10) // Uncomfortably close to page bottom
				{

				}

				eax = (unsigned int)((UINT64)VpData & 0xFFFFFFFF);
				edx = (unsigned int)((UINT64)VpData >> 32);
				ecx = 0x5353564D; // 'SSVM' — devirtualization confirmed

				GuestContext->VpRegs->GuestRsp =
				    VpData->GuestVmcb.StateSaveArea.Rsp;
				GuestContext->VpRegs->GuestRip = VpData->GuestVmcb.ControlArea.NRip;
				GuestContext->VpRegs->GuestRflags =
				    VpData->GuestVmcb.StateSaveArea.Rflags;

				GuestContext->ExitVm = true;
			}
		}
		break;

	default:
	
		//
		// For every other leaf: pure passthrough, zero modification.
		//
		break;
	}

	// Update guest's GPRs with results.
	VpData->GuestVmcb.StateSaveArea.Rax = eax;
	GuestContext->VpRegs->Rbx = ebx;
	GuestContext->VpRegs->Rcx = ecx;
	GuestContext->VpRegs->Rdx = edx;

	/*
	 * Nrip maybe
	 */
	VpData->GuestVmcb.StateSaveArea.Rip += 2;  /* CPUID is always 2 bytes */
}

/*! =========================================================================
    @brief          Handles #VMEXIT due to execution of WRMSR and RDMSR.
    ========================================================================= */
static void SvHandleMsrAccess(_Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
			      _Inout_ PGUEST_CONTEXT GuestContext)
{
	u64 value;
	unsigned int msr;
	bool writeAccess;

	msr = (unsigned int)(GuestContext->VpRegs->Rcx & 0xFFFFFFFF);
	writeAccess = (VpData->GuestVmcb.ControlArea.ExitInfo1 != 0);

	if (msr == MSR_EFER) {
		// #VMEXIT on IA32_MSR_EFER access should only occur on write access.
		value = ((u64)(GuestContext->VpRegs->Rdx & 0xFFFFFFFF) << 32) |
			(VpData->GuestVmcb.StateSaveArea.Rax & 0xFFFFFFFF);

		if (unlikely((value & EFER_SVME) == 0)) {
			// Guest attempts to clear SVME bit (disable SVM).
			// This would break the hypervisor. Instead of injecting #GP
			// (which may cause triple fault), devirtualize gracefully.
			pr_warn("[SimpleSvm] CPU%d: Guest attempted to clear EFER.SVME. "
				"Devirtualizing.\n",
				smp_processor_id());
			// Advance RIP before devirtualization to prevent re-execution
			VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
			GuestContext->ExitVm = true;
			return;
		}

		VpData->GuestVmcb.StateSaveArea.Efer = value;
	} else if (msr == 0xC0000082) // MSR_LSTAR — syscall entry point
	{
		if (writeAccess) {
		
			value = ((u64)(GuestContext->VpRegs->Rdx & 0xFFFFFFFF) << 32) |
				(VpData->GuestVmcb.StateSaveArea.Rax & 0xFFFFFFFF);
			VpData->GuestVmcb.StateSaveArea.LStar = value;
		} else {
		
			value = VpData->GuestVmcb.StateSaveArea.LStar;
			VpData->GuestVmcb.StateSaveArea.Rax = (unsigned int)(value & 0xFFFFFFFF);
			GuestContext->VpRegs->Rdx = (unsigned int)(value >> 32);
		}
	} else {
		// All other intercepted MSRs: passthrough to real hardware.
		if (writeAccess != false) {
			value = ((u64)(GuestContext->VpRegs->Rdx & 0xFFFFFFFF) << 32) |
				(VpData->GuestVmcb.StateSaveArea.Rax & 0xFFFFFFFF);
			if (unlikely(wrmsr_safe(msr, (u32)value, (u32)(value >> 32)) != 0)) {
				pr_warn("[SimpleSvm] CPU%d: WRMSR 0x%x failed. "
					"Devirtualizing.\n",
					smp_processor_id(), msr);
				VpData->GuestVmcb.StateSaveArea.Rip =
				    VpData->GuestVmcb.ControlArea.NRip;
				GuestContext->ExitVm = true;
				return;
			}
		} else {
			u32 low, high;
			if (unlikely(rdmsr_safe(msr, &low, &high) != 0)) {
				pr_warn("[SimpleSvm] CPU%d: RDMSR 0x%x failed. "
					"Devirtualizing.\n",
					smp_processor_id(), msr);
				VpData->GuestVmcb.StateSaveArea.Rip =
				    VpData->GuestVmcb.ControlArea.NRip;
				GuestContext->ExitVm = true;
				return;
			}
			value = ((u64)high << 32) | low;
			VpData->GuestVmcb.StateSaveArea.Rax = (unsigned int)value;
			GuestContext->VpRegs->Rdx = (unsigned int)(value >> 32);
		}
	}

	// Advance RIP
	VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
}

/*! =========================================================================
    @brief          Handles #VMEXIT due to execution of VMRUN instruction.
    ========================================================================= */
static void SvHandleVmrun(_Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
			  _Inout_ PGUEST_CONTEXT GuestContext)
{
	
	pr_err("[SimpleSvm] CPU%d: Guest attempted VMRUN at RIP=0x%llx. Devirtualizing.\n",
	       smp_processor_id(), VpData->GuestVmcb.StateSaveArea.Rip);
	// Advance RIP before devirtualization to prevent re-execution
	VpData->GuestVmcb.StateSaveArea.Rip = VpData->GuestVmcb.ControlArea.NRip;
	GuestContext->ExitVm = true;
}


bool
SvHandleVmExit(_Inout_ PVIRTUAL_PROCESSOR_DATA VpData, _Inout_ PGUEST_REGISTERS GuestRegisters)
{
	GUEST_CONTEXT guestContext;

	guestContext.VpRegs = GuestRegisters;
	guestContext.ExitVm = false;

	/* HOT PATH — ZERO logging here!
	 * IRQs are disabled. pr_info/WARN_ON/dump_stack with 12 CPUs
	 * fighting for console_lock → hard lockup → NMI → triple fault. */

	/*
	 * NOTE: vmsave (guest state) and vmload (host state) are now both
	 * performed in ssvm_asm.S BEFORE this function is called.
	 * This is critical because Linux's stack canary reads %gs:0x28 at
	 * function entry — host GS base must be restored before any C code.
	 */

	/* Note: Guest RAX is in VMCB.StateSaveArea.Rax (hardware saved).
	 * Guest RSP is in VMCB.StateSaveArea.Rsp (hardware saved).
	 * Neither field exists in GUEST_REGISTERS anymore. */

	vmexit_total_count++;

	/* Record early VMEXITs to per-CPU ring buffer for later dump.
	 * Safe: no locks, no I/O — just memcpy to per-CPU memory.
	 * 
	 */

	if (likely(VpData->GuestVmcb.ControlArea.ExitCode != VMEXIT_NMI))
		vmexit_log_record(VpData);

	// Dispatch table
	switch (VpData->GuestVmcb.ControlArea.ExitCode) {
	case VMEXIT_CPUID:
		vmexit_cpuid_count++;
		SvHandleCpuid(VpData, &guestContext);
		break;
	case VMEXIT_MSR:
		vmexit_msr_count++;
		SvHandleMsrAccess(VpData, &guestContext);
		break;
	case VMEXIT_VMRUN:
		vmexit_vmrun_count++;
		SvHandleVmrun(VpData, &guestContext);
		break;

	/*
	 * Physical interrupt exits (INTR/NMI/SMI).
	 *
	 * NOTE: INTR intercept is DISABLED to avoid TLB shootdown deadlock.
	 * This case should never execute. If it does, something is wrong.
	 */
	case VMEXIT_INTR: // 0x60 — external interrupt (SHOULD NOT HAPPEN)
		vmexit_intr_count++;
		pr_warn_ratelimited("[SimpleSvm] CPU%d: Unexpected VMEXIT_INTR at RIP 0x%llx "
				    "(INTR intercept should be disabled!)\n",
				    smp_processor_id(), VpData->GuestVmcb.StateSaveArea.Rip);
		/* INTR intercept is disabled. If we're here, it's a bug.
		 * Just continue - STGI will deliver the interrupt. */
		break;
	case VMEXIT_NMI: // 0x61 — non-maskable interrupt
		vmexit_nmi_count++;
		pr_warn_ratelimited("[SimpleSvm] CPU%d: Unexpected VMEXIT_NMI at RIP 0x%llx "
				    "(NMI intercept should be disabled!)\n",
				    smp_processor_id(), VpData->GuestVmcb.StateSaveArea.Rip);
		break;
	case VMEXIT_SMI: // 0x62 — system management interrupt
		break;
	case VMEXIT_INIT: // 0x63 — INIT signal
		break;

	/*
	 * HLT instruction intercept
	 * This should NOT happen in normal operation 
	 */
	case VMEXIT_HLT: // 0x78 — HLT instruction
		pr_warn_ratelimited("[SimpleSvm] CPU%d: Unexpected VMEXIT_HLT at RIP 0x%llx "
				    "(guest fallback or idle?)\n",
				    smp_processor_id(), VpData->GuestVmcb.StateSaveArea.Rip);
		// Advance RIP past HLT instruction (1 byte)
		VpData->GuestVmcb.StateSaveArea.Rip += 1;
		break;

	case VMEXIT_INVALID: // -1 (0xFFFFFFFFFFFFFFFF) — VMCB consistency failure
		pr_err("[SimpleSvm] FATAL: VMEXIT_INVALID (VMCB consistency check failure)\n"
		       "  CPU       = %d\n"
		       "  ExitInfo1 = 0x%llx\n"
		       "  Guest RIP = 0x%llx\n"
		       "  Guest RSP = 0x%llx\n"
		       "  Guest EFER= 0x%llx\n"
		       "  Guest CR0 = 0x%llx\n"
		       "  Guest CR3 = 0x%llx\n"
		       "  Guest CR4 = 0x%llx\n",
		       smp_processor_id(), VpData->GuestVmcb.ControlArea.ExitInfo1,
		       VpData->GuestVmcb.StateSaveArea.Rip, VpData->GuestVmcb.StateSaveArea.Rsp,
		       VpData->GuestVmcb.StateSaveArea.Efer, VpData->GuestVmcb.StateSaveArea.Cr0,
		       VpData->GuestVmcb.StateSaveArea.Cr3, VpData->GuestVmcb.StateSaveArea.Cr4);
		// Don't panic - try to devirtualize gracefully
		guestContext.ExitVm = true;
		break;

	case VMEXIT_NPF:
		vmexit_npf_count++;
		pr_err("[SimpleSvm] CPU%d: #NPF - GPA=0x%llx ErrorCode=0x%llx RIP=0x%llx\n",
		       smp_processor_id(), VpData->GuestVmcb.ControlArea.ExitInfo2,
		       VpData->GuestVmcb.ControlArea.ExitInfo1,
		       VpData->GuestVmcb.StateSaveArea.Rip);
		guestContext.ExitVm = TRUE;
		break;

	case VMEXIT_SHUTDOWN:
		/* Guest triple-faulted inside the VM */
		vmexit_triple_fault_count++;
		pr_err("[SimpleSvm] Guest SHUTDOWN (triple fault). Devirtualizing CPU %d.\n",
		       smp_processor_id());
		pr_err("[SimpleSvm] Last RIP=0x%llx RSP=0x%llx CR3=0x%llx\n",
		       VpData->GuestVmcb.StateSaveArea.Rip, VpData->GuestVmcb.StateSaveArea.Rsp,
		       VpData->GuestVmcb.StateSaveArea.Cr3);
		guestContext.ExitVm = TRUE;
		break;

	default:
	
		vmexit_other_count++;
		pr_warn_ratelimited("[SimpleSvm] CPU%d: Unhandled VMEXIT 0x%llx at RIP 0x%llx "
		       "ExitInfo1=0x%llx ExitInfo2=0x%llx\n",
		       smp_processor_id(), VpData->GuestVmcb.ControlArea.ExitCode,
		       VpData->GuestVmcb.StateSaveArea.Rip,
		       VpData->GuestVmcb.ControlArea.ExitInfo1,
		       VpData->GuestVmcb.ControlArea.ExitInfo2);
		// Do NOT set ExitVm=TRUE - just continue
		break;
	}

	// Terminate SimpleSvm if requested.
	if (unlikely(guestContext.ExitVm != FALSE)) {
		pr_info("[SimpleSvm] CPU%d: ExitVm=TRUE - Devirtualizing\n", smp_processor_id());
		return TRUE; /* Signal C loop to break and devirtualize */
	}

	pr_debug("[SimpleSvm] CPU%d: VMEXIT handled, returning to guest\n", smp_processor_id());

	return guestContext.ExitVm;
}

/*! =========================================================================
    @brief      Bare-metal style VMRUN loop (C wrapper).

    @details    Calls assembly SvLaunchVm in a loop. Assembly handles
		INTR/NMI/SMI fast-path entirely. Returns to this C code
		only for complex exits (CPUID, MSR, NPF, etc.)

		vmsave/vmload are done lazily here — only on slow-path
		exits that actually need C code.
    ========================================================================= */
/* Global volatile flags for debugging without logging */
static volatile u64 debug_svrunvmloop_entered = 0;
static volatile u64 debug_svrunvmloop_loop_start = 0;
static volatile u64 debug_svlaunchvm_called = 0;
static volatile u64 debug_svlaunchvm_returned = 0;

/* Assembly entry flag - written by SvLaunchVm first instruction */
static volatile u64 debug_asm_entered = 0;

/* Assembly stage markers - written after each critical SVM instruction */
static volatile u64 debug_post_vmexit = 0; /* After vmrun (VMEXIT happened) */
static volatile u64 debug_post_vmsave = 0; /* After vmsave guest */
static volatile u64 debug_post_vmload = 0; /* After vmload host */
static volatile u64 debug_vmrun_rsp = 0;   /* RSP at vmrun time (should be safe stack) */

/* Per-CPU debug storage for last VMEXIT, NPF, etc. */

/* Function to print debug flags - call from IRQ-enabled context */
void SvPrintDebugFlags(void)
{
	pr_emerg("[SimpleSvm] Debug flags: entered=0x%llx loop_start=0x%llx called=0x%llx "
		 "returned=0x%llx asm_entered=0x%llx\n",
		 debug_svrunvmloop_entered, debug_svrunvmloop_loop_start, debug_svlaunchvm_called,
		 debug_svlaunchvm_returned, debug_asm_entered);
	pr_emerg("[SimpleSvm] ASM stages: post_vmexit=0x%llx post_vmsave=0x%llx post_vmload=0x%llx "
		 "vmrun_rsp=0x%llx\n",
		 debug_post_vmexit, debug_post_vmsave, debug_post_vmload, debug_vmrun_rsp);
}


void SvRunVmLoop(_Inout_ PVIRTUAL_PROCESSOR_DATA VpData,
		 _In_ PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData, _In_ PSSVM_CONTEXT ContextRecord)
{
	GUEST_REGISTERS regs;
	u64 vmcb_pa = ssvm_virt_to_phys(&VpData->GuestVmcb);
	u64 host_vmcb_pa = ssvm_virt_to_phys(&VpData->HostVmcb);
	VMCB *vmcb = &VpData->GuestVmcb;
	bool exitVm;

	/* No log here
	 * We're called with local_irq_disable() + preempt_disable().
	 */

	/* Set flag to prove we entered */
	debug_svrunvmloop_entered = 0xDEADBEEF;

	/* Initialize guest registers
	 * 
	 * Initialize ALL registers from ContextRecord
	 * 
	 * We must preserve EXACT CPU state (all registers) for transparent
	 * virtualization. Even though x86-64 ABI allows caller-saved registers
	 * to be clobbered across function calls, we're not doing a function call
	 * we're virtualizing the entire CPU state.
	 */

	// Callee-saved registers (RBX, RBP, R12-R15)
	regs.Rbx = ContextRecord->Rbx;
	regs.Rbp = ContextRecord->Rbp;
	regs.R12 = ContextRecord->R12;
	regs.R13 = ContextRecord->R13;
	regs.R14 = ContextRecord->R14;
	regs.R15 = ContextRecord->R15;
	
	// Caller-saved registers (RCX, RDX, RSI, RDI, R8-R11)
	// RAX is not initialized here it lives in VMCB.StateSaveArea.Rax(hardware restored)
	regs.Rcx = ContextRecord->Rcx;
	regs.Rdx = ContextRecord->Rdx;
	regs.Rsi = ContextRecord->Rsi;
	regs.Rdi = ContextRecord->Rdi;
	regs.R8 = ContextRecord->R8;
	regs.R9 = ContextRecord->R9;
	regs.R10 = ContextRecord->R10;
	regs.R11 = ContextRecord->R11;
	
	// Initialize it from CaptureContext snapshot
	vmcb->StateSaveArea.Rax = ContextRecord->Rax;
	
	/* Devirtualization fields - will be set by CPUID_UNLOAD handler */
	regs.GuestRsp = 0;
	regs.GuestRip = 0;
	regs.GuestRflags = 0;

	debug_svrunvmloop_loop_start = 0xCAFEBABE;

	for (;;) {
		/* Reset VMCB fields before each VMRUN */
		vmcb->ControlArea.VmcbClean = 0;
		vmcb->ControlArea.InterruptShadow = 0;

		/* NO LOGGING - we're in IRQ-disabled context with spinlock risk */

		debug_svlaunchvm_called = 0xBEEFCAFE;

		/*
		 * Call SvLaunchVm - returns ExitCode in RAX.
		 */
		u64 exitCode = SvLaunchVm(vmcb_pa, &regs, vmcb, host_vmcb_pa);
		debug_svlaunchvm_returned = exitCode;

		/*
		 * SvLaunchVm returned with:
		 *   - All guest GPRs saved to regs struct
		 *   - Host GS.base restored via vmload host_vmcb
		 *   - GIF=1
		 *   - EFLAGS.IF=0 
		 *
		 * Safe to use per-cpu data and kernel functions now.
		 *
		 * We're back in C. Guest state was already saved and host state
		 * restored by the assembly loop BEFORE stgi, ensuring safe execution.
		 */

		/* Dispatch the exit in C */
		exitVm = SvHandleVmExit(VpData, &regs);

		if (exitVm) {
			/* Populate devirt state from VMCB for SvTeardownVm.
			 * CPUID_UNLOAD already set these (with RIP+2 to skip CPUID).
			 * For other exits (NPF, INVALID, SHUTDOWN), use VMCB directly.
			 * Without this, GuestRsp=0 → SvTeardownVm skipped → broken devirt. */
			if (regs.GuestRsp == 0) {
				regs.GuestRsp = vmcb->StateSaveArea.Rsp;
				regs.GuestRip = vmcb->StateSaveArea.Rip;
				regs.GuestRflags = vmcb->StateSaveArea.Rflags;
			}
			break;
		}
	}

	/*
	 * Devirtualization:
	 * If guest never ran successfully (first VMRUN failed), GuestRsp/GuestRip
	 * are 0 (from memset). We MUST return to caller, not SvTeardownVm.
	 */
	if (regs.GuestRsp == 0 || regs.GuestRip == 0) {
		pr_err(
		    "[SimpleSvm] CPU%d: VMRUN failed (no valid guest state), returning to caller\n",
		    smp_processor_id());
		return; /* SvCallOnStack handles this error path */
	}

	/*
	 * Normal devirtualization:
	 * We MUST NOT return to the caller (VirtualizeProcessor), because the
	 * guest already returned from it long ago. Returning now would "time travel"
	 * and execute the original caller a second time with a corrupted stack.
	 *
	 * Instead, we use SvTeardownVm to JMP directly to the guest's RIP/RSP
	 * that we captured during the CPUID_UNLOAD VMEXIT.
	 */

	/* Restore original LSTAR if hooked */
	if (SharedVpData->OriginalLstar != 0)
		vmcb->StateSaveArea.LStar = SharedVpData->OriginalLstar;

	 
	atomic_inc(&sv_devirt_count);

	/*
	 * NOTE: Disable interrupts before vmload!
	 *
	 * vmload restores guest GS_BASE. If an interrupt arrives after vmload
	 * but before we jump to guest, the interrupt handler will use guest
	 * GS_BASE to access per-CPU data (e.g., stack canary at %gs:0x28).
	 * This causes data corruption or panic.
	 *
	 * NMI Window
	 * Between vmload and wrmsrl(EFER), an NMI can arrive. NMI is NOT
	 * maskable by cli (IF=0). If the NMI handler uses %gs (e.g., this_cpu_*
	 * macros, stack canary check), it will access guest GS_BASE → corruption.
	 *
	 * Mitigation: Keep window as small as possible. It will be safer in the future.
	 */
	local_irq_disable();

	/* clgi BEFORE vmload to protect from NMI during segment load.
	 * vmload loads guest GS_BASE NMI with wrong GS would crash. */
	asm volatile("clgi" ::: "memory");

	/* Restore guest segment state to CPU */
	asm volatile("vmload %0" : : "a"(vmcb_pa) : "memory");

	/* NOTE stgi BEFORE clearing SVME!
	 * STGI requires SVME=1. If we clear SVME with GIF=0, GIF stays 0
	 * forever all interrupts permanently blocked → triple fault.
	 */

	asm volatile("stgi" ::: "memory");

	/* Disable SVM GIF=1 now, interrupts behave normally after this */
	{
		u64 efer;
		rdmsrl(MSR_EFER, efer);
		wrmsrl(MSR_EFER, efer & ~EFER_SVME);
	}

	/* Teardown the hypervisor and JMP directly into the guest. */
	extern void SvTeardownVm(PGUEST_REGISTERS regs, u64 guest_rax);
	SvTeardownVm(&regs, vmcb->StateSaveArea.Rax);
	__builtin_unreachable();
}
STACK_FRAME_NON_STANDARD(SvRunVmLoop);
