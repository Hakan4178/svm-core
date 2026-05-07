/* SPDX-License-Identifier: GPL-2.0-only */

/*  
    Module lifecycle: module_init, module_exit, power management,
    processor orchestration, SVM support check, and memory utilities.
 */
#include <asm/msr.h>
#include <asm/processor.h>
#include <linux/console.h>
#include <linux/cpu.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/suspend.h>
#include <linux/tty.h>

#include "SimpleSvm_internal.h"

// ESP32 serial console for capturing logs during triple fault
static struct file *uart_file = NULL;

static void esp32_console_write(struct console *con, const char *buf,
                                unsigned int len) {
  if (!uart_file || IS_ERR(uart_file))
    return;
  kernel_write(uart_file, buf, len, &uart_file->f_pos);
}

static struct console esp32_console = {
    .name = "esp32log",
    .write = esp32_console_write,
    .flags = CON_ENABLED | CON_PRINTBUFFER,
    .index = -1,
};

// Emergency sync wrapper - forces log flush to disk
// SAFE: Works in both atomic and non-atomic context!
static inline void ssvm_sync_logs(void) {
  // mdelay() does busy-wait (spin), doesn't call schedule()
  // Safe in atomic context (preempt_disable, spinlock, interrupt)
  // Gives printk buffer time to flush to console/disk
  mdelay(10); // 10ms busy-wait
  mb();
}

//
// A power state callback handle.
//
static struct notifier_block ssvm_pm_notifier;

/*! =========================================================================
    @brief  Compute the physical address upper bound for the NPT identity map.

    @details
    AMD Manual Vol.2 §15.25.3: Every GPA the guest generates must have a
    mapping in the nested page table, otherwise a #NPF (VMEXIT_NPF) occurs.
    We therefore need to cover:
      1. All physical RAM:   high_memory == __va(max_pfn << PAGE_SHIFT) on
                             x86_64, so virt_to_phys(high_memory) gives the
                             end of RAM without touching non-exported max_pfn.
      2. Legacy MMIO < 4 GB: HPET (0xFED00000), LAPIC (0xFEE00000),
                             PCI legacy BARs, etc.

    max_pfn and zone iterators are NOT exported in all kernel builds
    (Arch 6.x+).  high_memory is EXPORT_SYMBOL'd in vmlinux and is the
    canonical way to obtain the physical RAM ceiling from a module.

    AMD Manual Vol.2 §15.25.6: 2 MB leaf pages require a 2 MB-aligned
    physical base address, so the result is rounded up to the next 2 MB.
    ========================================================================= */
static u64 ssvm_npt_phys_limit(void) {
  /*
   * high_memory = __va(max_pfn << PAGE_SHIFT) on x86_64.
   * virt_to_phys() inverts __va(), giving the physical ceiling of RAM.
   */
  u64 ram_end = (u64)virt_to_phys(high_memory);

  /*
   * Problem: Guest kernel accesses GPU MMIO → #NPF → Devirtualization.
   *
   * Solution: Map up to 1TB to cover GPU BARs.
   * - System RAM: ~18GB
   * - GPU MMIO: 288-296GB
   * - Coverage: 1TB (safe margin for almost all setups)
   *
   * Page table overhead: ~524K pages = 4MB memory (negligible).
   */
  u64 min_coverage = max(ram_end, 1024ULL << 30); /* Max(RAM, 1TB) */
  u64 result = ALIGN(min_coverage, 2ULL << 20);

  return result;
}

/*! =========================================================================
    @brief      Sends a message to the kernel logger.
    ========================================================================= */
void SvDebugPrint(const char *Format, ...) {
  va_list argList;
  char buffer[256];

  va_start(argList, Format);
  vsnprintf(buffer, sizeof(buffer), Format, argList);
  va_end(argList);

  pr_info("[SimpleSvm] %s", buffer);
}

/*! =========================================================================
    @brief      Allocates page aligned, zero filled physical memory.

    @details    kmalloc <3

                Size limit: kmalloc max is typically 4MB (KMALLOC_MAX_SIZE).
                Our SHARED_VIRTUAL_PROCESSOR_DATA is ~64KB, well within limit.
    ========================================================================= */
