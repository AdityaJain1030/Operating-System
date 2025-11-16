#### Compile Methodology
`riscv64-unknown-elf-gcc -nostdlib -T linker.ld -o test.elf test.c`
- `-nostdlib`: No standard library linking
- `-T`: Use linker script specified next (`linker.ld`: linker script)
- `-o`: Output file name, set as `test.elf`
- Finally file to convert to elf file is `test.c`


##### Makefile:
- In the makefile there is an `LDFLAGS`: LDFLAGS = `-melf64lriscv`
    - Use this instead of the `linker.ld`
    - `PREFIX=riscv64-unknown-elf-` and `LD=$(PREFIX)ld` mean that we use the `riscv64-unknown-elf-ld` linker
        - `LDFLAGS = -melf64lriscv` flags for linker
        - Remeber that Linker "links" together `.o` files!
        - Linker != Linker Script!! For ECE391 our linker script is `kernel.ld`


#### Sinlge test case:
- `riscv64-unknown-elf-gcc  -c test.c -o test.o`       : compilation
    - `-c`: compile only
- `riscv64-unknown-elf-ld -melf64lriscv -T kernel.ld -o test.elf test.o` // this links
    - linker into elf file

##### To view the elf file stuff:
- `riscv64-unknown-elf-readelf`
    - `-h`: Basic metadata like class, machine etc,
    - `-l`: Program headers
    - `-S`: Seciton headers
- `riscv64-unknown-elf-objdump`
    - `-d`: Dump everything
    - `f` : Elf Header
    - `p` : Program header
    - `d` : Disassembly
-  `riscv64-unknown-elf-objcopy`: Trasnforms and copies object files
    - `-O binary`:  mini.elf blob.raw ??? or can we just do `mv`