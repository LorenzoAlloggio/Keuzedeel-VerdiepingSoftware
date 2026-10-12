#include <string.h>
#include <stdarg.h>
#include <stdlib.h>
#include <malloc.h>
#include <unistd.h>
#include <stdio.h>
#include <fcntl.h>
#include "dynamic_libs/os_functions.h"
#include "dynamic_libs/fs_functions.h"
#include "dynamic_libs/gx2_functions.h"
#include "dynamic_libs/sys_functions.h"
#include "dynamic_libs/vpad_functions.h"
#include "dynamic_libs/socket_functions.h"
#include "dynamic_libs/proc_ui_functions.h"
#include "fs/fs_utils.h"
#include "fs/sd_fat_devoptab.h"
#include "system/memory.h"
#include "utils/logger.h"
#include "utils/utils.h"
#include "common/common.h"
#include "menu.h"
#include "main.h"
#include "ios_exploit.h"

void TestRawOpen(void);

static int exitToHBLOnLaunch = 0;

bool CheckRunning() {
    switch (ProcUIProcessMessages(true)) {
        case PROCUI_STATUS_EXITING: {
            return false;
        }
        case PROCUI_STATUS_RELEASE_FOREGROUND: {
            ProcUIDrawDoneRelease();
            break;
        }
        case PROCUI_STATUS_IN_FOREGROUND: {
            break;
        }
        case PROCUI_STATUS_IN_BACKGROUND:
        default:
            break;
    }
    return true;
}


int Menu_Main(int argc, char **argv)
{
    // Initialize system function pointers first.
    InitOSFunctionPointers();

    InitSysFunctionPointers();
    InitSocketFunctionPointers();
    InitVPadFunctionPointers();
    InitProcUIFunctionPointers();

    // Initialize logging before writing any log messages.
    log_init_();
    log_printf_("\n[nanddumper] Menu_Main entered\n");

    cfw_config_t config;
    default_config(&config);

    log_printf_("[nanddumper] Showing menu\n");

    int launch = ShowMenu(&config);

    log_printf_("[nanddumper] ShowMenu returned: %d\n", launch);
    log_printf_("[nanddumper] dumpNetwork: %d\n", config.dumpNetwork);

    int exploitStatus = -1;
    if (launch) {
        log_printf_("[nanddumper] Calling ExecuteIOSExploit\n");

        exploitStatus = ExecuteIOSExploit(&config);

        log_printf_("[nanddumper] ExecuteIOSExploit result: %d\n", exploitStatus);

        if (exploitStatus < 0) {
            log_printf_("[nanddumper] IOS exploit failed: %d\n", exploitStatus);
        } else if (exploitStatus == 0) {
            log_printf_("[nanddumper] Testing raw-open on active iosuhax\n");
            TestRawOpen();
        } else {
            log_printf_("[nanddumper] Skipping raw-open until IOS reload completes\n");
        }
    } else {
        log_printf_("[nanddumper] Launch cancelled by user\n");
    }

    int fullRelaunch = launch && (!config.dumpNetwork || exploitStatus == 1);

    unsigned long long titleID = OSGetTitleID();

    log_printf_("[nanddumper] Current title ID: %llx\n", titleID);
    log_printf_("[nanddumper] fullRelaunch: %d\n", fullRelaunch);

    // Handle Mii Maker separately.
    if (titleID == 0x000500101004A200ULL ||
        titleID == 0x000500101004A100ULL ||
        titleID == 0x000500101004A000ULL) {

        log_printf_("[nanddumper] Running under Mii Maker\n");

        if (fullRelaunch) {
            log_printf_("[nanddumper] Requesting full relaunch\n");
            OSForceFullRelaunch();
            SYSLaunchMenu();
        } else if (!launch) {
            log_printf_("[nanddumper] Shutting down\n");
            OSShutdown();
        }

        log_printf_("[nanddumper] Returning to original Mii Maker entry point\n");

        return ((int (*)(int, char **))(*(unsigned int *)0x1005E040))(
            argc, argv
        );
    }

    log_printf_("[nanddumper] Initializing ProcUI\n");

    ProcUIInit(OSSavesDone_ReadyToRelease);

    if (fullRelaunch) {
        log_printf_("[nanddumper] Full relaunch requested\n");
        OSForceFullRelaunch();
        int result = SYSLaunchMenu();
        log_printf_("[nanddumper] SYSLaunchMenu returned: %d\n", result);
    } else if (launch) {
        log_printf_("[nanddumper] Launching system menu\n");
        SYSLaunchMenu();
    } else {
        log_printf_("[nanddumper] Shutting down\n");
        OSShutdown();
    }

    log_printf_("[nanddumper] Entering ProcUI loop\n");

    while (CheckRunning()) {
        OSSleepTicks(MILLISECS_TO_TICKS(100));
    }

    log_printf_("[nanddumper] ProcUI loop ended\n");

    ProcUIShutdown();

    log_printf_("[nanddumper] Menu_Main exiting\n");

    return 0;
}
