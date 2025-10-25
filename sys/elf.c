/*! @file elf.c‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌‍‌⁠​⁠⁠‌
    @brief ELF file loader
    @copyright Copyright (c) 2024-2025 University of Illinois
    @license SPDX-License-identifier: NCSA

*/

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

/*
Very good reference
https://linux.die.net/man/5/elf
*/

struct elf64_ehdr {
    unsigned char e_ident[16];              //  contain the elf magic "\x7fELF": "0x7f454c46"
    uint16_t e_type;                        //  file type. Should be ET_EXEC
    uint16_t e_machine;                     //  Required Architecture                
    uint32_t e_version;                     //  File Version
    uint64_t e_entry;                       //  Virtual address to which the system first transfers control, thus starting the processs. If no associated entry point, set to 0
    uint64_t e_phoff;                       //  IMPORTANT: Program header table's offset in bytes
    uint64_t e_shoff;                       //  Section Header Table's offset
    uint32_t e_flags;                       //  
    uint16_t e_ehsize;                      //  Size of this ELF header
    uint16_t e_phentsize;                   //  Size in bytes of one entry in the file's program header table in ELF file?
    uint16_t e_phnum;                       //  Number of entries in the program header table; if # of entries is >= PN_XNUM (0xffff) then real number of entries is held in the sh_info member of initial entry in section header table
    uint16_t e_shentsize;                   //  Size in bytes of each section header
    uint16_t e_shnum;                       //   Section header number
    uint16_t e_shstrndx;                    //
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
    uint32_t p_type;            // if p_type == PT_LOAD load into program
    uint32_t p_flags;
    uint64_t p_offset;          //  offset in ELF file where the segment data begins
    uint64_t p_vaddr;           //  virtual address at which first byte of the segment should be put into memory
    uint64_t p_paddr;           //  physical address (mostly unused I think)???
    uint64_t p_filesz;          //  Number of bytes to read from the file
    uint64_t p_memsz;           //  Number of bytes to reserve in memory for this segment. If p_memz > p_filsz zero the extra bytes
    uint64_t p_align;           //  CP2: Alignment required in memory
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
    // FIXME
    /*
        Call the uio interface and open from storage device, reading the bytes
    
    
    
    */
    struct elf64_ehdr ehdr;         // ELF header

    // read the elf64_ehdr header
    long ehdr_bytes_read = uio_read(uio, &ehdr, sizeof(ehdr));

    // Did not read enough for the header. Invalid argument
    if (ehdr_bytes_read != sizeof(ehdr))
    {
        return -EBADFMT;
    }

    // Start doing checks

    // must be ELF magic number
    if (ehdr.e_ident[0] != 0x7F || ehdr.e_ident[1] != 'E' || ehdr.e_ident[2] != 'L' || ehdr.e_ident[3] != 'F')
    {
        return -EBADFMT;
    }

    // must be RISCV machine
    if (ehdr.e_machine != EM_RISCV)
    {
        return -EBADFMT;
    }

    //  set e_entry
    *eptr = (void (*)(void))ehdr.e_entry;


    // Get program header
    // set the ops for control
    

    // start iterating over program header table
    //  get each header
    for (int i = 0; i < ehdr.e_phnum; i++)
    {
        struct elf64_phdr phdr;

        unsigned long long phdr_pos = ehdr.e_phoff + i * ehdr.e_phentsize;
        uio_cntl(uio, FCNTL_SETPOS, &phdr_pos);     // set starting address of read to program header table

        long phdr_bytes_read = uio_read(uio, &phdr, sizeof(phdr));
        
        // Not sure if i need to check if phdr_bytes_read == sizeof(phdr)

        if (phdr.p_type == PT_LOAD)
        {
            if (phdr.p_vaddr < RAM_START || phdr.p_vaddr > RAM_END)
            {
                return -ENOTSUP;    // Does not support this operation?
            }
            uint8_t *ram_mem = (uint8_t*)(phdr.p_vaddr);    // pointer to main memory where we will load in the segment

            // first check that the memory is valid

            memset(ram_mem, 0, phdr.p_memsz);               // number of bytes to set to 0 first/reserve in memory

            unsigned long long seg_pos = phdr.p_offset;
            uio_cntl(uio, FCNTL_SETPOS, &seg_pos);          //  Set address where to read ELF file from external storage
            uio_read(uio, ram_mem, phdr.p_filesz);
        }

    }
    return 0;
    




    //return -ENOTSUP;
}