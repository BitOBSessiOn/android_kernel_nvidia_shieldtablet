/*
 * GK20A memory management
 *
 * Copyright (c) 2011-2015, NVIDIA CORPORATION.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#ifndef MM_GK20A_H
#define MM_GK20A_H

#include <linux/scatterlist.h>
#include <linux/dma-attrs.h>
#include <linux/iommu.h>
#include <linux/tegra-soc.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <asm/dma-iommu.h>
#include <asm/cacheflush.h>
#include "gk20a_allocator.h"

#ifdef CONFIG_ARM64
#define outer_flush_range(a, b)
#define __cpuc_flush_dcache_area __flush_dcache_area
#endif

#define FLUSH_CPU_DCACHE(va, pa, size)	\
	do {	\
		__cpuc_flush_dcache_area((void *)(va), (size_t)(size));	\
		outer_flush_range(pa, pa + (size_t)(size));		\
	} while (0)

struct mem_desc {
	void *cpu_va;
	struct page **pages;
	struct sg_table *sgt;
	size_t size;
	u64 gpu_va;
};

struct mem_desc_sub {
	u32 offset;
	u32 size;
};

struct gpfifo_desc {
	struct mem_desc mem;
	u32 entry_num;

	u32 get;
	u32 put;

	bool wrap;
};

struct patch_desc {
	struct mem_desc mem;
	u32 data_count;
};

struct zcull_ctx_desc {
	u64 gpu_va;
	u32 ctx_attr;
	u32 ctx_sw_mode;
};

struct gk20a;
struct gr_ctx_buffer_desc {
	void (*destroy)(struct gk20a *, struct gr_ctx_buffer_desc *);
	struct mem_desc mem;
	void *priv;
};

#ifdef CONFIG_ARCH_TEGRA_18x_SOC
#include "gr_t18x.h"
#endif

struct gr_ctx_desc {
	struct mem_desc mem;

	int preempt_mode;
#ifdef CONFIG_ARCH_TEGRA_18x_SOC
	struct gr_ctx_desc_t18x t18x;
#endif
};

#define NVGPU_GR_PREEMPTION_MODE_WFI		0
#define NVGPU_GR_PREEMPTION_MODE_CTA		2

struct compbit_store_desc {
	struct mem_desc mem;

	/* The value that is written to the hardware. This depends on
	 * on the number of ltcs and is not an address. */
	u64 base_hw;
};

struct gk20a_buffer_state {
	struct list_head list;

	/* The valid compbits and the fence must be changed atomically. */
	struct mutex lock;

	/* Offset of the surface within the dma-buf whose state is
	 * described by this struct (one dma-buf can contain multiple
	 * surfaces with different states). */
	size_t offset;

	/* A bitmask of valid sets of compbits (0 = uncompressed). */
	u32 valid_compbits;

	/* The ZBC color used on this buffer. */
	u32 zbc_color;

	/* This struct reflects the state of the buffer when this
	 * fence signals. */
	struct gk20a_fence *fence;
};

enum gmmu_pgsz_gk20a {
	gmmu_page_size_small  = 0,
	gmmu_page_size_big    = 1,
	gmmu_page_size_kernel = 2,
	gmmu_nr_page_sizes    = 3,
};

struct gk20a_comptags {
	u32 offset;
	u32 lines;
	u32 allocated_lines;
	bool user_mappable;
};

struct gk20a_mm_entry {
	/* backing for */
	void *cpu_va;
	struct sg_table *sgt;
	struct page **pages;
	size_t size;
	int pgsz;
	struct gk20a_mm_entry *entries;
	int num_entries;
};

struct priv_cmd_queue {
	struct mem_desc mem;
	u32 size;	/* num of entries in words */
	u32 put;	/* put for priv cmd queue */
	u32 get;	/* get for priv cmd queue */
};

struct priv_cmd_entry {
	u32 *ptr;
	u64 gva;
	u32 get;	/* start of entry in queue */
	u32 size;	/* in words */
	u32 gp_get;	/* gp_get when submitting last priv cmd */
	u32 gp_put;	/* gp_put when submitting last priv cmd */
	u32 gp_wrap;	/* wrap when submitting last priv cmd */
	struct list_head list;	/* node for lists */
};

