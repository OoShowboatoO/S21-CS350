/*
 * Copyright (c) 2000, 2001, 2002, 2003, 2004, 2005, 2008, 2009
 *	The President and Fellows of Harvard College.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE UNIVERSITY AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE UNIVERSITY OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <spinlock.h>
#include <proc.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include "opt-A3.h"
#include <mainbus.h>
#include <syscall.h>

/*
 * Dumb MIPS-only "VM system" that is intended to only be just barely
 * enough to struggle off the ground.
 */

/* under dumbvm, always have 48k of user stack */
#define DUMBVM_STACKPAGES    12

/*
 * Wrap rma_stealmem in a spinlock.
 */
static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;

#if OPT_A3
static struct spinlock coremap_lock = SPINLOCK_INITIALIZER;
static int number_of_pages = 0;
static paddr_t coremap_start = 0;
static paddr_t frame_start = 0;
static paddr_t frame_end = 0;
static bool coremap_created = false;
#endif

void
vm_bootstrap(void)
{
	/* Do nothing. */
#if OPT_A3
	paddr_t curr = 0;
	ram_getsize(&coremap_start,&curr);
	paddr_t diff_to_start = curr - coremap_start;
	number_of_pages = diff_to_start / (PAGE_SIZE + 4);
	coremap_start = PADDR_TO_KVADDR(coremap_start);
	for (int i = 0; i < number_of_pages; i++){
		((int *)coremap_start)[i] = 0;
	}
	
	frame_start = (coremap_start - MIPS_KSEG0) + number_of_pages * 4;
	paddr_t reminder = frame_start % PAGE_SIZE;
	if (reminder != 0) {
		frame_start = PAGE_SIZE * ((frame_start / PAGE_SIZE) + 1 );
	}
	
	coremap_created = true;
	number_of_pages = (curr - frame_start) / PAGE_SIZE;
	paddr_t total_size = PAGE_SIZE * number_of_pages;
	frame_end = total_size + frame_start;
	
#endif
}

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;

#if OPT_A3
	if (coremap_created) {
		spinlock_acquire(&coremap_lock);
		addr = frame_start;
		paddr_t ending_point = npages * PAGE_SIZE + addr;
		while (ending_point <= frame_end){
			bool is_found = true;
			int entry = 0;
			for (size_t i = 0; i < npages; i++){
				int index = (addr + i * PAGE_SIZE - frame_start) / PAGE_SIZE;
				entry = ((int *)coremap_start)[index];
				if (entry != 0){
					is_found = false;
					addr += PAGE_SIZE * (i + 1);
					ending_point = PAGE_SIZE * npages + addr;
					break;
				}
			}
			
			if (is_found){
				for (size_t j = 0; j < npages; j++){				
					int index = ( PAGE_SIZE * j + addr - frame_start) / PAGE_SIZE;	
					((int *)coremap_start)[index] = j + 1;
				}
				spinlock_release(&coremap_lock);
				return addr;
			}
		}
		spinlock_release(&coremap_lock);
		return 0;
	}
#endif
	spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	
	spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t 
alloc_kpages(int npages)
{
	paddr_t pa;
	pa = getppages(npages);
	if (pa==0) {
		return 0;
	}
	return PADDR_TO_KVADDR(pa);
}

void 
free_kpages(vaddr_t addr)
{
	/* nothing - leak the memory. */
#if OPT_A3
	spinlock_acquire(&coremap_lock);
	int index = 1;
	int start = ((addr - MIPS_KSEG0) - frame_start) / PAGE_SIZE;
	for(int i = start; i < number_of_pages; i++) {
		if (((int *)coremap_start)[i] == index) {
			((int *)coremap_start)[i] = 0;
			index ++;
		}
	}
	spinlock_release(&coremap_lock);

#else
	(void)addr;
#endif
}

void
vm_tlbshootdown_all(void)
{
	panic("dumbvm tried to do tlb shootdown?!\n");
}

