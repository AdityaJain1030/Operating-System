/*! @file memory.c
    @brief Physical and virtual memory manager
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#ifdef MEMORY_TRACE
#define TRACE
#endif

#ifdef MEMORY_DEBUG
#define DEBUG
#endif

#include "memory.h"

#include "conf.h"
#include "console.h"
#include "error.h"
#include "heap.h"
#include "misc.h"
#include "process.h"
#include "riscv.h"
#include "string.h"
#include "thread.h"

// COMPILE-TIME CONFIGURATION
//

// Minimum amount of memory in the initial heap block.

#ifndef HEAP_INIT_MIN
#define HEAP_INIT_MIN 256
#endif

// INTERNAL CONSTANT DEFINITIONS
//

#define MEGA_SIZE ((1UL << 9) * PAGE_SIZE)  // megapage size
#define GIGA_SIZE ((1UL << 9) * MEGA_SIZE)  // gigapage size

#define PTE_ORDER 3
#define PTE_CNT (1U << (PAGE_ORDER - PTE_ORDER))

#ifndef PAGING_MODE
#define PAGING_MODE RISCV_SATP_MODE_Sv39
#endif

#ifndef ROOT_LEVEL
#define ROOT_LEVEL 2
#endif

// IMPORTED GLOBAL SYMBOLS
//

// linker-provided (kernel.ld)
// kimg = kernel image
extern char _kimg_start[];
extern char _kimg_text_start[];
extern char _kimg_text_end[];
extern char _kimg_rodata_start[];
extern char _kimg_rodata_end[];
extern char _kimg_data_start[];
extern char _kimg_data_end[];
extern char _kimg_end[];

// EXPORTED GLOBAL VARIABLES
//

char memory_initialized = 0;

// INTERNAL TYPE DEFINITIONS
//

// We keep free physical pages in a linked list of _chunks_, where each chunk
// consists of several consecutive pages of memory. Initially, all free pages
// are in a single large chunk. To allocate a block of pages, we break up the
// smallest chunk on the list.

// -Goals/Idea: Free list of page chunks should be sorted by physical addresses so that way when freeing it is easy to merge

//tracks physical pages I believe?

/**
 * @brief Section of consecutive physical pages. We keep free physical pages in a
 * linked list of chunks. Initially, all free pages are in a single large chunk. To
 * allocate a block of pages, we break up the smallest chunk in the list
 */
struct page_chunk {
    struct page_chunk *next;  ///< Next page in list
    unsigned long pagecnt;    ///< Number of pages in chunk
    //uint64_t      address;      // start address of page_chunk removed?
};



/**
 * @brief RISC-V PTE. RTDC (RISC-V docs) for what each of these fields means!
 * 
 * ^ So useless. :/
 * 
 * Unhelpful commnet use this iNSTEAD: https://docs.riscv.org/reference/isa/_attachments/riscv-privileged.pdf
 * CHAPTER 12.4 SV39 RISCV. 
 * Chpater 14 for Svpmbt extension
 * Page table entry: N | PBMT | Reserved | PPN[2] = [53:28] | PPN[1] = [27:19] | PPN[0] = [18:10] | RSW | Flags
 * 
 * each page table entry is 64 bits. Hence we let the array do pointer arihtmetic for us!
 */
struct pte {
    uint64_t flags : 8;
    uint64_t rsw : 2;
    uint64_t ppn : 44;                      // 
    uint64_t reserved : 7;                  //
    uint64_t pbmt : 2;                      //
    uint64_t n : 1;
};

// STUFF TO KNOW! RISC V 12.3-4 PRIVILEGED
// - ASID: Address Space Identifer: see 4.11?? Not sure slides are uncleaer :/

// - SATP Register
//     - SATP[43:0]: Holds PPN of root page table (so physical address divide by 4096) So to find address multiply by 4 * 1024
//     - So upper 20 cleared and we can get address by doing (SATP << 20) >> 20 and then taking that result adn doing << 12
// - Leaf versus non-leaf PTE:
//     - Leaf PTE: PPN field gives teh physical page number to the actual physical memor py page. I.e. paddr = (PPN << 12) | page_offset
//         - When oen of R | W | X = 1
//     - Non-leaf PTE: Gives phsycial page number of ANOTHER page table
//         - Page tables are stored in RAM!!!!!!!!!!
//         - Look at teh R, W, X bits. Shoudl all be 0

