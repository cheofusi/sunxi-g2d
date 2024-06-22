// SPDX-License-Identifier: GPL-2.0
/*
 * Allwinner G2D - 2D Graphics Accelerator driver
 *
 * Copyright (C) 2016 Allwinner.
 * Copyright (C) 2024 Brandon Cheo Fusi <fusibrandon13@gmail.com>
 *
 */

#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/reset.h>
#include <linux/fs.h>

#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>

#include "sunxi_g2d.h"
#include "sunxi_g2d_hw.h"

#define V4L2_CID_CUSTOM_BASE				(V4L2_CID_USER_BASE + 0x1000)
#define V4L2_CID_SUNXI_G2D_OP_SELECT		(V4L2_CID_CUSTOM_BASE + 1)
#define V4L2_CID_SUNXI_G2D_IN_ALPHA_MODE 	(V4L2_CID_CUSTOM_BASE + 2)
#define V4L2_CID_SUNXI_G2D_IN_ALIGNMENT		(V4L2_CID_CUSTOM_BASE + 3)
#define V4L2_CID_SUNXI_G2D_OUT_ALPHA_MODE	(V4L2_CID_CUSTOM_BASE + 4)
#define V4L2_CID_SUNXI_G2D_OUT_ALIGNMENT	(V4L2_CID_CUSTOM_BASE + 5)
/* Rectfill specific ctrls */
#define V4L2_CID_SUNXI_G2D_RECTFILL_COLOR		(V4L2_CID_CUSTOM_BASE + 6)
#define V4L2_CID_SUNXI_G2D_RECTFILL_COLOR_ALPHA	(V4L2_CID_CUSTOM_BASE + 7)

/* 
 * TODO: Add all supported formats. For now only include formats that
 * are supported by the G2D engine both as input and as output
 */
static struct g2d_fmt g2d_supported_fmts[] = {
	{
		.fourcc	= V4L2_PIX_FMT_XBGR32,
		.depth	= 32,
		.hw_id  = G2D_FORMAT_BGRX8888,
	},
};

#define NUM_SUPPORTED_FMTS ARRAY_SIZE(g2d_supported_fmts)

#define DEF_IMG_W 800
#define DEF_IMG_H 480
#define DEF_PIX_FMT V4L2_PIX_FMT_XBGR32
#define DEF_RECTFILL_COLOR 0xffff0100
#define DEF_RECTFILL_COLOR_ALPHA 0xff

#define MIN_SRC_BUFS 1
#define MIN_DST_BUFS 1

struct g2d_fmt *find_fmt(struct v4l2_pix_format *v4l2_pix_fmt)
{
	unsigned int i;
	for (i = 0; i < NUM_SUPPORTED_FMTS; i++) {
		if (g2d_supported_fmts[i].fourcc == v4l2_pix_fmt->pixelformat)
			return &g2d_supported_fmts[i];
	}

	return NULL;
}

static inline struct sunxi_g2d_ctx *g2d_file2ctx(struct file *file)
{
	return container_of(file->private_data, struct sunxi_g2d_ctx, fh);
}

/** TODO:
 * - Add support for V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE and
 *   V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
*/
static struct g2d_frame *get_frame(struct sunxi_g2d_ctx *ctx,
				   enum v4l2_buf_type type)
{
	switch (type) {
	case V4L2_BUF_TYPE_VIDEO_OUTPUT:
		return &ctx->src;
	case V4L2_BUF_TYPE_VIDEO_CAPTURE:
		return &ctx->dst;
	default:
		return ERR_PTR(-EINVAL);
	}
}

/* Controls */

