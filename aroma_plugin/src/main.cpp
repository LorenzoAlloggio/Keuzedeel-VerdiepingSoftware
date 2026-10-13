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

WUPS_PLUGIN_NAME("NandDumper Auto Raw-Open Test");
WUPS_PLUGIN_DESCRIPTION("Tests iosuhax raw opens and TCP connectivity.");
WUPS_PLUGIN_VERSION("2.0");
WUPS_PLUGIN_AUTHOR("LorenzoAlloggio");
WUPS_PLUGIN_LICENSE("Unspecified");

WUPS_USE_WUT_DEVOPTAB();

#define IOCTL_FSA_OPEN 0x40
#define IOCTL_FSA_CLOSE 0x41
#define IOCTL_FSA_RAW_OPEN 0x54
#define IOCTL_FSA_RAW_CLOSE 0x57

#define TRANSFER_SERVER_IP "192.168.2.50" // Mac en0 address; update if it changes.
#define TRANSFER_SERVER_PORT 4406

static unsigned int in_buf[0x40] __attribute__((aligned(0x40)));
static unsigned int out_buf[0x10] __attribute__((aligned(0x40)));
static bool test_started;
static bool module_logging;
static bool cafe_logging;
static bool udp_logging;

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

static void run_network_test(void)
{
    static const char test_message[] = "NANDDUMPER NETWORK TEST\n";
    socket_lib_init();

    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        log_message("TCP socket creation failed: %d", socket_fd);
        return;
    }

    sockaddr_in server_address = {};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(TRANSFER_SERVER_PORT);
    if (inet_pton(AF_INET, TRANSFER_SERVER_IP, &server_address.sin_addr) != 1) {
        log_message("Invalid transfer server IP: %s", TRANSFER_SERVER_IP);
        close(socket_fd);
        return;
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
    run_raw_open_test();
    run_network_test();
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
