`kernel-modules/x8h7`
=====================
#### Build/Deploy/Install
```bash
bitbake x8h7
adb push deploy/ipk/portenta_x8/x8h7_0.1-r1_portenta_x8.ipk /home/fio
```
```bash
adb shell
cd /home/fio
sudo opkg install --force-reinstall --force-depends x8h7_0.1-r1_portenta_x8.ipk
```
or
```bash
adb push ./tmp-lmp_xwayland/sysroots-components/portenta_x8/x8h7/usr/lib/modules/5.10.93-lmp-standard/extra/*.ko /home/fio
adb shell
sudo rmmod x8h7_can
sudo mount -o remount,rw /usr
sudo mv *.ko /lib/modules/5.10.93-lmp-standard/extra/
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
