# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux kernel device drivers.
#
# 15 Sep 2000, Christoph Hellwig <hch@infradead.org>
# Rewritten to use lists instead of if-statements.
#

obj-y				+= irqchip/
obj-y				+= bus/

obj-$(CONFIG_GENERIC_PHY)	+= phy/

# GPIO must come after pinctrl as gpios may need to mux pins etc
obj-$(CONFIG_PINCTRL)		+= pinctrl/
obj-$(CONFIG_GPIOLIB)		+= gpio/
obj-y				+= pwm/

obj-y				+= pci/

obj-y				+= video/

# IPMI must come before ACPI in order to provide IPMI opregion support
obj-y				+= char/ipmi/

obj-$(CONFIG_ACPI)		+= acpi/
# PnP must come after ACPI since it will eventually need to check if acpi
# was used and do nothing if so
obj-$(CONFIG_PNP)		+= pnp/
obj-y				+= amba/

obj-y				+= clk/
# Many drivers will want to use DMA so this has to be made available
# really early.
obj-$(CONFIG_DMADEVICES)	+= dma/

# SOC specific infrastructure drivers.
obj-y				+= soc/

obj-y				+= virtio/

# regulators early, since some subsystems rely on them to initialize
obj-$(CONFIG_REGULATOR)		+= regulator/

# reset controllers early, since gpu drivers might rely on them to initialize
obj-$(CONFIG_RESET_CONTROLLER)	+= reset/

# tty/ comes before char/ so that the VT console is the boot-time
# default.
obj-y				+= tty/
obj-y				+= char/

# iommu/ comes before gpu as gpu are using iommu controllers
obj-y				+= iommu/

# gpu/ comes after char for AGP vs DRM startup and after iommu
obj-y				+= gpu/

obj-$(CONFIG_CONNECTOR)		+= connector/

# i810fb and intelfb depend on char/agp/
obj-$(CONFIG_FB_I810)           += video/fbdev/i810/
obj-$(CONFIG_FB_INTEL)          += video/fbdev/intelfb/

obj-y				+= base/ block/ mfd/
obj-$(CONFIG_DMA_SHARED_BUFFER) += dma-buf/
obj-y				+= scsi/
obj-y				+= nvme/
obj-$(CONFIG_ATA)		+= ata/
obj-$(CONFIG_SPI)		+= spi/
obj-y				+= net/
obj-$(CONFIG_ATA_OVER_ETH)	+= block/aoe/
obj-$(CONFIG_PARIDE) 		+= block/paride/
obj-$(CONFIG_USB_PHY)		+= usb/
obj-$(CONFIG_USB)		+= usb/
obj-$(CONFIG_USB_SUPPORT)	+= usb/
obj-$(CONFIG_PCI)		+= usb/
obj-$(CONFIG_USB_GADGET)	+= usb/
obj-$(CONFIG_OF)		+= usb/
obj-$(CONFIG_SERIO)		+= input/serio/
obj-$(CONFIG_GAMEPORT)		+= input/gameport/
obj-$(CONFIG_INPUT)		+= input/
obj-$(CONFIG_RTC_LIB)		+= rtc/
obj-y				+= i2c/
obj-$(CONFIG_PPS)		+= pps/
obj-y				+= ptp/
obj-y				+= power/
obj-$(CONFIG_HWMON)		+= hwmon/
obj-$(CONFIG_THERMAL)		+= thermal/
obj-$(CONFIG_WATCHDOG)		+= watchdog/
obj-$(CONFIG_MD)		+= md/
obj-$(CONFIG_CPU_FREQ)		+= cpufreq/
obj-$(CONFIG_CPU_IDLE)		+= cpuidle/
obj-$(CONFIG_NEW_LEDS)		+= leds/
obj-y				+= firmware/
obj-$(CONFIG_CRYPTO)		+= crypto/
ifndef CONFIG_ARCH_USES_GETTIMEOFFSET
obj-y				+= clocksource/
endif
obj-$(CONFIG_HID)		+= hid/
obj-$(CONFIG_OF)		+= of/
obj-y				+= platform/

obj-$(CONFIG_MAILBOX)		+= mailbox/

# Virtualization drivers

obj-$(CONFIG_POWERCAP)		+= powercap/
obj-$(CONFIG_RAS)		+= ras/
obj-$(CONFIG_NVMEM)		+= nvmem/
