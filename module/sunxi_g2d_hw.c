/*
 * Allwinner SoCs g2d driver.
 *
 * Copyright (C) 2016 Allwinner.
 * Copyright (C) 2024 Brandon Cheo Fusi <fusibrandon13@gmail.com>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2.  This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */
#include <linux/types.h>
#include <linux/stddef.h>
#include <linux/dmaengine.h>
#include <linux/bitfield.h>

#include "sunxi_g2d_hw.h"
#include "sunxi_g2d_regs.h"

static inline uint32_t g2d_read(struct sunxi_g2d *g2d, uint32_t reg)
{
	return readl(g2d->base + reg);
}

static inline void g2d_write(struct sunxi_g2d *g2d,
				     uint32_t reg, uint32_t val)
{
	writel(val, g2d->base + reg);
}

static inline void g2d_set_bits(struct sunxi_g2d *g2d,
					uint32_t reg, uint32_t bits)
{
	writel(readl(g2d->base + reg) | bits, g2d->base + reg);
}

static inline void g2d_clr_bits(struct sunxi_g2d *g2d,
					    uint32_t reg, uint32_t bits)
{
	writel(readl(g2d->base + reg) & ~bits, g2d->base + reg);
}

static uint32_t v4l2_fmt_to_hw_id(struct v4l2_pix_format *v4l2_pix_fmt)
{
	struct g2d_fmt *fmt;

	fmt = find_fmt(v4l2_pix_fmt); // !! (rectfill assumption) src_fmt
	return (fmt) ? fmt->hw_id : G2D_FORMAT_BGRX8888;
}

void g2d_hw_open(struct sunxi_g2d *g2d)
{
	g2d_set_bits(g2d, G2D_SCLK_GATE,
			(G2D_SCLK_GATE_MIXER | G2D_SCLK_GATE_ROT));
	g2d_set_bits(g2d, G2D_HCLK_GATE,
			(G2D_HCLK_GATE_MIXER | G2D_HCLK_GATE_ROT));
	g2d_set_bits(g2d, G2D_AHB_RESET, 
			(G2D_AHB_MIXER_RESET | G2D_AHB_ROT_RESET));
}

void g2d_hw_close(struct sunxi_g2d *g2d)
{
	g2d_write(g2d, G2D_SCLK_GATE, 0);
	g2d_write(g2d, G2D_HCLK_GATE, 0);
	g2d_write(g2d, G2D_AHB_RESET, 0);
}

void g2d_hw_reset(struct sunxi_g2d *g2d)
{
	g2d_write(g2d, G2D_AHB_RESET, 0);
	g2d_set_bits(g2d, G2D_AHB_RESET, 
			(G2D_AHB_MIXER_RESET | G2D_AHB_ROT_RESET));
}

static void g2d_mixer_irq_enable(struct sunxi_g2d *g2d)
{
	g2d_write(g2d, G2D_MIXER_INT, G2D_MIXER_INT_FINISH_IRQ_EN);
}

int g2d_mixer_irq_query(struct sunxi_g2d *g2d)
{
	uint32_t tmp;

	tmp = g2d_read(g2d, G2D_MIXER_INT);
	if (tmp & G2D_MIXER_INT_IRQ_PENDING) {
		g2d_clr_bits(g2d, G2D_MIXER_INT, G2D_MIXER_INT_IRQ_PENDING 
					| G2D_MIXER_INT_FINISH_IRQ_EN);

		return 1;
	}

	return 0;
}

void g2d_mixer_reset(struct sunxi_g2d *g2d)
{
	g2d_clr_bits(g2d, G2D_AHB_RESET, G2D_AHB_MIXER_RESET);
	g2d_set_bits(g2d, G2D_AHB_RESET, G2D_AHB_MIXER_RESET);
}

void g2d_rot_reset(struct sunxi_g2d *g2d)
{
	g2d_clr_bits(g2d, G2D_AHB_RESET, G2D_AHB_MIXER_RESET);
	g2d_set_bits(g2d, G2D_AHB_RESET, G2D_AHB_MIXER_RESET);
}

/*
 * Basically, this should map the format to a tuple (ycnt, ucnt, vcnt),
 * with each entry corresponding to the # of bytes for each channel in the
 * YUV representation
 * 
 * TODO: Map only formats defined in g2d_formats.
 */
void fmt2yuvcnt(uint32_t format, uint32_t *ycnt, uint32_t *ucnt, uint32_t *vcnt)
{
	*ycnt = 0;
	*ucnt = 0;
	*vcnt = 0;
	if (format <= G2D_FORMAT_BGRX8888)
		*ycnt = 4;

	else if (format <= G2D_FORMAT_BGR888)
		*ycnt = 3;

	else if (format <= G2D_FORMAT_BGRA5551)
		*ycnt = 2;

	else if (format <= G2D_FORMAT_BGRA1010102)
		*ycnt = 4;

	else if (format <= 0x23) {
		*ycnt = 2;
	}

	else if (format <= 0x25) {
		*ycnt = 1;
		*ucnt = 2;
	}

	else if (format == 0x26) {
		*ycnt = 1;
		*ucnt = 1;
		*vcnt = 1;
	}

	else if (format <= 0x29) {
		*ycnt = 1;
		*ucnt = 2;
	}

	else if (format == 0x2a) {
		*ycnt = 1;
		*ucnt = 1;
		*vcnt = 1;
	}

	else if (format <= 0x2d) {
		*ycnt = 1;
		*ucnt = 2;
	}

	else if (format == 0x2e) {
		*ycnt = 1;
		*ucnt = 1;
		*vcnt = 1;
	}

	else if (format == 0x30)
		*ycnt = 1;

	else if (format <= 0x36) {
		*ycnt = 2;
		*ucnt = 4;
	}

	else if (format <= 0x39)
		*ycnt = 6;
}

/* TODO: convert layer_no to an enumeration */
void g2d_fc_set(struct sunxi_g2d *g2d, uint32_t layer_no, uint32_t color_value)
{
	G2D_INFO_MSG("FILLCOLOR: sel: %d, color: 0x%x\n", layer_no, color_value);

	switch (layer_no) 
	{
		case 0:
			/* Video Layer */
			g2d_set_bits(g2d, V0_ATTCTL, V0_ATTCTL_FILLCOLOR_EN);
			g2d_write(g2d, V0_FILLC, color_value);
			break;

		case 1:
			/* UI0 Layer */
			g2d_set_bits(g2d, UI0_ATTR, BIT(4));
			g2d_write(g2d, UI0_FILLC, color_value);
			break;

		case 2:
			/* UI1 Layer */
			g2d_set_bits(g2d, UI1_ATTR, BIT(4));
			g2d_write(g2d, UI1_FILLC, color_value);
			break;

		case 3:
			/* UI2 Layer */
			g2d_set_bits(g2d, UI2_ATTR, BIT(4));
			g2d_write(g2d, UI2_FILLC, color_value);
			break;

		default:
			return;
	}
}

/* TODO: convert pipe_no to an enumeration */
void g2d_bldin_set(struct sunxi_g2d *g2d, struct g2d_frame *frm,
		uint32_t pipe_no)
{
	uint32_t rect_x, rect_y, rect_w, rect_h;
	uint32_t reg;
	uint32_t tmp;

	if (!pipe_no) {
		g2d_set_bits(g2d, BLD_EN_CTL, BLD_PIPE0_EN);
		if (frm->premult_alpha)
			g2d_set_bits(g2d, BLD_PREMUL_CTL,
				BLD_PREMUL_CTL_PIPE0_ALPHA_MODE);
	}

	else {
		g2d_set_bits(g2d, BLD_EN_CTL, BLD_PIPE1_EN);
		if (frm->premult_alpha)
			g2d_set_bits(g2d, BLD_PREMUL_CTL,
				BLD_PREMUL_CTL_PIPE1_ALPHA_MODE);
	}

	/* the horizontal (rect_x) and vertical (rect_y) blend offsets are 
	 * always set to zero.
	 */
	rect_x = 0;
	rect_y = 0;
	rect_w = frm->sel.r.width;
	rect_h = frm->sel.r.height;

	tmp = ((rect_h - 1) << 16) | (rect_w - 1);
	G2D_INFO_MSG("BLD_CH_ISIZE W:  0x%x\n", rect_w);
	G2D_INFO_MSG("BLD_CH_ISIZE H:  0x%x\n", rect_h);

	reg = (pipe_no) ? BLD_CH_ISIZE1 : BLD_CH_ISIZE0;
	g2d_write(g2d, reg, tmp);

	tmp = ((rect_y <= 0 ? 0 : rect_y - 1) << 16) 
		| (rect_x <= 0 ? 0 : rect_x - 1);
	G2D_INFO_MSG("BLD_CH_ISIZE X:  0x%x\n", rect_x);
	G2D_INFO_MSG("BLD_CH_ISIZE Y:  0x%x\n", rect_y);

	reg = (pipe_no) ? BLD_CH_OFFSET1 : BLD_CH_OFFSET0;
	g2d_write(g2d, reg, tmp);
}

/**
 * set the bld color space based on the format
 * if the format is UI, then set the bld in RGB color space
 * if the format is Video, then set the bld in YUV color space
 */
void g2d_bld_cs_set(struct sunxi_g2d *g2d, struct g2d_frame *frm)
{
	uint32_t fmt_hw_id;

	fmt_hw_id = v4l2_fmt_to_hw_id(&frm->v4l2_pix_fmt);

	if (fmt_hw_id <= G2D_FORMAT_BGRA1010102)
		g2d_clr_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_ALPHA_MODE);
	else if (fmt_hw_id <= G2D_FORMAT_YUV411_PLANAR)
		g2d_set_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_ALPHA_MODE);
}

void g2d_wb_set(struct sunxi_g2d *g2d, struct g2d_frame *frm, 
		dma_addr_t addr[3])
{
	uintptr_t addr0, addr1, addr2;
	uint32_t fmt_hw_id;
	uint32_t ycnt, ucnt, vcnt;
	uint32_t pitch0, pitch1, pitch2;
	uint32_t cw, cy, cx;
	uint32_t tmp;

	/* write-back pixel format */
	fmt_hw_id = v4l2_fmt_to_hw_id(&frm->v4l2_pix_fmt);
	g2d_write(g2d, WB_ATT, fmt_hw_id); 

	/* write-back size */
	tmp = FIELD_PREP(WB_SIZE_WIDTH, (frm->sel.r.width == 0 ?
				0 : frm->sel.r.width - 1));
	tmp |= FIELD_PREP(WB_SIZE_HEIGHT, (frm->sel.r.height == 0 ? 
				0 : frm->sel.r.height - 1));
	g2d_write(g2d, WB_SIZE, tmp);

	/* blend output size */
	G2D_INFO_MSG("BLD_CH_OSIZE W:  0x%x\n", frm->sel.r.width);
	G2D_INFO_MSG("BLD_CH_OSIZE H:  0x%x\n", frm->sel.r.height);
	g2d_write(g2d, BLD_OUT_SIZE, tmp);

	if (frm->premult_alpha)
		g2d_set_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_PREMUL_EN);
	else
		g2d_clr_bits(g2d, BLD_OUT_COLOR, BLD_OUT_COLOR_PREMUL_EN);

	if ((fmt_hw_id >= G2D_FORMAT_YUV422UVC_V1U1V0U0)
	      && (fmt_hw_id <= G2D_FORMAT_YUV422_PLANAR)) {
		cw = frm->v4l2_pix_fmt.width >> 1;
		cx = frm->sel.r.left >> 1;
		cy = frm->sel.r.top;
	}

	else if ((fmt_hw_id >= G2D_FORMAT_YUV420UVC_V1U1V0U0)
		 && (fmt_hw_id <= G2D_FORMAT_YUV420_PLANAR)) {
		cw = frm->v4l2_pix_fmt.width >> 1;
		cx = frm->sel.r.left >> 1;
		cy = frm->sel.r.top >> 1;
	}

	else if ((fmt_hw_id >= G2D_FORMAT_YUV411UVC_V1U1V0U0)
		 && (fmt_hw_id <= G2D_FORMAT_YUV411_PLANAR)) {
		cw = frm->sel.r.left >> 2;
		cy = frm->sel.r.top;
	}

	else {
		cw = 0;
		cx = 0;
		cy = 0;
	}

	fmt2yuvcnt(fmt_hw_id, &ycnt, &ucnt, &vcnt);

	pitch0 = ALIGN(ycnt * frm->v4l2_pix_fmt.width, frm->alignment);
	g2d_write(g2d, WB_PITCH0, pitch0);

	pitch1 = ALIGN(ucnt * cw, frm->alignment);
	g2d_write(g2d, WB_PITCH1, pitch1);
	
	pitch2 = ALIGN(vcnt * cw, frm->alignment);
	g2d_write(g2d, WB_PITCH2, pitch2);

	G2D_INFO_MSG("OutputPitch: %d, %d, %d\n", pitch0, pitch1, pitch2);

	addr0 =
		addr[0] + pitch0 * frm->sel.r.top + ycnt * frm->sel.r.left;
	g2d_write(g2d, WB_LADD0, addr0 & GENMASK(31, 0));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	g2d_write(g2d, WB_HADD0, addr0 >> 32);