// - Bits 63-39 fetch and store addresses must equal to bit 38 otherwis page-fault exception!
// - However final mapped physical address IS zero extended??
// - 

// INTERNAL MACRO DEFINITIONS
//

#define VPN(vma) ((vma) / PAGE_SIZE)
#define VPN2(vma) ((VPN(vma) >> (2 * 9)) % PTE_CNT) // gets vpn2. Essentially shift out the offset. Then find the page index for the page table
#define VPN1(vma) ((VPN(vma) >> (1 * 9)) % PTE_CNT)
#define VPN0(vma) ((VPN(vma) >> (0 * 9)) % PTE_CNT)

// The following macros test is a PTE is valid, global, or a leaf. The argument
// is a struct pte (*not* a pointer to a struct pte).

#define PTE_VALID(pte) (((pte).flags & PTE_V) != 0)
#define PTE_GLOBAL(pte) (((pte).flags & PTE_G) != 0)
#define PTE_LEAF(pte) (((pte).flags & (PTE_R | PTE_W | PTE_X)) != 0)

#define PT_INDEX(lvl, vpn) \
    (((vpn) & (0x1FF << (lvl * (PAGE_ORDER - PTE_ORDER)))) >> (lvl * (PAGE_ORDER - PTE_ORDER)))
// INTERNAL FUNCTION DECLARATIONS
//

static void ptab_reset(struct pte *ptab  // page table to reset
);

static struct pte *ptab_clone(struct pte *ptab  // page table to clone
);

static void ptab_discard(struct pte *ptab  // page table to discard
);

static void ptab_insert(struct pte *ptab,   // page table to modify
                        unsigned long vpn,  // virtual page number to insert
                        void *pp,           // pointer to physical page to insert
                        int rwxug_flags     // flags for inserted mapping
);

static void *ptab_remove(struct pte *ptab, unsigned long vpn);

static void ptab_adjust(struct pte *ptab, unsigned long vpn, int rwxug_flags);

struct pte *ptab_fetch(struct pte *ptab, unsigned long vpn);

static inline mtag_t active_space_mtag(void);
static inline mtag_t ptab_to_mtag(struct pte *root, unsigned int asid);
static inline struct pte *mtag_to_ptab(mtag_t mtag);
static inline struct pte *active_space_ptab(void);

static inline void *pageptr(uintptr_t n);
static inline uintptr_t pagenum(const void *p);
static inline int wellformed(uintptr_t vma);

static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags);
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag);
static inline struct pte null_pte(void);

// INTERNAL GLOBAL VARIABLES
//

static mtag_t main_mtag;

static struct pte main_pt2[PTE_CNT] __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt1_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct pte main_pt0_0x80000[PTE_CNT]
    __attribute__((section(".bss.pagetable"), aligned(4096)));

static struct page_chunk *free_chunk_list;

// EXPORTED FUNCTION DECLARATIONS
//

