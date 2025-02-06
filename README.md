:floppy_disk: `portentax8-x8h7`
===============================
[![Sync Labels status](https://github.com/arduino/portentax8-x8h7/actions/workflows/sync-labels.yml/badge.svg)](https://github.com/arduino/portentax8-x8h7/actions/workflows/sync-labels.yml)

This repository contains the kernel modules for interfacing with various IO devices via a special [firmware](https://github.com/arduino/portentax8-stm32h7-fw) running on the `STM32H747AIIX`/Cortex-M7 core.

This driver compiles against [linux-imx](https://github.com/nxp-imx/linux-imx):**6.1.24**.

#### Build/Deploy/Install
* Edit `conf/auto.conf` (`vim conf/auto.conf`)
```diff
# Extra options that can be changed by the user
-INHERIT += "rm_work"
-INHERIT += "buildstats buildstats-summary"
-INHERIT += "buildhistory"
-BUILDHISTORY_COMMIT = "1"

+#INHERIT += "rm_work"
+#INHERIT += "buildstats buildstats-summary"
+#INHERIT += "buildhistory"
+#BUILDHISTORY_COMMIT = "1"
```
* Clean the global state and rebuild x8h7 module
```bash
bitbake x8h7 -c do_cleansstate && bitbake x8h7
```
* Search for x8h7 kernel module location (`find . -maxdepth 4 -name x8h7`) within build folder (i.e. `build-lmp-base`)
* Rebuild x8h7 kernel modules with `bitbake -c do_compile` (force the compilation or configure with the modifications you just did in WORKDIR) (and withouth `-c do_cleansstate`)
```bash
bitbake -c do_compile -f x8h7 && bitbake -c do_install x8h7
```
* Copy files to Portenta X8
```bash
adb push deploy/ipk/portenta_x8/x8h7_0.1-r1_portenta_x8.ipk /home/fio
```
```bash
adb shell
cd /home/fio
sudo opkg install --force-reinstall --force-depends x8h7_0.1-r1_portenta_x8.ipk
```
or
```bash
adb push ./tmp-lmp_xwayland/sysroots-components/portenta_x8/x8h7/usr/lib/modules/6.1.24-lmp-standard/extra/*.ko /home/fio
adb shell
sudo rmmod x8h7_can
sudo mount -o remount,rw /usr
sudo mv *.ko /lib/modules/6.1.24-lmp-standard/extra/
sudo modprobe x8h7_can
```
#### Unload/Reload
```bash
lsmod | grep x8h7_
cd /usr/arduino/extra
sudo ./unload_modules.sh
sudo ./load_modules_pre.sh
sudo ./load_modules_post.sh
```