#endif

	addr1 = addr[1] + pitch1 * cy + ucnt * cx;
	g2d_write(g2d, WB_LADD1, addr1 & GENMASK(31, 0));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	g2d_write(g2d, WB_HADD1, addr1 >> 32);
#endif

	addr2 = addr[2] + pitch2 * cy + vcnt * cx;
	g2d_write(g2d, WB_LADD2, addr2 & GENMASK(31, 0));
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	g2d_write(g2d, WB_HADD2, addr2 >> 32);
#endif

	G2D_INFO_MSG("WbAddr: 0x%lx, 0x%lx, 0x%lx\n", addr0, addr1, addr2);
}

void g2d_vlayer_set(struct sunxi_g2d *g2d, struct g2d_frame *frm,
		dma_addr_t addr[3], uint32_t layer_alpha)
{
	uintptr_t addr0, addr1, addr2;
	uint32_t fmt_hw_id;
	uint32_t ycnt, ucnt, vcnt;
	uint32_t pitch0, pitch1, pitch2;
	uint32_t cw, cy, cx;
	uint32_t tmp;

	tmp = FIELD_PREP(V0_ATTCTL_GLBALPHA, layer_alpha);

	if (frm->premult_alpha)
		tmp |= FIELD_PREP(V0_ATTCTL_PREMUL_CTL, 0x2);
	
	fmt_hw_id = v4l2_fmt_to_hw_id(&frm->v4l2_pix_fmt);
	tmp |= FIELD_PREP(V0_ATTCTL_FBFMT, fmt_hw_id);
	tmp |= FIELD_PREP(V0_ATTCTL_ALPHA_MODE, frm->alpha_bld_mode);
	tmp |= FIELD_PREP(V0_ATTCTL_EN, 1);
	g2d_write(g2d, V0_ATTCTL, tmp);

	tmp = FIELD_PREP(V0_MBSIZE_WIDTH, (frm->sel.r.width == 0 ?
				0 : frm->sel.r.width - 1));
	tmp |= FIELD_PREP(V0_MBSIZE_HEIGHT, (frm->sel.r.height == 0 ? 
				0 : frm->sel.r.height - 1));
	g2d_write(g2d, V0_MBSIZE, tmp);

	/* offset is set to 0, overlay size is set to layer size */
	g2d_write(g2d, V0_SIZE, tmp);
	g2d_write(g2d, V0_COOR, 0);

	if ((fmt_hw_id >= G2D_FORMAT_YUV422UVC_V1U1V0U0)
	      && (fmt_hw_id <= G2D_FORMAT_YUV422_PLANAR)) {
		cw = frm->sel.r.width >> 1;
		cx = frm->sel.r.left >> 1;
		cy = frm->sel.r.top;
	}

	else if ((fmt_hw_id >= G2D_FORMAT_YUV420UVC_V1U1V0U0)
		 && (fmt_hw_id <= G2D_FORMAT_YUV420_PLANAR)) {
		cw = frm->sel.r.width >> 1;
		cx = frm->sel.r.left >> 1;
		cy = frm->sel.r.top >> 1;
	}

	else if ((fmt_hw_id >= G2D_FORMAT_YUV411UVC_V1U1V0U0)
		 && (fmt_hw_id <= G2D_FORMAT_YUV411_PLANAR)) {
		cx = frm->sel.r.left >> 2;
		cy = frm->sel.r.top;
	}

	else {
		cw = 0;
		cx = 0;
		cy = 0;
	}

	fmt2yuvcnt(fmt_hw_id, &ycnt, &ucnt, &vcnt);

	pitch0 = ALIGN(ycnt * frm->v4l2_pix_fmt.width, frm->alignment);
	g2d_write(g2d, V0_PITCH0, pitch0);
	
	pitch1 = ALIGN(ucnt * cw, frm->alignment);
	g2d_write(g2d, V0_PITCH1, pitch1);

	pitch2 = ALIGN(vcnt * cw, frm->alignment);
	g2d_write(g2d, V0_PITCH2, pitch2);
	
	G2D_INFO_MSG("VInPITCH: %d, %d, %d\n",
				pitch0, pitch1, pitch2);
	G2D_INFO_MSG("VInAddrB: 0x%x, 0x%x, 0x%x\n",
			addr[0], addr[1], addr[2]);

	/* address of the rectangle */
	addr0 =
		addr[0] + pitch0 * frm->sel.r.top + ycnt * frm->sel.r.left;
	g2d_write(g2d, V0_LADDR0, addr0 & GENMASK(31, 0));

	addr1 = addr[1] + pitch1 * cy + ucnt * cx;
	g2d_write(g2d, V0_LADDR1, addr1 & GENMASK(31, 0));

	addr2 = addr[2] + pitch2 * cy + vcnt * cx;
	g2d_write(g2d, V0_LADDR2, addr2 & GENMASK(31, 0));

	/* The G2D can support 40-bit bus addresses. Only fill V0_HADDR if we're dealing
	 * with 64-bit DMA addresses
	 */
#ifdef CONFIG_ARCH_DMA_ADDR_T_64BIT
	tmp = FIELD_PREP(V0_HADDR0, addr[0]);
	tmp |= FIELD_PREP(V0_HADDR1, addr[1]);
	tmp |= FIELD_PREP(V0_HADDR2, addr[2]);
	g2d_write(g2d, V0_HADDR, tmp);`
#endif

	G2D_INFO_MSG("VInAddrA: 0x%lx, 0x%lx, 0x%lx\n",
							addr0, addr1, addr2);
}

void g2d_rectfill(struct sunxi_g2d_ctx *ctx, dma_addr_t addr[3])
{
	/* Maybe only reset the mixer ?? */
	// g2d_mixer_reset(ctx->g2d);
	g2d_hw_reset(ctx->g2d); 

	/* prepare the mixer video layer */
	g2d_vlayer_set(ctx->g2d, &ctx->dst, addr, ctx->rectfill_color_alpha);

	/* set the fill color */
	g2d_fc_set(ctx->g2d, 0, ctx->rectfill_color);

	g2d_bldin_set(ctx->g2d, &ctx->dst, 0);
	g2d_bld_cs_set(ctx->g2d, &ctx->dst);

	/* ROP sel ch0 pass */
	g2d_write(ctx->g2d, ROP_CTL, ROP_CTL_BLUE_BYPASS_EN 
				| ROP_CTL_GREEN_BYPASS_EN
				| ROP_CTL_RED_BYPASS_EN 
				| ROP_CTL_ALPHA_BYPASS_EN);
	
	g2d_wb_set(ctx->g2d, &ctx->dst, addr);

	/* start the module */
	G2D_INFO_MSG("Starting the module");
	g2d_mixer_irq_enable(ctx->g2d);
	g2d_set_bits(ctx->g2d, G2D_MIXER_CTL, G2D_MIXER_CTL_START);
}