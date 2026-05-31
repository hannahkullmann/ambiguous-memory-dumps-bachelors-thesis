// kernel/kernel_instance_pool.c - dedicated page pool for the ghost instance

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/genalloc.h>
#include <linux/gfp.h>
#include <linux/mm.h>
#include <linux/page-flags.h>
#include <linux/kernel_instance.h>
#include <linux/export.h>
#include <linux/string.h>

static struct gen_pool *ki_page_pool; // Pointer to the page pool
static phys_addr_t ki_pool_cage_start; // Start address of the ghost cage
static phys_addr_t ki_pool_cage_size; // Size of the ghost cage

static phys_addr_t ki_genpool_phys_start; // Start address of the gen_pool
static phys_addr_t ki_genpool_phys_end; // End address of the gen_pool

// Initialization of the ghost page pool using the reserved ghost window
static int __init ki_page_pool_init(void)
{
	phys_addr_t pstart = READ_ONCE(ghost_ki_mem_start);
	phys_addr_t psize  = READ_ONCE(ghost_ki_mem_size);
	unsigned long vaddr;
	int ret;

	// Check activity of ghost cage
	if (!pstart || !psize) {
		pr_info("[BA Adjustments] KI: page pool disabled (no ghost window)\n");
		ki_genpool_phys_start = 0;
		ki_genpool_phys_end = 0;
		return 0;
	}

	// Create the page pool (gen_pool)
	ki_page_pool = gen_pool_create(PAGE_SHIFT, -1); // PAGE_SHIFT: minimum allocation order, NUMA node ID = -1: pool works on all nodes
	if (!ki_page_pool) {
		pr_err("[BA Adjustments] KI: gen_pool_create failed\n");
		ki_genpool_phys_start = 0;
		ki_genpool_phys_end = 0;
		return -ENOMEM;
	}

	// Convert the physical address to a virtual address
	vaddr = (unsigned long)__va(pstart);

	// Add the virtual address to the page pool
	ret = gen_pool_add_virt(ki_page_pool, vaddr, pstart, psize, -1);
	if (ret) {
		pr_err("[BA Adjustments] KI: gen_pool_add failed (%d)\n", ret);
		gen_pool_destroy(ki_page_pool);
		ki_page_pool = NULL;
		ki_genpool_phys_start = 0;
		ki_genpool_phys_end = 0;
		return ret;
	}

	// Set the start and size of the ghost cage and the gen_pool
	ki_pool_cage_start = pstart;
	ki_pool_cage_size = psize;
	ki_genpool_phys_start = pstart;
	ki_genpool_phys_end = pstart + psize;

	{
		// Provide debug information
		phys_addr_t pend = pstart + psize;

		pr_info("[BA Adjustments] KI: ghost page pool online: cage [%pa .. %pa), gen_pool %llu MiB\n",
			&pstart, &pend, (unsigned long long)(psize >> 20));
	}
	return 0;
}
subsys_initcall(ki_page_pool_init);

// Check if the page is in the ghost cage
bool ki_page_in_ghost_cage(const struct page *page)
{
	phys_addr_t g0 = READ_ONCE(ki_genpool_phys_start);
	phys_addr_t g1 = READ_ONCE(ki_genpool_phys_end);
	phys_addr_t p;

	// Safety Check
	if (!page || !g0 || !g1 || g1 <= g0)
		return false;

	// Convert the page to a physical address	
	p = page_to_phys((struct page *)page);

	// Check if the physical address is in the ghost cage
	return p >= g0 && p < g1;
}

// Check if the address space is in the ghost cage
static bool ki_phys_in_genpool(phys_addr_t p, size_t bytes)
{
	phys_addr_t g0 = READ_ONCE(ki_genpool_phys_start);
	phys_addr_t g1 = READ_ONCE(ki_genpool_phys_end);

	// Safety Check
	if (!g0 || !g1 || g1 <= g0 || !bytes)
		return false;
	// Check if physical address is within the ghost cage
	if (p < g0)
		return false;
	if (p >= g1)
		return false;
	// Check if the size is within the ghost cage
	if (bytes > (size_t)(g1 - p))
		return false;
	return true;
}

struct page *ki_alloc_ghost_pages(gfp_t gfp, unsigned int order)
{
	// Calculate the number of bytes to allocate (2^order * PAGE_SIZE)
	size_t bytes = PAGE_SIZE << order;

	unsigned long vaddr;
	phys_addr_t paddr;
	struct page *page;
	unsigned int i;

	// Check if the page pool is initialized
	if (!ki_page_pool)
		return NULL;
 
	{
		// Ensure that the allocated block is aligned to the block size
		struct genpool_data_align align_data = {
			.align = PAGE_SIZE << order,
		};

		// Allocate the pages from the page pool
		vaddr = gen_pool_alloc_algo(ki_page_pool, bytes,
					    gen_pool_first_fit_align,
					    &align_data);
	}

	// Check if allocation succeeded
	if (!vaddr)
		return NULL;

	// Convert the virtual address to a physical address
	paddr = __pa((void *)vaddr);
	page = phys_to_page(paddr);

	// Clear the page flags
	// Loop through all pages in the allocated block
	for (i = 0; i < (1u << order); i++) {
		struct page *p = page + i;

		if (PageReserved(p))			// If the page is reserved, clear the reserved flag
			ClearPageReserved(p);
		if (PageHead(p))				// If the page is a head, clear the head flag
			ClearPageHead(p);
		
		clear_compound_head(p);			// Clear the compound head
		set_page_count(p, 0);			// Set the page count to 0 
	}

	return page;
}

void ki_free_ghost_pages(struct page *page, unsigned int order)
{
	size_t bytes;
	unsigned long vaddr;
	phys_addr_t paddr;
	unsigned int i;
	struct folio *folio;
	unsigned int fo;

	// Check for page and ki_page_pool
	if (!page || !ki_page_pool)
		return;

	folio = page_folio(page); // Get the folio from the page
	fo = folio_order(folio); // Get the order from the folio (block size)
	if (fo) {
		order = fo;
		page = &folio->page; //Pointer to the first page (head) of the block
	} else if (PageCompound(page)) {
		page = compound_head(page);
		order = compound_order(page);
	}

	// Check if the page is in the ghost pool
	bytes = PAGE_SIZE << order;
	paddr = page_to_phys(page);
	if (!ki_phys_in_genpool(paddr, bytes))
		return;

	// Clear the page flags
	// Loop through all pages in the freed block

	for (i = 0; i < (1u << order); i++) {
		struct page *p = page + i;

		if (PageHead(p))
			ClearPageHead(p);
		clear_compound_head(p);
		set_page_count(p, 0);
		SetPageReserved(p);
	}

	// Free the pages
	vaddr = (unsigned long)__va(paddr);
	gen_pool_free(ki_page_pool, vaddr, bytes);
}