void *
SvAllocatePageAlignedPhysicalMemory(size_t NumberOfBytes) {
  void * memory;

  pr_emerg(
      "[SimpleSvm] SvAllocatePageAlignedPhysicalMemory: Allocating %zu bytes\n",
      NumberOfBytes);
  ssvm_sync_logs(); // SAFE: preemption enabled (called before preempt_disable)

  if (NumberOfBytes < PAGE_SIZE) {
    pr_warn("[SimpleSvm] NumberOfBytes < PAGE_SIZE\n");
  }

  pr_emerg("[SimpleSvm] Calling kzalloc(%zu, GFP_KERNEL)...\n", NumberOfBytes);
  ssvm_sync_logs(); // SAFE: preemption enabled

  memory = kzalloc(NumberOfBytes, GFP_KERNEL);

  pr_emerg("[SimpleSvm] kzalloc returned: %p\n", memory);
  ssvm_sync_logs(); // SAFE: preemption enabled

  if (memory != NULL) {
    pr_emerg("[SimpleSvm] kzalloc successful, memory at %px\n", memory);
    ssvm_sync_logs(); // SAFE: preemption enabled
  }

  return memory;
}

/*! =========================================================================
    @brief      Frees memory allocated by SvAllocatePageAlignedPhysicalMemory.
                Note: In Linux, we need the size to free the pages. Modified
                signature to include NumberOfBytes.
    ========================================================================= */
void SvFreePageAlignedPhysicalMemory(void *BaseAddress,
                                     size_t NumberOfBytes
                                     __attribute__((unused))) {
  if (BaseAddress != NULL) {
    kfree(BaseAddress);
  }
}

/*! =========================================================================
    @brief      Allocates page aligned, zero filled contiguous physical memory.

    @details    Uses __get_free_pages() with GFP_KERNEL, which can sleep.

                Current usage: Called during module init (safe) and in
                VirtualizeProcessor() before preempt_disable() (safe).
    ========================================================================= */
void *
SvAllocateContiguousMemory(size_t NumberOfBytes) {
  void * memory;
  unsigned int order = (unsigned int)get_order(NumberOfBytes);

  // Allocate physically contiguous, page-aligned memory
  memory = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO, order);

  if (memory == NULL) {
    pr_err("[SimpleSvm] Failed to allocate %zu bytes (order %u)\n",
           NumberOfBytes, order);
    return NULL;
  }

  if (unlikely(!IS_ALIGNED((unsigned long)memory, PAGE_SIZE))) {
    pr_err("[SimpleSvm] BUG: __get_free_pages returned unaligned address %p\n",
           memory);
    free_pages((unsigned long)memory, order);
    return NULL;
  }

  return memory;
}

/*! =========================================================================
    @brief      Frees memory allocated by SvAllocateContiguousMemory.
                Note: Included NumberOfBytes for Linux API compatibility.
    ========================================================================= */
void SvFreeContiguousMemory(void *BaseAddress, size_t NumberOfBytes) {
  if (BaseAddress != NULL) {
    unsigned int order = (unsigned int)get_order(NumberOfBytes);
    free_pages((unsigned long)BaseAddress, order);
  }
}


static bool SvIsSvmSupported(void) {
  bool svmSupported = false;
  unsigned int eax, ebx, ecx, edx;
  u64 vmcr;

  //
  // Test if the current processor is AMD one.
  //
  cpuid(CPUID_MAX_STANDARD_FN_NUMBER_AND_VENDOR_STRING, &eax, &ebx, &ecx, &edx);
  if ((ebx != 0x68747541) || // 'htuA'
      (edx != 0x69746e65) || // 'itne'
      (ecx != 0x444d4163))   // 'DMAc'
  {
    goto Exit;
  }

  //
  // Test if the SVM feature is supported by the current processor.
  //
  cpuid(CPUID_PROCESSOR_AND_PROCESSOR_FEATURE_IDENTIFIERS_EX, &eax, &ebx, &ecx,
        &edx);
  if ((ecx & CPUID_FN8000_0001_ECX_SVM) == 0) {
    goto Exit;
  }

  //
  // Test if Nested Page Tables feature is supported.
  //
  cpuid(CPUID_SVM_FEATURES, &eax, &ebx, &ecx, &edx);
  if ((edx & CPUID_FN8000_000A_EDX_NP) == 0) {
    goto Exit;
  }

  //
  // Test if the SVM feature can be enabled.
  //
  rdmsrl(SVM_MSR_VM_CR, vmcr);
  if ((vmcr & SVM_VM_CR_SVMDIS) != 0) {
    goto Exit;
  }

  svmSupported = true;

Exit:
  return svmSupported;
}