static int g2d_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct sunxi_g2d_ctx *ctx = container_of(ctrl->handler,
					      struct sunxi_g2d_ctx,
					      ctrl_handler);

	switch (ctrl->id) {
	case V4L2_CID_SUNXI_G2D_OP_SELECT:
		ctx->chosen_g2d_op = ctrl->val;
		/* TODO: activate selected control and deactivate other controls */				
		break;
	case V4L2_CID_SUNXI_G2D_IN_ALPHA_MODE:
		ctx->src.alpha_bld_mode = ctrl->val;
		break;
	case V4L2_CID_SUNXI_G2D_OUT_ALPHA_MODE:
		ctx->dst.alpha_bld_mode = ctrl->val;
		break;
	case V4L2_CID_SUNXI_G2D_IN_ALIGNMENT:
		ctx->src.alignment = ctrl->val;
		break;
	case V4L2_CID_SUNXI_G2D_OUT_ALIGNMENT:
		ctx->dst.alignment = ctrl->val;
		break;
	case V4L2_CID_SUNXI_G2D_RECTFILL_COLOR:
		ctx->rectfill_color = ctrl->p_new.p_u32[0];
		break;
	case V4L2_CID_SUNXI_G2D_RECTFILL_COLOR_ALPHA:
		ctx->rectfill_color_alpha = ctrl->p_new.p_u8[0];
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int g2d_try_ctrl(struct v4l2_ctrl *ctrl)
{
	if (ctrl->id == V4L2_CID_SUNXI_G2D_IN_ALIGNMENT || 
		ctrl->id == V4L2_CID_SUNXI_G2D_OUT_ALIGNMENT) {
		if ((ctrl->val) & (ctrl->val - 1)) /* must be power of 2 */
			return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops g2d_ctrl_ops = {
	.s_ctrl = g2d_s_ctrl,
	.try_ctrl = g2d_try_ctrl,
};

static const char * const g2d_op_menu[] = {
	"Rectfill",
	"Bitblit",
	NULL,
};

static const char * const g2d_alpha_mode_menu[] = {
	"Pixel alpha",
	"Plane alpha",
	"Multi-Plane alpha",
	NULL,
};

static const struct v4l2_ctrl_config g2d_ctrls[] = {
	{
		.ops = &g2d_ctrl_ops,
		.id = V4L2_CID_SUNXI_G2D_OP_SELECT,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "G2D Operation",
		.min = 0,
		.max = 1,
		.def = 0,
		.qmenu = g2d_op_menu,
	},
	{
		.ops = &g2d_ctrl_ops,
		.id = V4L2_CID_SUNXI_G2D_IN_ALPHA_MODE,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "G2D Input Alpha Blend Mode",
		.min = 0,
		.max = 2,
		.def = 0,
		.qmenu = g2d_alpha_mode_menu,
	},
		{
		.ops = &g2d_ctrl_ops,
		.id = V4L2_CID_SUNXI_G2D_OUT_ALPHA_MODE,
		.type = V4L2_CTRL_TYPE_MENU,
		.name = "G2D Output Alpha Blend Mode",
		.min = 0,
		.max = 2,
		.def = 0,
		.qmenu = g2d_alpha_mode_menu,
	},
	{
		.ops = &g2d_ctrl_ops,
		.id = V4L2_CID_SUNXI_G2D_IN_ALIGNMENT,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "G2D Input Alignment",
		.min = 1,
		.max = 1 << 6, /* Arbitrary */
		.def = 1,
		.step = 1, /* not sufficient (must be power of 2), hence try_ctrl */
	},
	{
		.ops = &g2d_ctrl_ops,
		.id = V4L2_CID_SUNXI_G2D_OUT_ALIGNMENT,
		.type = V4L2_CTRL_TYPE_INTEGER,
		.name = "G2D Output Alignment",
		.min = 1,
		.max = 1 << 6, /* Arbitrary */
		.def = 1,
		.step = 1, /* not sufficient (must be power of 2), hence try_ctrl */
	},
	{
		.ops = &g2d_ctrl_ops,
		.id = V4L2_CID_SUNXI_G2D_RECTFILL_COLOR,
		.type = V4L2_CTRL_TYPE_U32,
		.name = "G2D Rectfill Color",
		.min = 0,
		.max = 0xffffffff,
		.def = DEF_RECTFILL_COLOR,
		.step = 1,
		.dims = { 1 },
	},
	{
		.ops = &g2d_ctrl_ops,
		.id = V4L2_CID_SUNXI_G2D_RECTFILL_COLOR_ALPHA,
		.type = V4L2_CTRL_TYPE_U8,
		.name = "G2D Rectfill Color Alpha",
		.min = 0,
		.max = 0xff,
		.def = DEF_RECTFILL_COLOR_ALPHA,
		.step = 1,
		.dims = { 1 },
	},
};

#define NUM_CTRLS ARRAY_SIZE(g2d_ctrls)

static int g2d_job_ready(void *priv)
{
	struct sunxi_g2d_ctx *ctx = priv;

	switch (ctx->chosen_g2d_op) {
	case G2D_RECTFILL:
		/*
		 * In reality Rectfill requires no source buffer and only a single
		 * destination buffer and its selection to use as the fill rectangle
		 * The hardware DMAs directly into the area specified by the selection
		 */
		
		if ((v4l2_m2m_num_dst_bufs_ready(ctx->fh.m2m_ctx)) < 1)
			return 0;
		break;
	default:
		break;
	}

	return 1;
} 

static void g2d_device_run(void *priv)
{
	struct sunxi_g2d_ctx *ctx = priv;
	struct sunxi_g2d *g2d = ctx->g2d;
	struct vb2_v4l2_buffer *src, *dst;
	dma_addr_t src_addr, dst_addr, addr[3];

	dev_info(g2d->dev, "In g2d_device_run");

	src = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	v4l2_m2m_buf_copy_metadata(src, dst, true);

	src_addr = vb2_dma_contig_plane_dma_addr(&src->vb2_buf, 0);
	dst_addr = vb2_dma_contig_plane_dma_addr(&dst->vb2_buf, 0);

	switch (ctx->chosen_g2d_op) {
	case G2D_RECTFILL:
		/*
		* The rectfill op only requires a destination addr for the
		* result, since it works 'in place'
		*/
		addr[0] = dst_addr;
		addr[1] = 0;
		addr[2] = 0;
		g2d_rectfill(ctx, addr);

		break;

	default:
		break; /* TODO: act like default op was set */
	}
}

irqreturn_t g2d_irq(int irq, void *data)
{
	pr_info("-------- Got g2d interrupt!!");
	struct sunxi_g2d *g2d = data;
	struct sunxi_g2d_ctx *ctx;
	struct vb2_v4l2_buffer *src, *dst;

	ctx = v4l2_m2m_get_curr_priv(g2d->m2m_dev);
	if (!ctx) {
		v4l2_err(&g2d->v4l2_dev,
			 "Instance released before the end of transaction\n");
		return IRQ_NONE;
	}

	if (!g2d_mixer_irq_query(g2d))
		return IRQ_NONE;

	g2d_mixer_reset(g2d);

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	v4l2_m2m_buf_done(src, VB2_BUF_STATE_DONE);
	v4l2_m2m_buf_done(dst, VB2_BUF_STATE_DONE);
	v4l2_m2m_job_finish(g2d->m2m_dev, ctx->fh.m2m_ctx);

	return IRQ_HANDLED;
}

/* v42l_ioctl_ops */

static int g2d_querycap(struct file *file, void *priv,
				struct v4l2_capability *cap)
{
	strscpy(cap->driver, G2D_NAME, sizeof(cap->driver));
	strscpy(cap->card, G2D_NAME, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info),
		 "platform:%s", G2D_NAME);

	return 0;
}

static int g2d_enum_fmt(struct file *file, void *priv,
				struct v4l2_fmtdesc *f)
{
	if (f->index < NUM_SUPPORTED_FMTS) {
		f->pixelformat = g2d_supported_fmts[f->index].fourcc;

		return 0;
	}

	return -EINVAL;
}

static int g2d_g_fmt(struct file *file, void *priv,
				     struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = g2d_file2ctx(file);
	struct g2d_frame *frm;

	frm = get_frame(ctx, f->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);

	f->fmt.pix = frm->v4l2_pix_fmt;

	return 0;
}

static int g2d_try_fmt(struct file *file, void *priv,
				       struct v4l2_format *f)
{
	struct g2d_fmt *fmt;
	uint32_t width = f->fmt.pix.width;
	uint32_t height = f->fmt.pix.height;

	fmt = find_fmt(&f->fmt.pix);
	if (!fmt)
		f->fmt.pix.pixelformat = g2d_supported_fmts[0].fourcc;

	width = clamp(width, G2D_MIN_WIDTH, G2D_MAX_WIDTH);
	height = clamp(height, G2D_MIN_HEIGHT, G2D_MAX_HEIGHT);

	f->fmt.pix.width = width;
	f->fmt.pix.height = height;
	f->fmt.pix.bytesperline = (f->fmt.pix.width * fmt->depth) >> 3;
	f->fmt.pix.sizeimage = f->fmt.pix.height * f->fmt.pix.bytesperline;

	return 0;
}

static int g2d_s_fmt(struct file *file, void *priv, struct v4l2_format *f)
{
	struct sunxi_g2d_ctx *ctx = g2d_file2ctx(file);
	struct vb2_queue *vq;
	struct g2d_frame *frm;
	int ret;

	ret = g2d_try_fmt(file, priv, f);
	if (ret)
		return ret;

	frm = get_frame(ctx, f->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_busy(vq))
		return -EBUSY;

	frm->v4l2_pix_fmt = f->fmt.pix;
	frm->premult_alpha = (f->fmt.pix.flags & V4L2_PIX_FMT_FLAG_PREMUL_ALPHA);

	return 0;
}

static int g2d_g_selection(struct file *file, void *priv,
			      struct v4l2_selection *sel)
{
	struct sunxi_g2d_ctx *ctx = g2d_file2ctx(file);
	struct g2d_frame *frm;

	frm = get_frame(ctx, sel->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
		if (!V4L2_TYPE_IS_OUTPUT(sel->type))
			return -EINVAL;
		break;
	case V4L2_SEL_TGT_COMPOSE:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		if (!V4L2_TYPE_IS_CAPTURE(sel->type))
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_COMPOSE:
		sel->r.left = frm->sel.r.left;
		sel->r.top = frm->sel.r.top;
		sel->r.width = frm->sel.r.width;
		sel->r.height = frm->sel.r.height;
		break;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_COMPOSE_DEFAULT:
	case V4L2_SEL_TGT_COMPOSE_BOUNDS:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = frm->v4l2_pix_fmt.width;
		sel->r.height = frm->v4l2_pix_fmt.height;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static int g2d_try_selection(struct file *file, void *priv,
				const struct v4l2_selection *sel)
{
	struct sunxi_g2d_ctx *ctx = g2d_file2ctx(file);
	struct g2d_frame *frm;

	frm = get_frame(ctx, sel->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);

	if (V4L2_TYPE_IS_CAPTURE(sel->type)) {
		if (sel->target != V4L2_SEL_TGT_COMPOSE)
			return -EINVAL;
	} else if (V4L2_TYPE_IS_OUTPUT(sel->type)) {
		if (sel->target != V4L2_SEL_TGT_CROP)
			return -EINVAL;
	}

	if (sel->r.top < 0 || sel->r.left < 0) {
		v4l2_err(&ctx->g2d->v4l2_dev,
			"doesn't support negative values for top & left\n");
		return -EINVAL;
	}

	if ((sel->r.left > frm->v4l2_pix_fmt.width - 1) ||
		(sel->r.top > frm->v4l2_pix_fmt.height - 1))
		return -EINVAL;
	
	if ((sel->r.left + sel->r.width) > (frm->v4l2_pix_fmt.width - 1))
		return -EINVAL;

	if ((sel->r.top + sel->r.height) > (frm->v4l2_pix_fmt.height - 1))
		return -EINVAL;

	return 0;
}

static int g2d_s_selection(struct file *file, void *priv,
			      struct v4l2_selection *sel)
{
	struct sunxi_g2d_ctx *ctx = g2d_file2ctx(file);
	struct g2d_frame *frm;
	int ret;

	ret = g2d_try_selection(file, priv, sel);
	if (ret)
		return ret;

	frm = get_frame(ctx, sel->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);

	frm->sel.r.width = sel->r.width;
	frm->sel.r.height	= sel->r.height;
	frm->sel.r.left	= sel->r.left;
	frm->sel.r.top	= sel->r.top;

	return 0;
}

static const struct v4l2_ioctl_ops g2d_ioctl_ops = {
	.vidioc_querycap		= g2d_querycap,

	.vidioc_enum_fmt_vid_cap	= g2d_enum_fmt,
	.vidioc_g_fmt_vid_cap		= g2d_g_fmt,
	.vidioc_try_fmt_vid_cap		= g2d_try_fmt,
	.vidioc_s_fmt_vid_cap		= g2d_s_fmt,

	.vidioc_enum_fmt_vid_out	= g2d_enum_fmt,
	.vidioc_g_fmt_vid_out		= g2d_g_fmt,
	.vidioc_try_fmt_vid_out		= g2d_try_fmt,
	.vidioc_s_fmt_vid_out		= g2d_s_fmt,

	.vidioc_reqbufs			= v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf		= v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf			= v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf			= v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf		= v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs		= v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf			= v4l2_m2m_ioctl_expbuf,

	.vidioc_streamon		= v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff		= v4l2_m2m_ioctl_streamoff,

	.vidioc_g_selection		= g2d_g_selection,
	.vidioc_s_selection		= g2d_s_selection,
};

/* vb2_ops */

static int g2d_queue_setup(struct vb2_queue *vq, unsigned int *nbuffers,
				   unsigned int *nplanes, unsigned int sizes[],
				   struct device *alloc_devs[])
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct g2d_frame *frm;

	frm = get_frame(ctx, vq->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);

	if (*nplanes) {
		if (sizes[0] < frm->v4l2_pix_fmt.sizeimage)
			return -EINVAL;
	} else {
		sizes[0] = frm->v4l2_pix_fmt.sizeimage;
		*nplanes = 1;
	}

	return 0;
}

static int g2d_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_queue *vq = vb->vb2_queue;
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct g2d_frame *frm;

	frm = get_frame(ctx, vq->type);
	if (IS_ERR(frm))
		return PTR_ERR(frm);
	if (vb2_plane_size(vb, 0) < frm->v4l2_pix_fmt.sizeimage)
		return -EINVAL;

	vb2_set_plane_payload(vb, 0, frm->v4l2_pix_fmt.sizeimage);

	return 0;
}

static void g2d_buf_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static void g2d_queue_cleanup(struct vb2_queue *vq, uint32_t state)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct vb2_v4l2_buffer *vbuf;

	do {
		if (V4L2_TYPE_IS_OUTPUT(vq->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

		if (vbuf)
			v4l2_m2m_buf_done(vbuf, state);
	} while (vbuf);
}

static int g2d_start_streaming(struct vb2_queue *vq, unsigned int count)
{
	struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vq);
	struct device *dev = ctx->g2d->dev;
	int ret;

	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		ret = pm_runtime_resume_and_get(dev);
		if (ret < 0) {
			dev_err(dev, "Failed to enable module\n");
			g2d_queue_cleanup(vq, VB2_BUF_STATE_QUEUED);
		}

		return ret;
	}

	return 0;
}

static void g2d_stop_streaming(struct vb2_queue *vq)
{
	if (V4L2_TYPE_IS_OUTPUT(vq->type)) {
		struct sunxi_g2d_ctx *ctx = vb2_get_drv_priv(vq);

		pm_runtime_put(ctx->g2d->dev);
	}

	g2d_queue_cleanup(vq, VB2_BUF_STATE_ERROR);
}

static const struct vb2_ops g2d_qops = {
	.queue_setup		= g2d_queue_setup,
	.buf_prepare		= g2d_buf_prepare,
	/* .buf_init			= g2d_buf_init, TODO: setup sg lists for USERPTR buffers */
	.buf_queue		= g2d_buf_queue,
	.start_streaming	= g2d_start_streaming,
	.stop_streaming		= g2d_stop_streaming,
	.wait_prepare		= vb2_ops_wait_prepare,
	.wait_finish		= vb2_ops_wait_finish,
};

static int g2d_queue_init(void *priv, struct vb2_queue *src_vq,
				  struct vb2_queue *dst_vq)
{
	struct sunxi_g2d_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
	src_vq->io_modes = VB2_MMAP | VB2_USERPTR |VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->min_buffers_needed = 1;
	src_vq->ops = &g2d_qops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->g2d->dev_mutex;
	src_vq->dev = ctx->g2d->dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	dst_vq->io_modes = VB2_MMAP | VB2_USERPTR | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->min_buffers_needed = 1;
	dst_vq->ops = &g2d_qops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->g2d->dev_mutex;
	dst_vq->dev = ctx->g2d->dev;

	ret = vb2_queue_init(dst_vq);
	if (ret)
		return ret;

	return 0;
}

static int g2d_setup_ctrls(struct sunxi_g2d_ctx *ctx)
{
	struct sunxi_g2d *g2d = ctx->g2d;
	struct v4l2_ctrl *ctrl;
	int i;

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, NUM_CTRLS);

	ctx->g2d->v4l2_dev.ctrl_handler = &ctx->ctrl_handler;

	for (i = 0; i < NUM_CTRLS; ++i) {
		ctrl = v4l2_ctrl_new_custom(&ctx->ctrl_handler, &g2d_ctrls[i], NULL);
	}

	if (ctx->ctrl_handler.error) {
		int err = ctx->ctrl_handler.error;
		v4l2_err(&g2d->v4l2_dev, "g2d_setup_ctrls failed\n");
		v4l2_ctrl_handler_free(&ctx->ctrl_handler);
		return err;
	}

	return v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
}

