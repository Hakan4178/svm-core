#ifndef _SIMPLESVM_INTERNAL_H_
#define _SIMPLESVM_INTERNAL_H_

#include <asm/io.h>
#include <linux/bug.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

// ============================================================================
//  Linux Kernel Type Definitions
// ============================================================================
typedef u8 UINT8;
typedef u16 UINT16;
typedef u32 UINT32;
typedef u64 UINT64;


#ifndef TRUE
#define TRUE true
#define FALSE false
#endif

#define MAXUINT64 (~0ULL)

#define DECLSPEC_ALIGN(x) __aligned(x)
#define EXTERN_C
#define NTAPI

// Define SAL-style annotations as empty macros for documentation purposes
#define _In_
#define _Inout_
#define _Out_
#define _In_opt_
#define _Out_opt_
#define _In_z_

// ============================================================================
//  x86-64 defined structures
// ============================================================================

typedef struct _PML4_ENTRY_2MB {
	union {
		UINT64 AsUInt64;
		struct {
			UINT64 Valid : 1;	     // [0]
			UINT64 Write : 1;	     // [1]
			UINT64 User : 1;	     // [2]
			UINT64 WriteThrough : 1;     // [3]
			UINT64 CacheDisable : 1;     // [4]
			UINT64 Accessed : 1;	     // [5]
			UINT64 Reserved1 : 3;	     // [6:8]
			UINT64 Avl : 3;		     // [9:11]
			UINT64 PageFrameNumber : 40; // [12:51]
			UINT64 Reserved2 : 11;	     // [52:62]
			UINT64 NoExecute : 1;	     // [63]
		} Fields;
	};
} PML4_ENTRY_2MB, *PPML4_ENTRY_2MB, PDPT_ENTRY_2MB, *PPDPT_ENTRY_2MB;

typedef struct _PD_ENTRY_2MB {
	union {
		UINT64 AsUInt64;
		struct {
			UINT64 Valid : 1;	     // [0]
			UINT64 Write : 1;	     // [1]
			UINT64 User : 1;	     // [2]
			UINT64 WriteThrough : 1;     // [3]
			UINT64 CacheDisable : 1;     // [4]
			UINT64 Accessed : 1;	     // [5]
			UINT64 Dirty : 1;	     // [6]
			UINT64 LargePage : 1;	     // [7]
			UINT64 Global : 1;	     // [8]
			UINT64 Avl : 3;		     // [9:11]
			UINT64 Pat : 1;		     // [12]
			UINT64 Reserved1 : 8;	     // [13:20]
			UINT64 PageFrameNumber : 31; // [21:51]
			UINT64 Reserved2 : 11;	     // [52:62]
			UINT64 NoExecute : 1;	     // [63]
		} Fields;
	};
} PD_ENTRY_2MB, *PPD_ENTRY_2MB;

// Packed descriptor table register
#pragma pack(push, 1)
typedef struct _DESCRIPTOR_TABLE_REGISTER {
	UINT16 Limit;
	unsigned long Base;
} DESCRIPTOR_TABLE_REGISTER, *PDESCRIPTOR_TABLE_REGISTER;
#pragma pack(pop)

typedef struct _SEGMENT_DESCRIPTOR {
	union {
		UINT64 AsUInt64;
		struct {
			UINT16 LimitLow;	// [0:15]
			UINT16 BaseLow;		// [16:31]
			UINT32 BaseMiddle : 8;	// [32:39]
			UINT32 Type : 4;	// [40:43]
			UINT32 System : 1;	// [44]
			UINT32 Dpl : 2;		// [45:46]
			UINT32 Present : 1;	// [47]
			UINT32 LimitHigh : 4;	// [48:51]
			UINT32 Avl : 1;		// [52]
			UINT32 LongMode : 1;	// [53]
			UINT32 DefaultBit : 1;	// [54]
			UINT32 Granularity : 1; // [55]
			UINT32 BaseHigh : 8;	// [56:63]
		} Fields;
	};
} SEGMENT_DESCRIPTOR, *PSEGMENT_DESCRIPTOR;