/*! =========================================================================
    @brief      Execute a callback on all processors one-by-one.

    @details    Uses for_each_online_cpu() to iterate only online CPUs,
                and properly saves/restores CPU affinity mask.

                CDisables preemption during the entire loop to prevent
                race conditions where scheduler migrates us to a different CPU
                between callback return and next iteration.
    ========================================================================= */
static int
SvExecuteOnEachProcessor(_In_ int (*Callback)(void *),
                         _In_opt_ void * Context,
                         _Out_opt_ unsigned long *NumOfProcessorCompleted) {
  int status = 0;
  unsigned long numOfProcessorsCompleted = 0;
  int cpu;
  cpumask_var_t old_mask;

  // Allocate cpumask for saving current affinity
  if (!alloc_cpumask_var(&old_mask, GFP_KERNEL)) {
    return -ENOMEM;
  }

  // Save current CPU affinity mask (proper API)
  cpumask_copy(old_mask, &current->cpus_mask);


  // Execute callback on each online CPU
  for_each_online_cpu(cpu) {

    /*
     * We check return value BEFORE the spin loop below.
     * If set_cpus_allowed_ptr() fails, we break immediately and
     * never enter the spin loop. This prevents infinite spinning
     * if migration was rejected by the scheduler.
     */

    if (set_cpus_allowed_ptr(current, cpumask_of(cpu)) != 0) {
      pr_err("[SimpleSvm] Failed to migrate to CPU %d\n", cpu);
      status = -EFAULT;
      break;
    }

    
    while (smp_processor_id() != cpu) {
      cpu_relax(); // Hint to CPU: we're spinning (reduces power)
    }

    // Execute the callback (now guaranteed to be on correct CPU)
    status = Callback(Context);

    // Exit if the callback returned error
    if (status != 0) {
      break;
    }

    numOfProcessorsCompleted++;
  }


  if (set_cpus_allowed_ptr(current, old_mask) != 0) {
    pr_warn("[SimpleSvm] Failed to restore original CPU affinity! "
            "Thread may be stuck on CPU %d\n",
            smp_processor_id());
    // Don't overwrite status - virtualization may have succeeded
  } else {
    // Wait for migration back to original CPU set
    while (!cpumask_test_cpu(smp_processor_id(), old_mask)) {
      cpu_relax();
    }
  }

  free_cpumask_var(old_mask);

  // Set number of processors that successfully executed callback
  if (NumOfProcessorCompleted != NULL) {
    *NumOfProcessorCompleted = numOfProcessorsCompleted;
  }

  return status;
}

/* Force CPUID execution to trigger devirtualization check.
 * Called via IPI on each CPU during module unload.
 *
 */
static void force_devirt_cpuid(void *info) {
  unsigned int eax, ebx, ecx, edx;
  (void)info;

  /* Execute CPUID - this triggers VMEXIT_CPUID.
   * Handler checks sv_should_exit flag and devirtualizes if set.
   *
   * Use leaf 0 (vendor string) - safe, fast, always supported.
   * If virtualized: VMEXIT → check flag → devirtualize → never returns
   * If bare-metal: Just returns vendor string, callback returns normally
   */
  cpuid_count(0, 0, &eax, &ebx, &ecx, &edx);

  /* If we reach here, CPU was not virtualized (or already devirtualized).
   * Just return normally. */
}

/*! =========================================================================
    @brief      De-virtualize all virtualized processors.
    ========================================================================= */