static int g2d_open(struct file *file)
{
	struct sunxi_g2d *g2d = video_drvdata(file);
	struct sunxi_g2d_ctx *ctx = NULL;
	int ret;

	if (mutex_lock_interruptible(&g2d->dev_mutex))
		return -ERESTARTSYS;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		mutex_unlock(&g2d->dev_mutex);
		return -ENOMEM;
	}

	/* default output format */
	ctx->src.v4l2_pix_fmt.pixelformat = V4L2_PIX_FMT_XBGR32;
	ctx->src.v4l2_pix_fmt.field = V4L2_FIELD_NONE;
	ctx->src.v4l2_pix_fmt.width = DEF_IMG_W;
	ctx->src.v4l2_pix_fmt.height = DEF_IMG_H;
	ctx->src.premult_alpha = true;
	ctx->src.alpha_bld_mode = G2D_PIXEL_ALPHA;
	ctx->src.alignment = 1;
	ctx->src.sel.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;

	/* default capture format */
	ctx->dst = ctx->src;
	ctx->src.sel.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	/** TODO: Remove this!!! Only used for testing rectfill operation
	 *  without usersapce 
	 */
	ctx->dst.sel.r.left = 200;
	ctx->dst.sel.r.top = 120;
	ctx->dst.sel.r.width = 400;
	ctx->dst.sel.r.height = 240;

	v4l2_fh_init(&ctx->fh, video_devdata(file));
	file->private_data = &ctx->fh;
	ctx->g2d = g2d;

	ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(g2d->m2m_dev, ctx,
					    &g2d_queue_init);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		goto err_free;
	}

	v4l2_fh_add(&ctx->fh);

	ret = g2d_setup_ctrls(ctx);
	if (ret)
		goto err_free;

	ctx->fh.ctrl_handler = &ctx->ctrl_handler;

	mutex_unlock(&g2d->dev_mutex);

	return 0;

