TARGET_MODULE:=macphy_driver

ifneq ($(KERNELRELEASE),)
	$(TARGET_MODULE)-objs := mac_phy.o open_alliance.o
	obj-m := $(TARGET_MODULE).o 
else
	BUILDSYSTEM_DIR:=/lib/modules/$(shell uname -r)/build
	PWD:=$(shell pwd)


all : 
# run kernel build system to make module
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) modules

clean:
# run kernel build system to cleanup in current directory
	$(MAKE) -C $(BUILDSYSTEM_DIR) M=$(PWD) clean

load:
	dtc -O dtb -o device_tree.dtbo device_tree.dts
	dtoverlay device_tree.dtbo
	insmod ./$(TARGET_MODULE).ko
	
endif

