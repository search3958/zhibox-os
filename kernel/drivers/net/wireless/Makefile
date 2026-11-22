# SPDX-License-Identifier: GPL-2.0
#
# Makefile for the Linux Wireless network device drivers.
#

obj-$(CONFIG_WLAN_VENDOR_ATH) += ath/
obj-$(CONFIG_WLAN_VENDOR_INTEL) += intel/

# 16-bit wireless PCMCIA client drivers
obj-$(CONFIG_PCMCIA_RAYCS)	+= ray_cs.o
obj-$(CONFIG_PCMCIA_WL3501)	+= wl3501_cs.o

obj-$(CONFIG_USB_NET_RNDIS_WLAN)	+= rndis_wlan.o

obj-$(CONFIG_MAC80211_HWSIM)	+= mac80211_hwsim.o

obj-$(CONFIG_VIRT_WIFI)	+= virt_wifi.o