static void SvDevirtualizeAllProcessors(void) {
  int retry_count = 0;
  const int MAX_RETRIES = 5000; // 5000ms = 5 seconds (increased for debugging)
  int expected_devirt_count = num_online_cpus();
  int current_count;

  /* NOTE: NO pr_info/pr_err during devirtualization!
   * Console output triggers GPU MMIO access. If NPT is still active,
   * GPU MMIO causes NPF → recursive devirtualization → triple fault.
   *
   * All logging MUST happen BEFORE or AFTER devirtualization, never during.
   */

  // Reset devirtualization counter
  atomic_set(&sv_devirt_count, 0);

  // Set global exit flag - all CPUs will check this on next CPUID VMEXIT
  atomic_set(&sv_should_exit, 1);

  /* NOTE: Memory barrier to ensure flag write is visible to all CPUs
   * before we start polling the counter. Without this, a CPU might read
   * stale sv_should_exit=0 from cache and never devirtualize. */
  smp_mb();

  /* NOTE: Force CPUID execution on all CPUs!
   *
   * Problem: Idle CPUs may not execute CPUID for seconds/minutes.
   * If we timeout and destroy NPT while a CPU is idle, it will
   * wake up later and cause NPF on freed page → triple fault.
   *
   */
  on_each_cpu(force_devirt_cpuid, NULL, 0); // Force CPUID on all CPUs (async)

  /* Wait for all CPUs to devirtualize.
   * Poll sv_devirt_count until all CPUs have incremented it.
   * Each CPU increments counter after disabling EFER.SVME.
   *
   * NOTE: This loop MUST complete before npt_destroy()!
   * If any CPU is still virtualized when NPT is freed, next
   * memory access will cause NPF on freed page → triple fault.
   *
   */

  while (retry_count < MAX_RETRIES) {
    current_count = atomic_read(&sv_devirt_count);

    /* All CPUs devirtualized? */
    if (current_count >= expected_devirt_count)
      break;

    /* Small delay to allow devirtualization to complete.
     * 1ms is overkill (expected ~200µs), but safe. */
    msleep(1);

    retry_count++;
  }

  /* At this point, all virtualized CPUs should have devirtualized.
   * If timeout occurred, some CPUs may still be virtualized, but we
   * proceed anyway (better than hanging forever). The NPT free will
   * likely cause a crash if any CPU is still virtualized. */

  // Now we can safely free shared resources

  if (g_sharedVpData != NULL) {
    // Free MSRPM
    if (g_sharedVpData->MsrPermissionsMap != NULL) {
      SvFreeContiguousMemory(g_sharedVpData->MsrPermissionsMap,
                             SVM_MSR_PERMISSIONS_MAP_SIZE);
    }

    // Free NPT page tables
    npt_destroy(&g_sharedVpData->npt_ctx);

    // Free shared data structure
    SvFreePageAlignedPhysicalMemory(g_sharedVpData,
                                    sizeof(SHARED_VIRTUAL_PROCESSOR_DATA));

    g_sharedVpData = NULL;
  }
}

/*! =========================================================================
    @brief      Virtualizes all processors on the system.
    ========================================================================= */
