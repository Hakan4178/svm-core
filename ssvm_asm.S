/* SPDX-License-Identifier: GPL-2.0-only */

#include <linux/linkage.h>
#include <linux/objtool.h>

.text

.macro SAVE_HOST_REGS
    pushq   %rbp
    pushq   %r15
    pushq   %r14
    pushq   %r13
    pushq   %r12
    pushq   %rbx
.endm

.macro RESTORE_HOST_REGS
    popq    %rbx
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15
    popq    %rbp
.endm

/*
 * ═══════════════════════════════════════════════════════════════════════
 *  SvCaptureContext — Register Capture 
 * ═══════════════════════════════════════════════════════════════════════
 *
 * NOTE: This function captures GPR registers atomically without
 * any gcc interference. We cannot trust gcc inline assembly for in this
 *
 * Arguments:
 *   RDI = ctx (PSSVM_CONTEXT pointer)
 *
 * SSVM_CONTEXT struct layout (SimpleSvm_internal.h):
 *   0x00: SegCs (u16)
 *   0x02: SegDs (u16)
 *   0x04: SegEs (u16)
 *   0x06: SegSs (u16)
 *   0x08: SegFs (u16)
 *   0x0A: SegGs (u16)
 *   0x0C: (padding 4 bytes)
 *   0x10: EFlags (u64)
 *   0x18: Rsp (u64)
 *   0x20: Rip (u64)
 *   0x28: GdtrBase (u64)
 *   0x30: GdtrLimit (u16)
 *   0x32: (padding 6 bytes)
 *   0x38: IdtrBase (u64)
 *   0x40: IdtrLimit (u16)
 *   0x42: (padding 6 bytes)
 *   0x48: Rbx (u64)
 *   0x50: Rbp (u64)
 *   0x58: R12 (u64)
 *   0x60: R13 (u64)
 *   0x68: R14 (u64)
 *   0x70: R15 (u64)
 *   0x78: Rax (u64)
 *   0x80: Rcx (u64)
 *   0x88: Rdx (u64)
 *   0x90: Rsi (u64)
 *   0x98: Rdi (u64)
 *   0xA0: R8 (u64)
 *   0xA8: R9 (u64)
 *   0xB0: R10 (u64)
 *   0xB8: R11 (u64)
 *
 * Returns: void (all state written to ctx)
 */
    .align 16
SYM_FUNC_START(SvCaptureContext)
    endbr64
    
    /* Capture RAX first before using it as temp register! */
    movq    %rdi, %r15          /* Save ctx pointer to R15 */
    movq    %rax, 0x78(%r15)    /* Capture RAX immediately */
    
    /* Now we can use RAX as temp register safely */
    
    /* Capture segment selectors (16 bit) */
    movw    %cs, 0x00(%r15)
    movw    %ds, 0x02(%r15)
    movw    %es, 0x04(%r15)
    movw    %ss, 0x06(%r15)
    movw    %fs, 0x08(%r15)
    movw    %gs, 0x0A(%r15)
    
    /* Capture RFLAGS */
    pushfq
    popq    %rax
    movq    %rax, 0x10(%r15)
    
    /* Capture GDTR/IDTR before RSP
     * sgdt/sidt use stack, so we must capture RSP after stack operations */
    
    /* Capture GDTR */
    subq    $16, %rsp           /* Allocate space for GDTR (10 bytes + padding) */
    sgdt    (%rsp)
    movq    2(%rsp), %rax       /* GDTR base (8 bytes at offset 2) */
    movq    %rax, 0x28(%r15)
    movzwq  (%rsp), %rax        /* GDTR limit (2 bytes at offset 0) */
    movw    %ax, 0x30(%r15)
    addq    $16, %rsp           /* Restore stack */
    
    /* Capture IDTR */
    subq    $16, %rsp           /* Allocate space for IDTR (10 bytes + padding) */
    sidt    (%rsp)
    movq    2(%rsp), %rax       /* IDTR base (8 bytes at offset 2) */
    movq    %rax, 0x38(%r15)
    movzwq  (%rsp), %rax        /* IDTR limit (2 bytes at offset 0) */
    movw    %ax, 0x40(%r15)
    addq    $16, %rsp           /* Restore stack */
    
    /* Now capture RSP (after all stack operations) */
    movq    %rsp, 0x18(%r15)
    
    /* NOTE: RIP will be set by C caller (VirtP) using inline assembly with its own label. */
    
    /* Capture callee-saved registers */
    movq    %rbx, 0x48(%r15)
    movq    %rbp, 0x50(%r15)
    movq    %r12, 0x58(%r15)
    movq    %r13, 0x60(%r15)
    movq    %r14, 0x68(%r15)
    /* R15 will be captured last (it holds ctx pointer) */
    
    /* Capture remaining caller-saved registers (RAX already captured) */
    movq    %rcx, 0x80(%r15)
    movq    %rdx, 0x88(%r15)
    movq    %rsi, 0x90(%r15)
    /* RDI will be captured last (it's the original ctx pointer) */
    movq    %r8,  0xA0(%r15)
    movq    %r9,  0xA8(%r15)
    movq    %r10, 0xB0(%r15)
    movq    %r11, 0xB8(%r15)
    
    /* Capture R15 (was holding ctx pointer) */
    movq    %r15, 0x70(%r15)
    
    /* Capture RDI (function argument = ctx pointer)
     * NOTE: This is NOT the caller's RDI (already clobbered by call convention).
     * Guest will initialize RDI properly before first use. */
    movq    %r15, 0x98(%r15)
    
    ret
