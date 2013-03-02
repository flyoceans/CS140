#include "userprog/pagedir.h"
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include "threads/init.h"
#include "threads/pte.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "userprog/exception.h"
#include "vm/swap.h"
#include "vm/frame.h"

static uint32_t *active_pd (void);
extern struct swap_table swap_table;
extern struct pool user_pool;
extern struct lock flush_lock;
extern struct condition flush_cond;

/* Creates a new page directory that has mappings for kernel
   virtual addresses, but none for user virtual addresses.
   Returns the new page directory, or a null pointer if memory
   allocation fails. */
uint32_t *
pagedir_create (void) 
{
  uint32_t *pd = palloc_get_page (0, NULL);
  if (pd != NULL)
    memcpy (pd, init_page_dir, PGSIZE);
  return pd;
}

/* Destroys page directory PD, freeing all the pages it
   references. */
void
pagedir_destroy (uint32_t *pd) 
{
  uint32_t *pde;

  if (pd == NULL)
    return;

  ASSERT (pd != init_page_dir);
  for (pde = pd; pde < pd + pd_no (PHYS_BASE); pde++)
    if (*pde & PTE_P) 
    {
      uint32_t *pt = pde_get_pt (*pde);
      uint32_t *pte;
      for (pte = pt; pte < pt + PGSIZE / sizeof *pte; pte++)
      {
        struct lock *pin_lock = pool_get_pin_lock (&user_pool, pte);
        if (!(*pte & PTE_P))
        {
          if (*pte & PTE_ADDR)
          {
            size_t swap_frame_no = (*pte >> PGBITS);
            swap_free ( &swap_table, swap_frame_no);
          }
        }
        else
        {
          bool to_be_released = false;
          lock_acquire (pin_lock);
          *pte |= PTE_I;
          if (*pte & PTE_P)
            to_be_released = true;
          lock_release (pin_lock);

          if (to_be_released)
          {
            palloc_free_page (pte_get_page (*pte));
          }
          else
          {
            lock_acquire (&flush_lock);
            while (*pte & PTE_F)
            {
              cond_wait (&flush_cond, &flush_lock);
            }
            lock_release (&flush_lock);

            ASSERT (*pte & PTE_ADDR);
            size_t swap_frame_no = (*pte >> PGBITS);
            swap_free ( &swap_table, swap_frame_no);
          }
        }
      }
      palloc_free_page (pt);
    }
  palloc_free_page (pd);
}

/* Returns the address of the page table entry for virtual
   address VADDR in page directory PD.
   If PD does not have a page table for VADDR, behavior depends
   on CREATE.  If CREATE is true, then a new page table is
   created and a pointer into it is returned.  Otherwise, a null
   pointer is returned. */
uint32_t *
lookup_page (uint32_t *pd, const void *vaddr, bool create)
{
  uint32_t *pt, *pde;

  ASSERT (pd != NULL);

  /* Shouldn't create new kernel virtual mappings. */
  ASSERT (!create || is_user_vaddr (vaddr));

  /* Check for a page table for VADDR.
     If one is missing, create one if requested. */
  pde = pd + pd_no (vaddr);
  if (*pde == 0) 
    {
      if (create)
        {
          pt = palloc_get_page (PAL_ZERO, NULL);
          if (pt == NULL) 
            return NULL; 
      
          *pde = pde_create (pt);
        }
      else
        return NULL;
    }

  /* Return the page table entry. */
  pt = pde_get_pt (*pde);
  return &pt[pt_no (vaddr)];
}

/* Pins a PAGE pointed by PTE.
   If the page is not in memory, page it in first.
   Returns true if the page is successfully pinned. */