static int SvVirtualizeAllProcessors(void) {
  int status;
  PSHARED_VIRTUAL_PROCESSOR_DATA sharedVpData = NULL;
  unsigned long numOfProcessorsCompleted = 0;

  pr_emerg("[SimpleSvm] === SvVirtualizeAllProcessors ENTERED ===\n");

  if (SvIsSvmSupported() == false) {
    SvDebugPrint("SVM is not fully supported on this processor.\n");
    status = -ENODEV;
    goto Exit;
  }

  pr_emerg("[SimpleSvm] SVM is supported, about to allocate sharedVpData\n");
  pr_emerg("[SimpleSvm] Size to allocate: %zu bytes\n",
           sizeof(SHARED_VIRTUAL_PROCESSOR_DATA));

  sharedVpData =
      (PSHARED_VIRTUAL_PROCESSOR_DATA)SvAllocatePageAlignedPhysicalMemory(
          sizeof(SHARED_VIRTUAL_PROCESSOR_DATA));

  pr_emerg("[SimpleSvm] SvAllocatePageAlignedPhysicalMemory returned: %p\n",
           sharedVpData);

  if (sharedVpData == NULL) {
    SvDebugPrint("Insufficient memory.\n");
    status = -ENOMEM;
    goto Exit;
  }

  pr_emerg("[SimpleSvm] sharedVpData allocated successfully at %px\n",
           sharedVpData);
  ssvm_sync_logs();


  pr_info(
      "[SimpleSvm] sharedVpData ready (kzalloc pre-zeroed, no touch needed)\n");
  ssvm_sync_logs();

  // Set magic for validation
  sharedVpData->Magic = SSVM_SHARED_DATA_MAGIC;

  // Initialize MSRPM pointer to NULL for safe error handling
  sharedVpData->MsrPermissionsMap = NULL;
  sharedVpData->IoPermissionsMap = NULL;

  pr_info("[SimpleSvm] Building NPT identity map...\n");
  ssvm_sync_logs();

  // Cache type: addr < ram_end → WB (system RAM), else → UC (MMIO/GPU BARs).
  // For MMIO holes inside RAM range, AMD MTRR enforces UC regardless of NPT.

  {
    u64 phys_limit = ssvm_npt_phys_limit();
    u64 ram_end = (u64)virt_to_phys(high_memory);

    pr_info("[SimpleSvm] NPT: phys_limit=0x%llx ram_end=0x%llx\n", phys_limit,
            ram_end);
    ssvm_sync_logs();

    int npt_ret =
        npt_build_identity_map(&sharedVpData->npt_ctx, phys_limit, ram_end);
    if (npt_ret != 0) {
      pr_err("[SimpleSvm] NPT identity map failed (err=%d)\n", npt_ret);
      ssvm_sync_logs();
      SvFreePageAlignedPhysicalMemory(sharedVpData,
                                      sizeof(SHARED_VIRTUAL_PROCESSOR_DATA));
      status = -ENOMEM;
      goto Exit;
    }

    pr_info("[SimpleSvm] NPT build complete, validating...\n");
    ssvm_sync_logs();

    // NOTE: Validate that NPT was actually built
    if (sharedVpData->npt_ctx.pml4_pa == 0) {
      pr_err("[SimpleSvm] NPT build succeeded but pml4_pa is NULL!\n");
      ssvm_sync_logs();
      SvFreePageAlignedPhysicalMemory(sharedVpData,
                                      sizeof(SHARED_VIRTUAL_PROCESSOR_DATA));
      status = -EFAULT;
      goto Exit;
    }

    pr_info("[SimpleSvm] NPT validated: pml4_pa=0x%llx\n",
            sharedVpData->npt_ctx.pml4_pa);
    ssvm_sync_logs();
  }

  // Store global pointer for cleanup during devirtualization
  g_sharedVpData = sharedVpData;

  // AMD Manual Vol.2 §15.11: MSRPM (MSR Permissions Map) must reside at a
  // 4KB-aligned physical address and spans 12KB (3 pages).
  // SVM_MSR_PERMISSIONS_MAP_SIZE = PAGE_SIZE * 3 = 12KB.
  sharedVpData->MsrPermissionsMap =
      SvAllocateContiguousMemory(SVM_MSR_PERMISSIONS_MAP_SIZE);
  if (sharedVpData->MsrPermissionsMap == NULL) {
    pr_err("[SimpleSvm] MSRPM allocation failed\n");
    npt_destroy(&sharedVpData->npt_ctx);
    SvFreePageAlignedPhysicalMemory(sharedVpData,
                                    sizeof(SHARED_VIRTUAL_PROCESSOR_DATA));
    status = -ENOMEM;
    goto Exit;
  }

  BuildMsrPermissionsMap(sharedVpData->MsrPermissionsMap);

  // IOPM is not allocated: IOIO_PROT is not set in InterceptMisc1,
  // so hardware never consults the IOPM. IopmBasePa is set to 0.
  sharedVpData->IoPermissionsMap = NULL;

  status = SvExecuteOnEachProcessor(VirtualizeProcessor, sharedVpData,
                                    &numOfProcessorsCompleted);

Exit:
  if (status != 0) {
    if (numOfProcessorsCompleted != 0) {
      SvDevirtualizeAllProcessors();
    } else {
      if (sharedVpData != NULL) {
        if (sharedVpData->MsrPermissionsMap != NULL) {
          SvFreeContiguousMemory(sharedVpData->MsrPermissionsMap,
                                 SVM_MSR_PERMISSIONS_MAP_SIZE);
        }
        // npt_destroy() is safe when page_count == 0 (build failed or never
        // called)
        npt_destroy(&sharedVpData->npt_ctx);
        SvFreePageAlignedPhysicalMemory(sharedVpData,
                                        sizeof(SHARED_VIRTUAL_PROCESSOR_DATA));
      }
    }
  }
  return status;
}

/*! =========================================================================
    @brief      Power management callback.
    ========================================================================= */
static int SvPowerCallbackRoutine(struct notifier_block *nb
                                  __attribute__((unused)),
                                  unsigned long action,
                                  void *data __attribute__((unused))) {
  switch (action) {
  case PM_HIBERNATION_PREPARE:
  case PM_SUSPEND_PREPARE:
    SvDevirtualizeAllProcessors();
    break;
  case PM_POST_HIBERNATION:
  case PM_POST_SUSPEND:
    /* NOTE: Reset exit flag before re-virtualization!
     *
     */
    atomic_set(&sv_should_exit, 0);
    smp_mb();  /* Ensure flag write visible to all CPUs */
    
    SvVirtualizeAllProcessors();
    break;
  }
  return NOTIFY_OK;
}