typedef struct _SEGMENT_ATTRIBUTE {
	union {
		UINT16 AsUInt16;
		struct {
			UINT16 Type : 4;	// [0:3]
			UINT16 System : 1;	// [4]
			UINT16 Dpl : 2;		// [5:6]
			UINT16 Present : 1;	// [7]
			UINT16 Avl : 1;		// [8]
			UINT16 LongMode : 1;	// [9]
			UINT16 DefaultBit : 1;	// [10]
			UINT16 Granularity : 1; // [11]
			UINT16 Reserved1 : 4;	// [12:15]
		} Fields;
	};
} SEGMENT_ATTRIBUTE, *PSEGMENT_ATTRIBUTE;

// ============================================================================
//  SimpleSVM specific structures
// ============================================================================

// Original SimpleSvm.hpp is still used for VMEXIT/VMCB structs
#include "SimpleSvm.hpp"

// npt_walk.h: dynamic sparse NPT allocator (MTRR-safe, page_is_ram based)
#include "npt_walk.h"

typedef struct _SHARED_VIRTUAL_PROCESSOR_DATA {
	UINT64 Magic; // Structure magic for validation (SSVM)

	void* MsrPermissionsMap;
	void * IoPermissionsMap; // IOPM (12KB = 3 pages), separate from MSRPM

	//deleted 0x03
	
	UINT64 TargetCr3;	// CR3 of the target game process
	UINT64 TargetPid;	// PID for debug/logging
	UINT64 GameData;	// Stored via CPUID 0x336933
	UINT64 TscOffset;	// TSC offset for timing spoofing
	UINT64 OriginalLstar;	// Original LSTAR (MSR 0xC0000082 — syscall entry)
	bool StealthEnabled; // Master stealth toggle

	// NPT: dynamic sparse allocator — replaces static Pml4Entries/Pml4eTrees
	// npt_build_identity_map() sets pml4_pa; npt_destroy() frees all pages.
	struct npt_context npt_ctx;
} SHARED_VIRTUAL_PROCESSOR_DATA, *PSHARED_VIRTUAL_PROCESSOR_DATA;

// Magic value for SHARED_VIRTUAL_PROCESSOR_DATA validation
#define SSVM_SHARED_DATA_MAGIC 0x5353564DULL // 'SSVM'

// Dummy KTRAP_FRAME to satisfy layout requirements from Windows port
// 0x190 bytes size strictly maintained
typedef struct _KTRAP_FRAME {
	UINT8 ReservedSpace[0x180];
	UINT64 Rip;
	UINT64 Rsp;
} KTRAP_FRAME;

// KERNEL_STACK_SIZE equivalent for THREAD_SIZE (commonly 16KB on x86_64 linux)
#define SVM_KERNEL_STACK_SIZE (64 * 1024) /* 64KB — NMIs can fire on safe stack */

typedef struct _VIRTUAL_PROCESSOR_DATA {
	union {
		DECLSPEC_ALIGN(PAGE_SIZE) UINT8 HostStackLimit[SVM_KERNEL_STACK_SIZE];
		struct {
			UINT8 StackContents[SVM_KERNEL_STACK_SIZE - (sizeof(void *) * 6) -
					    sizeof(KTRAP_FRAME)];
			KTRAP_FRAME TrapFrame;
			UINT64 GuestVmcbPa;			     // HostRsp (+0)
			UINT64 HostVmcbPa;			     // (+8)
			struct _VIRTUAL_PROCESSOR_DATA *Self;	     // (+16)
			PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData; // (+24)
			UINT64 RegsPtr;				     // (+32) RSP+0x20
			UINT64 VmcbVa;				     // (+40) RSP+0x28
		} HostStackLayout;
	};

	// Aligned pointers for VMCBs (allocated separately to guarantee 4KB alignment)
	VMCB *GuestVmcbPtr;
	VMCB *HostVmcbPtr;
	UINT8 *HostStateAreaPtr;

	// Original structure members (kept for compatibility, but not used)
	DECLSPEC_ALIGN(PAGE_SIZE) VMCB GuestVmcb;
	DECLSPEC_ALIGN(PAGE_SIZE) VMCB HostVmcb;
	DECLSPEC_ALIGN(PAGE_SIZE) UINT8 HostStateArea[PAGE_SIZE];
} VIRTUAL_PROCESSOR_DATA, *PVIRTUAL_PROCESSOR_DATA;