SYM_FUNC_END(SvCaptureContext)

/*
 * ═══════════════════════════════════════════════════════════════════════
 *  SvLaunchVm — VMRUN Loop Entry Point
 * ═══════════════════════════════════════════════════════════════════════
 *
 */
    .align 32
SYM_FUNC_START(SvLaunchVm)
    endbr64
    SAVE_HOST_REGS

    /*
     * GIF=0 immediately! Block ALL interrupts (NMI vb) for the
     * entire duration of guest GPR setup + vmload + vmrun.
     *
     */

    clgi

    movq    %rdi, %rax

    /*
     * Stack layout after pushes (LIFO order: last push = lowest address):
     *   RSP+0x00 = regs_ptr      (RSI)  <- Pushed last
     *   RSP+0x08 = vmcb_va       (RDX)
     *   RSP+0x10 = vmcb_pa       (RDI)
     *   RSP+0x18 = host_vmcb_pa  (RCX)  <- Pushed first
     */
    pushq   %rcx            /* host_vmcb_pa   -> RSP+0x18 */
    pushq   %rdi            /* vmcb_pa        -> RSP+0x10 */
    pushq   %rdx            /* vmcb_va        -> RSP+0x08 */
    pushq   %rsi            /* regs_ptr       -> RSP+0x00 */

    /* Load guest GPR from GUEST_REGISTERS struct
     * RSI is the base pointer, so load it last
     * All other registers can be loaded in any order
     */
    movq    0x00(%rsi), %rbx
    movq    0x08(%rsi), %rcx
    movq    0x10(%rsi), %rdx
    movq    0x28(%rsi), %rbp
    movq    0x30(%rsi), %r8
    movq    0x38(%rsi), %r9
    movq    0x40(%rsi), %r10
    movq    0x48(%rsi), %r11
    movq    0x50(%rsi), %r12
    movq    0x58(%rsi), %r13
    movq    0x60(%rsi), %r14
    movq    0x68(%rsi), %r15
    /* Load RDI and RSI last */
    movq    0x20(%rsi), %rdi    /* guest RDI */
    movq    0x18(%rsi), %rsi    /* guest RSI last (base pointer destroyed) */

    /*
     * ===== VMRUN =====
     * GIF=0 
     * vmload loads guest FS/GS/TR/LDTR from guest VMCB.
     * vmrun enters guest execution.
     */
    vmload  %rax            /* Load guest FS/GS/TR/LDTR */
    vmrun   %rax         /* Uç uç uç */ 

    /*
     * ===== #VMEXIT =====
     * GIF=0, RAX = guest VMCB PA (host RAX restored from VM_HSAVE_PA)
     */


    .align 32