bool
pin_pte (uint32_t *pte, void *page)
{
  if (pte == NULL || (*pte & PTE_ADDR) == 0)
    return false;

  struct lock *pin_lock = pool_get_pin_lock (&user_pool, pte);

  if (pin_lock != NULL)
    lock_acquire (pin_lock);
  *pte |= PTE_I;
  if (pin_lock != NULL)
    lock_release (pin_lock);

  /* If the page is not in memory, load it. */
  if ((*pte & PTE_P) == 0)
  {
    if ((*pte & PTE_M) == 0)
    {
      load_page_from_swap (pte, page);
    }
    else
    {
      struct suppl_pte *spte;
      spte = suppl_pt_get_spte (&thread_current ()->suppl_pt, pte);
      load_page_from_file (spte, page);
    }
  }
  return true;
}

/* Pins a PAGE in the page directory PD to prevent it being paged out.
   If the page is not in memory, page it in first.
   Returns true if the page is successfully pinned. */
bool
pin_page (uint32_t *pd, void *page)
{
  /* Note: currently only support pinning user pages
     since no page out in kernel memory */
  if (page > PHYS_BASE)
    return false;

  uint32_t *pte = lookup_page (pd, page, false);
  return pin_pte (pte, page);
}

/* Unpins a page pointed by PTE. */
bool
unpin_pte (uint32_t *pte)
{
  if (pte == NULL || (*pte & PTE_ADDR) == 0)
    return false;

  struct lock *pin_lock = pool_get_pin_lock (&user_pool, pte);

  if (pin_lock != NULL)
    lock_acquire (pin_lock);
  ASSERT (*pte & PTE_I);
  *pte &= ~PTE_I;
  if (pin_lock != NULL)
    lock_release (pin_lock);

  return true;
}

/* Unpins a PAGE in the page directory PD.
   Returns true if the page is successfully unpinned. */
bool
unpin_page (uint32_t *pd, const void *page)
{
  /* Note: currently only support pinning user pages
     since no page out in kernel memory */
  if (page > PHYS_BASE)
    return false;

  uint32_t *pte = lookup_page (pd, page, false);
  return unpin_pte (pte);
}

/* Adds a mapping in page directory PD from user virtual page
   UPAGE to the physical frame identified by kernel virtual
   address KPAGE.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   If WRITABLE is true, the new page is read/write;
   otherwise it is read-only.
   Returns true if successful, false if memory allocation
   failed. */
bool
pagedir_set_page (uint32_t *pd, void *upage, void *kpage, bool writable)
{
  uint32_t *pte;

  ASSERT (pg_ofs (upage) == 0);
  ASSERT (pg_ofs (kpage) == 0);
  ASSERT (is_user_vaddr (upage));
  ASSERT (vtop (kpage) >> PTSHIFT < init_ram_pages);
  ASSERT (pd != init_page_dir);

  pte = lookup_page (pd, upage, true);

  if (pte != NULL) 
    {
      ASSERT ((*pte & PTE_P) == 0);

      struct lock *pin_lock = pool_get_pin_lock (&user_pool, pte);

      if (pin_lock != NULL)
        lock_acquire (pin_lock);
      bool pin = (*pte & PTE_I) != 0;
      *pte = pte_create_user (kpage, writable) | (pin ? PTE_I : 0);
      if (pin_lock != NULL)
        lock_release (pin_lock);

      return true;
    }
  else
    return false;
}

/* Looks up the physical address that corresponds to user virtual
   address UADDR in PD.  Returns the kernel virtual address
   corresponding to that physical address, or a null pointer if
   UADDR is unmapped. */
void *
pagedir_get_page (uint32_t *pd, const void *uaddr) 
{
  uint32_t *pte;

  ASSERT (is_user_vaddr (uaddr));
  
  pte = lookup_page (pd, uaddr, false);
  if (pte != NULL && (*pte & PTE_P) != 0)
    return pte_get_page (*pte) + pg_ofs (uaddr);
  else
    return NULL;
}

/* Marks user virtual page UPAGE "not present" in page
   directory PD.  Later accesses to the page will fault.  Other
   bits in the page table entry are preserved.
   UPAGE need not be mapped. */
