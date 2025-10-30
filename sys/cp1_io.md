# How all parts of the system work

## STORAGE BACKENDS

In general a storage backend must implement a `read`, `write`, `cntl`, `open`, `close`, and `attach`. `read`, `write`, and `cntl` are the only ones that have a fixed spec for i/o

`read` must take in a `struct storage *sto, unsigned long long pos, void *buf, unsigned long bytecnt` and return `bytes_read`.
- It must not read more than the data available in the file
- byte_read $leq$ bytecnt
- must handle null sto and buf, OOB pos.

... everything looks fine here

### RAMDISK
If we mount the file system into the elf `.rodata` section, ramdisk should work as intended, although *im not sure where the `_kimg_blob_start` gets set...

Regardless ramdisk `open`, `close` should just handle nulptrs gracefully and do nothing
`attach` should just initialize the device driver, no i/o spec

## VIOBLK
