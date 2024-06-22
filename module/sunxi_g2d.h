/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Allwinner G2D - 2D Graphics Accelerator driver
 *
 * Copyright (C) 2016 Allwinner.
 * Copyright (C) 2024 Brandon Cheo Fusi <fusibrandon13@gmail.com>
 */

#ifndef _SUNXI_G2D_H_
#define _SUNXI_G2D_H_

#include <media/v4l2-device.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-v4l2.h>
#include <media/videobuf2-dma-contig.h>
#include <media/v4l2-ctrls.h>

#include <linux/platform_device.h>
#include <linux/types.h> 

#define G2D_NAME "sunxi-g2d"

#define G2D_MIN_WIDTH	8U
#define G2D_MIN_HEIGHT	8U
#define G2D_MAX_WIDTH	2048U
#define G2D_MAX_HEIGHT	2048U

enum g2d_op {
	G2D_RECTFILL,
	G2D_BITBLT
};

/*
 * Blend Layer alpha modes
 * G2D_PIXEL_ALPHA: Each pixel carries its own alpha value
 * G2D_GLOBAL_ALPHA: Each layer has an alpha value shared by all pixels
 * in that layer
 * G2D_MIXER_ALPHA: All pixels in all layers share a single alpha value
 */
enum g2d_alpha_bld_mode {
	G2D_PIXEL_ALPHA,
	G2D_GLOBAL_ALPHA,
	G2D_MIXER_ALPHA,
};

struct g2d_fmt {
	u32	fourcc;
	int depth;
	u32 hw_id;
};

struct g2d_frame {
	struct v4l2_pix_format v4l2_pix_fmt;
	bool premult_alpha;
	enum g2d_alpha_bld_mode alpha_bld_mode;
	uint32_t alignment;
	struct v4l2_selection sel;
};

struct sunxi_g2d {
	void __iomem	*base;
	int irq;
	struct clk *mod_clk;
	struct clk *bus_clk;
	struct clk *ram_clk;
	struct reset_control *rstc;

    /* Device file mutex */
	struct mutex		dev_mutex;

	struct device		*dev;
    struct v4l2_device	v4l2_dev;
	struct video_device	vfd;
	struct v4l2_m2m_dev	*m2m_dev;

	struct g2d_fmt *supported_fmts;
};

struct sunxi_g2d_ctx {
	struct v4l2_fh		fh;
	struct sunxi_g2d	*g2d;

	struct g2d_frame src;
	struct g2d_frame dst;

	/* only useful for rectfill operations */
	uint32_t rectfill_color;
	uint32_t rectfill_color_alpha;
	
	/* active g2d operation */
	enum g2d_op chosen_g2d_op;

	struct v4l2_ctrl_handler ctrl_handler;
};

struct g2d_fmt *find_fmt(struct v4l2_pix_format *);

#endif