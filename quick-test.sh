if grub-file --is-x86-multiboot builds/iso/gems.elf; then
  echo multiboot confirmed
else
  echo the file is not multiboot
fi
