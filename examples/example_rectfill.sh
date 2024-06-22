#!/bin/sh

# With Rectfill operations, because of
# (i)  V4L2 mem2mem's design with 2 queues
# (ii) The hardware DMA'ing directly into the fill rectangle

# Only the capture buffer is really required. And that holds the
# the input image instead of the output buffer, as is usually the
# case

VDEV=/dev/video0

v4l2-ctl --device $VDEV \
    --set-ctrl g2d_rectfill_color=0xff100100 \
    --set-fmt-video=width=800,height=480,pixelformat=XR24 \
    --set-fmt-video-out=width=800,height=480,pixelformat=XR24 \
    --stream-out-user 1 --stream-user 1 --stream-to g2d_output.raw --stream-count=1