err_free:
	kfree(ctx);
	mutex_unlock(&g2d->dev_mutex);

	return ret;
}

static int g2d_release(struct file *file)
{
	struct sunxi_g2d *g2d = video_drvdata(file);
	struct sunxi_g2d_ctx *ctx = container_of(file->private_data,
						   struct sunxi_g2d_ctx, fh);

	mutex_lock(&g2d->dev_mutex);

	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);

	kfree(ctx); // ??

	mutex_unlock(&g2d->dev_mutex);

	return 0;
}

static const struct v4l2_file_operations g2d_fops = {
	.owner		= THIS_MODULE,
	.open		= g2d_open,
	.release	= g2d_release,
	.poll		= v4l2_m2m_fop_poll,
	.unlocked_ioctl	= video_ioctl2,
	.mmap		= v4l2_m2m_fop_mmap,
};

static const struct video_device g2d_videodev = {
	.name		= G2D_NAME,
	.vfl_dir	= VFL_DIR_M2M,
	.fops		= &g2d_fops,
	.ioctl_ops	= &g2d_ioctl_ops,
	.minor		= -1,
	.release	= video_device_release_empty,
	.device_caps	= V4L2_CAP_VIDEO_M2M | V4L2_CAP_STREAMING,
};

static const struct v4l2_m2m_ops g2d_m2m_ops = {
	.device_run	= g2d_device_run,
	.job_ready = g2d_job_ready,
};

