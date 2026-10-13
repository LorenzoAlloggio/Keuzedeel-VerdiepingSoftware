#include "text.h"
#include "sdio.h"
#include "dumper.h"
#include "imports.h"
#include "fs_config.h"

#define INITIALIZING_FLA        0x07
#define INITIALIZING_MMC        0x0D

extern fs_config dumper_config;

int getPhysicalDeviceHandle(u32 device);

void createDevThread_entry(int initialization_type)
{
    if(initialization_type == INITIALIZING_MMC)
    {
        if (!dumper_config.dump_network)
        {
            sdcard_init();
        }
    }

    //if(initialization_type == INITIALIZING_FLA)
    //{
        //dump_nand_complete();
    //}

    if(initialization_type == 0x01) // unknown but after SLC init no read/write done at this point yet
    {
        //if(check_nand_dump() == 0)
        //{
        clearScreen(0x000000FF);
        _printf(20, 20, "welcome to NAND dumper!");

        dump_nand_complete();
        //}
    }
}