vmexit_fast_path:
    /* ═══════════════════════════════════════════════════════════════
     * STEP 1: Save Guest Segment State
     * ═══════════════════════════════════════════════════════════════
     * GIF=0 (hardware set on VMEXIT)
     * Guest FS/GS/TR/LDTR still in CPU, must save to VMCB
     */
    pushq   %r11                /* Temp save: need R11 for later, stack is safe */
    vmsave  %rax                /* Guest segments → VMCB (RAX = guest_vmcb_pa) */

    /* ═══════════════════════════════════════════════════════════════
     * STEP 2: Restore Host Segment State
     * ═══════════════════════════════════════════════════════════════
     * Load host FS/GS/TR/LDTR from host VMCB
     * After this: Host GS_BASE active, %gs:0x28 canary safe
     *
     * Stack layout (R11 pushed, RSP shifted -8):
     *   RSP+0x00 = guest R11   ← just pushed
     *   RSP+0x08 = regs_ptr
     *   RSP+0x10 = vmcb_va
     *   RSP+0x18 = vmcb_pa
     *   RSP+0x20 = host_vmcb_pa
     */
    movq    0x20(%rsp), %rax    /* RAX = host_vmcb_pa */
    vmload  %rax                /* Host segments → CPU (GS_BASE now safe!) */

    /* ═══════════════════════════════════════════════════════════════
     * STEP 3: Save Guest GPRs to Struct
     * ═══════════════════════════════════════════════════════════════
     * GIF=0 still (NMI blocked), Host GS_BASE active
     * Prefetch optimization: Load cache lines before writes
     */
    movq    0x08(%rsp), %rax    /* RAX = regs_ptr (GUEST_REGISTERS*) */
    
    /* Zen 4 optimization: Prefetch struct to L1 cache */
    prefetcht0 0x00(%rax)       /* Cache line 0: RBX-RBP (0x00-0x3F) */
    prefetcht0 0x40(%rax)       /* Cache line 1: R8-R11  (0x40-0x7F) */

    /* Save guest GPRs (order: struct layout) */
    movq    %rbx, 0x00(%rax)    /* offset 0x00 */
    movq    %rcx, 0x08(%rax)    /* offset 0x08 */
    movq    %rdx, 0x10(%rax)    /* offset 0x10 */
    movq    %rsi, 0x18(%rax)    /* offset 0x18 */
    movq    %rdi, 0x20(%rax)    /* offset 0x20 */
    movq    %rbp, 0x28(%rax)    /* offset 0x28 */
    movq    %r8,  0x30(%rax)    /* offset 0x30 */
    movq    %r9,  0x38(%rax)    /* offset 0x38 */
    movq    %r10, 0x40(%rax)    /* offset 0x40 */
    
    popq    %r11                /* Restore guest R11 from stack (RSP += 8) */
    movq    %r11, 0x48(%rax)    /* offset 0x48 */
    movq    %r12, 0x50(%rax)    /* offset 0x50 */
    movq    %r13, 0x58(%rax)    /* offset 0x58 */
    movq    %r14, 0x60(%rax)    /* offset 0x60 - CRITICAL: Save BEFORE clobber! */
    movq    %r15, 0x68(%rax)    /* offset 0x68 */
    
    /* Stack restored after R11 pop:
     *   RSP+0x00 = regs_ptr
     *   RSP+0x08 = vmcb_va
     *   RSP+0x10 = vmcb_pa
     *   RSP+0x18 = host_vmcb_pa
     */

    /* ═══════════════════════════════════════════════════════════════
     * STEP 4: Read ExitCode from VMCB
     * ═══════════════════════════════════════════════════════════════
     * Now safe to use R14 as temp (guest R14 already saved)
     */
    movq    0x08(%rsp), %r14    /* R14 = vmcb_va */
    prefetcht0 0x70(%r14)       /* Prefetch ExitCode field (offset 0x70) */
    movq    0x70(%r14), %rax    /* RAX = ExitCode (return value) */


    stgi                        /* GIF=1: NMI can now arrive (safe) */

    /* Clean up and return to C */
    addq    $32, %rsp           /* Discard 4 pushed args (4 × 8 = 32 bytes) */
    RESTORE_HOST_REGS           /* Pop RBX,R12-R15,RBP */
    RET                         /* Return to SvRunVmLoop (RAX = ExitCode) */
SYM_FUNC_END(SvLaunchVm)
STACK_FRAME_NON_STANDARD(SvLaunchVm)


/*
 * void SvCallOnStack(void *safe_stack,                     RDI
 *                    PVIRTUAL_PROCESSOR_DATA vpData,        RSI
 *                    PSHARED_VIRTUAL_PROCESSOR_DATA shared,  RDX
 *                    PSSVM_CONTEXT contextRecord);           RCX
 *
 * Switches RSP to safe_stack (top of HostStackLimit), then calls
 * SvRunVmLoop. This isolates the hypervisor's stack from the guest's
 * kernel stack, preventing the guest from overwriting VMRUN frames.
 */

