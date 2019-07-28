HOW TO BUILD KERNEL FOR GT-S7568

1. How to Build
 - get Toolchain
 download and install arm-eabi-4.4.3 toolchain for ARM EABI.
 Extract kernel source and move into the top directory.

 $ export CROSS_COMPILE=../arm-eabi-4.4.3/bin/arm-eabi-
 $ export ARCH=arm
 $ export USE_SEC_FIPS_MODE=true
 $ make kyletd-vlx_defconfig
 $ make
 $ make zImage

2. Output files
 - Kernel : Kernel/arch/arm/boot/zImage
 - module : Kernel/drivers/*/*.ko
 
3. How to make .tar binary for downloading into target.
 - change current directory to Kernel/arch/arm/boot
 - type following command
 $ tar cvf GT-S7568_Kernel.tar zImage