/*! =========================================================================
    @brief      Module Entry Point
    ========================================================================= */
static int __init ssvm_init(void) {
  int status;
  unsigned int eax, ebx, ecx, edx;

  // Try to open ESP32 serial console for logging
  uart_file = filp_open("/dev/ttyUSB0", O_WRONLY | O_NOCTTY | O_NONBLOCK, 0);
  if (IS_ERR(uart_file)) {
    pr_warn("[SimpleSvm] ESP32 console: ttyUSB0 not available, continuing "
            "without it\n");
    uart_file = NULL;
  } else {
    register_console(&esp32_console);
    pr_info("[SimpleSvm] ESP32 console: registered on /dev/ttyUSB0\n");
  }

  pr_emerg("[SimpleSvm] === SimpleSvm Loading (module_init) ===\n");
  ssvm_sync_logs(); // SAFE: preemption enabled

  /* Create workqueue for VMEXIT log dumps (process context, serialized) */
  vmexit_log_wq = alloc_workqueue("vmexit_log", WQ_UNBOUND | WQ_MEM_RECLAIM, 1);
  if (!vmexit_log_wq) {
    pr_err("[SimpleSvm] Failed to create vmexit_log workqueue\n");
    if (uart_file) {
      unregister_console(&esp32_console);
      filp_close(uart_file, NULL);
      uart_file = NULL;
    }
    return -ENOMEM;
  }

  pr_emerg("[SimpleSvm] Checking CPU vendor...\n");
  ssvm_sync_logs(); // SAFE: preemption enabled

  cpuid(CPUID_MAX_STANDARD_FN_NUMBER_AND_VENDOR_STRING, &eax, &ebx, &ecx, &edx);
  if ((ebx != 0x68747541) || (edx != 0x69746e65) || (ecx != 0x444d4163)) {
    pr_err("[SimpleSvm] CPU is not AMD\n");
    ssvm_sync_logs(); // SAFE: preemption enabled
    destroy_workqueue(vmexit_log_wq);
    if (uart_file) {
      unregister_console(&esp32_console);
      filp_close(uart_file, NULL);
      uart_file = NULL;
    }
    return -ENODEV;
  }

  pr_emerg("[SimpleSvm] CPU is AMD, calling SvVirtualizeAllProcessors...\n");
  ssvm_sync_logs(); // SAFE: preemption enabled

  // NOTE: Find init_mm BEFORE virtualization (IRQs enabled)!
  // register_kprobe() requires IRQs enabled (calls set_memory_nx).
  // PrepareForVirtualization runs with IRQs disabled, so we must do this here.
  pr_emerg("[SimpleSvm] Finding init_mm symbol...\n");
  status = SvFindInitMm();
  if (status != 0) {
    pr_err("[SimpleSvm] Failed to find init_mm, aborting\n");
    ssvm_sync_logs();
    destroy_workqueue(vmexit_log_wq);
    if (uart_file) {
      unregister_console(&esp32_console);
      filp_close(uart_file, NULL);
      uart_file = NULL;
    }
    return status;
  }
  pr_emerg("[SimpleSvm] init_mm found successfully\n");
  ssvm_sync_logs();

  status = SvVirtualizeAllProcessors();

  pr_emerg("[SimpleSvm] SvVirtualizeAllProcessors returned: %d\n", status);
  ssvm_sync_logs(); // SAFE: preemption enabled

  if (status == 0) {
    ssvm_pm_notifier.notifier_call = SvPowerCallbackRoutine;
    register_pm_notifier(&ssvm_pm_notifier);
    pr_emerg("[SimpleSvm] Virtualized all %d online CPUs\n", num_online_cpus());
    pr_emerg("[SimpleSvm] Module loaded successfully!\n");
    ssvm_sync_logs(); // SAFE: preemption enabled

    /* Start periodic VMEXIT counter dump (1s interval) */
    // 		extern void ssvm_start_counter_dump(void);
    // 		ssvm_start_counter_dump();

    /* DIAGNOSTIC TEST: Trigger CPUID VMEXIT to verify hypervisor is working */
    pr_emerg("[SimpleSvm] === DIAGNOSTIC TEST: Calling CPUID to trigger VMEXIT "
             "===\n");
    ssvm_sync_logs();

    cpuid_count(CPUID_CHECK_PRESENCE, CPUID_CHECK_PRESENCE, &eax, &ebx, &ecx,
                &edx);

    pr_emerg("[SimpleSvm] CPUID result: EAX=0x%x EBX=0x%x ECX=0x%x EDX=0x%x\n",
             eax, ebx, ecx, edx);
    pr_emerg("[SimpleSvm] Expected EAX=0x%x (CPUID_CHECK_PRESENCE)\n",
             CPUID_CHECK_PRESENCE);
    ssvm_sync_logs();

    if (eax == CPUID_CHECK_PRESENCE) {
      pr_emerg(
          "[SimpleSvm] ✓ CPUID intercept WORKING - hypervisor is active!\n");
    } else {
      pr_emerg("[SimpleSvm] ✗ CPUID intercept FAILED - hypervisor may not be "
               "active!\n");
    }
    ssvm_sync_logs();

    /* Wait 2 seconds for VMEXIT log workqueue to dump */
    pr_emerg("[SimpleSvm] Waiting 2 seconds for VMEXIT logs...\n");
    ssvm_sync_logs();
    msleep(2000);

    pr_emerg("[SimpleSvm] === DIAGNOSTIC TEST COMPLETE ===\n");
    ssvm_sync_logs();

    return 0;
  }

  pr_err("[SimpleSvm] Module load failed with status %d\n", status);
  ssvm_sync_logs(); // SAFE: preemption enabled

  /* NOTE: Flush workqueue before destroying it!
   * If virtualization partially succeeded on some CPUs, there may be
   * pending VMEXIT log work. Flush ensures all work completes before
   * we destroy the workqueue. */
  if (vmexit_log_wq)
    flush_workqueue(vmexit_log_wq);

  destroy_workqueue(vmexit_log_wq);
  vmexit_log_wq = NULL;

  if (uart_file) {
    unregister_console(&esp32_console);
    filp_close(uart_file, NULL);
    uart_file = NULL;
  }

  return -ENODEV;
}

