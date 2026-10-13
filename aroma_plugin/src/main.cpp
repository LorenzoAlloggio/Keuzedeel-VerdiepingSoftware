#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <nsysnet/_socket.h>
#include <sys/socket.h>
#include <unistd.h>

#include <coreinit/debug.h>
#include <coreinit/ios.h>
#include <coreinit/title.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/log_module.h>
#include <whb/log_udp.h>
#include <wups.h>

// Current behavior: test raw opens, then send a TCP marker. No NAND dump yet.
WUPS_PLUGIN_NAME("NandDumper Auto Raw-Open Test");
WUPS_PLUGIN_DESCRIPTION("Tests iosuhax raw opens and TCP connectivity.");
WUPS_PLUGIN_VERSION("2.0");
WUPS_PLUGIN_AUTHOR("LorenzoAlloggio");
WUPS_PLUGIN_LICENSE("Unspecified");

WUPS_USE_WUT_DEVOPTAB();

#define IOCTL_FSA_OPEN      0x40
#define IOCTL_FSA_CLOSE     0x41
#define IOCTL_FSA_RAW_OPEN  0x54
#define IOCTL_FSA_RAW_READ  0x55
#define IOCTL_FSA_RAW_CLOSE 0x57

#define TRANSFER_SERVER_IP   "192.168.2.50" // Receiver IP, update if it changes.
#define TRANSFER_SERVER_PORT 4406

static constexpr uint32_t SLC_BYTES_PER_SECTOR = 0x840;
static constexpr uint32_t SLC_SECTOR_COUNT = 0x40000;
static constexpr uint32_t CHUNK_SECTORS = 0x80;

static uint8_t raw_read_output[0x40 + CHUNK_SECTORS * SLC_BYTES_PER_SECTOR]
    __attribute__((aligned(0x40)));
static uint32_t raw_read_args[6] __attribute__((aligned(0x40)));

// Buffers for the existing raw-open test.
static unsigned int in_buf[0x40] __attribute__((aligned(0x40)));
static unsigned int out_buf[0x10] __attribute__((aligned(0x40)));

// Run the tests only once.
static bool test_started;
static bool module_logging;
static bool cafe_logging;
static bool udp_logging;

// Logging
static void log_message(const char *format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    if (!WHBLogPrintf("[NandDumper Auto Test] %s", message)) {
        OSReport("[NandDumper Auto Test] %s\n", message);
    }
}

static void initialize_logging(void)
{
    module_logging = WHBLogModuleInit();
    if (!module_logging) {
        cafe_logging = WHBLogCafeInit();
        udp_logging = WHBLogUdpInit();
    }
}