void
pagedir_clear_page (uint32_t *pd, void *upage) 
{
  uint32_t *pte;

  ASSERT (pg_ofs (upage) == 0);
  ASSERT (is_user_vaddr (upage));

  pte = lookup_page (pd, upage, false);
  if (pte != NULL && (*pte & PTE_P) != 0)
    {
      *pte &= ~PTE_P;
      invalidate_pagedir (pd);
    }
}

/* Returns true if the PTE for virtual page VPAGE in PD is dirty,
   that is, if the page has been modified since the PTE was
   installed.
   Returns false if PD contains no PTE for VPAGE. */
bool
pagedir_is_dirty (uint32_t *pd, const void *vpage) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  return pte != NULL && (*pte & PTE_D) != 0;
}

/* Set the dirty bit to DIRTY in the PTE in PD */
void
pagedir_set_dirty_pte (uint32_t *pd, uint32_t *pte, bool dirty)
{
  if (pte != NULL) 
    {
      if (dirty)
        *pte |= PTE_D;
      else 
        {
          *pte &= ~(uint32_t) PTE_D;
          invalidate_pagedir (pd);
        }
    }
}

/* Set the dirty bit to DIRTY in the PTE for virtual page VPAGE
   in PD. */
void
pagedir_set_dirty (uint32_t *pd, const void *vpage, bool dirty)
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  pagedir_set_dirty_pte (pd, pte, dirty);
}

/* Returns true if the PTE for virtual page VPAGE in PD has been
   accessed recently, that is, between the time the PTE was
   installed and the last time it was cleared.  Returns false if
   PD contains no PTE for VPAGE. */
bool
pagedir_is_accessed (uint32_t *pd, const void *vpage) 
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  return pte != NULL && (*pte & PTE_A) != 0;
}

/* Sets the accessed bit to ACCESSED in the PTE in PD */
void
pagedir_set_accessed_pte (uint32_t *pd, uint32_t *pte, bool accessed)
{
  if (pte != NULL) 
    {
      if (accessed)
        *pte |= PTE_A;
      else 
        {
          *pte &= ~(uint32_t) PTE_A; 
          invalidate_pagedir (pd);
        }
    }
}

/* Sets the accessed bit to ACCESSED in the PTE for virtual page
   VPAGE in PD. */
void
pagedir_set_accessed (uint32_t *pd, const void *vpage, bool accessed)
{
  uint32_t *pte = lookup_page (pd, vpage, false);
  pagedir_set_accessed_pte (pd, pte, accessed);
}

/* Loads page directory PD into the CPU's page directory base
   register. */
void
pagedir_activate (uint32_t *pd) 
{
  if (pd == NULL)
    pd = init_page_dir;

  /* Store the physical address of the page directory into CR3
     aka PDBR (page directory base register).  This activates our
     new page tables immediately.  See [IA32-v2a] "MOV--Move
     to/from Control Registers" and [IA32-v3a] 3.7.5 "Base
     Address of the Page Directory". */
  asm volatile ("movl %0, %%cr3" : : "r" (vtop (pd)) : "memory");
}

/* Returns the currently active page directory. */
static uint32_t *
active_pd (void) 
{
  /* Copy CR3, the page directory base register (PDBR), into
     `pd'.
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 3.7.5 "Base Address of the Page Directory". */
  uintptr_t pd;
  asm volatile ("movl %%cr3, %0" : "=r" (pd));
  return ptov (pd);
}

/* Some page table changes can cause the CPU's translation
   lookaside buffer (TLB) to become out-of-sync with the page
   table.  When this happens, we have to "invalidate" the TLB by
   re-activating it.

   This function invalidates the TLB if PD is the active page
   directory.  (If PD is not active then its entries are not in
   the TLB, so there is no need to invalidate anything.) */
void
invalidate_pagedir (uint32_t *pd) 
{
  if (active_pd () == pd) 
    {
      /* Re-activating PD clears the TLB.  See [IA32-v3a] 3.12
         "Translation Lookaside Buffers (TLBs)". */
      pagedir_activate (pd);
    } 
}
