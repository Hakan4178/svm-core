#include <stdio.h>
#include <stdint.h>
#include <x86intrin.h>
#include <stdlib.h>

#define ITERATIONS 1000000

// İşlemcinin komutları sırasıyla yürütmesini zorlamak için bariyerler
static inline uint64_t read_tsc(void) {
    unsigned int dummy;
    return __rdtscp(&dummy); // rdtsc yerine rdtscp daha hassastır
}

int main() {
    uint64_t start, end, total = 0;
    uint64_t min_latency = -1; // Max value
    uint64_t max_latency = 0;
    uint64_t *latencies = malloc(ITERATIONS * sizeof(uint64_t));

    printf("[+] Benchmarking CPUID (VMEXIT) Latency...\n");
    printf("[+] Iterations: %d\n", ITERATIONS);

    // 1. Isınma Döngüsü (Cache ve Branch Predictor hazırlığı)
    for(int i = 0; i < 100; i++) {
        __asm__ volatile("cpuid" : : "a"(1) : "ebx", "ecx", "edx");
    }

    // 2. Ana Ölçüm Döngüsü
    for(int i = 0; i < ITERATIONS; i++) {
        _mm_lfence(); // Bellek bariyeri: Önceki komutların bittiğinden emin ol
        start = read_tsc();
        
        __asm__ volatile("cpuid" : : "a"(1) : "ebx", "ecx", "edx");
        
        end = read_tsc();
        _mm_lfence();

        uint64_t diff = end - start;
        latencies[i] = diff;
        total += diff;

        if (diff < min_latency) min_latency = diff;
        if (diff > max_latency) max_latency = diff;
    }

    // 3. İstatistikler
    uint64_t avg = total / ITERATIONS;
    printf("\n--- Sonuçlar (Cycles) ---\n");
    printf("Ortalama : %llu\n", avg);
    printf("En Düşük : %llu\n", min_latency);
    printf("En Yüksek: %llu\n", max_latency);

    // Zen 4 işlemcin için tahmini yorum
    if (avg < 500) {
        printf("\n[!] Durum: Native (Hypervisor muhtemelen kapalı veya intercept etmiyor).\n");
    } else if (avg < 3000) {
        printf("\n[!] Durum: Hypervisor Aktif. Gecikme kabul edilebilir seviyede.\n");
    } else {
        printf("\n[!] Durum: Yüksek Gecikme! Handler kodun çok ağır veya çok fazla printk var.\n");
    }

    free(latencies);
    return 0;
}
