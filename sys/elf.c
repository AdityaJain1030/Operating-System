/*! @file elf.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌‌‌​‌‍‍⁠​⁠⁠‌
    @brief ELF file loader
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

#include <stddef.h>
#ifdef ELF_TRACE
#define TRACE
#endif

#ifdef ELF_DEBUG
#define DEBUG
#endif

#include "elf.h"

#include <stdint.h>

#include "conf.h"
#include "error.h"
#include "memory.h"
#include "misc.h"
#include "string.h"
#include "uio.h"

// Offsets into e_ident

#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9

// ELF header e_ident[EI_CLASS] values

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

// ELF header e_ident[EI_DATA] values

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

// ELF header e_ident[EI_VERSION] values

#define EV_NONE 0
#define EV_CURRENT 1

// ELF header e_type values

enum elf_et { ET_NONE = 0, ET_REL, ET_EXEC, ET_DYN, ET_CORE };

/*! @struct elf64_ehdr
    @brief ELF header struct
*/
struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};

/*! @enum elf_pt
    @brief Program header p_type values
*/
enum elf_pt { PT_NULL = 0, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS };

// Program header p_flags bits

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

/*! @struct elf64_phdr
    @brief Program header struct
*/
struct elf64_phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
};

// ELF header e_machine values (short list)

#define EM_RISCV 243
/**
 * \brief Validates and loads an ELF file into memory.
 *
 * This function validates an ELF file, then loads its contents into memory,
 * returning the start of the entry point through \p eptr.
 *
 * The loader processes only program header entries of type `PT_LOAD`. The layouts
 * of structures and magic values can be found in the Linux ELF header file
 * `<uapi/linux/elf.h>`
 * The implementation should ensure that all loaded sections of the program are
 * mapped within the memory range `0x80100000` to `0x81000000`.
 *
 * Let's do some reading! The following documentation will be very helpful!
 * [Helpful doc](https://linux.die.net/man/5/elf)
 * Good luck!
 * [Educational video](https://www.youtube.com/watch?v=dQw4w9WgXcQ)
 *
 * \param[in]  uio  Pointer to an user I/O corresponding to the ELF file.
 * \param[out] eptr   Double pointer used to return the ELF file's entry point.
 *
 * \return 0 on success, or a negative error code on failure.
 */
int elf_load(struct uio* uio, void (**eptr)(void)) {
    int retval;
    if (uio== NULL|| eptr == NULL) return -EINVAL;

    ////VALIDATE ELF HEADER
    struct elf64_ehdr ehdr;
    unsigned long pos = 0;
    if (uio_cntl(uio,FCNTL_SETPOS, &pos) <0) return -EIO;
    retval = uio_read(uio, (void *)&ehdr, (unsigned long)sizeof(struct elf64_ehdr));
    if (retval <0) return -EIO;
    assert(retval == sizeof(ehdr));//an assertion makes sense here
    
    //validates magic number sequence
    if (ehdr.e_ident[0] != 0x7F) return -EBADFMT; 
    if (ehdr.e_ident[1] != 0x45) return -EBADFMT; //E
    if (ehdr.e_ident[2] != 0x4C) return -EBADFMT; //L
    if (ehdr.e_ident[3] != 0x46) return -EBADFMT; //F

    
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) return -EBADFMT;//validates the class as 64b
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) return -EBADFMT;//validates little endian format
    if (ehdr.e_ident[EI_VERSION] != EV_CURRENT) return -EBADFMT;//validates version as 1 (should always be 1)
    
    if (!((ehdr.e_type == ET_EXEC) || (ehdr.e_type == ET_DYN))) return -EBADFMT;

    if (ehdr.e_machine != EM_RISCV) return -EBADFMT;

    ////VALIDATE PROGRAM HEADERS
    //programn header offset: ehdr.e_phoff  
    //size of each program header: ehdr.e_phentsize
    //number of program headers: ehdr.e_phnum
    
    //"but use the vaddr so you have to do less work for cp2" - Ninjapod
    //well that has changed completely now
    for (int i =0; i < ehdr.e_phnum; i++){
        pos = ehdr.e_phoff + i*ehdr.e_phentsize;
        if (uio_cntl(uio, FCNTL_SETPOS, &pos) < 0) return -EIO;
        struct elf64_phdr phdr;
        retval = uio_read(uio, (void *)&phdr, sizeof(phdr));
        if (retval <0) return -ENOTSUP;
        assert(retval == sizeof(phdr));

        if (phdr.p_type != PT_LOAD) continue;

        //new groups code checks for filesz > memsz lmao wth
        //WHY DID HE CHECK FOR INTEGER OVERFLOW :skull:
        
        
        //if (phdr.p_vaddr < 0x80100000) return -EBADFMT;
        //if ((phdr.p_vaddr + phdr.p_memsz) > 0x81000000) return -EBADFMT; 
        //CP1 IMPL: FIXED ADDRESS LOAD

        //CP2 IMPL: we now need to deal with all the possibilitied and edgecases introduced by mapping into the userland
        if (phdr.p_vaddr < 0x0C0000000UL || (phdr.p_vaddr + phdr.p_memsz) > 0x100000000UL){ //made sure to typecast to UL this time because of MSB
            trace("nice try diddy I see u trying to touch addresses outside of USR_START_VMA\n");   // bro wth is this comment ;(
            return -EBADFMT;
        }
        // if (phdr.p_align > 1) {
        //     if ((phdr.p_vaddr % phdr.p_align) != (phdr.p_offset % phdr.p_align)) {
        //         return -EBADFMT;
        //     }
        // }
        if (phdr.p_align <= 1) continue;
        if (phdr.p_align & (phdr.p_align -1)) return -EBADFMT;//continue if not an integer multiple of 2
        if (phdr.p_vaddr % phdr.p_align != 0) return -EBADFMT;//these two must be congruent
        //the elf docs don't say anything about the paddr needing to be congruent

        int program_rwxug = PTE_U;//literally all ELF segments that get loaded should have this since they're all user executable
        if (phdr.p_flags & PF_R) program_rwxug |= PTE_R;
        if (phdr.p_flags & PF_W) program_rwxug |= PTE_W;
        if (phdr.p_flags & PF_X) program_rwxug |= PTE_X;
        void* addr = alloc_and_map_range((uintptr_t)phdr.p_vaddr, (size_t)phdr.p_memsz, PTE_R | PTE_X | PTE_U | PTE_W); //yeah just make sure sstatus.SUM is on for this next part


        //char * vaddr = (char *) phdr.p_vaddr;
        // elf should not bear responsibility of cleaning the page thats allocs job
        // memset((void *)phdr.p_vaddr, 0, phdr.p_memsz); //we want a clear slate for 

        pos = phdr.p_offset;
        if (uio_cntl(uio, FCNTL_SETPOS, &pos)<0) return -ENOTSUP;
        retval  = uio_read(uio, (void *)phdr.p_vaddr, phdr.p_filesz);

        set_range_flags((void *)phdr.p_vaddr, (size_t)phdr.p_memsz, program_rwxug);
        //if (retval< 0) return -ENOTSUP;
    }

    

    *eptr = (void (*) (void)) ehdr.e_entry;
    return 0;
}
