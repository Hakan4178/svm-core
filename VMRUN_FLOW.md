/*
 * ssvm_asm.S — svm-core VMRUN Assembly Bi varsın Bi yoksun 
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  REGISTER-LEVEL RUNTIME CHAIN  (her adımda CPU state ne durumda?)
 * ═══════════════════════════════════════════════════════════════════════
 *
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │ PHASE 0: C CALLER  (VirtualizeProcessor → SvCallOnStack)          │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │ Stack : Kernel stack (per-CPU, ~16KB)                              │
 * │ GIF   : 1 (normal)      IF: 0 (local_irq_disable)                 │
 * │ GS    : HOST per-CPU    EFER.SVME: 1 (SVM enabled)                │
 * │                                                                    │
 * │ SvCallOnStack(safe_stack, vpData, shared, ctx):                    │
 * │   1. SAVE callee-saved regs (RBP,R15..R12,RBX) → kernel stack     │
 * │   2. RBP = RSP (bookmark kernel stack)                             │
 * │   3. RSP = safe_stack (64KB ayrı alan, vpData içinde)              │
 * │   4. call SvRunVmLoop(vpData, shared, ctx)                         │
 * │      └─→ SvRunVmLoop C loop: her VMEXIT'te SvLaunchVm çağırır     │
 * └─────────────────────────────────────────────────────────────────────┘
 *                              │
 *                              ▼
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │ PHASE 1: SvLaunchVm ENTRY  (C → Assembly geçişi)                  │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │ Argümanlar (x86-64 ABI):                                          │
 * │   RDI = vmcb_pa        (Guest VMCB fiziksel adresi)                │
 * │   RSI = regs_ptr       (GUEST_REGISTERS struct pointer)            │
 * │   RDX = vmcb_va        (Guest VMCB sanal adresi)                   │
 * │   RCX = host_vmcb_pa   (Host VMCB fiziksel adresi)                 │
 * │                                                                    │
 * │ Stack : Safe stack (64KB)                                          │
 * │ GIF   : 1               IF: 0                                     │
 * │ GS    : HOST per-CPU                                               │
 * │                                                                    │
 * │ Adımlar:                                                           │
 * │   1. endbr64           (IBT: Indirect Branch Tracking)             │
 * │   2. SAVE_HOST_REGS    (RBP,R15..R12,RBX → safe stack'e push)     │
 * │   3. clgi              ★ GIF=0! Tüm interrupt'lar BLOCKED          │
 * │   4. RAX = RDI         (RAX = vmcb_pa, VMRUN bunu bekler)          │
 * │   5. Push: RCX,RDI,RDX,RSI → stack'e (VMEXIT sonrası lazım)       │
 * │   6. Guest GPR yükleme (regs struct → RBX..R15, RSI en son)        │
 * └─────────────────────────────────────────────────────────────────────┘
 *                              │
 *                              ▼
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │ PHASE 2: VMRUN  (Guest'e giriş)                                   │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │ GIF=0 (clgi'den beri).  NMI/SMI/INIT hepsi bloklu.                │
 * │                                                                    │
 * │   vmload RAX   → Guest FS/GS/TR/LDTR donanıma yüklenir            │
 * │                  (Guest GS_BASE artık aktif — ama GIF=0,           │
 * │                   NMI gelemez, canary okunamaz, güvenli)            │
 * │                                                                    │
 * │   vmrun  RAX   → CPU guest moduna geçer:                           │
 * │                  • Host state → VM_HSAVE_PA'ya kaydedilir           │
 * │                  • VMCB'den: CS,DS,ES,SS,GDTR,IDTR,CR*,EFER,      │
 * │                    RIP,RSP,RFLAGS,RAX yüklenir                     │
 * │                  • GIF=0 kalır (VMRUN temizlemez)                   │
 * │                  • Guest kodu çalışmaya başlar                      │
 * │                                                                    │
 * │ ╔══════════════════════════════════════════════════════════════╗    │
 * │ ║  GUEST ÇALIŞIYOR  (Linux kernel — bluepill, kendisi guest)  ║    │
 * │ ║  • Tüm register'lar guest'e ait                             ║    │
 * │ ║  • NPT aktif: GPA → HPA (identity map, GPA == HPA)         ║    │
 * │ ║  • INTR/NMI intercept YOK → doğrudan guest'e teslim         ║    │
 * │ ║  • CPUID/MSR/VMRUN intercept → #VMEXIT tetikler             ║    │
 * │ ╚══════════════════════════════════════════════════════════════╝    │
 * └─────────────────────────────────────────────────────────────────────┘
 *                              │
 *                    #VMEXIT tetiklenir
 *                              │
 *                              ▼
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │ PHASE 3: #VMEXIT  (Guest → Host geçişi)                          │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │ Hardware otomatik yapar:                                           │
 * │   • GIF = 0 (otomatik, NMI bloklu)                                │
 * │   • Host state VM_HSAVE_PA'dan geri yüklenir:                      │
 * │     CS,DS,ES,SS,GDTR,IDTR,CR*,EFER,RFLAGS,RIP,RSP,RAX            │
 * │   • RAX = guest VMCB PA (host RAX olarak restore edilir)           │
 * │   • RSP = host RSP (safe stack, VMRUN öncesi değer)                │
 * │   • RIP = vmrun'dan sonraki instruction (pushq %r11)              │
 * │   • Diğer GPR'ler (RBX..R15,RSI,RDI,RCX,RDX) = GUEST değerleri!  │
 * │     (Hardware bunları RESTORE ETMEZ — bizim kaydetmemiz lazım)     │
 * │                                                                    │
 * │ Assembly adımları (hepsi GIF=0 ile):                               │
 * │                                                                    │
 * │   1. pushq R11          → Guest R11'i stack'e kurtar (temp lazım)  │
 * │   2. vmsave RAX         → Guest FS/GS/TR/LDTR → guest VMCB'ye     │
 * │   3. RAX = [RSP+0x20]   → host_vmcb_pa stack'ten oku              │
 * │   4. vmload RAX         → Host FS/GS/TR/LDTR donanıma geri yükle  │
 * │      ★ HOST GS_BASE artık aktif! %gs:0x28 canary güvenli.         │
 * │                                                                    │
 * │   5. RAX = [RSP+0x08]   → regs_ptr stack'ten oku                  │
 * │   6. Guest GPR'leri struct'a kaydet:                               │
 * │      RBX→[0x00] RCX→[0x08] RDX→[0x10] RSI→[0x18]                 │
 * │      RDI→[0x20] RBP→[0x28] R8→[0x30]  R9→[0x38]                  │
 * │      R10→[0x40] R11→[0x48](pop) R12→[0x50] R13→[0x58]            │
 * │      R14→[0x60] R15→[0x68]                                        │
 * │                                                                    │
 * │   7. R14 = vmcb_va      → VMCB sanal adresini oku                 │
 * │   8. RAX = [R14+0x70]   → ExitCode (VMCB offset 0x070)            │
 * │                                                                    │
 * │   9. stgi               ★ GIF=1! NMI artık gelebilir.             │
 * │      (Güvenli: Host GS_BASE zaten #4'te restore edildi)            │
 * │                                                                    │
 * │  10. RSP += 32           → 4 pushed arg temizle                    │
 * │  11. RESTORE_HOST_REGS   → RBX,R12..R15,RBP pop                   │
 * │  12. RET                 → C caller'a dön (RAX = ExitCode)         │
 * └─────────────────────────────────────────────────────────────────────┘
 *                              │
 *                              ▼
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │ PHASE 4: C HANDLER  (SvRunVmLoop → SvHandleVmExit)                │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │ GIF=1, IF=0 (local_irq_disable hâlâ aktif)                        │
 * │ GS = HOST per-CPU (vmload host #4'te restore etti)                 │
 * │ Stack = safe stack                                                 │
 * │ RAX = ExitCode                                                     │
 * │                                                                    │
 * │ SvHandleVmExit dispatch:                                           │
 * │   CPUID  → SvHandleCpuid (stealth, backdoor, devirt)              │
 * │   MSR    → SvHandleMsrAccess (EFER, LSTAR, GS_BASE→VMCB)         │
 * │   VMRUN  → SvHandleVmrun (#GP inject)                             │
 * │   NPF    → Log + devirtualize                                     │
 * │   INTR/NMI → no-op (shouldn't happen, not intercepted)            │
 * │   default → Log + devirtualize                                     │
 * │                                                                    │
 * │ ExitVm == FALSE → SvLaunchVm tekrar çağır (PHASE 1'e dön) ←──┐   │
 * │ ExitVm == TRUE  → PHASE 5'e geç (devirtualization)            │   │
 * └────────────────────────────────────────────────────────────────┘   │
 *                              │                                       │
 *               (normal VMEXIT loop)───────────────────────────────────┘
 *                              │
 *                              ▼
 * ┌─────────────────────────────────────────────────────────────────────┐
 * │ PHASE 5: DEVIRTUALIZATION  (SvTeardownVm — geri dönüşü YOK)      │
 * ├─────────────────────────────────────────────────────────────────────┤
 * │ C kodu hazırlık yapar:                                             │
 * │   1. clgi               (GIF=0, NMI blokla)                       │
 * │   2. vmload guest_vmcb  (Guest FS/GS/TR/LDTR → donanım)           │
 * │   3. wrmsrl(EFER, ~SVME)(SVM kapat — artık bare-metal)            │
 * │   4. SvTeardownVm(regs, guest_rax) çağır                          │
 * │                                                                    │
 * │ SvTeardownVm assembly:                                             │
 * │   RAX = guest_rax       (RSI'dan, VMCB'den gelen)                  │
 * │   RSI = regs_ptr        (RDI'dan)                                  │
 * │                                                                    │
 * │   R12 = GuestRsp  [0x70]   (cpuid anındaki kernel RSP)             │
 * │   R13 = GuestRip  [0x78]   (cpuid'den sonraki instruction)         │
 * │   R14 = GuestRflags [0x80] (cpuid anındaki RFLAGS)                 │
 * │                                                                    │
 * │   RSP = R12              ★ Safe stack terk edilir, kernel stack'e! │
 * │   push R13               (GuestRip → return address olarak)        │
 * │   push real R12,R13,R14  (temp olarak kullandıklarımızı kurtar)    │
 * │                                                                    │
 * │   GPR restore: RBX,RCX,RDX,RDI,RBP,R8..R11,R15 ← struct          │
 * │   RSI ← struct (en son, base pointer idi)                          │
 * │                                                                    │
 * │   push R14 + popfq       → RFLAGS restore (IF=1 olabilir)         │
 * │   pop R14,R13,R12        → Gerçek guest değerleri geri             │
 * │   ret                    → GuestRip'e atla!                        │
 * │                                                                    │
 * │ ★ Artık bare-metal Linux kernel, hypervisor hiç olmamış gibi.      │
 * │   DevirtualizeProcessor devam eder, VpData'yı free eder.           │
 * └─────────────────────────────────────────────────────────────────────┘
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  STACK LAYOUT ÖZET
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  Kernel Stack (per-CPU, ~16KB):
 *    VirtualizeProcessor frame → SvCallOnStack frame → [terk edilir]
 *
 *  Safe Stack (vpData->HostStackLimit, 64KB):
 *    SvRunVmLoop frame → SvLaunchVm frame → [VMRUN sırasında donuk]
 *    VMEXIT sonrası: SvLaunchVm return → SvRunVmLoop devam
 *
 *  Devirt: SvTeardownVm safe stack'i terk eder, kernel stack'e döner.
 *
 * ═══════════════════════════════════════════════════════════════════════
 *  GIF/IF ZAMAN ÇİZELGESİ
 * ═══════════════════════════════════════════════════════════════════════
 *
 *  [C: IF=0]──→[clgi: GIF=0]──→[vmload guest]──→[vmrun]
 *                  │                                 │
 *              NMI BLOCKED                      GUEST ÇALIŞIR
 *                                                    │
 *              [#VMEXIT: GIF=0 otomatik]←────────────┘
 *                  │
 *              [vmsave guest]──→[vmload host]──→[GPR kaydet]
 *                  │                                 │
 *              NMI BLOCKED                    HOST GS HAZIR
 *                                                    │
 *              [stgi: GIF=1]←────────────────────────┘
 *                  │
 *              NMI gelebilir ama HOST GS aktif → güvenli
 *                  │
 *              [C handler]──→[tekrar SvLaunchVm]──→ ... loop
 *
 * ═══════════════════════════════════════════════════════════════════════
 */