static int g2d_probe(struct platform_device *pdev)
{
	struct sunxi_g2d *g2d;
	struct video_device *vfd;
	int irq, ret;

	g2d = devm_kzalloc(&pdev->dev, sizeof(*g2d), GFP_KERNEL);
	if (!g2d)
		return -ENOMEM;
	
	g2d->vfd = g2d_videodev;
	g2d->dev = &pdev->dev;

	irq = platform_get_irq(pdev, 0);
	if (irq < 0)
		return irq;

	ret = devm_request_irq(g2d->dev, irq, g2d_irq,
			       0, dev_name(g2d->dev), g2d);
	if (ret) {
		dev_err(g2d->dev, "Failed to request IRQ\n");
		return ret;
	}

	g2d->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(g2d->base))
		return PTR_ERR(g2d->base);
	// base_addr = g2d->base; // TODO: remove

	g2d->bus_clk = devm_clk_get(g2d->dev, "bus");
	if (IS_ERR(g2d->bus_clk)) {
		dev_err(g2d->dev, "Failed to get bus clock\n");
		return PTR_ERR(g2d->bus_clk );
	}

	g2d->mod_clk = devm_clk_get(g2d->dev, "mod");
	if (IS_ERR(g2d->mod_clk)) {
		dev_err(g2d->dev, "Failed to get mod clock\n");
		return PTR_ERR(g2d->mod_clk);
	}

	g2d->ram_clk = devm_clk_get(g2d->dev, "ram");
	if (IS_ERR(g2d->ram_clk)) {
		dev_err(g2d->dev, "Failed to get ram clock\n");
		return PTR_ERR(g2d->ram_clk);
	}

	g2d->rstc = devm_reset_control_get(g2d->dev, NULL);
	if (IS_ERR(g2d->rstc)) {
		dev_err(g2d->dev, "No reset controller specified\n");
		return PTR_ERR(g2d->rstc);
	}

	mutex_init(&g2d->dev_mutex);

	ret = v4l2_device_register(g2d->dev, &g2d->v4l2_dev);
	if (ret) {
		dev_err(g2d->dev, "Failed to register V4L2 device\n");
		return ret;
	}

	vfd = &g2d->vfd;
	vfd->lock = &g2d->dev_mutex;
	vfd->v4l2_dev = &g2d->v4l2_dev;

	snprintf(vfd->name, sizeof(vfd->name), "%s",
		 g2d_videodev.name);
	video_set_drvdata(vfd, g2d);

	ret = video_register_device(vfd, VFL_TYPE_VIDEO, -1);
	if (ret) {
		v4l2_err(vfd->v4l2_dev, "Failed to register video device\n");
		goto err_v4l2;
	}

	v4l2_info(vfd->v4l2_dev,
		  "Device registered as /dev/video%d\n", vfd->num);

	g2d->m2m_dev = v4l2_m2m_init(&g2d_m2m_ops);
	if (IS_ERR(g2d->m2m_dev)) {
		v4l2_err(&g2d->v4l2_dev,
			 "Failed to initialize V4L2 M2M device\n");
		ret = PTR_ERR(g2d->m2m_dev);

		goto err_video;
	}

	g2d->supported_fmts = g2d_supported_fmts; 

	platform_set_drvdata(pdev, g2d);

	pm_runtime_enable(g2d->dev);

	return 0;

