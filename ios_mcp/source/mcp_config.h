#ifndef _MCP_CONFIG_H_
#define _MCP_CONFIG_H_

#include "types.h"

typedef struct
{
    u32 filled; // 1 once ios_kernel has copied the data from the MCP to this struct, 0 otherwise
    u8 otp[0x400]; // 1 KB OTP
    u8 seeprom[0x200]; // 512 bytes
} mcp_config_t;

#endif