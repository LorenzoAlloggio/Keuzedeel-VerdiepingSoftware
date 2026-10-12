#include <string.h>
#include "dynamic_libs/os_functions.h"
#include "utils/logger.h"

#define IOCTL_FSA_OPEN      0x40
#define IOCTL_FSA_CLOSE     0x41
#define IOCTL_FSA_RAW_OPEN  0x54
#define IOCTL_FSA_RAW_CLOSE 0x57

static unsigned int in_buf[0x40]  __attribute__((aligned(0x40)));
static unsigned int out_buf[0x10] __attribute__((aligned(0x40)));

void TestRawOpen(void)
{
    static const char *devices[3] = { "/dev/slc01", "/dev/slccmpt01", "/dev/mlc01" };
    int i;

    int hax = IOS_Open("/dev/iosuhax", 0);
    log_printf_("iosuhax: %d\n", hax);
    if (hax < 0) {
        return;
    }

    IOS_Ioctl(hax, IOCTL_FSA_OPEN, NULL, 0, out_buf, 4);
    int fsa = (int)out_buf[0];
    log_printf_("fsa: %d\n", fsa);

    for (i = 0; i < 3; i++) {
        int pathLen = strlen(devices[i]) + 1;

        memset(in_buf, 0, sizeof(in_buf));
        in_buf[0] = fsa;
        in_buf[1] = 8;                              // the path starts 8 bytes in
        strcpy((char*)in_buf + 8, devices[i]);
        IOS_Ioctl(hax, IOCTL_FSA_RAW_OPEN, in_buf, 8 + pathLen, out_buf, 8);

        int res = (int)out_buf[0];
        int handle = (int)out_buf[1];
        log_printf_("%s -> res=%d handle=%d\n", devices[i], res, handle);

        if (res >= 0) {
            in_buf[0] = fsa;
            in_buf[1] = handle;
            IOS_Ioctl(hax, IOCTL_FSA_RAW_CLOSE, in_buf, 8, out_buf, 4);
        }
    }

    in_buf[0] = fsa;
    IOS_Ioctl(hax, IOCTL_FSA_CLOSE, in_buf, 4, out_buf, 4);
    IOS_Close(hax);

    os_usleep(1000 * 1000);   // give the last log packets a second to leave
}