// Compile-time alignment checks
_Static_assert(sizeof(VMCB) == 0x1000, "VMCB size must be 4KB");
_Static_assert(__alignof__(VMCB) >= PAGE_SIZE || 1, "VMCB alignment check (informational)");
_Static_assert(offsetof(VIRTUAL_PROCESSOR_DATA, GuestVmcb) % PAGE_SIZE == 0,
	       "GuestVmcb must be 4KB aligned within struct");
_Static_assert(offsetof(VIRTUAL_PROCESSOR_DATA, HostVmcb) % PAGE_SIZE == 0,
	       "HostVmcb must be 4KB aligned within struct");
_Static_assert(offsetof(VIRTUAL_PROCESSOR_DATA, HostStateArea) % PAGE_SIZE == 0,
	       "HostStateArea must be 4KB aligned within struct");


typedef struct _GUEST_REGISTERS {
	UINT64 Rbx; // 0x00
	UINT64 Rcx; // 0x08
	UINT64 Rdx; // 0x10
	UINT64 Rsi; // 0x18
	UINT64 Rdi; // 0x20
	UINT64 Rbp; // 0x28
	UINT64 R8;  // 0x30
	UINT64 R9;  // 0x38
	UINT64 R10; // 0x40
	UINT64 R11; // 0x48
	UINT64 R12; // 0x50
	UINT64 R13; // 0x58
	UINT64 R14; // 0x60
	UINT64 R15; // 0x68

	// Devirtualization state (used by SvTeardownVm)
	UINT64 GuestRsp;    // 0x70 - Guest stack pointer for devirt
	UINT64 GuestRip;    // 0x78 - Guest return address for devirt
	UINT64 GuestRflags; // 0x80 - Guest flags for devirt
} GUEST_REGISTERS, *PGUEST_REGISTERS;

typedef struct _GUEST_CONTEXT {
	PGUEST_REGISTERS VpRegs;
	bool ExitVm;
} GUEST_CONTEXT, *PGUEST_CONTEXT;

// Custom definition for context capture since Linux doesn't have PCONTEXT natively.
typedef struct _SSVM_CONTEXT {
	u16 SegCs, SegDs, SegEs, SegSs;
	u16 SegFs, SegGs; // Selectors only (base/limit/attrib come from VMSAVE)
	u64 EFlags;
	u64 Rsp;
	u64 Rip;
	// Capture GDTR/IDTR per-CPU to prevent race condition
	u64 GdtrBase;
	u16 GdtrLimit;
	u64 IdtrBase;
	u16 IdtrLimit;
	/* Callee-saved GPRs needed for guest resumption */
	u64 Rbx, Rbp, R12, R13, R14, R15;

	/* Hypervisor is NOT a function, it's a LAYER. We must capture
	 * ALL registers (caller-saved + callee-saved) to maintain exact
	 * CPU state. */
	 
	u64 Rax, Rcx, Rdx, Rsi, Rdi;  // Caller-saved
	u64 R8, R9, R10, R11;          // Caller-saved
} SSVM_CONTEXT, *PSSVM_CONTEXT;

// ============================================================================
//  x86-64 defined constants
// ============================================================================
#define IA32_MSR_PAT 0x00000277
#define IA32_MSR_EFER 0xc0000080

// Note: MSR_FS_BASE, MSR_GS_BASE, MSR_KERNEL_GS_BASE are already defined
// in <asm/msr-index.h> (included via <asm/msr.h>)

#ifndef EFER_SVME
#define EFER_SVME (1UL << 12)
#endif

#ifndef EFER_LME
#define EFER_LME (1UL << 8)
#endif

#ifndef X86_CR0_PG
#define X86_CR0_PG (1UL << 31)
#endif

#ifndef X86_CR0_CD
#define X86_CR0_CD (1UL << 30)
#endif

#ifndef X86_CR0_NW
#define X86_CR0_NW (1UL << 29)
#endif

#ifndef X86_CR4_PAE
#define X86_CR4_PAE (1UL << 5)
#endif

#ifndef MSR_GS_BASE
#define MSR_GS_BASE 0xc0000101
#endif

#ifndef MSR_KERNEL_GS_BASE
#define MSR_KERNEL_GS_BASE 0xc0000102
#endif

