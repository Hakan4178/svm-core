# SVM-Core

**Educational AMD SVM (Secure Virtual Machine) hypervisor for Linux kernel**

A minimal, production-quality type-1.5 hypervisor demonstrating AMD-V virtualization technology. This project implements a bluepill style hypervisor that virtualizes a running Linux system transparently.

[![License: GPL-2.0](https://img.shields.io/badge/License-GPL--2.0-blue.svg)](LICENSE)
[![Kernel: 6.x-7.x](https://img.shields.io/badge/Kernel-6.x--7.x-green.svg)]()
[![Arch: x86_64](https://img.shields.io/badge/Arch-x86__64-orange.svg)]()

---

## Features

### Core Hypervisor
- **Transparent Virtualization**: Virtualizes running Linux kernel without reboot
- **AMD SVM/NPT**: Full AMD-V support with Nested Page Tables
- **Multi-Core**: Per-CPU virtualization with ASID-based TLB isolation
- **Minimal Overhead**: ~100-1000 VMEXITs/sec (CPUID intercepts only)
- **Safe Stack**: 64KB isolated stack prevents guest corruption
- **Tested on**: Arch 7.04, Debian 6.12.x 

### Advanced Features
- **Statik NPT**: page table allocator 1TB (2MB Huge Pages)
- **Power Management**: Suspend/resume support with automatic devirtualization
- **Diagnostic Tools**: Per-CPU VMEXIT counters, ring buffer logging

### Security & Stability
- **Paranoid Validation**: Comprehensive VMCB/NPT/GDTR/IDTR checks
- **Race-Free**: Atomic operations, memory barriers, per-CPU flags
- **NMI-Safe**: GIF-based interrupt masking during critical sections
- **Graceful Degradation**: Automatic devirtualization on errors

---

### Virtualization Flow

```
1. Module Load (module_init)
   ├─ Check AMD SVM support (CPUID)
   ├─ Build NPT identity map (1TB coverage)
   ├─ Allocate MSRPM (MSR permissions)
   └─ Virtualize all CPUs (SMP)

2. Per-CPU Virtualization
   ├─ Allocate VpData (VMCB, safe stack)
   ├─ Capture CPU context (segments, GDTR, IDTR, GPRs)
   ├─ Setup Guest VMCB (intercepts, NPT, ASID)
   ├─ Setup Host VMCB (VMSAVE for host state)
   └─ Enter VMRUN loop

3. VMRUN Loop (assembly)
   ├─ clgi (block NMI/SMI)
   ├─ vmload guest (load FS/GS/TR/LDTR)
   ├─ vmrun (enter guest)
   │   └─ Guest executes (Linux kernel)
   ├─ #VMEXIT (hardware transition)
   ├─ vmsave guest (save FS/GS/TR/LDTR)
   ├─ vmload host (restore host state)
   ├─ stgi (unblock interrupts)
   └─ Return to C handler

4. VMEXIT Handling
   ├─ CPUID → Devirt check
   ├─ MSR   → EFER/LSTAR intercepts
   ├─ VMRUN → Prevent nested virtualization
   └─ NPF   → Log and devirtualize

5. Devirtualization (module_exit)
   ├─ Set global exit flag
   ├─ Force CPUID on all CPUs
   ├─ Wait for all CPUs to devirtualize
   ├─ Free NPT, MSRPM, VpData
   └─ System returns to bare-metal
```

---

## Requirements

### Hardware
- **CPU**: AMD processor with SVM support
  - Check: `grep -o 'svm' /proc/cpuinfo`
  - Requires: AMD-V, NPT (Nested Page Tables)
- **RAM**: Minimum 4GB (8GB+ recommended)
- **Architecture**: x86_64 only

### Software
- **Kernel**: Linux 5.11 - 7.x
  - Tested on: Arch Linux 7.x
- **Build Tools**:
  ```bash
  # Arch Linux
  sudo pacman -S base-devel linux-headers

  # Ubuntu/Debian
  sudo apt install build-essential linux-headers-$(uname -r)
  ```
- **Optional**: `sparse` for static analysis
  ```bash
  # Arch Linux
  sudo pacman -S sparse

  # Ubuntu/Debian
  sudo apt install sparse
  ```

### BIOS/UEFI Settings
- **Enable**: AMD-V / SVM
- **Disable**: Secure Boot (if module signing not configured)

---

## Installation

### 1. Clone Repository
```bash
git clone https://github.com/Hakan4178/svm-core.git
cd svm-core
```

### 2. Prepare System
```bash
# Remove conflicting modules (KVM, ZRAM)
./prepare_hv.sh

# Verify SVM support
grep -o 'svm' /proc/cpuinfo
```

### 3. Build Module
```bash
# Standard build
make

# Clean build
make clean && make
```

### 4. Load Module
```bash
# Load module
sudo insmod svm-core.ko

# Verify virtualization
sudo dmesg -w
# Expected: "Virtualized all X online CPUs"

# Check VMEXIT counters
cat /sys/module/svm-core/parameters/vmexit_total_count
```

### 5. Unload Module
```bash
# Unload (automatic devirtualization)
sudo rmmod svm_core

# Verify bare-metal
sudo dmesg -w
# Expected: "All CPUs devirtualized"
```

---

#### Custom VMEXIT Handling
Modify `ssvm_vmexit.c` to add custom handlers:
```c
case VMEXIT_CUSTOM:
    // Your handler here
    SvHandleCustomExit(VpData, GuestContext);
    break;
```

---

### Error Handling
- Comprehensive NULL checks
- Proper Linux errno codes (-ENOMEM, -EFAULT, -ENODEV)
- Graceful degradation (devirtualization on errors)
- Atomic operations for race-free SMP

---

## Troubleshooting

### System Hangs/Crashes

#### Triple Fault
```bash
# Check dmesg before crash and share me
journalctl -k -b -1

# Common causes:
# 1. NPF (Nested Page Fault) - GPU MMIO not mapped
# 2. Invalid VMCB - alignment or reserved bits
# 3. Stack corruption - safe stack overflow
```


## License

This project is licensed under the **GPL-2.0 License** - see the [LICENSE](LICENSE) file for details.

### Third-Party Components
- **SimpleSvm.hpp**: Based on [SimpleSVM](https://github.com/tandasat/SimpleSvm) by Satoshi Tanda (MIT License)
- **AMD SVM Specification**: Public documentation (AMD Architecture Programmer's Manual)

---

## Acknowledgments

### Original Work
- **Satoshi Tanda** - [SimpleSVM](https://github.com/tandasat/SimpleSvm) (Windows hypervisor)
  - VMCB structures, VMEXIT codes, core virtualization logic

### Technical References
- **AMD Architecture Programmer's Manual** - Vol. 2 (System Programming)
- **Linux Kernel** - KVM/SVM implementation (`arch/x86/kvm/svm/`)

---


<div align="center">

**⚠️ Educational Purpose Only ⚠️**

This software is provided for educational and research purposes.  
This software does not provide any guarantees

Made with ❤️ 

</div>

