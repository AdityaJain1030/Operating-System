/*! @file elf.c
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

enum elf_pt { PT_NULL = 0, PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_NOTE, PT_SHLIB, PT_PHDR, PT_TLS };

// Program header p_flags bits
#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4

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

#define EM_RISCV 243

/**
 * \brief Validates and loads an ELF file into memory.
 *
 * This function validates an ELF file, then loads its contents into memory,
 * returning the start of the entry point through \p eptr.
 *
 * \param[in]  uio  Pointer to an user I/O corresponding to the ELF file.
 * \param[out] eptr   Double pointer used to return the ELF file's entry point.
 *
 * \return 0 on success, or a negative error code on failure.
 */
int elf_load(struct uio* uio, void (**eptr)(void)) {
    struct elf64_ehdr ehdr;
    long bytes_read;
    int result;
    
    // Validate parameters
    if (uio == NULL || eptr == NULL) {
        return -EINVAL;
    }
    
    // Reset file position to beginning
    unsigned long long pos = 0;
    __sync_synchronize();
    result = uio_cntl(uio, FCNTL_SETPOS, &pos);
    __sync_synchronize();
    if (result != 0) {
        return -EIO;
    }
    
    // Read ELF header with proper synchronization
    __sync_synchronize();
    bytes_read = uio_read(uio, &ehdr, sizeof(ehdr));
    __sync_synchronize();
    
    if (bytes_read != sizeof(ehdr)) {
        return -EBADFMT;
    }
    
    // Validate ELF magic number
    if (ehdr.e_ident[0] != 0x7F || 
        ehdr.e_ident[1] != 'E' || 
        ehdr.e_ident[2] != 'L' || 
        ehdr.e_ident[3] != 'F') {
        return -EBADFMT;
    }
    
    // Validate ELF class and version
    if (ehdr.e_ident[EI_CLASS] != ELFCLASS64) {
        return -EBADFMT;
    }
    
    if (ehdr.e_ident[EI_DATA] != ELFDATA2LSB) {
        return -EBADFMT;  // RISC-V is little-endian
    }
    
    if (ehdr.e_ident[EI_VERSION] != EV_CURRENT) {
        return -EBADFMT;
    }
    
    if (ehdr.e_version != EV_CURRENT) {
        return -EBADFMT;
    }
    
    // Validate file type - must be executable
    if (ehdr.e_type != ET_EXEC) {
        return -EBADFMT;
    }
    
    // Validate machine type
    if (ehdr.e_machine != EM_RISCV) {
        return -EBADFMT;
    }
    
    // Validate entry point
    uint64_t ram_start = 0x80100000;  // Per spec requirement
    uint64_t ram_end = 0x81000000;    // Per spec requirement
    
    if (ehdr.e_entry < ram_start || ehdr.e_entry >= ram_end) {
        return -EBADFMT;
    }
    
    // Validate program header size
    if (ehdr.e_phentsize != sizeof(struct elf64_phdr)) {
        return -EBADFMT;
    }
    
    // Validate program header count (prevent excessive allocations)
    if (ehdr.e_phnum == 0 || ehdr.e_phnum > 128) {
        return -EBADFMT;
    }
    
    // Validate program header table location
    unsigned long long ph_table_size = (unsigned long long)ehdr.e_phnum * ehdr.e_phentsize;
    if (ehdr.e_phoff > UINT64_MAX - ph_table_size) {
        return -EBADFMT;
    }
    
    // Process program headers
    for (uint16_t i = 0; i < ehdr.e_phnum; i++) {
        struct elf64_phdr phdr;
        unsigned long long phdr_pos = ehdr.e_phoff + (i * ehdr.e_phentsize);
        
        // Seek to program header
        __sync_synchronize();
        result = uio_cntl(uio, FCNTL_SETPOS, &phdr_pos);
        __sync_synchronize();
        if (result != 0) {
            return -EIO;
        }
        
        // Read program header
        __sync_synchronize();
        bytes_read = uio_read(uio, &phdr, sizeof(phdr));
        __sync_synchronize();
        
        if (bytes_read != sizeof(phdr)) {
            return -EBADFMT;
        }
        
        // Process PT_LOAD segments
        if (phdr.p_type == PT_LOAD) {
            // Validate segment sizes
            if (phdr.p_filesz > phdr.p_memsz) {
                return -EBADFMT;
            }
            
            // Check for integer overflow
            if (phdr.p_vaddr > UINT64_MAX - phdr.p_memsz) {
                return -EBADFMT;
            }
            
            uint64_t seg_start = phdr.p_vaddr;
            uint64_t seg_end = seg_start + phdr.p_memsz;
            
            // Validate segment is within allowed memory range
            if (seg_start < ram_start || seg_end > ram_end) {
                return -ENOTSUP;
            }
            
            // Check alignment if specified
            if (phdr.p_align > 1) {
                if ((phdr.p_vaddr % phdr.p_align) != (phdr.p_offset % phdr.p_align)) {
                    return -EBADFMT;
                }
            }
            
            // Map segment to memory
            void* mem_ptr = (void*)(uintptr_t)phdr.p_vaddr;
            
            // Zero the entire memory region (handles BSS)
            memset(mem_ptr, 0, (size_t)phdr.p_memsz);
            
            // Load file data if present
            if (phdr.p_filesz > 0) {
                unsigned long long file_pos = phdr.p_offset;
                
                // Validate file offset
                __sync_synchronize();
                result = uio_cntl(uio, FCNTL_SETPOS, &file_pos);
                __sync_synchronize();
                if (result != 0) {
                    return -EIO;
                }
                
                // Read segment data
                __sync_synchronize();
                bytes_read = uio_read(uio, mem_ptr, (unsigned long)phdr.p_filesz);
                __sync_synchronize();
                
                if (bytes_read != (long)phdr.p_filesz) {
                    return -EIO;
                }
            }
            
            // Note: In a real OS, we'd set memory permissions based on p_flags
            // but since this is a simple loader, we skip that
        }
    }
    
    // Set entry point
    *eptr = (void (*)(void))(uintptr_t)ehdr.e_entry;
    
    // Ensure all writes are visible before returning
    __sync_synchronize();
    
    return 0;
}