#define RPL_MASK 3
#define DPL_SYSTEM 0

#define CPUID_FN8000_0001_ECX_SVM (1UL << 2)
#define CPUID_FN0000_0001_ECX_HYPERVISOR_PRESENT (1UL << 31)
#define CPUID_FN8000_000A_EDX_NP (1UL << 0)

#define CPUID_MAX_STANDARD_FN_NUMBER_AND_VENDOR_STRING 0x00000000
#define CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS 0x00000001
#define CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS_EX 0x80000001
#define CPUID_SVM_FEATURES 0x8000000a

// ============================================================================
//  The Microsoft Hypervisor interface defined constants
// ============================================================================
#define CPUID_HV_VENDOR_AND_MAX_FUNCTIONS 0x40000000
#define CPUID_HV_INTERFACE 0x40000001

// ============================================================================
//  SimpleSVM specific constants
// ============================================================================
#define CPUID_UNLOAD_SIMPLE_SVM 0x41414141
#define CPUID_CHECK_PRESENCE 0x42424242
#define CPUID_HV_MAX CPUID_HV_INTERFACE

//deleted0x00

#define CPUID_REGISTER_GAME_CR3 0x69696969 // Store current CR3 as target
#define CPUID_SET_TARGET_PID 0x1337	   // Set target PID (RDX = PID)
#define CPUID_STORE_GAME_DATA 0x336933	   // Store game-specific data

// CPU Brand String leaves
#define CPUID_BRAND_STRING_1 0x80000002
#define CPUID_BRAND_STRING_2 0x80000003
#define CPUID_BRAND_STRING_3 0x80000004

// Breaking on error
#define SV_DEBUG_BREAK()                                                                           \
	do {                                                                                       \
		dump_stack();                                                                      \
		BUG();                                                                             \
	} while (0)

static inline u64 ssvm_virt_to_phys(void *va)
{
	if (is_vmalloc_addr(va)) {
		return page_to_phys(vmalloc_to_page(va)) + offset_in_page(va);
	}
	return virt_to_phys(va);
}

// ============================================================================
//  Cross-file function declarations
// ============================================================================

// Assembly (ssvm_asm.S) — bare-metal style VMRUN loop
// Returns: ExitCode (u64)
u64 SvLaunchVm(u64 vmcb_pa, GUEST_REGISTERS *regs, VMCB *vmcb_va, u64 host_vmcb_pa);

// ssvm_vmexit.c — C wrapper for VMRUN loop
void SvRunVmLoop(PVIRTUAL_PROCESSOR_DATA VpData, PSHARED_VIRTUAL_PROCESSOR_DATA SharedVpData,
		 PSSVM_CONTEXT ContextRecord);
void SvPrintDebugFlags(void);

// Global devirtualization flag (defined in ssvm_ghost.c)
extern atomic_t sv_should_exit;

// Global devirtualized CPU counter (defined in ssvm_ghost.c)
extern atomic_t sv_devirt_count;

// Global shared data pointer (defined in ssvm_ghost.c)
extern PSHARED_VIRTUAL_PROCESSOR_DATA g_sharedVpData;

// Global workqueue for VMEXIT log dumps (defined in ssvm_vmexit.c)
extern struct workqueue_struct *vmexit_log_wq;

// ssvm_main.c
void SvDebugPrint(const char * Format, ...);
void * SvAllocatePageAlignedPhysicalMemory(size_t NumberOfBytes);
void SvFreePageAlignedPhysicalMemory(void * BaseAddress, size_t NumberOfBytes);
void * SvAllocateContiguousMemory(size_t NumberOfBytes);
void SvFreeContiguousMemory(void * BaseAddress, size_t NumberOfBytes);

// ssvm_ghost.c
int SvFindInitMm(void);
int VirtualizeProcessor(void * Context);
int DevirtualizeProcessor(void * Context);
void BuildMsrPermissionsMap(void * MsrPermissionsMap);

// ssvm_vmexit.c
bool SvHandleVmExit(PVIRTUAL_PROCESSOR_DATA VpData, PGUEST_REGISTERS GuestRegisters);
void ssvm_start_counter_dump(void);
void ssvm_stop_counter_dump(void);

#endif // _SIMPLESVM_INTERNAL_H_
