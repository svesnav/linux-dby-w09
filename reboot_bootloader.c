#define _GNU_SOURCE
#include <linux/reboot.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
    sync();
    return (int)syscall(SYS_reboot, LINUX_REBOOT_MAGIC1,
                        LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2,
                        "bootloader");
}