struct mapped_buffer_node {
	struct vm_gk20a *vm;
	struct rb_node node;
	struct list_head unmap_list;
	struct list_head va_buffers_list;
	struct vm_reserved_va_node *va_node;
	u64 addr;
	u64 size;
	struct dma_buf *dmabuf;
	struct sg_table *sgt;
	struct kref ref;
	u32 user_mapped;
	bool own_mem_ref;
	u32 pgsz_idx;
	u32 ctag_offset;
	u32 ctag_lines;
	u32 ctag_allocated_lines;

	/* For comptag mapping, these are the mapping window parameters */
	bool ctags_mappable;
	u64 ctag_map_win_addr; /* non-zero if mapped */
	u64 ctag_map_win_size; /* non-zero if ctags_mappable */
	u32 ctag_map_win_ctagline; /* ctagline at win start, set if
				    * ctags_mappable */

	u32 flags;
	u32 kind;
	bool va_allocated;
};

struct vm_reserved_va_node {
	struct list_head reserved_va_list;
	struct list_head va_buffers_list;
	u32 pgsz_idx;
	u64 vaddr_start;
	u64 size;
	bool sparse;
};

struct gk20a_mmu_level {
	int hi_bit[2];
	int lo_bit[2];
	int (*update_entry)(struct vm_gk20a *vm,
			   struct gk20a_mm_entry *pte,
			   u32 i, u32 gmmu_pgsz_idx,
			   struct scatterlist **sgl,
			   u64 *offset,
			   u64 *iova,
			   u32 kind_v, u32 *ctag,
			   bool cacheable, bool unmapped_pte,
			   int rw_flag, bool sparse, bool priv);
	size_t entry_size;
};

/* map/unmap batch state */
struct vm_gk20a_mapping_batch
{
	bool gpu_l2_flushed;
	bool need_tlb_invalidate;
};

struct vm_gk20a {
	struct mm_gk20a *mm;
	struct gk20a_as_share *as_share; /* as_share this represents */

	u64 va_start;
	u64 va_limit;

	int num_user_mapped_buffers;

	bool big_pages;   /* enable large page support */
	bool enable_ctag;
	bool mapped;

	u32 big_page_size;

	bool userspace_managed;

	const struct gk20a_mmu_level *mmu_levels;

	struct kref ref;

	struct mutex update_gmmu_lock;

	struct gk20a_mm_entry pdb;

	struct gk20a_allocator vma[gmmu_nr_page_sizes];
	struct rb_root mapped_buffers;

	struct list_head reserved_va_list;

#ifdef CONFIG_TEGRA_GR_VIRTUALIZATION
	u64 handle;
#endif
	u32 gmmu_page_sizes[gmmu_nr_page_sizes];

	/* if non-NULL, kref_put will use this batch when
	   unmapping. Must hold vm->update_gmmu_lock. */
	struct vm_gk20a_mapping_batch *kref_put_batch;
};

struct gk20a;
struct channel_gk20a;

int gk20a_init_mm_support(struct gk20a *g);
int gk20a_init_mm_setup_sw(struct gk20a *g);
int gk20a_init_mm_setup_hw(struct gk20a *g);

int gk20a_mm_fb_flush(struct gk20a *g);
void gk20a_mm_l2_flush(struct gk20a *g, bool invalidate);
void gk20a_mm_l2_invalidate(struct gk20a *g);

struct mm_gk20a {
	struct gk20a *g;

	/* GPU VA default sizes address spaces for channels */
	struct {
		u64 user_size;   /* userspace-visible GPU VA region */
		u64 kernel_size; /* kernel-only GPU VA region */
	} channel;

	struct {
		u32 aperture_size;
		struct vm_gk20a vm;
		struct mem_desc inst_block;
	} bar1;

	struct {
		u32 aperture_size;
		struct vm_gk20a vm;
		struct mem_desc inst_block;
	} bar2;

	struct {
		u32 aperture_size;
		struct vm_gk20a vm;
		struct mem_desc inst_block;
	} pmu;

	struct {
		/* using pmu vm currently */
		struct mem_desc inst_block;
	} hwpm;


	struct mutex l2_op_lock;
#ifdef CONFIG_ARCH_TEGRA_18x_SOC
	struct mem_desc bar2_desc;
#endif
	void (*remove_support)(struct mm_gk20a *mm);
	bool sw_ready;
	int physical_bits;
	bool use_full_comp_tag_line;
#ifdef CONFIG_DEBUG_FS
	u32 ltc_enabled;
	u32 ltc_enabled_debug;
	u32 bypass_smmu;
	u32 disable_bigpage;
#endif
};

int gk20a_mm_init(struct mm_gk20a *mm);

