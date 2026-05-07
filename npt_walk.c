// SPDX-License-Identifier: GPL-2.0-only
/*
 * NPT Identity Map Builder
 *
 * Creates a 4-level AMD64 NPT (Nested Page Table) that identity-maps
 * physical RAM (GPA == HPA) using 2MB leaf entries for performance.
 */

#include <asm/page.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "npt_walk.h"

/*
 * NPT PAT/Cache type bits for 2MB pages (AMD64 APM Vol.2 Table 5-6):
 *   PWT (bit 3), PCD (bit 4), PAT (bit 12 for large pages)
 *
 * WB  (Write-Back)     = PWT=0 PCD=0 PAT=0  → normal RAM
 * UC  (Uncacheable)     = PWT=1 PCD=1 PAT=0  → MMIO / I/O regions
 */
#define NPT_PWT (1ULL << 3)
#define NPT_PCD (1ULL << 4)
#define NPT_PAT_LARGE (1ULL << 12)

#define NPT_CACHE_WB 0ULL
#define NPT_CACHE_UC (NPT_PWT | NPT_PCD)

/*
 * Allocate a zeroed page and track it in the context for cleanup.
 * Returns the kernel virtual address, or NULL on failure.
 */
static u64 *npt_alloc_page(struct npt_context *ctx)
{
	struct page *p;

	if (ctx->page_count >= NPT_MAX_PAGES) {
		pr_err("[NPT] page limit (%d) exceeded\n", NPT_MAX_PAGES);
		return NULL;
	}

	p = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!p)
		return NULL;

	ctx->pages[ctx->page_count++] = p;
	return (u64 *)page_address(p);
}

/*
 * npt_build_identity_map - Build a 4-level identity mapped NPT
 * @ctx: NPT context to populate
 * @phys_limit: upper bound of physical address space to map (bytes)
 *
 * Creates PML4 → PDPT → PD entries with 2MB leaf pages (PS=1).
 * No PT level is needed for 2MB identity mapping.
 *
 * Address space breakdown for 2MB pages:
 *   PML4 index: bits [47:39]  — up to 512 entries
 *   PDPT index: bits [38:30]  — up to 512 entries per PML4e
 *   PD   index: bits [29:21]  — up to 512 entries per PDPTe
 *   Each PD entry maps 2MB when PS=1.
 *
 * For 4GB RAM: 1 PML4e, 4 PDPTe, 2048 PDe → ~6 pages total.
 * For 64GB RAM: 1 PML4e, 64 PDPTe, 32768 PDe → ~129 pages total.
 */
int npt_build_identity_map(struct npt_context *ctx, u64 phys_limit, u64 ram_end)
{
	u64 *pml4, *pdpt, *pd;

	memset(ctx, 0, sizeof(*ctx));

	/* Allocate PML4 (root) */
	pml4 = npt_alloc_page(ctx);
	if (!pml4)
		return -ENOMEM;

	ctx->pml4 = pml4;
	ctx->pml4_pa = virt_to_phys(pml4);

	pr_info("[NPT] Building identity map up to 0x%llx (%llu GB), ram_end=0x%llx\n", phys_limit,
		phys_limit >> 30, ram_end);

	/*
	 * Three-level nested loop: PML4 → PDPT → PD (2MB leaf pages).
	 *
	 * Cache type strategy (replaces page_is_ram() which deadlocked):
	 *   addr < ram_end  → WB (Write-Back):  System RAM
	 *   addr >= ram_end → UC (Uncacheable): GPU BARs, MMIO, PCI
	 *
	 * For MMIO holes below ram_end (APIC at 0xFEE00000, HPET, GPU
	 * doorbell), AMD MTRR enforces UC regardless of NPT cache type.
	 * AMD APM Vol.2 §7.8: final type = most restrictive(MTRR, NPT).
	 * So WB in NPT + UC in MTRR = UC. Safe.
	 */
	{
		int n_pml4 = (int)((phys_limit + (1ULL << 39) - 1) >> 39);
		int n_pdpt, n_pd;
		int pi, di, ei;

		if (n_pml4 > 512)
			n_pml4 = 512;

		for (pi = 0; pi < n_pml4; pi++) {
			pdpt = npt_alloc_page(ctx);
			if (!pdpt)
				goto fail;
			pml4[pi] = virt_to_phys(pdpt) | NPT_DEFAULT_FLAGS;

			{
				u64 pml4_end = (u64)(pi + 1) << 39;
				if (pml4_end > phys_limit)
					n_pdpt = (int)((phys_limit - ((u64)pi << 39) +
							(1ULL << 30) - 1) >>
						       30);
				else
					n_pdpt = 512;
				if (n_pdpt > 512)
					n_pdpt = 512;
			}

			for (di = 0; di < n_pdpt; di++) {
				u64 gb_base = ((u64)pi << 39) | ((u64)di << 30);

				cond_resched();

				pd = npt_alloc_page(ctx);
				if (!pd)
					goto fail;
				pdpt[di] = virt_to_phys(pd) | NPT_DEFAULT_FLAGS;

				{
					u64 pdpt_end = gb_base + (1ULL << 30);
					if (pdpt_end > phys_limit)
						n_pd = (int)((phys_limit - gb_base + (2ULL << 20) -
							      1) >>
							     21);
					else
						n_pd = 512;
					if (n_pd > 512)
						n_pd = 512;
				}

				for (ei = 0; ei < n_pd; ei++) {
					u64 addr = gb_base | ((u64)ei << 21);
					u64 cache_flags;

					/*
					 * Simple O(1) cache type: no locks, no semaphores.
					 * page_is_ram() acquired iomem_resource lock per
					 * call → 524K lock acquisitions → deadlock.
					 */
					cache_flags =
					    (addr < ram_end) ? NPT_CACHE_WB : NPT_CACHE_UC;

					pd[ei] = addr | NPT_DEFAULT_FLAGS | NPT_PS | cache_flags;
				}
			}
		}
	}

	pr_info("[NPT] Identity map built: %d pages allocated, root PA=0x%llx\n", ctx->page_count,
		(u64)ctx->pml4_pa);

	/*
	 * HPET MMIO guard: Mark 0xFED00000 (HPET base) as NX.
	 * Prevents guest from reading hardware timer via MMIO,
	 * which would reveal real elapsed time despite TSC offset.
	 */
	npt_set_page_nx(ctx, 0xFED00000ULL);

	return 0;

fail:
	pr_err("[NPT] Allocation failed after %d pages\n", ctx->page_count);
	npt_destroy(ctx);
	return -ENOMEM;
}


