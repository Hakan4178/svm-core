#!/bin/bash

echo "=== SimpleSvm VMEXIT Diagnostic Test ==="
echo ""

# Unload if already loaded
sudo rmmod svm-core 2>/dev/null

# Clear dmesg
sudo dmesg -C

echo "Loading module..."
sudo insmod svm-core.ko

echo ""
echo "=== DMESG OUTPUT ==="
sudo dmesg | sudo tail -100

echo ""
echo "=== VMEXIT COUNTERS ==="
if [ -d /sys/module/simple_svm/parameters ]; then
    echo "Total VMEXITs: $(cat /sys/module/simple_svm/parameters/vmexit_total_count 2>/dev/null || echo 'N/A')"
    echo "CPUID VMEXITs: $(cat /sys/module/simple_svm/parameters/vmexit_cpuid_count 2>/dev/null || echo 'N/A')"
    echo "MSR VMEXITs:   $(cat /sys/module/simple_svm/parameters/vmexit_msr_count 2>/dev/null || echo 'N/A')"
    echo "NPF VMEXITs:   $(cat /sys/module/simple_svm/parameters/vmexit_npf_count 2>/dev/null || echo 'N/A')"
    echo "Other VMEXITs: $(cat /sys/module/simple_svm/parameters/vmexit_other_count 2>/dev/null || echo 'N/A')"
else
    echo "Module parameters not available module may have crashed"
fi

echo ""
echo "=== TEST COMPLETE ==="
