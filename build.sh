echo "Setting target as 32bit ELF"
export TARGET=i386-elf
echo "Making directories"
mkdir builds
mkdir builds/blocks
mkdir builds/iso
mkdir builds/iso/boot
mkdir builds/iso/boot/grub
echo "Building bootloader"
nasm -f elf32 source/bootloader.asm -o builds/blocks/bootloader.o
echo "Building basic keyboard support"
nasm -f elf32 source/detect-kbinput.asm -o builds/blocks/detectkeys.o
echo "Building OS"
set disassembly-flavor intel
opt/cross/bin/i686-elf-gcc builds/blocks/bootloader.o -ffreestanding -nostdlib builds/blocks/detectkeys.o source/os.c -w -g -m32 -o builds/iso/gems.elf -I"/usr/include" -I"source/THIRDPARTY/lwext4-master/include/" -I"source/THIRDPARTY/linux-old/include/linux" -I"source/THIRDPARTY/linux-old/include/asm" -I"source/THIRDPARTY/xnu/EXTERNAL_HEADERS" -I"source/THIRDPARTY/xnu/libkern" -I"source/THIRDPARTY/xnu/security" -I"source/THIRDPARTY/xnu/bsd"
echo "Creating GRUB config"
echo "set default=0" > builds/iso/boot/grub/grub.cfg
echo "set timeout=60" >> builds/iso/boot/grub/grub.cfg
echo 'menuentry "GEMS" {' >> builds/iso/boot/grub/grub.cfg
echo "  multiboot /gems.elf" >> builds/iso/boot/grub/grub.cfg
echo "  boot" >> builds/iso/boot/grub/grub.cfg
echo "}" >> builds/iso/boot/grub/grub.cfg
echo "Creating ISO"
echo "Note: NOT CREATING ISO. USE CREATE-ISO.SH TO DO THIS."