#define gk20a_from_mm(mm) ((mm)->g)
#define gk20a_from_vm(vm) ((vm)->mm->g)

#define dev_from_vm(vm) dev_from_gk20a(vm->mm->g)

#define DEFAULT_ALLOC_ALIGNMENT (4*1024)

static inline int bar1_aperture_size_mb_gk20a(void)
{
	return 16; /* 16MB is more than enough atm. */
}

/*The maximum GPU VA range supported */
#define NV_GMMU_VA_RANGE          38

/* The default userspace-visible GPU VA size */
#define NV_MM_DEFAULT_USER_SIZE   (1ULL << 37)

/* The default kernel-reserved GPU VA size */
#define NV_MM_DEFAULT_KERNEL_SIZE (1ULL << 32)

/*
 * The bottom 16GB of the space are used for small pages, the remaining high
 * memory is for large pages.
 */
static inline u64 __nv_gmmu_va_small_page_limit(void)
{
	return ((u64)SZ_1G * 16);
}

static inline int __nv_gmmu_va_is_big_page_region(struct vm_gk20a *vm, u64 addr)
{
	if (!vm->big_pages)
		return 0;

	return addr >= vm->vma[gmmu_page_size_big].base &&
		addr < vm->vma[gmmu_page_size_big].base +
		vm->vma[gmmu_page_size_big].length;
}

/*
 * This determines the PTE size for a given alloc. Used by both the GVA space
 * allocator and the mm core code so that agreement can be reached on how to
 * map allocations.
 */
static inline enum gmmu_pgsz_gk20a __get_pte_size(struct vm_gk20a *vm,
						  u64 base, u64 size)
{
	/*
	 * Currently userspace is not ready for a true unified address space.
	 * As a result, even though the allocator supports mixed address spaces
	 * the address spaces must be treated as separate for now.
	 */
	if (__nv_gmmu_va_is_big_page_region(vm, base))
		return gmmu_page_size_big;
	else
		return gmmu_page_size_small;
}

#if 0 /*related to addr bits above, concern below TBD on which is accurate */
#define bar1_instance_block_shift_gk20a() (max_physaddr_bits_gk20a() -\
					   bus_bar1_block_ptr_s())
#else
#define bar1_instance_block_shift_gk20a() bus_bar1_block_ptr_shift_v()
#endif

int gk20a_alloc_inst_block(struct gk20a *g, struct mem_desc *inst_block);
void gk20a_free_inst_block(struct gk20a *g, struct mem_desc *inst_block);
void gk20a_init_inst_block(struct mem_desc *inst_block, struct vm_gk20a *vm,
		u32 big_page_size);

void gk20a_mm_dump_vm(struct vm_gk20a *vm,
		u64 va_begin, u64 va_end, char *label);

int gk20a_mm_suspend(struct gk20a *g);

phys_addr_t gk20a_get_phys_from_iova(struct device *d,
				u64 dma_addr);

int gk20a_get_sgtable(struct device *d, struct sg_table **sgt,
			void *cpuva, u64 iova,
			size_t size);

int gk20a_get_sgtable_from_pages(struct device *d, struct sg_table **sgt,
			struct page **pages, u64 iova,
			size_t size);

void gk20a_free_sgtable(struct sg_table **sgt);

u64 gk20a_mm_iova_addr(struct gk20a *g, struct scatterlist *sgl,
		u32 flags);
u64 gk20a_mm_smmu_vaddr_translate(struct gk20a *g, dma_addr_t iova);

void gk20a_mm_ltc_isr(struct gk20a *g);

bool gk20a_mm_mmu_debug_mode_enabled(struct gk20a *g);

int gk20a_mm_mmu_vpr_info_fetch(struct gk20a *g);

u64 gk20a_gmmu_map(struct vm_gk20a *vm,
		struct sg_table **sgt,
		u64 size,
		u32 flags,
		int rw_flag,
		bool priv);

int gk20a_gmmu_alloc_map(struct vm_gk20a *vm,
		size_t size,
		struct mem_desc *mem);

int gk20a_gmmu_alloc_map_attr(struct vm_gk20a *vm,
		enum dma_attr attr,
		size_t size,
		struct mem_desc *mem);

void gk20a_gmmu_unmap_free(struct vm_gk20a *vm,
		struct mem_desc *mem);

int gk20a_gmmu_alloc(struct gk20a *g,
		size_t size,
		struct mem_desc *mem);

