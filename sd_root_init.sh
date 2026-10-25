#!/bin/busybox sh
# Linux 7.0 initramfs for the DBY-W09: boots the Ubuntu copy on the microSD
# card (ext4 label LINUX70ROOT).  Never touches userdata.  Goes to fastboot
# when the card or its root is missing, and once when the card carries
# /var/lib/linux70/fastboot-next-boot (opt-in, for tests).
PATH=/bin
export PATH
umask 022

/bin/busybox mkdir -p /bin /dev /proc /sys /cache /newroot /tmp
/bin/busybox --install -s /bin
mount -t devtmpfs devtmpfs /dev 2>/dev/null
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t configfs configfs /sys/kernel/config 2>/dev/null

logfile=/tmp/linux70-initramfs.log
log() {
	line="dby-init[$(cut -d' ' -f1 /proc/uptime)]: $*"
	echo "$line" > /dev/kmsg 2>/dev/null
	echo "$line" >> "$logfile"
}

to_fastboot() {
	log "to fastboot: $*"
	sync
	/reboot-bootloader
	sleep 5
	echo b > /proc/sysrq-trigger
}

log "kernel $(uname -r)"

# USB ACM gadget only: the card's serial-getty@ttyGS0 gives the root shell.
# (A shell loop started here outlives switch_root, which deletes busybox's
# applet links, and floods the console with "setsid: not found".)
g=/sys/kernel/config/usb_gadget/dby
if [ -d /sys/kernel/config/usb_gadget ]; then
	mkdir -p $g/strings/0x409 $g/configs/c.1/strings/0x409 $g/functions/acm.usb0
	echo 0x1d6b > $g/idVendor
	echo 0x0104 > $g/idProduct
	echo DBYW09LINUX70 > $g/strings/0x409/serialnumber
	echo Huawei > $g/strings/0x409/manufacturer
	echo "DBY-W09 Linux 7.0" > $g/strings/0x409/product
	echo "ACM shell" > $g/configs/c.1/strings/0x409/configuration
	ln -s $g/functions/acm.usb0 $g/configs/c.1/
	for i in $(seq 1 20); do
		udc=$(ls /sys/class/udc 2>/dev/null | head -n 1)
		[ -n "$udc" ] && break
		sleep 1
	done
	[ -n "$udc" ] && echo "$udc" > $g/UDC
fi

root=
for i in $(seq 1 30); do
	root=$(findfs LABEL=LINUX70ROOT 2>/dev/null) && [ -b "$root" ] && break
	root=
	sleep 1
done
[ -n "$root" ] || to_fastboot "no LINUX70ROOT partition"
mount -t ext4 -o rw,noatime "$root" /newroot || to_fastboot "mount $root failed"
log "root $root mounted"

request=/newroot/var/lib/linux70/fastboot-next-boot
if [ -e "$request" ]; then
	rm -f "$request"
	umount /newroot
	to_fastboot "requested by $request"
fi
rm -f /newroot/var/lib/linux70/booted	# the old one-boot marker
mkdir -p /newroot/var/lib/linux70 /newroot/var/log
cat "$logfile" > /newroot/var/log/linux70-initramfs.log

[ -x /newroot/sbin/init ] || to_fastboot "no /sbin/init on $root"
umount /sys/kernel/config 2>/dev/null
mount --move /dev /newroot/dev
mount --move /proc /newroot/proc
mount --move /sys /newroot/sys
exec switch_root /newroot /sbin/init
