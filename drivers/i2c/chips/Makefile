#
# Makefile for miscellaneous I2C chip drivers.
#
# Do not add new drivers to this directory! It is DEPRECATED.
#
# Device drivers are better grouped according to the functionality they
# implement rather than to the bus they are connected to. In particular:
# * Hardware monitoring chip drivers go to drivers/hwmon
# * RTC chip drivers go to drivers/rtc
# * I/O expander drivers go to drivers/gpio
#

## ZTE_MODIFY zhaoyang add I2C sensor driver ##
obj-$(CONFIG_ALS_TSL2583_ZTE)	+= taos_v3.o
obj-$(CONFIG_PROX_TMD26713_ZTE) += TMD26713.o
obj-$(CONFIG_ALS_PROX_TMD27713_ZTE) += TMD27713.o
## ZTE_MODIFY end ##

ifeq ($(CONFIG_I2C_DEBUG_CHIP),y)
EXTRA_CFLAGS += -DDEBUG
endif