err_video:
	video_unregister_device(&g2d->vfd);
err_v4l2:
	v4l2_device_unregister(&g2d->v4l2_dev);

	return ret;
}

static int g2d_remove(struct platform_device *pdev)
{
	struct sunxi_g2d *g2d = platform_get_drvdata(pdev);

	v4l2_m2m_release(g2d->m2m_dev);
	video_unregister_device(&g2d->vfd);
	v4l2_device_unregister(&g2d->v4l2_dev);

	pm_runtime_force_suspend(&pdev->dev);

	return 0;
}

static int sunxi_g2d_runtime_resume(struct device *device)
{
	struct sunxi_g2d *g2d = dev_get_drvdata(device);
	int ret;

	ret = reset_control_deassert(g2d->rstc);
	if (ret) {
		dev_err(g2d->dev, "Failed to deassert reset\n");
		return ret;
	}

	/*
	 * This 300MHz value is gotten from the BSP and is absolutely necessary
     * for the g2d block to not hang (i.e never issuing an interrupt after
     * completing an operation).
	 * TODO: try other closer rates to pin down [min, max] of the 
     * functional range. 
	 */
	ret = clk_set_rate_exclusive(g2d->mod_clk, 300000000);
	if (ret) {
		dev_err(g2d->dev, "Failed to set exclusive mod clock rate\n");
		goto err_reset_assert;
	}

	ret = clk_prepare_enable(g2d->bus_clk);
	if (ret) {
		dev_err(g2d->dev, "Failed to enable bus clock\n");
		goto err_exclusive_rate;
	}

	ret = clk_prepare_enable(g2d->mod_clk);
	if (ret) {
		dev_err(g2d->dev, "Failed to enable mod clock\n");

		goto err_put_bus_clk;
	}

	ret = clk_prepare_enable(g2d->ram_clk);
	if (ret) {
		dev_err(g2d->dev, "Failed to enable ram clock\n");
		goto err_put_mod_clk;
	}

	g2d_hw_open(g2d);

	return 0;

err_put_mod_clk:
	clk_disable_unprepare(g2d->mod_clk);
err_put_bus_clk:
	clk_disable_unprepare(g2d->bus_clk);
err_exclusive_rate:
	clk_rate_exclusive_put(g2d->mod_clk);
err_reset_assert:
	reset_control_assert(g2d->rstc);

	return ret;
}