int gk20a_gmmu_alloc_attr(struct gk20a *g,
		enum dma_attr attr,
		size_t size,
		struct mem_desc *mem);

void gk20a_gmmu_free(struct gk20a *g,
		struct mem_desc *mem);

void gk20a_gmmu_free_attr(struct gk20a *g,
		enum dma_attr attr,
		struct mem_desc *mem);

static inline phys_addr_t gk20a_mem_phys(struct mem_desc *mem)
{
	/* FIXME: the sgt/sgl may get null if this is accessed e.g. in an isr
	 * during channel deletion - attempt to fix at least null derefs */
	struct sg_table *sgt = mem->sgt;

	if (sgt) {
		struct scatterlist *sgl = sgt->sgl;
		if (sgl)
			return sg_phys(sgl);
	}

	return 0;
}

u64 gk20a_locked_gmmu_map(struct vm_gk20a *vm,
			u64 map_offset,
			struct sg_table *sgt,
			u64 buffer_offset,
			u64 size,
			int pgsz_idx,
			u8 kind_v,
			u32 ctag_offset,
			u32 flags,
			int rw_flag,
			bool clear_ctags,
			bool sparse,
			bool priv,
			struct vm_gk20a_mapping_batch *batch);

void gk20a_gmmu_unmap(struct vm_gk20a *vm,
		u64 vaddr,
		u64 size,
		int rw_flag);

void gk20a_locked_gmmu_unmap(struct vm_gk20a *vm,
			u64 vaddr,
			u64 size,
			int pgsz_idx,
			bool va_allocated,
			int rw_flag,
			bool sparse,
			struct vm_gk20a_mapping_batch *batch);

struct sg_table *gk20a_mm_pin(struct device *dev, struct dma_buf *dmabuf);
void gk20a_mm_unpin(struct device *dev, struct dma_buf *dmabuf,
		    struct sg_table *sgt);

u64 gk20a_vm_map(struct vm_gk20a *vm,
		struct dma_buf *dmabuf,
		u64 offset_align,
		u32 flags /*NVGPU_AS_MAP_BUFFER_FLAGS_*/,
		int kind,
		struct sg_table **sgt,
		bool user_mapped,
		int rw_flag,
		 u64 buffer_offset,
		 u64 mapping_size,
		 struct vm_gk20a_mapping_batch *mapping_batch);

int gk20a_vm_get_compbits_info(struct vm_gk20a *vm,
			       u64 mapping_gva,
			       u64 *compbits_win_size,
			       u32 *compbits_win_ctagline,
			       u32 *mapping_ctagline,
			       u32 *flags);

int gk20a_vm_map_compbits(struct vm_gk20a *vm,
			  u64 mapping_gva,
			  u64 *compbits_win_gva,
			  u64 *mapping_iova,
			  u32 flags);

/* unmap handle from kernel */
void gk20a_vm_unmap(struct vm_gk20a *vm, u64 offset);

void gk20a_vm_unmap_locked(struct mapped_buffer_node *mapped_buffer,
			   struct vm_gk20a_mapping_batch *batch);

/* get reference to all currently mapped buffers */
int gk20a_vm_get_buffers(struct vm_gk20a *vm,
			 struct mapped_buffer_node ***mapped_buffers,
			 int *num_buffers);

/* put references on the given buffers */
void gk20a_vm_put_buffers(struct vm_gk20a *vm,
			  struct mapped_buffer_node **mapped_buffers,
			  int num_buffers);

/* invalidate tlbs for the vm area */
void gk20a_mm_tlb_invalidate(struct vm_gk20a *vm);

/* find buffer corresponding to va */
int gk20a_vm_find_buffer(struct vm_gk20a *vm, u64 gpu_va,
			 struct dma_buf **dmabuf,
			 u64 *offset);

void gk20a_vm_get(struct vm_gk20a *vm);
void gk20a_vm_put(struct vm_gk20a *vm);

void gk20a_vm_remove_support(struct vm_gk20a *vm);

u64 gk20a_vm_alloc_va(struct vm_gk20a *vm,
		     u64 size,
		     enum gmmu_pgsz_gk20a gmmu_pgsz_idx);

int gk20a_vm_free_va(struct vm_gk20a *vm,
		     u64 offset, u64 size,
		     enum gmmu_pgsz_gk20a pgsz_idx);

/* vm-as interface */
struct nvgpu_as_alloc_space_args;
struct nvgpu_as_free_space_args;
int gk20a_vm_alloc_share(struct gk20a_as_share *as_share, u32 big_page_size,
			 u32 flags);