void
vm_tlbshootdown(const struct tlbshootdown *ts)
{
	(void)ts;
	panic("dumbvm tried to do tlb shootdown?!\n");
}

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;

	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);
	// kprintf("vm_fault start!\n");

	switch (faulttype) {
	    case VM_FAULT_READONLY:
	
	#if OPT_A3
		return EINVAL;
	#endif
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	// #if OPT_A3
	// 	return EINVAL;
	// #endif

	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	if (curproc == NULL) {
		/*
		 * No process. This is probably a kernel fault early
		 * in boot. Return EFAULT so as to panic instead of
		 * getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	as = curproc_getas();
	if (as == NULL) {
		/*
		 * No address space set up. This is probably also a
		 * kernel fault early in boot.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	KASSERT(as->as_vbase1 != 0);
#if OPT_A3
	KASSERT(as->as_pbase1 != NULL);
#else
	KASSERT(as->as_pbase1 != 0);
#endif
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);

#if OPT_A3
	KASSERT(as->as_pbase2 != NULL);
#else
	KASSERT(as->as_pbase2 != 0);
#endif
	KASSERT(as->as_npages2 != 0);

#if OPT_A3
	KASSERT(as->as_stackpbase != NULL);
#else
	KASSERT(as->as_stackpbase != 0);
#endif
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);

#if OPT_A3
	for (size_t i = 0; i < as->as_npages1; i++) {
		KASSERT((as->as_pbase1[i] & PAGE_FRAME) == as->as_pbase1[i]);
	}
#else
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
#endif
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);

#if OPT_A3
	for (size_t i = 0; i < as->as_npages2; i++) {
		KASSERT((as->as_pbase2[i] & PAGE_FRAME) == as->as_pbase2[i]);
	}
	for (size_t i = 0; i < DUMBVM_STACKPAGES; i++) {
		KASSERT((as->as_stackpbase[i] & PAGE_FRAME) == as->as_stackpbase[i]);
	}
#else
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);
#endif

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;

#if OPT_A3
	// kprintf("one!\n");
	bool read_only = false;
	bool loadelf_finished = as->loadelf_done;

	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		read_only = true;
		int base1_diff = faultaddress - vbase1;
		int index = base1_diff / PAGE_SIZE;
		int offset = base1_diff % PAGE_SIZE;
		paddr = as->as_pbase1[index] + offset;
		// kprintf("code segemnt\n");
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		int base2_diff = faultaddress - vbase2;
		int index = base2_diff / PAGE_SIZE;
		int offset = base2_diff % PAGE_SIZE;
		paddr = as->as_pbase2[index] + offset;
		// kprintf("data segment\n");
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		int stakebase_diff = faultaddress - stackbase;
		int index = stakebase_diff/PAGE_SIZE;
		int offset = stakebase_diff % PAGE_SIZE;
		paddr = as->as_stackpbase[index] + offset;
		// kprintf("stack!\n");
	}
	else {
		return EFAULT;
	}

#else
	if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}
#endif

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	#if OPT_A3
		// kprintf("two!\n");
		// Here we found a unused entry, so we can simply use the entry
		if (read_only && loadelf_finished) {
			elo &= ~TLBLO_DIRTY;
		}
	#endif
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		// kprintf("faultaddress is 0x%d\n", faultaddress);
		// kprintf("paddr is 0x%d\n", paddr);
		tlb_write(ehi, elo, i);
		splx(spl);
		return 0;
	}

#if OPT_A3
	// kprintf("three!\n");
	// When TLB is full, it will call tlb_random() to write the entry into a 
	//	random TLB slot.
	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	if (read_only && loadelf_finished) {
		elo &= ~TLBLO_DIRTY;
	}
	DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
	tlb_random(ehi, elo);
	splx(spl);
	return 0;

#else
	kprintf("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	splx(spl);
	return EFAULT;
#endif
}

struct addrspace *
as_create(void)
{
	struct addrspace *as = kmalloc(sizeof(struct addrspace));
	if (as==NULL) {
		return NULL;
	}

#if OPT_A3
	as->loadelf_done = false;
#endif

	as->as_vbase1 = 0;

#if OPT_A3
	as->as_pbase1 = NULL;
#else 
	as->as_pbase1 = 0;
#endif

	as->as_npages1 = 0;
	as->as_vbase2 = 0;

#if OPT_A3
	as->as_pbase2 = NULL;
#else
	as->as_pbase2 = 0;
#endif
	
	as->as_npages2 = 0;

#if OPT_A3
	as->as_stackpbase = NULL;
#else
	as->as_stackpbase = 0;
#endif

	return as;
}

void
as_destroy(struct addrspace *as)
{
#if OPT_A3
	// Free address for code segments
	for (size_t i = 0; i < as->as_npages1; i++){
    	free_kpages(PADDR_TO_KVADDR(as->as_pbase1[i]));
	}
	kfree(as->as_pbase1);

	// Free address for data segments
	for (size_t i = 0; i < as->as_npages2; i++){
    	free_kpages(PADDR_TO_KVADDR(as->as_pbase2[i]));
	}
	kfree(as->as_pbase2);

	// Free address of stacks
	for (size_t i = 0; i < DUMBVM_STACKPAGES; i++){
    	free_kpages(PADDR_TO_KVADDR(as->as_stackpbase[i]));
	}
	kfree(as->as_stackpbase);
#endif
	kfree(as);
}

void
as_activate(void)
{
	int i, spl;
	struct addrspace *as;

	as = curproc_getas();
#ifdef UW
        /* Kernel threads don't have an address spaces to activate */
#endif
	if (as == NULL) {
		return;
	}

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();

	for (i=0; i<NUM_TLB; i++) {
		tlb_write(TLBHI_INVALID(i), TLBLO_INVALID(), i);
	}

	splx(spl);
}

void
as_deactivate(void)
{
	/* nothing */
}

int
as_define_region(struct addrspace *as, vaddr_t vaddr, size_t sz,
		 int readable, int writeable, int executable)
{
	size_t npages; 

	/* Align the region. First, the base... */
	sz += vaddr & ~(vaddr_t)PAGE_FRAME;
	vaddr &= PAGE_FRAME;

	/* ...and now the length. */
	sz = (sz + PAGE_SIZE - 1) & PAGE_FRAME;

	npages = sz / PAGE_SIZE;

	/* We don't use these - all pages are read-write */
	(void)readable;
	(void)writeable;
	(void)executable;

	if (as->as_vbase1 == 0) {
		as->as_vbase1 = vaddr;
		as->as_npages1 = npages;
	
	#if OPT_A3
		// Set up a region of memory for code segment
		size_t code_seg_size = sizeof(paddr_t) * npages;
		as->as_pbase1 = kmalloc(code_seg_size);
		for (size_t i = 0; i < as->as_npages1; i++){
			as->as_pbase1[i] = 0;
		}
	#endif
		return 0;
	}

	if (as->as_vbase2 == 0) {
		as->as_vbase2 = vaddr;
		as->as_npages2 = npages;
	
	#if OPT_A3
		// Set up a region of memory for data segment
		size_t code_seg_size = sizeof(paddr_t) * npages;
		as->as_pbase2 = kmalloc(code_seg_size);
		for (size_t i = 0; i < as->as_npages2; i++){
			as->as_pbase2[i] = 0;
		}
	#endif	
		return 0;
	}

	/*
	 * Support for more than two regions is not available.
	 */
	kprintf("dumbvm: Warning: too many regions\n");
	return EUNIMP;
}

static
void
as_zero_region(paddr_t paddr, unsigned npages)
{
	bzero((void *)PADDR_TO_KVADDR(paddr), npages * PAGE_SIZE);
}

int
as_prepare_load(struct addrspace *as)
{
#if OPT_A3
	for (size_t i = 0; i < as->as_npages1; i++){
	  KASSERT(as->as_pbase1[i] == 0);
	}
	
	for (size_t j = 0; j < as->as_npages2; j++){
	  KASSERT(as->as_pbase2[j] == 0);
	 }
	
	KASSERT(as->as_stackpbase == NULL);
#else
	KASSERT(as->as_pbase1 == 0);
	KASSERT(as->as_pbase2 == 0);
	KASSERT(as->as_stackpbase == 0);
#endif

#if OPT_A3
	unsigned single_page = 1;
	// Check the region of code segment
	for (size_t i = 0; i < as->as_npages1; i++){
		as->as_pbase1[i] = getppages(single_page);
		if (as->as_pbase1[i] == 0) {
			return ENOMEM;
		}
		as_zero_region(as->as_pbase1[i], single_page);
	}

	// Check the region of data segment
	for (size_t i = 0; i < as->as_npages2; i++){
		as->as_pbase2[i] = getppages(single_page);
		if (as->as_pbase2[i] == 0) {
			return ENOMEM;
		}
		as_zero_region(as->as_pbase2[i], single_page);
	}

	// Check the region of stack
	size_t stackpage_size = DUMBVM_STACKPAGES * sizeof(paddr_t);
	as->as_stackpbase = kmalloc(stackpage_size);
	for (size_t i = 0; i < DUMBVM_STACKPAGES; i++){
		as->as_stackpbase[i] = getppages(single_page);
		if (as->as_stackpbase[i] == 0) {
			return ENOMEM;
		}
		as_zero_region(as->as_stackpbase[i], single_page);
	}

#else
	as->as_pbase1 = getppages(as->as_npages1);
	if (as->as_pbase1 == 0) {
		return ENOMEM;
	}

	as->as_pbase2 = getppages(as->as_npages2);
	if (as->as_pbase2 == 0) {
		return ENOMEM;
	}


	as->as_stackpbase = getppages(DUMBVM_STACKPAGES);
	if (as->as_stackpbase == 0) {
		return ENOMEM;
	}
	
	as_zero_region(as->as_pbase1, as->as_npages1);
	as_zero_region(as->as_pbase2, as->as_npages2);
	as_zero_region(as->as_stackpbase, DUMBVM_STACKPAGES);
#endif
	
	return 0;

}

int
as_complete_load(struct addrspace *as)
{
	(void)as;
	return 0;
}

int
as_define_stack(struct addrspace *as, vaddr_t *stackptr)
{
	KASSERT(as->as_stackpbase != 0);

	*stackptr = USERSTACK;
	return 0;
}

int
as_copy(struct addrspace *old, struct addrspace **ret)
{
	struct addrspace *new;

	new = as_create();
	if (new==NULL) {
		return ENOMEM;
	}

	new->as_vbase1 = old->as_vbase1;
	new->as_npages1 = old->as_npages1;
	new->as_vbase2 = old->as_vbase2;
	new->as_npages2 = old->as_npages2;

#if OPT_A3
	// Initialize code segment for the new address space 
	size_t new_code_seg_size = new->as_npages1 * sizeof(paddr_t);
	new->as_pbase1 = kmalloc(new_code_seg_size);
	for (size_t i = 0; i < new->as_npages1; i++){
		new->as_pbase1[i] = 0;
	}

	// Initialize data segment for the new address space
	size_t new_data_seg_size = new->as_npages2 * sizeof(paddr_t);
	new->as_pbase2 = kmalloc(new_data_seg_size);
	for (size_t i = 0; i < new->as_npages2; i++){
		new->as_pbase2[i] = 0;
	}

#endif

	/* (Mis)use as_prepare_load to allocate some physical memory. */
	if (as_prepare_load(new)) {
		as_destroy(new);
		return ENOMEM;
	}

#if OPT_A3
	KASSERT(new->as_pbase1 != NULL);
	for (size_t i = 0; i < new->as_npages1; i++) {
		KASSERT(new->as_pbase1[i] != 0);
	}
	
	KASSERT(new->as_pbase2 != NULL);
	for (size_t i = 0; i < new->as_npages2; i++){
		KASSERT(new->as_pbase2[i] != 0);
	}
	
	KASSERT(new->as_stackpbase != NULL);
	for (size_t i = 0; i < DUMBVM_STACKPAGES; i++){
		KASSERT(new->as_stackpbase[i] != 0);
	}

	// Move memory from code segment
	for (size_t i = 0; i < new->as_npages1; i++){
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase1[i]), 
				(const void *)PADDR_TO_KVADDR(old->as_pbase1[i]),
				PAGE_SIZE);
	}
	
	// Move memory from data segment
	for (size_t i = 0; i < new->as_npages2; i++){
		memmove((void *)PADDR_TO_KVADDR(new->as_pbase2[i]),
				(const void *)PADDR_TO_KVADDR(old->as_pbase2[i]),
				PAGE_SIZE);
	}
	
	// Move memory from data segment
	for (size_t i = 0; i < DUMBVM_STACKPAGES; i++){
		memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase[i]),
				(const void *)PADDR_TO_KVADDR(old->as_stackpbase[i]),
				PAGE_SIZE);
	}

#else
	KASSERT(new->as_pbase1 != 0);
	KASSERT(new->as_pbase2 != 0);
	KASSERT(new->as_stackpbase != 0);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase1),
		(const void *)PADDR_TO_KVADDR(old->as_pbase1),
		old->as_npages1*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_pbase2),
		(const void *)PADDR_TO_KVADDR(old->as_pbase2),
		old->as_npages2*PAGE_SIZE);

	memmove((void *)PADDR_TO_KVADDR(new->as_stackpbase),
		(const void *)PADDR_TO_KVADDR(old->as_stackpbase),
		DUMBVM_STACKPAGES*PAGE_SIZE);
#endif
	
	*ret = new;
	return 0;
}