SYM_FUNC_START(SvCallOnStack)
    endbr64
    /* Save host caller's non-volatile registers on ORIGINAL stack */
    pushq   %rbp
    pushq   %r15
    pushq   %r14
    pushq   %r13
    pushq   %r12
    pushq   %rbx

    /* Save original RSP, switch to safe_stack */
    movq    %rsp, %rbp          /* RBP = original RSP (for return) */
    movq    %rdi, %rsp          /* RSP = safe_stack top */

    /* Shift args: SvRunVmLoop(vpData, sharedVpData, contextRecord) */
    movq    %rsi, %rdi          /* RDI = vpData */
    movq    %rdx, %rsi          /* RSI = sharedVpData */
    movq    %rcx, %rdx          /* RDX = contextRecord */

    /* Align stack to 16 bytes (x86-64 ABI) */
    andq    $-16, %rsp

    call    SvRunVmLoop

    /* If SvRunVmLoop returns (error), restore original stack */
    movq    %rbp, %rsp
    popq    %rbx
    popq    %r12
    popq    %r13
    popq    %r14
    popq    %r15
    popq    %rbp
    RET
SYM_FUNC_END(SvCallOnStack)
STACK_FRAME_NON_STANDARD(SvCallOnStack)

/*
 * void SvTeardownVm(PGUEST_REGISTERS regs, u64 guest_rax);
 * Restores guest state and JMPs directly into guest execution,
 * effectively abandoning the host/safe stack.
 *
 * Called after: vmload guest_vmcb, wrmsrl(EFER, ~SVME)
 * SVM is already OFF. This is a one-way jump.
 *
 */

SYM_FUNC_START(SvTeardownVm)
    endbr64
    /* RDI = PGUEST_REGISTERS, RSI = guest_rax */
    movq    %rsi, %rax          /* Restore Guest RAX */
    movq    %rdi, %rsi          /* RSI = regs pointer */

    /*
     * Load devirt state from extended fields:
     *   0x70 = GuestRsp
     *   0x78 = GuestRip
     *   0x80 = GuestRflags
     */
    movq    0x70(%rsi), %r12    /* R12 = GuestRsp */
    movq    0x78(%rsi), %r13    /* R13 = GuestRip */
    movq    0x80(%rsi), %r14    /* R14 = GuestRflags */

    /* Switch to guest stack */
    movq    %r12, %rsp

    /*
     * Stack build order (push = grow DOWN):
     *
     *   RSP+0x20 = GuestRip       ← ret will pop this
     *   RSP+0x18 = GuestRflags    ← popfq will pop this (LAST pop)
     *   RSP+0x10 = real guest R12 ← pop FIRST (IF still 0)
     *   RSP+0x08 = real guest R13
     *   RSP+0x00 = real guest R14 ← top of stack
     */
    pushq   %r13                /* GuestRip (return address) */
    pushq   %r14                /* GuestRflags */
    pushq   0x50(%rsi)          /* Real guest R12 */
    pushq   0x58(%rsi)          /* Real guest R13 */
    pushq   0x60(%rsi)          /* Real guest R14 */

    /* Load all other Guest GPRs */
    movq    0x00(%rsi), %rbx
    movq    0x08(%rsi), %rcx
    movq    0x10(%rsi), %rdx
    movq    0x20(%rsi), %rdi
    movq    0x28(%rsi), %rbp
    movq    0x30(%rsi), %r8
    movq    0x38(%rsi), %r9
    movq    0x40(%rsi), %r10
    movq    0x48(%rsi), %r11
    movq    0x68(%rsi), %r15

    /* Restore Guest RSI (last, since we used it as base pointer) */
    movq    0x18(%rsi), %rsi

    /*
     * SAFE RESTORE ORDER:
     *   1. Pop R14/R13/R12 — IF=0 still, no interrupts possible
     *   2. popfq — IF may become 1, but ALL regs are now correct
     *   3. ret — jump to GuestRip, execution resumes as bare-metal
     */
    popq    %r14                /* Real guest R14 (IF=0, safe) */
    popq    %r13                /* Real guest R13 (IF=0, safe) */
    popq    %r12                /* Real guest R12 (IF=0, safe) */
    popfq                       /* Restore RFLAGS — IF=1 now, all regs correct */
    ret                         /* Jump to GuestRip */
SYM_FUNC_END(SvTeardownVm)
STACK_FRAME_NON_STANDARD(SvTeardownVm)
