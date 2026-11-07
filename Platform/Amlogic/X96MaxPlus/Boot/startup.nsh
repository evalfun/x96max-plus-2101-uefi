@echo -off

if exist fs0:\X96_LINUX.EFI then
  initrd fs0:\X96_INITRD
  fs0:\X96_LINUX.EFI root=UUID=e92a34bb-3bd5-457c-8816-6200a6e21fbf rootwait rootfstype=ext4 ro console=tty1 console=ttyAML0,115200 earlycon=meson,0xff803000 ignore_loglevel loglevel=8 consoleblank=0 coherent_pool=2M cgroup_enable=memory
endif

if exist fs1:\X96_LINUX.EFI then
  initrd fs1:\X96_INITRD
  fs1:\X96_LINUX.EFI root=UUID=e92a34bb-3bd5-457c-8816-6200a6e21fbf rootwait rootfstype=ext4 ro console=tty1 console=ttyAML0,115200 earlycon=meson,0xff803000 ignore_loglevel loglevel=8 consoleblank=0 coherent_pool=2M cgroup_enable=memory
endif

echo X96 Linux EFI payload not found on fs0: or fs1:
