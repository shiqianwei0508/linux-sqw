/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/*
 * Copyright (C) 2006 Sony Computer Entertainment Inc.
 * Copyright 2006, 2007 Sony Corporation
 */

#ifndef _ASM_POWERPC_PS3FB_H_
#define _ASM_POWERPC_PS3FB_H_

#include <linux/types.h>
#include <linux/ioctl.h>

/* ioctl */
#define PS3FB_IOCTL_SETMODE       _IOW('r',  1, int) /* set video mode */
#define PS3FB_IOCTL_GETMODE       _IOR('r',  2, int) /* get video mode */
#define PS3FB_IOCTL_SCREENINFO    _IOR('r',  3, int) /* get screen info */
#define PS3FB_IOCTL_ON            _IO('r', 4)        /* use IOCTL_FSEL */
#define PS3FB_IOCTL_OFF           _IO('r', 5)        /* return to normal-flip */
#define PS3FB_IOCTL_FSEL          _IOW('r', 6, int)  /* blit and flip request */

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC         _IOW('F', 0x20, __u32) /* wait for vsync */
#endif

struct ps3fb_ioctl_res {
	__u32 xres; /* frame buffer x_size */
	__u32 yres; /* frame buffer y_size */
	__u32 xoff; /* margine x  */
	__u32 yoff; /* margine y */
	__u32 num_frames; /* num of frame buffers */
};

#endif /* _ASM_POWERPC_PS3FB_H_ */