static int sunxi_g2d_runtime_suspend(struct device *device)
{
	struct sunxi_g2d *g2d = dev_get_drvdata(device);

	g2d_hw_close(g2d);

	clk_disable_unprepare(g2d->ram_clk);
	clk_disable_unprepare(g2d->mod_clk);
	clk_disable_unprepare(g2d->bus_clk);
	clk_rate_exclusive_put(g2d->mod_clk);

	reset_control_assert(g2d->rstc);

	return 0;
}

static const struct of_device_id sunxi_g2d_match[] = {
	{ .compatible = "allwinner,sunxi-g2d" },
	{},
};
MODULE_DEVICE_TABLE(of, sunxi_g2d_match);

static const struct dev_pm_ops sunxi_g2d_pm_ops = {
	.runtime_resume		= sunxi_g2d_runtime_resume,
	.runtime_suspend	= sunxi_g2d_runtime_suspend,
};

struct platform_driver g2d_driver = {
	.probe		= g2d_probe,
	.remove		= g2d_remove,
	.driver		= {
		.name	= "sunxi-g2d",
		.owner	= THIS_MODULE,
		.of_match_table = sunxi_g2d_match,
		.pm		= &sunxi_g2d_pm_ops,
	},
};
module_platform_driver(g2d_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Brandon Cheo Fusi <fusibrandon13@gmail.com>");
MODULE_DESCRIPTION("Allwinner 2D Graphics Accelerator driver");