void npt_destroy(struct npt_context *ctx)
{
	int i;

	/* Free all allocated pages - safe only after all CPUs devirtualized */
	for (i = 0; i < ctx->page_count; i++) {
		if (ctx->pages[i])
			__free_page(ctx->pages[i]);
	}

	ctx->pml4 = NULL;
	ctx->pml4_pa = 0;
	ctx->page_count = 0;

	/* NO LOGGING - caller will log after devirtualization completes */
}

/*
 * npt_set_page_nx - Mark a GPA's 2MB region as NX in the NPT
 * @ctx: NPT context
 * @gpa: guest physical address within a 2MB region
 *
 * Used to hide our own hypervisor code pages from guest execution.
 */
int npt_set_page_nx(struct npt_context *ctx, u64 gpa)
{
	int pml4_idx = (gpa >> 39) & 0x1FF;
	int pdpt_idx = (gpa >> 30) & 0x1FF;
	int pd_idx = (gpa >> 21) & 0x1FF;
	u64 *pdpt, *pd;

	if (!ctx->pml4 || !(ctx->pml4[pml4_idx] & NPT_PRESENT))
		return -ENOENT;

	pdpt = phys_to_virt(ctx->pml4[pml4_idx] & ~0xFFFULL);
	if (!(pdpt[pdpt_idx] & NPT_PRESENT))
		return -ENOENT;

	pd = phys_to_virt(pdpt[pdpt_idx] & ~0xFFFULL);
	pd[pd_idx] |= NPT_NX;

	pr_info("[NPT] GPA 0x%llx marked NX\n", gpa & ~((2ULL << 20) - 1));
	return 0;
}

/*
 * npt_set_page_ro - Mark a GPA's 2MB region as Read-Only in the NPT
 * @ctx: NPT context
 * @gpa: guest physical address
 */
int npt_set_page_ro(struct npt_context *ctx, u64 gpa)
{
	int pml4_idx = (gpa >> 39) & 0x1FF;
	int pdpt_idx = (gpa >> 30) & 0x1FF;
	int pd_idx = (gpa >> 21) & 0x1FF;
	u64 *pdpt, *pd;

	if (!ctx->pml4 || !(ctx->pml4[pml4_idx] & NPT_PRESENT))
		return -ENOENT;

	pdpt = phys_to_virt(ctx->pml4[pml4_idx] & ~0xFFFULL);
	if (!(pdpt[pdpt_idx] & NPT_PRESENT))
		return -ENOENT;

	pd = phys_to_virt(pdpt[pdpt_idx] & ~0xFFFULL);
	pd[pd_idx] &= ~NPT_WRITE;

	return 0;
}

/*
 * npt_set_page_rw - Mark a GPA's 2MB region as Read-Write in the NPT
 */
int npt_set_page_rw(struct npt_context *ctx, u64 gpa)
{
	int pml4_idx = (gpa >> 39) & 0x1FF;
	int pdpt_idx = (gpa >> 30) & 0x1FF;
	int pd_idx = (gpa >> 21) & 0x1FF;
	u64 *pdpt, *pd;

	if (!ctx->pml4 || !(ctx->pml4[pml4_idx] & NPT_PRESENT))
		return -ENOENT;

	pdpt = phys_to_virt(ctx->pml4[pml4_idx] & ~0xFFFULL);
	if (!(pdpt[pdpt_idx] & NPT_PRESENT))
		return -ENOENT;

	pd = phys_to_virt(pdpt[pdpt_idx] & ~0xFFFULL);
	pd[pd_idx] |= NPT_WRITE;

	return 0;
}
