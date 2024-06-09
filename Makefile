KPATH ?= ../linux
KMAKE := $(MAKE) -C $(KPATH) M=$(CURDIR)/module

modules:
	$(KMAKE) CONFIG_VIDEO_SUNXI_G2D=m modules

clean:
	$(KMAKE) clean