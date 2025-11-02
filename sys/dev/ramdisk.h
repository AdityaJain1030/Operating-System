/*! @file ramdisk.h‌‌‍‍‌‍⁠‌‌​‌‌‌⁠‍‌‌​⁠‍‌‌‌‍​⁠‍‌‌‍⁠​‌‌‍‌​⁠​‍‌‌‌‌‌⁠‍‍‌​⁠⁠‌‌‌​‌​‌‍‌‍‌‍‌‌‍‍​⁠​⁠‌​‍‍‌⁠‌‍‌‍‌​‌‌‍​‌​​‍‌‍‌‍‌​⁠‍‌​‌​‌‍‌⁠​⁠⁠‌
    @brief Memory-backed storage interface
    @copyright Copyright (c) 2024-2025 University of Illinois

*/

#ifndef _RAMDISK_H_
#define _RAMDISK_H_

#include <stddef.h>

struct storage;  // forward declaration

// EXPORTED FUNCTION DECLARATIONS
//

extern int ramdisk_make_uio(struct ramdisk *rd, struct uio **uioptr);

extern void ramdisk_attach();

#endif  // _RAMDISK_H_