int gk20a_vm_release_share(struct gk20a_as_share *as_share);
int gk20a_vm_alloc_space(struct gk20a_as_share *as_share,
			 struct nvgpu_as_alloc_space_args *args);
int gk20a_vm_free_space(struct gk20a_as_share *as_share,
			struct nvgpu_as_free_space_args *args);
int gk20a_vm_bind_channel(struct gk20a_as_share *as_share,
			  struct channel_gk20a *ch);
int __gk20a_vm_bind_channel(struct vm_gk20a *vm, struct channel_gk20a *ch);

/* batching eliminates redundant cache flushes and invalidates */
void gk20a_vm_mapping_batch_start(struct vm_gk20a_mapping_batch *batch);
void gk20a_vm_mapping_batch_finish(
	struct vm_gk20a *vm, struct vm_gk20a_mapping_batch *batch);
/* called when holding vm->update_gmmu_lock */
void gk20a_vm_mapping_batch_finish_locked(
	struct vm_gk20a *vm, struct vm_gk20a_mapping_batch *batch);


/* Note: batch may be NULL if map op is not part of a batch */
int gk20a_vm_map_buffer(struct vm_gk20a *vm,
			int dmabuf_fd,
			u64 *offset_align,
			u32 flags, /* NVGPU_AS_MAP_BUFFER_FLAGS_ */
			int kind,
			u64 buffer_offset,
			u64 mapping_size,
			struct vm_gk20a_mapping_batch *batch);

int gk20a_init_vm(struct mm_gk20a *mm,
		struct vm_gk20a *vm,
		u32 big_page_size,
		u64 low_hole,
		u64 kernel_reserved,
		u64 aperture_size,
		bool big_pages,
		bool userspace_managed,
		char *name);
void gk20a_deinit_vm(struct vm_gk20a *vm);

/* Note: batch may be NULL if unmap op is not part of a batch */
int gk20a_vm_unmap_buffer(struct vm_gk20a *vm, u64 offset,
			  struct vm_gk20a_mapping_batch *batch);
void gk20a_get_comptags(struct device *dev, struct dma_buf *dmabuf,
			struct gk20a_comptags *comptags);
dma_addr_t gk20a_mm_gpuva_to_iova_base(struct vm_gk20a *vm, u64 gpu_vaddr);

int gk20a_dmabuf_alloc_drvdata(struct dma_buf *dmabuf, struct device *dev);

int gk20a_dmabuf_get_state(struct dma_buf *dmabuf, struct device *dev,
			   u64 offset, struct gk20a_buffer_state **state);

int map_gmmu_pages(struct gk20a_mm_entry *entry);
void unmap_gmmu_pages(struct gk20a_mm_entry *entry);
void pde_range_from_vaddr_range(struct vm_gk20a *vm,
					      u64 addr_lo, u64 addr_hi,
					      u32 *pde_lo, u32 *pde_hi);
int gk20a_mm_pde_coverage_bit_count(struct vm_gk20a *vm);
u32 *pde_from_index(struct vm_gk20a *vm, u32 i);
u32 pte_index_from_vaddr(struct vm_gk20a *vm,
			       u64 addr, enum gmmu_pgsz_gk20a pgsz_idx);
void free_gmmu_pages(struct vm_gk20a *vm,
		     struct gk20a_mm_entry *entry);

u32 gk20a_mm_get_physical_addr_bits(struct gk20a *g);

struct gpu_ops;
void gk20a_init_mm(struct gpu_ops *gops);
const struct gk20a_mmu_level *gk20a_mm_get_mmu_levels(struct gk20a *g,
						      u32 big_page_size);
void gk20a_mm_init_pdb(struct gk20a *g, void *inst_ptr, u64 pdb_addr);

extern const struct gk20a_mmu_level gk20a_mm_levels_64k[];
extern const struct gk20a_mmu_level gk20a_mm_levels_128k[];

static inline void *nvgpu_alloc(size_t size, bool clear)
{
	void *p;

	if (size > PAGE_SIZE) {
		if (clear)
			p = vzalloc(size);
		else
			p = vmalloc(size);
	} else {
		if (clear)
			p = kzalloc(size, GFP_KERNEL);
		else
			p = kmalloc(size, GFP_KERNEL);
	}

	return p;
}

static inline void nvgpu_free(void *p)
{
	if (virt_addr_valid(p))
		kfree(p);
	else
		vfree(p);
}

#endif /* MM_GK20A_H */