/*! =========================================================================
    @brief      Module Exit Point
    ========================================================================= */
static void __exit ssvm_exit(void) {
  /* Stop counter dump timer FIRST (before devirt) */
  // 	extern void ssvm_stop_counter_dump(void);
  // 	ssvm_stop_counter_dump();

  unregister_pm_notifier(&ssvm_pm_notifier);

  /* NOTE: Flush workqueue BEFORE devirtualization!
   * This ensures all pending VMEXIT log dumps complete before
   * we destroy the workqueue. Without this, a VMEXIT handler
   * could queue work while we're destroying the workqueue,
   * causing NULL pointer dereference → kernel panic. */
  if (vmexit_log_wq) {
    flush_workqueue(vmexit_log_wq);
  }

  /* NOTE: NO pr_info/pr_err during devirtualization! */
  SvDevirtualizeAllProcessors();

  /* NOW SAFE: NPT disabled, can use console */
  {
    int devirt_count = atomic_read(&sv_devirt_count);
    int expected_count = num_online_cpus();

    if (devirt_count >= expected_count) {
      pr_info("[SimpleSvm] Devirtualization complete (%d/%d CPUs)\n",
              devirt_count, expected_count);
    } else {
      pr_warn(
          "[SimpleSvm] Devirtualization incomplete (%d/%d CPUs) - timeout!\n",
          devirt_count, expected_count);
      pr_warn(
          "[SimpleSvm] Some CPUs may still be virtualized - crash likely\n");
    }
  }

  /* Now safe to destroy workqueue - no more VMEXITs can occur */
  if (vmexit_log_wq) {
    destroy_workqueue(vmexit_log_wq);
    vmexit_log_wq = NULL;
  }

  // Cleanup ESP32 console
  if (uart_file) {
    unregister_console(&esp32_console);
    filp_close(uart_file, NULL);
    uart_file = NULL;
    pr_info("[SimpleSvm] ESP32 console: unregistered\n");
  }

  pr_info("[SimpleSvm] Module unloaded successfully\n");
}

module_init(ssvm_init);
module_exit(ssvm_exit);

MODULE_AUTHOR("hberk (Satoshi Tanda %40 ported)");
MODULE_DESCRIPTION("SVM-Core educational hypervisor");
MODULE_LICENSE("GPL");