// Network test
// TCP may accept fewer bytes than requested, so keep sending until complete.
static bool send_all(int socket_fd, const void *data, size_t size)
{
    const char *bytes = static_cast<const char *>(data);
    size_t sent_total = 0;

    while (sent_total < size) {
        ssize_t sent = send(socket_fd, bytes + sent_total, size - sent_total, 0);
        if (sent <= 0) {
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }

    return true;
}

static int __attribute__((unused)) read_raw_sectors(
    IOSHandle iosuhax,
    int fsa_handle,
    int raw_device_handle,
    uint64_t first_sector,
    uint32_t sector_count
)
{
    if (sector_count == 0 || sector_count > CHUNK_SECTORS ||
        first_sector >= SLC_SECTOR_COUNT ||
        sector_count > SLC_SECTOR_COUNT - first_sector) {
        return IOS_ERROR_INVALID;
    }

    memset(raw_read_args, 0, sizeof(raw_read_args));
    memset(raw_read_output, 0, 0x40);

    raw_read_args[0] = static_cast<uint32_t>(fsa_handle);
    raw_read_args[1] = SLC_BYTES_PER_SECTOR;
    raw_read_args[2] = sector_count;
    raw_read_args[3] = static_cast<uint32_t>(first_sector >> 32);
    raw_read_args[4] = static_cast<uint32_t>(first_sector);
    raw_read_args[5] = static_cast<uint32_t>(raw_device_handle);

    IOSError result = IOS_Ioctl(
        iosuhax,
        IOCTL_FSA_RAW_READ,
        raw_read_args,
        sizeof(raw_read_args),
        raw_read_output,
        0x40 + sector_count * SLC_BYTES_PER_SECTOR
    );
    return result < 0 ? result : static_cast<int>(*reinterpret_cast<uint32_t *>(raw_read_output));
}

// Connect to the Mac receiver and send a marker, not NAND data.
static int connect_to_receiver(void)
{
    socket_lib_init();

    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        log_message("TCP socket creation failed: %d", socket_fd);
        return -1;
    }

    sockaddr_in server_address = {};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(TRANSFER_SERVER_PORT);
    if (inet_pton(AF_INET, TRANSFER_SERVER_IP, &server_address.sin_addr) != 1) {
        log_message("Invalid transfer server IP: %s", TRANSFER_SERVER_IP);
        close(socket_fd);
        return -1;
    }

    log_message(
        "Connecting to %s:%u",
        TRANSFER_SERVER_IP,
        static_cast<unsigned int>(TRANSFER_SERVER_PORT)
    );
    if (connect(
            socket_fd,
            reinterpret_cast<const sockaddr *>(&server_address),
            sizeof(server_address)
        ) < 0) {
        log_message("TCP connection failed");
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void run_network_test(void)
{
    static const char test_message[] = "NANDDUMPER NETWORK TEST\n";
    int socket_fd = connect_to_receiver();
    if (socket_fd < 0) {
        return;
    }

    if (!send_all(socket_fd, test_message, sizeof(test_message) - 1)) {
        log_message("Failed to send network test marker");
    } else {
        log_message("Network test marker sent");
    }

    shutdown(socket_fd, SHUT_WR);
    close(socket_fd);
}

// Dump SLC
static void __attribute__((unused)) dump_slc_example(void)
{
    IOSHandle iosuhax = IOS_Open("/dev/iosuhax", (IOSOpenMode)0);
    if (iosuhax < 0) {
        log_message("SLC dump: iosuhax open failed: %d", iosuhax);
        return;
    }

    int fsa_handle = -1;
    int raw_device_handle = -1;
    int socket_fd = -1;
    bool fsa_open = false;

    memset(out_buf, 0, sizeof(out_buf));
    IOSError result = IOS_Ioctl(iosuhax, IOCTL_FSA_OPEN, NULL, 0, out_buf, 4);
    fsa_handle = static_cast<int>(out_buf[0]);
    if (result < 0 || fsa_handle < 0) {
        log_message("SLC dump: FSA open failed: ioctl=%d handle=%d", result, fsa_handle);
    } else {
        fsa_open = true;
    }

    if (fsa_open) {
        const char device_path[] = "/dev/slc01";
        memset(in_buf, 0, sizeof(in_buf));
        memset(out_buf, 0, sizeof(out_buf));
        in_buf[0] = static_cast<uint32_t>(fsa_handle);
        in_buf[1] = 8;
        memcpy(reinterpret_cast<char *>(in_buf) + 8, device_path, sizeof(device_path));

        result = IOS_Ioctl(
            iosuhax,
            IOCTL_FSA_RAW_OPEN,
            in_buf,
            8 + sizeof(device_path),
            out_buf,
            8
        );
        int raw_result = static_cast<int>(out_buf[0]);
        raw_device_handle = static_cast<int>(out_buf[1]);
        if (result < 0 || raw_result < 0) {
            log_message(
                "SLC dump: raw open failed: ioctl=%d result=%d",
                result,
                raw_result
            );
            raw_device_handle = -1;
        }
    }

    if (raw_device_handle >= 0) {
        socket_fd = connect_to_receiver();
        if (socket_fd < 0) {
            log_message("SLC dump: start the Mac receiver, then retry");
        }
    }

    if (socket_fd >= 0) {
        log_message("SLC dump: reading %u sectors", SLC_SECTOR_COUNT);
        uint64_t sector = 0;
        while (sector < SLC_SECTOR_COUNT) {
            uint32_t remaining = SLC_SECTOR_COUNT - sector;
            uint32_t count = remaining < CHUNK_SECTORS ? remaining : CHUNK_SECTORS;
            int read_result = read_raw_sectors(
                iosuhax,
                fsa_handle,
                raw_device_handle,
                sector,
                count
            );
            if (read_result < 0) {
                log_message("SLC dump: read failed at sector %llu: %d",
                            static_cast<unsigned long long>(sector),
                            read_result);
                break;
            }

            size_t bytes = static_cast<size_t>(count) * SLC_BYTES_PER_SECTOR;
            if (!send_all(socket_fd, raw_read_output + 0x40, bytes)) {
                log_message("SLC dump: network send failed at sector %llu",
                            static_cast<unsigned long long>(sector));
                break;
            }

            sector += count;
            if (sector % 0x1000 == 0 || sector == SLC_SECTOR_COUNT) {
                log_message("SLC dump progress: %llu / %u sectors",
                            static_cast<unsigned long long>(sector),
                            SLC_SECTOR_COUNT);
            }
        }
    }

    if (socket_fd >= 0) {
        shutdown(socket_fd, SHUT_WR);
        close(socket_fd);
    }

    if (raw_device_handle >= 0) {
        in_buf[0] = static_cast<uint32_t>(fsa_handle);
        in_buf[1] = static_cast<uint32_t>(raw_device_handle);
        result = IOS_Ioctl(iosuhax, IOCTL_FSA_RAW_CLOSE, in_buf, 8, out_buf, 4);
        log_message("SLC dump: raw close -> %d", result);
    }

    if (fsa_open) {
        in_buf[0] = static_cast<uint32_t>(fsa_handle);
        result = IOS_Ioctl(iosuhax, IOCTL_FSA_CLOSE, in_buf, 4, out_buf, 4);
        log_message("SLC dump: FSA close -> %d", result);
    }

    IOS_Close(iosuhax);
}

// Raw device open test
static void run_raw_open_test(void)
{
    static const char *devices[] = {
        "/dev/slc01",
        "/dev/slccmpt01",
        "/dev/mlc01",
    };

    IOSHandle iosuhax = IOS_Open("/dev/iosuhax", (IOSOpenMode)0);
    if (iosuhax < 0) {
        log_message("iosuhax unavailable: %d", iosuhax);
        return;
    }

    memset(out_buf, 0, sizeof(out_buf));
    IOSError result = IOS_Ioctl(iosuhax, IOCTL_FSA_OPEN, NULL, 0, out_buf, 4);
    int fsa = (int)out_buf[0];
    if (result < 0 || fsa < 0) {
        log_message("FSA open failed: ioctl=%d fsa=%d", result, fsa);
        IOS_Close(iosuhax);
        return;
    }
    log_message("FSA handle: %d", fsa);

    for (unsigned int i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        unsigned int path_len = strlen(devices[i]) + 1;

        memset(in_buf, 0, sizeof(in_buf));
        memset(out_buf, 0, sizeof(out_buf));
        in_buf[0] = (unsigned int)fsa;
        in_buf[1] = 8;
        strcpy((char *)in_buf + 8, devices[i]);

        result = IOS_Ioctl(
            iosuhax,
            IOCTL_FSA_RAW_OPEN,
            in_buf,
            8 + path_len,
            out_buf,
            8
        );
        int raw_result = (int)out_buf[0];
        int handle = (int)out_buf[1];
        log_message(
            "%s -> ioctl=%d res=%d handle=%d\n",
            devices[i],
            result,
            raw_result,
            handle
        );

        if (result >= 0 && raw_result >= 0) {
            in_buf[0] = (unsigned int)fsa;
            in_buf[1] = (unsigned int)handle;
            result = IOS_Ioctl(iosuhax, IOCTL_FSA_RAW_CLOSE, in_buf, 8, out_buf, 4);
            log_message("%s close -> %d\n", devices[i], result);
        }
    }

    in_buf[0] = (unsigned int)fsa;
    result = IOS_Ioctl(iosuhax, IOCTL_FSA_CLOSE, in_buf, 4, out_buf, 4);
    log_message("FSA close -> %d\n", result);
    IOS_Close(iosuhax);
}

// Plugin startup and supported-title detection
static void run_test_for_current_title(void)
{
    uint64_t title_id = OSGetTitleID();
    log_message("Plugin active in title %016llx\n", (unsigned long long)title_id);

    static const uint64_t system_menu_title_ids[] = {
        0x0005001010004000ULL, // System Menu (Japan)
        0x0005001010004001ULL, // System Menu (USA)
        0x0005001010004002ULL, // System Menu (Europe)
        0x0005001010004003ULL, // System Menu (China)
        0x000500101000400AULL, // System Menu (Korea)
        0x000500101004E000ULL, // Mii Maker (Japan)
        0x000500101004E100ULL, // Mii Maker (USA)
        0x000500101004E200ULL, // Mii Maker (Europe)
    };
    bool is_system_menu = false;
    for (unsigned int i = 0;
         i < sizeof(system_menu_title_ids) / sizeof(system_menu_title_ids[0]);
         i++) {
        if (title_id == system_menu_title_ids[i]) {
            is_system_menu = true;
            break;
        }
    }

    if (!is_system_menu || test_started) {
        return;
    }

    test_started = true;
    log_message("System Menu detected; checking iosuhax raw opens\n");

    dump_slc_example();
}

INITIALIZE_PLUGIN() {
    initialize_logging();
    run_test_for_current_title();
}

ON_APPLICATION_START() {
    run_test_for_current_title();
}

DEINITIALIZE_PLUGIN() {
    if (module_logging) {
        WHBLogModuleDeinit();
    } else {
        if (cafe_logging) {
            WHBLogCafeDeinit();
        }
        if (udp_logging) {
            WHBLogUdpDeinit();
        }
    }
}
