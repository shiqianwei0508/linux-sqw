// SPDX-License-Identifier: GPL-2.0
/*
 * Definitions and wrapper functions for kernel decompressor
 *
 * Copyright IBM Corp. 2010
 *
 * Author(s): Martin Schwidefsky <schwidefsky@de.ibm.com>
 */

#include <linux/kernel.h>
#include <linux/string.h>
#include <asm/boot_data.h>
#include <asm/page.h>
#include "decompressor.h"
#include "boot.h"

/*
 * gzip declarations
 */
#define STATIC static

#undef memset
#undef memcpy
#undef memmove
#define memmove memmove
#define memzero(s, n) memset((s), 0, (n))

#if defined(CONFIG_KERNEL_BZIP2)
#define BOOT_HEAP_SIZE	0x400000
#elif defined(CONFIG_KERNEL_ZSTD)
#define BOOT_HEAP_SIZE	0x30000
#else
#define BOOT_HEAP_SIZE	0x10000
#endif

static unsigned long free_mem_ptr = (unsigned long) _end;
static unsigned long free_mem_end_ptr = (unsigned long) _end + BOOT_HEAP_SIZE;

#ifdef CONFIG_KERNEL_GZIP
#include "../../../../lib/decompress_inflate.c"
#endif

#ifdef CONFIG_KERNEL_BZIP2
#include "../../../../lib/decompress_bunzip2.c"
#endif

#ifdef CONFIG_KERNEL_LZ4
#include "../../../../lib/decompress_unlz4.c"
#endif

#ifdef CONFIG_KERNEL_LZMA
#include "../../../../lib/decompress_unlzma.c"
#endif

#ifdef CONFIG_KERNEL_LZO
#include "../../../../lib/decompress_unlzo.c"
#endif

#ifdef CONFIG_KERNEL_XZ
#include "../../../../lib/decompress_unxz.c"
#endif

#ifdef CONFIG_KERNEL_ZSTD
#include "../../../../lib/decompress_unzstd.c"
#endif

static void decompress_error(char *m)
{
	if (bootdebug)
		boot_rb_dump();
	boot_emerg("Decompression error: %s\n", m);
	boot_emerg(" -- System halted\n");
	disabled_wait();
}

unsigned long mem_safe_offset(void)
{
	return ALIGN(free_mem_end_ptr, PAGE_SIZE);
}

void deploy_kernel(void *output)
{
	__decompress(_compressed_start, _compressed_end - _compressed_start,
		     NULL, NULL, output, vmlinux.image_size, NULL, decompress_error);
}