void memory_init(void) {
    const void *const text_start = _kimg_text_start;
    const void *const text_end = _kimg_text_end;
    const void *const rodata_start = _kimg_rodata_start;
    const void *const rodata_end = _kimg_rodata_end;
    const void *const data_start = _kimg_data_start;

    void *heap_start;
    void *heap_end;

    uintptr_t pma;
    const void *pp;

    trace("%s()", __func__);

    assert(RAM_START == _kimg_start);

    debug("           RAM: [%p,%p): %zu MB", RAM_START, RAM_END, RAM_SIZE / 1024 / 1024);
    debug("  Kernel image: [%p,%p)", _kimg_start, _kimg_end);

    // Kernel must fit inside 2MB megapage (one level 1 PTE)

    if (MEGA_SIZE < _kimg_end - _kimg_start) panic(NULL);

    // Initialize main page table with the following direct mapping:
    //
    //         0 to RAM_START:           RW gigapages (MMIO region)
    // RAM_START to _kimg_end:           RX/R/RW pages based on kernel image
    // _kimg_end to RAM_START+MEGA_SIZE: RW pages (heap and free page pool)
    // RAM_START+MEGA_SIZE to RAM_END:   RW megapages (free page pool)
    //
    // RAM_START = 0x80000000
    // MEGA_SIZE = 2 MB
    // GIGA_SIZE = 1 GB


    /*
    
        root level table some can be directly mapping to pages while others do not
    
    */
    // Identity mapping of MMIO region as two gigapage mappings
    for (pma = 0; pma < RAM_START_PMA; pma += GIGA_SIZE)    // GIGA_SIZE = 0x4000_0000
        main_pt2[VPN2(pma)] = leaf_pte((void *)pma, PTE_R | PTE_W | PTE_G);

    // Third gigarange has a second-level subtable
    main_pt2[VPN2(RAM_START_PMA)] = ptab_pte(main_pt1_0x80000, PTE_G);

    // First physical megarange of RAM is mapped as individual pages with
    // permissions based on kernel image region.


    // look at lecture 10/30 : 38:35 Timestamp
    main_pt1_0x80000[VPN1(RAM_START_PMA)] = ptab_pte(main_pt0_0x80000, PTE_G);

    for (pp = text_start; pp < text_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_X | PTE_G);
    }

    for (pp = rodata_start; pp < rodata_end; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_G);
    }
    // 2 * 2^10 = 2 MB of pages = 1 entry in level 1 subtable
    for (pp = data_start; pp < RAM_START + MEGA_SIZE; pp += PAGE_SIZE) {
        main_pt0_0x80000[VPN0((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Remaining RAM mapped in 2MB megapages
    // mapping for level 1 subtable
    // 
    for (pp = RAM_START + MEGA_SIZE; pp < RAM_END; pp += MEGA_SIZE) {
        main_pt1_0x80000[VPN1((uintptr_t)pp)] = leaf_pte(pp, PTE_R | PTE_W | PTE_G);
    }

    // Enable paging; this part always makes me nervous.

    main_mtag = ptab_to_mtag(main_pt2, 0);
    csrw_satp(main_mtag);

    // Give the memory between the end of the kernel image and the next page
    // boundary to the heap allocator, but make sure it is at least
    // HEAP_INIT_MIN bytes.

    heap_start = _kimg_end;
    heap_end = (void *)ROUND_UP((uintptr_t)heap_start, PAGE_SIZE);

    if (heap_end - heap_start < HEAP_INIT_MIN) {
        heap_end += ROUND_UP(HEAP_INIT_MIN - (heap_end - heap_start), PAGE_SIZE);
    }

    if (RAM_END < heap_end) panic("out of memory");

    // Initialize heap memory manager

    heap_init(heap_start, heap_end);

    debug("Heap allocator: [%p,%p): %zu KB free", heap_start, heap_end,
          (heap_end - heap_start) / 1024);

    // FIXME: Initialize the free chunk list here
    /*
        idk any more; heap is stored in RAM right
        use kimg_end = kernel image end which is teh stsart of our thing
    */

    // free_chunk_list = new
    // old way of using kmalloc. Instead we directly embed!!!! However, maybe we still allocate a sentinel in case?
    //struct page_chunk* head = kmalloc(sizeof(struct page_chunk));  // create the initial page chunk which holds from end of kernel image to end of RAM
    //struct page_chunk* sentinel = kmalloc(sizeof(struct page_chunk));   // create a sentinel
    /*
        In supervisor/kernel mode we have ALREADY mapped the stuff!!! in the levl 1 subtbale and thast fine!! 
        so we can directly assign 
    */

    /*
     * Mon Nov 17 07:10:12 PM CST 2025: ART CHANGES ==============================================================================================================
     * (regarding the code below and explanations) no you cannot kmalloc that, that puts it on the kernel heap. instead I figured out the "lazy faulting" strategy.
     * basically, we can have a certian page fault response configured for trying to translate an address within [USR_START_VMA, USR_END_VMA]. which means we can 
     * just point the free_chunk_list to the place we want to EMBED it
     */
    free_chunk_list = (struct page_chunk *)UMEM_START_VMA;//I'll just deal in increments of the offset from now on since there are no other way to work with raw PMAs (genuinely almost used the abreviation for physical pointers instead)
    free_chunk_list->next = NULL;
    free_chunk_list->pagecnt = (UMEM_END_VMA - UMEM_START_VMA)/PAGE_SIZE;
    

    // END OF ART CHANGES ========================================================================================================================================

    //struct page_chunk* sentinel = kmalloc(sizeof(struct page_chunk));   // create a sentinel. We can kmalloc this 
    // sentinel->next = (struct page_chunk*)(heap_end);                    // embed into acutal page thingy
    // sentinel->next->next = NULL;
    // sentinel->next->pagecnt = ((uintptr_t)RAM_END - (uintptr_t)heap_end) / PAGE_SIZE;        // find total number of pages we can have for user. We use the heap end
    // if (sentinel == NULL)
    // {
    //     kprintf("FAILED TO ALLOCATE THE FIRST PAGE SENTINEL!!!!!!!!\n");
    //     return;
    // }
    // sentinel->pagecnt = 0;      // set page count for sentinel to 0
    // free_chunk_list = sentinel;

    // ^
    // Allow supervisor to access user memory. We could be more precise by only
    // enabling supervisor access to user memory when we are explicitly trying
    // to access user memory, and disable it at other times. This would catch
    // bugs that cause inadvertent access to user memory (due to bugs).

    csrs_sstatus(RISCV_SSTATUS_SUM);

    memory_initialized = 1;
}

mtag_t active_mspace(void) { return active_space_mtag(); }

mtag_t switch_mspace(mtag_t mtag) {
    mtag_t prev;

    prev = csrrw_satp(mtag);
    sfence_vma();
    return prev;
}


/* 
    Copies all pages and page tables from the active memory space into newly allocated memory.
    Returns: Tag corresponding to newly allocated memory 
*/
mtag_t clone_active_mspace(void) { 
    //Mon Nov 17 07:35:54 PM CST 2025 - STARTED BY ART MULEY... Ah shoot I didn't realise this was due in mp3
    //mtag_t original;

    //original = csrr_satp();//I forget what exactly this is a pseudo-instruction for (something with x0) but basically there are no side effects on the satp
    
    //TODO: make the DEEP copy here.
    return (mtag_t)0;
}

/*
    Unmaps and frees all non-global pages from the active memory space.
    Returns: None
*/
void reset_active_mspace(void) {
    // FIXME
    // what this should do is just traverse our page tree and just calloc everything to zero??? This can easilly be done
    // with like a dfs type thing but im not sure if there is anything else we have to clean up
    // also how do we handle global pages do we have to keep the level page above that active then??
    // deet was here hehehehehe
    return;
}



/*
    Switches memory spaces to main, unmaps and frees all non-global pages from the previously active memory space.
    Returns: Tag corresponding to main memory space  
*/
mtag_t discard_active_mspace(void) {
    //Mon Nov 17 07:49:44 PM CST 2025 - AMMENDED/STARTED BY ART MULEY
    reset_active_mspace();
    return switch_mspace(main_mtag);//TODO: it seems like the function can be just this simple but I need to make sure
}

// The map_page() function maps a single page into the active address space at
// the specified address. The map_range() function maps a range of contiguous
// pages into the active address space. Note that map_page() is a special case
// of map_range(), so it can be implemented by calling map_range(). Or
// map_range() can be implemented by calling map_page() for each page in the
// range. The current implementation does the latter.

// We currently map 4K pages only. At some point it may be disirable to support
// mapping megapages and gigapages.

void *map_page(uintptr_t vma, void *pp, int rwxug_flags) {
    // FIXME
    // return phsycial pointer as actual address?
    /*
        NEED TO EXPLCIILTY TELL CPU THAT TRANSALTION HAS CHANEGD! CPU DOES NOT KNOW THIS
        USE A TLB FLUSH!!!!!!! CLEAR THE TLB!
        Use SFENCE.VMA!!!!!!
            - Allows us to specify specific entries and etc
            - Could have different ASID for each address space. So essneitaly we specify teh ASID in the SFENCE.VMA and then correpsodning ASID table
                entries will be flushed!
            - Look at lecture 10/30 Timestampe: 54:29
                - specify rs1 = rs2 = x0: Full flush

        DO NOT FORGET TO FLUSHHHHHHHHHHHHHHHHHHHHHHHHHHHh!!!!!!!!
        CPU may cahce PTE wehre V = 0. SO MAKE SURE YOU FLUSH EVEN IF YOU ARE ADDING ENTRIES

        syntax: asm volatile ("sfence.vma" ::: "memory");


        IN MAP PAGE: JUS USE THE LEVEL 1 SUBTABLE!!!!!! As we have R, W, X SO it is dieclty a LEAF!


        CP2: Pretty sure that we just use the given L1 table and then allocate leaf node. BUT NO ONE IS ANSWERING MY FRICIKING QUESTION IN DISCROD
        I AM TALKING TO MYSELF MAN. 



        Don't forget to set the RWX U G flags!

        I believe we can only map_page in Supervisor Mode!




        Possible edge cases:
            - 
    */

    /*
        General algorithm for mapping:
        Take vma: Go to PT2 get PT2 entry. PTE2 points to PT1 substable. Check valid bits. If valid
        we go down sub level. If not valid, we need to allocate a new page
    
    
    
    */
    if (vma & (PAGE_SIZE - 1))  // trick to see if its algiend or not. If its not aligend what do we do?
    {
        // do something not sure yet
        // throw an exception maybe?
    }


    uintptr_t   pt2_pma = (csrr_satp() << 20) >> 8;
    struct pte*  pt2 = (struct pte*)pt2_pma;
    // uintptr_t   pt1_ppn = pt2[VPN2(vma)].ppn;
    // uintptr_t pt1_pma = pt1_ppn << 12;
    // struct pte* pt1 = (struct pte*) pt1_pma;
    // uintptr_t pt0_ppn = pt1[VPN1(vma)].ppn;
    // uintptr_t pt0_pma = pt0_ppn << 12;
    // struct pte* pt0 = (struct pte*) pt0_pma;

    uintptr_t   pt1_ppn;
    uintptr_t pt1_pma;
    struct pte* pt1;
    uintptr_t pt0_ppn = pt1[VPN1(vma)].ppn;
    uintptr_t pt0_pma = pt0_ppn << 12;
    struct pte* pt0 = (struct pte*) pt0_pma;
    // check if valid first


    if (PTE_VALID(pt2[VPN2(vma)]))
    {
        pt1_ppn = pt2[VPN2(vma)].ppn;
        pt1_pma = pt1_ppn << 12;
        pt1 = (struct pte*) pt1_pma;
    }
    else
    {
        // allocate a new page
    }
    if (!PTE_VALID(pt1[VPN1(vma)]))  // check if entry in page table 1 is valid (page exists for it i.e. subable 0 exists, otherwise allcoate)
    {
        uintptr_t temp = (uintptr_t) alloc_phys_page(); // allocates one page. Temp is pointer to start of that page which will be our l0 subtablw
        pt1[VPN1(vma)] = ptab_pte((struct pte*) temp, PTE_G & rwxug_flags);        // sets/creates it
    }
    pt0_ppn = pt1[VPN1(vma)].ppn;
    pt0_pma = pt0_ppn << 12;
    pt0 = (struct pte*) pt0_pma;

    if (vma <= 0xC0000000)
    {
        // directly accessed using ppn. Technically we should not be remapping kernel. Please error here
    }
    // greater than that its in user space
    pt0[VPN0(vma)] = leaf_pte(pp, rwxug_flags);

    return (void *) vma;

//     //struct pte* pt2_pte = &main_pt2[VPN2(vma)];
//     struct pte* pt1_pte;
//     struct pte* pt0_pte;
//     // check if root entry is valid
//     if (PTE_VALID(*pt2_pte))
//     {
//         pt1_pte = (struct pte*) VPN(pt2_pte->ppn);
//     }
//     else
//     {
//         // exception???
//         // for cp2 probably excpetion??
//     }
//     if (PTE_VALID(*pt1_pte))
//     {
//         pt0_pte = (struct pte*) VPN(pt1_pte->ppn);
//     }
//     else
//     {
        
//         // exception???
//         // if not valid... then we need to set it valid
//     }
//     /*
//         struct pte {
//         uint64_t flags : 8;
//         uint64_t rsw : 2;
//         uint64_t ppn : 44;                      // 
//         uint64_t reserved : 7;                  //
//         uint64_t pbmt : 2;                      //
//         uint64_t n : 1;
// };
    
//     */
//     if (PTE_VALID(*pt0_pte))
//     {
//         pt0_pte->flags = rwxug_flags | PTE_V;
//         //pt0_pte->rsw  free for kernel to use
//         pt0_pte->ppn = VPN((uintptr_t) pp); 
//     }
//     else
//     {
//         // exception???
//     }


//     return (void*) vma;
}

void *map_range(uintptr_t vma, size_t size, void *pp, int rwxug_flags) {
    // FIXME
    /*
    DAmn it all. Idk if they are contiuosu or no. 
    
    */
    int num_pages = ROUND_UP(size, PAGE_SIZE) / PAGE_SIZE;
    for (int i = 0; i < num_pages; i++)
    {
        map_page(vma + (i * PAGE_SIZE), (void *)((uintptr_t)(pp) + (i * PAGE_SIZE)), rwxug_flags);
    }
    return (void *) vma;
}

void *alloc_and_map_range(uintptr_t vma, size_t size, int rwxug_flags) {
    // FIXME
    int num_pages = ROUND_UP(size, PAGE_SIZE) / PAGE_SIZE;
    void* pp = alloc_phys_pages(num_pages);
    return map_range(vma, size, pp, rwxug_flags);
}

void set_range_flags(const void *vp, size_t size, int rwxug_flags) {
    // FIXME
    return;
}

void unmap_and_free_range(void *vp, size_t size) {
    // FIXME
    return;
}

int validate_vptr(const void *vp, size_t len, int rwxu_flags) {
    // FIXME
    return 0;
}

int validate_vstr(const char *vs, int rug_flags) {
    // FIXME
    return 0;
}

// Allocating physicla pages := removing from chunk list??

void *alloc_phys_page(void) {
    // FIXME
    return alloc_phys_pages(1);
}

void free_phys_page(void *pp) {
    // FIXME
    free_phys_pages(pp, 1);
    return;
}

void *alloc_phys_pages(unsigned int cnt) {
    // FIXME
    if (cnt <= 0)
    {
        kprintf("ERROR: alloc_phys_pages: Count <= 0: Cnt: %d", cnt);
        return NULL;
    }
    // walk along chunk list
    struct page_chunk* head = free_chunk_list;
    struct page_chunk* prev_chunk;  // previous of actaul_chunk
    struct page_chunk* actual_chunk = NULL; // acutal chunk chosen
    // sentinel makes it easy to do head/edge cases
    // start at the sentinel
    uint32_t min_cnt = UINT32_MAX;  // set as -1
    while (head->next != NULL)
    {
        if (head->next->pagecnt >= cnt && head->next->pagecnt < min_cnt)
        {
            prev_chunk = head;
            actual_chunk = head->next;
            min_cnt = MIN(min_cnt, cnt);
        }
    }
    if (actual_chunk == NULL)
    {
        kprintf("No avaiable pages can be allocated fro cnt: %d", cnt);
        // shoudl panic here but idk how
        return NULL;
    }
    // otherwise head->next is not null and we found
    // actual start address
    uintptr_t pages_start_address = ((uintptr_t) actual_chunk) + cnt * PAGE_SIZE;
    if (actual_chunk->pagecnt > cnt)
    {
        prev_chunk->next = (struct page_chunk*) (((uintptr_t) actual_chunk) + cnt * PAGE_SIZE);
        prev_chunk->next->next = actual_chunk->next;
        prev_chunk->next->pagecnt = actual_chunk->pagecnt - cnt;
    }
    else    // chunk size is not equal
    {
        prev_chunk->next = actual_chunk->next;
    }
    return (void*) actual_chunk;
}

void free_phys_pages(void *pp, unsigned int cnt) {
    // FIXME
    // we do not implement check for double free. Although we can do that later
    if (cnt <= 0 || pp == NULL) return; //not sure what to return
    struct page_chunk* freed_chunk = (struct page_chunk*) pp;
    freed_chunk->pagecnt = cnt;
    freed_chunk->next = free_chunk_list->next;
    free_chunk_list->next = freed_chunk;
    return;
}

unsigned long free_phys_page_count(void) {
    // FIXME
    unsigned long cnt = 0;
    struct page_chunk* head = free_chunk_list->next;
    while (head != NULL)
    {
        cnt += head->pagecnt;
        head = head->next;
    }
    return cnt;
}

int handle_umode_page_fault(struct trap_frame *tfr, uintptr_t vma) {
    // FIXME
    return 0;  // no handled
}

/**
 * @brief Reads satp to retrieve tag for active memory space
 * @return Tag for active memory space
 */
mtag_t active_space_mtag(void) { return csrr_satp(); }

/**
 * @brief Constructs tag from page table address and address space identifier
 * @param ptab Pointer to page table to use in tag
 * @param asid Address space identifier to use in tag
 * @return Memory tag formed from paging mode, page table address, and ASID
 */
static inline mtag_t ptab_to_mtag(struct pte *ptab, unsigned int asid) {
    return (((unsigned long)PAGING_MODE << RISCV_SATP_MODE_shift) |
            ((unsigned long)asid << RISCV_SATP_ASID_shift) | pagenum(ptab) << RISCV_SATP_PPN_shift);
}

/**
 * @brief Retrives a page table address from a tag
 * @param mtag Tag to extract page table address from
 * @return Pointer to page table retrieved from tag
 */
static inline struct pte *mtag_to_ptab(mtag_t mtag) { return (struct pte *)((mtag << 20) >> 8); }

/**
 * @brief Returns the address of the page table corresponding to the active memory space
 * @return Pointer to page table extracted from active memory space tag
 */
static inline struct pte *active_space_ptab(void) { return mtag_to_ptab(active_space_mtag()); }

/**
 * @brief Constructs a physical pointer from a physical page number
 * @param n Physical page number to derive physical pointer from
 * @return Pointer to memory corresponding to physical page
 */
static inline void *pageptr(uintptr_t n) { return (void *)(n << PAGE_ORDER); }

/**
 * @brief Constructs a physical page number from a pointer
 * @param p Pointer to derive physical page number from
 * @return Physical page number corresponding to pointer
 */
static inline unsigned long pagenum(const void *p) { return (unsigned long)p >> PAGE_ORDER; }

/**
 * @brief Checks if bits 63:38 of passed virtual memory address are all 1 or all 0
 * @param vma Virtual memory address to check well-formedness of
 * @return 1 if pointer is well-formed, 0 otherwise
 */
static inline int wellformed(uintptr_t vma) {
    // Address bits 63:38 must be all 0 or all 1
    uintptr_t const bits = (intptr_t)vma >> 38;
    return (!bits || !(bits + 1));
}

/**
 * @brief Constructs a page table entry corresponding to a leaf
 * @details For our purposes, a leaf PTE has the A, D, and V flags set
 * @param pp Physical address to set physical page number of PTE from
 * @param rwxug_flags Flags to set on PTE
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte leaf_pte(const void *pp, uint_fast8_t rwxug_flags) {
    return (struct pte){.flags = rwxug_flags | PTE_A | PTE_D | PTE_V, .ppn = pagenum(pp)};
}

/**
 * @brief Constructs a page table entry corresponding to a page table
 * @param pt Physical address to set physical page number of PTE from
 * @param g_flag Flags to set on PTE (should either be G flag or nothing)
 * @return PTE initialized with proper flags and PPN
 */
static inline struct pte ptab_pte(const struct pte *pt, uint_fast8_t g_flag) {
    return (struct pte){.flags = g_flag | PTE_V, .ppn = pagenum(pt)};
}

/**
 * @brief Returns an empty pte
 * @return An empty pte
 */
static inline struct pte null_pte(void) { return (struct pte){}; }
