# Unix-like Operating System

## Project Overview

This is a Unix-like operating system built from scratch for UIUC's OS coursework. The entire OS includes a custom:

- **Kernel**: Custom kernel with process management, threading, and memory management
- **File System**: EXT2-like file system implementation with caching
- **Device Drivers**: UART, RTC, VirtIO block and network devices
- **System Calls**: Complete syscall interface for user programs
- **User Space**: Shell and various Unix utilities (cat, ls, echo, wc, etc.)
- **Interrupt Handling**: Custom interrupt and exception handling
- **Virtual Memory**: Memory management and heap allocation

## Architecture

- **Target Platform**: RISC-V architecture running on QEMU
- **Language**: C and RISC-V assembly
- **Build System**: Make-based build system with debugging support

## Directory Structure

- `sys/` - Kernel source code and core OS components
- `usr/` - User space programs and shell utilities
- `util/` - Build utilities and file system generation tools

## Building and Running

Use the provided VS Code tasks:

- **Build User & Shell**: Compiles user programs and generates file systems
- **Shell Program Debug Setup**: Builds and launches QEMU in debug mode
- **run QEMU (debug)**: Starts QEMU with debugging enabled

## Features

- Multi-threaded kernel
- Virtual file system with device abstraction
- Shell with pipes and I/O redirection support
- ELF binary loading
- Timer-based scheduling
- Games included: Rogue, Trek, Zork

## TODO

- Push the TCP/IP and vionet drivers
- Add required syscalls for libc
- 