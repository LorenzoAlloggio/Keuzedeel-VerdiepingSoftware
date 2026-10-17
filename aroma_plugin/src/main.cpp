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

WUPS_PLUGIN_NAME("NandDumper Auto Test");
WUPS_PLUGIN_DESCRIPTION("Streams OTP and SLC dumps over the network.");
WUPS_PLUGIN_VERSION("2.0");
WUPS_PLUGIN_AUTHOR("LorenzoAlloggio");
WUPS_PLUGIN_LICENSE("Unspecified");

WUPS_USE_WUT_DEVOPTAB();

#define IOCTL_FSA_OPEN 0x40
#define IOCTL_FSA_CLOSE 0x41
#define IOCTL_FSA_RAW_OPEN 0x54
#define IOCTL_FSA_RAW_READ 0x55
#define IOCTL_FSA_RAW_CLOSE 0x57
#define IOCTL_READ_OTP 0x08

#define TRANSFER_SERVER_IP "192.168.2.50" // Receiver IP, update if it changes.
#define TRANSFER_SERVER_PORT 4406
#define READ_RETRIES 3
#define MAX_READ_ERRORS 4096

#define DUMP_OTP 1
#define DUMP_SLC 1
#define DUMP_SLCCMPT 1
#define DUMP_SEEPROM 1

#define FSA_ERROR_DATA_CORRUPTED (-0x3001B)
#define FSA_ERROR_OUT_OF_RANGE (-0x3002B)

static constexpr uint32_t SLC_BYTES_PER_SECTOR = 0x800;
static constexpr uint32_t SLC_SECTOR_COUNT = 0x40000;
static constexpr uint32_t CHUNK_SECTORS = 0x80;
static constexpr uint32_t OTP_SIZE = 0x400;
static constexpr uint32_t SEEPROM_SIZE = 0x200;
static uint16_t seeprom_words[SEEPROM_SIZE / 2];

static uint8_t zero_sector[SLC_BYTES_PER_SECTOR];

static uint8_t raw_read_output[0x40 + CHUNK_SECTORS * SLC_BYTES_PER_SECTOR]
    __attribute__((aligned(0x40)));
static uint32_t raw_read_args[6] __attribute__((aligned(0x40)));
static uint8_t otp_buffer[OTP_SIZE] __attribute__((aligned(0x40)));

struct RawReadDiagnostics
{
    IOSError ioctl_result;
    int32_t raw_status;
};

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

    if (!WHBLogPrintf("[NandDumper Auto Test] %s", message))
    {
        OSReport("[NandDumper Auto Test] %s\n", message);
    }
}

static void initialize_logging(void)
{
    module_logging = WHBLogModuleInit();
    if (!module_logging)
    {
        cafe_logging = WHBLogCafeInit();
        udp_logging = WHBLogUdpInit();
    }
}

static int __attribute__((unused)) read_raw_sectors(
    IOSHandle iosuhax,
    int fsa_handle,
    int raw_device_handle,
    uint64_t first_sector,
    uint32_t sector_count,
    RawReadDiagnostics *diagnostics)
{
    if (sector_count == 0 || sector_count > CHUNK_SECTORS ||
        first_sector >= SLC_SECTOR_COUNT ||
        sector_count > SLC_SECTOR_COUNT - first_sector)
    {
        diagnostics->ioctl_result = IOS_ERROR_INVALID;
        diagnostics->raw_status = 0;
        return IOS_ERROR_INVALID;
    }

    memset(raw_read_args, 0, sizeof(raw_read_args));
    memset(raw_read_output, 0, 0x40);
    memset(raw_read_output + 0x40, 0xFF, sector_count * SLC_BYTES_PER_SECTOR);

    raw_read_args[0] = static_cast<uint32_t>(fsa_handle);
    raw_read_args[1] = SLC_BYTES_PER_SECTOR;
    raw_read_args[2] = sector_count;
    raw_read_args[3] = static_cast<uint32_t>(first_sector >> 32);
    raw_read_args[4] = static_cast<uint32_t>(first_sector);
    raw_read_args[5] = static_cast<uint32_t>(raw_device_handle);

    diagnostics->ioctl_result = IOS_Ioctl(
        iosuhax,
        IOCTL_FSA_RAW_READ,
        raw_read_args,
        sizeof(raw_read_args),
        raw_read_output,
        0x40 + sector_count * SLC_BYTES_PER_SECTOR);
    memcpy(&diagnostics->raw_status, raw_read_output, sizeof(diagnostics->raw_status));
    return diagnostics->ioctl_result < 0
               ? diagnostics->ioctl_result
               : diagnostics->raw_status;
}

static int read_with_retry(
    IOSHandle iosuhax,
    int fsa_handle,
    int raw_device_handle,
    uint64_t first_sector,
    uint32_t sector_count,
    RawReadDiagnostics *diagnostics,
    uint32_t *attempts_used)
{
    int result = -1;
    *attempts_used = 0;
    for (int attempt = 0; attempt < READ_RETRIES; attempt++)
    {
        (*attempts_used)++;
        result = read_raw_sectors(
            iosuhax, fsa_handle, raw_device_handle, first_sector, sector_count,
            diagnostics);
        if (result >= 0)
        {
            return result;
        }
    }
    return result;
}

static bool send_all(int socket_fd, const void *data, size_t size)
{
    const char *bytes = static_cast<const char *>(data);
    size_t sent_total = 0;

    while (sent_total < size)
    {
        ssize_t sent = send(socket_fd, bytes + sent_total, size - sent_total, 0);
        if (sent <= 0)
        {
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }

    return true;
}

static uint32_t crc32_table[256];

static void crc32_init(void)
{
    for (uint32_t i = 0; i < 256; i++)
    {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
        {
            c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        crc32_table[i] = c;
    }
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t size)
{
    crc = ~crc;
    for (size_t i = 0; i < size; i++)
    {
        crc = crc32_table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

static void put_u64(uint8_t *p, uint64_t v)
{
    put_u32(p, static_cast<uint32_t>(v >> 32));
    put_u32(p + 4, static_cast<uint32_t>(v));
}

static bool send_file_header(int socket_fd, const char *name, uint64_t size,
                             uint32_t chunk_size)
{
    uint8_t header[32] = {};
    memcpy(header, "FILE", 4);
    strncpy(reinterpret_cast<char *>(header + 4), name, 15);
    put_u64(header + 20, size);
    put_u32(header + 28, chunk_size);
    return send_all(socket_fd, header, sizeof(header));
}

static bool send_chunk(int socket_fd, uint64_t offset, const uint8_t *data,
                       uint32_t length, uint32_t *file_crc)
{
    uint8_t header[20];
    memcpy(header, "CHNK", 4);
    put_u64(header + 4, offset);
    put_u32(header + 12, length);
    put_u32(header + 16, crc32_update(0, data, length));
    *file_crc = crc32_update(*file_crc, data, length);
    return send_all(socket_fd, header, sizeof(header)) &&
           send_all(socket_fd, data, length);
}

static bool send_file_end(int socket_fd, uint32_t file_crc, uint32_t read_errors,
                          uint32_t unverified_sectors)
{
    uint8_t footer[16];
    memcpy(footer, "FEND", 4);
    put_u32(footer + 4, file_crc);
    put_u32(footer + 8, read_errors);
    put_u32(footer + 12, unverified_sectors);
    return send_all(socket_fd, footer, sizeof(footer));
}

static int connect_to_receiver(void)
{
    socket_lib_init();

    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0)
    {
        log_message("TCP socket creation failed: %d", socket_fd);
        return -1;
    }

    sockaddr_in server_address = {};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(TRANSFER_SERVER_PORT);
    if (inet_pton(AF_INET, TRANSFER_SERVER_IP, &server_address.sin_addr) != 1)
    {
        log_message("Invalid transfer server IP: %s", TRANSFER_SERVER_IP);
        close(socket_fd);
        return -1;
    }

    log_message(
        "Connecting to %s:%u",
        TRANSFER_SERVER_IP,
        static_cast<unsigned int>(TRANSFER_SERVER_PORT));
    if (connect(
            socket_fd,
            reinterpret_cast<const sockaddr *>(&server_address),
            sizeof(server_address)) < 0)
    {
        log_message("TCP connection failed");
        close(socket_fd);
        return -1;
    }

    return socket_fd;
}

static void __attribute__((unused))run_network_test(void)
{
    static const char test_message[] = "NANDDUMPER NETWORK TEST\n";
    int socket_fd = connect_to_receiver();
    if (socket_fd < 0)
    {
        return;
    }

    if (!send_all(socket_fd, test_message, sizeof(test_message) - 1))
    {
        log_message("Failed to send network test marker");
    }
    else
    {
        log_message("Network test marker sent");
    }

    shutdown(socket_fd, SHUT_WR);
    close(socket_fd);
}

static bool dump_otp(IOSHandle iosuhax, int socket_fd)
{
    memset(otp_buffer, 0, sizeof(otp_buffer));
    IOSError result = IOS_Ioctl(iosuhax, IOCTL_READ_OTP, NULL, 0, otp_buffer, OTP_SIZE);
    if (result < 0)
    {
        log_message("OTP dump: ioctl 0x%X failed: %d, skipping OTP", IOCTL_READ_OTP, result);
        return true;
    }

    uint32_t file_crc = 0;
    bool sent = send_file_header(socket_fd, "otp.bin", OTP_SIZE, OTP_SIZE) &&
                send_chunk(socket_fd, 0, otp_buffer, OTP_SIZE, &file_crc) &&
                send_file_end(socket_fd, file_crc, 0, 0);
    if (sent)
    {
        log_message("OTP dump: sent %u bytes, CRC32 %08X", OTP_SIZE, file_crc);
    }
    else
    {
        log_message("OTP dump: network send failed");
    }

    memset(otp_buffer, 0, sizeof(otp_buffer));
    return sent;
}

extern "C" int bspRead(const char *entity, uint32_t instance,
                       const char *attribute, uint32_t size, void *buffer);

static bool dump_seeprom(int socket_fd)
{
    memset(seeprom_words, 0, sizeof(seeprom_words));

    for (uint32_t i = 0; i < SEEPROM_SIZE / 2; i++)
    {
        int result = bspRead("EE", i, "access", 2, &seeprom_words[i]);
        if (result != 0)
        {
            log_message("SEEPROM dump: bspRead word %u failed: %d, skipping SEEPROM", i, result);
            memset(seeprom_words, 0, sizeof(seeprom_words));
            return true; // non-fatal, same as your OTP path
        }
    }

    uint32_t file_crc = 0;
    bool sent = send_file_header(socket_fd, "seeprom.bin", SEEPROM_SIZE, SEEPROM_SIZE) &&
                send_chunk(socket_fd, 0, reinterpret_cast<const uint8_t *>(seeprom_words), SEEPROM_SIZE, &file_crc) &&
                send_file_end(socket_fd, file_crc, 0, 0);

    if (sent)
        log_message("SEEPROM dump: sent %u bytes, CRC32 %08X", SEEPROM_SIZE, file_crc);
    else
        log_message("SEEPROM dump: network send failed");

    memset(seeprom_words, 0, sizeof(seeprom_words));
    return sent;
}

static bool dump_slccmpt(IOSHandle iosuhax, int socket_fd)
{
    int fsa_handle = -1;
    int raw_device_handle = -1;
    bool fsa_open = false;
    bool connection_ok = true;

    memset(out_buf, 0, sizeof(out_buf));
    IOSError result = IOS_Ioctl(iosuhax, IOCTL_FSA_OPEN, NULL, 0, out_buf, 4);
    fsa_handle = static_cast<int>(out_buf[0]);
    if (result < 0 || fsa_handle < 0)
    {
        log_message("SLC dump: FSA open failed: ioctl=%d handle=%d", result, fsa_handle);
    }
    else
    {
        fsa_open = true;
    }

    if (fsa_open)
    {
        const char device_path[] = "/dev/slccmpt01";
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
            8);
        int raw_result = static_cast<int>(out_buf[0]);
        raw_device_handle = static_cast<int>(out_buf[1]);
        if (result < 0 || raw_result < 0)
        {
            log_message(
                "SLC dump: raw open failed: ioctl=%d result=%d; WPS raw reads are disabled "
                "in favor of the native IOS-FS network dumper",
                result,
                raw_result);
            raw_device_handle = -1;
        }
    }

    if (raw_device_handle >= 0)
    {
        connection_ok = send_file_header(
            socket_fd, "slccmpt.bin",
            static_cast<uint64_t>(SLC_SECTOR_COUNT) * SLC_BYTES_PER_SECTOR,
            CHUNK_SECTORS * SLC_BYTES_PER_SECTOR);
        if (!connection_ok)
        {
            log_message("SLCCMPT dump: network send failed (file header)");
        }
    }

    if (raw_device_handle >= 0 && connection_ok)
    {
        log_message(
            "SLCCMPT dump: reading %u data sectors (%u bytes each; spare area excluded)",
            SLC_SECTOR_COUNT, SLC_BYTES_PER_SECTOR);
        uint64_t sector = 0;
        uint32_t read_errors = 0;
        uint32_t unverified_sectors = 0;
        bool aborted = false;
        uint32_t file_crc = 0;

        while (sector < SLC_SECTOR_COUNT && !aborted)
        {
            uint32_t remaining = SLC_SECTOR_COUNT - sector;
            uint32_t count = remaining < CHUNK_SECTORS ? remaining : CHUNK_SECTORS;

            RawReadDiagnostics read_diagnostics = {};
            uint32_t chunk_attempts = 0;
            int read_result = read_with_retry(
                iosuhax, fsa_handle, raw_device_handle, sector, count,
                &read_diagnostics, &chunk_attempts);

            if (read_result >= 0)
            {
                // Whole chunk read fine
                size_t bytes = static_cast<size_t>(count) * SLC_BYTES_PER_SECTOR;
                if (!send_chunk(socket_fd, sector * SLC_BYTES_PER_SECTOR,
                                raw_read_output + 0x40, static_cast<uint32_t>(bytes),
                                &file_crc))
                {
                    log_message("SLCCMPT dump: network send failed at sector %llu",
                                static_cast<unsigned long long>(sector));
                    connection_ok = false;
                    break;
                }
            }
            else
            {
                // Chunk failed after retries
                log_message("SLCCMPT dump: chunk at sector %llu failed after %d attempts "
                            "(ioctl=%d, raw_status=%d%s), reading one sector at a time",
                            static_cast<unsigned long long>(sector),
                            chunk_attempts,
                            read_diagnostics.ioctl_result,
                            read_diagnostics.raw_status,
                            read_diagnostics.ioctl_result < 0
                                ? ", raw status may be invalid"
                                : "");

                for (uint32_t i = 0; i < count; i++)
                {
                    RawReadDiagnostics sector_diagnostics = {};
                    uint32_t sector_attempts = 0;
                    int r = read_with_retry(
                        iosuhax, fsa_handle, raw_device_handle, sector + i, 1,
                        &sector_diagnostics, &sector_attempts);

                    const uint8_t *src;
                    if (r >= 0)
                    {
                        src = raw_read_output + 0x40;
                    }
                    else if (r == FSA_ERROR_DATA_CORRUPTED)
                    {
                        unverified_sectors++;
                        src = raw_read_output + 0x40;
                        log_message(
                            "SLCCMPT dump: sector %llu reported DATA_CORRUPTED after %d attempts "
                            "(ioctl=%d, raw_status=%d); preserving returned bytes as unverified",
                            static_cast<unsigned long long>(sector + i),
                            sector_attempts,
                            sector_diagnostics.ioctl_result,
                            sector_diagnostics.raw_status);
                    }
                    else if (r == FSA_ERROR_OUT_OF_RANGE)
                    {
                        log_message(
                            "SLCCMPT dump: raw device reports out of range at sector %llu; "
                            "stopping without fabricating the remaining data",
                            static_cast<unsigned long long>(sector + i));
                        aborted = true;
                        break;
                    }
                    else
                    {
                        read_errors++;
                        src = zero_sector; // keeps later data at the right offset
                        log_message("SLCCMPT dump: read error at sector %llu failed after %d attempts "
                                    "(ioctl=%d, raw_status=%d%s; zero-filled)",
                                    static_cast<unsigned long long>(sector + i),
                                    sector_attempts,
                                    sector_diagnostics.ioctl_result,
                                    sector_diagnostics.raw_status,
                                    sector_diagnostics.ioctl_result < 0
                                        ? ", raw status may be invalid"
                                        : "");
                    }

                    if (!send_chunk(socket_fd, (sector + i) * SLC_BYTES_PER_SECTOR,
                                    src, SLC_BYTES_PER_SECTOR, &file_crc))
                    {
                        log_message("SLCCMPT dump: network send failed at sector %llu",
                                    static_cast<unsigned long long>(sector + i));
                        connection_ok = false;
                        aborted = true;
                        break;
                    }

                    if (read_errors >= MAX_READ_ERRORS)
                    {
                        log_message("SLCCMPT dump: too many read errors, aborting");
                        aborted = true;
                        break;
                    }
                }
                if (aborted)
                {
                    break;
                }
            }

            sector += count;
            if (sector % 0x1000 == 0 || sector == SLC_SECTOR_COUNT)
            {
                log_message(
                    "SLCCMPT dump progress: %llu / %u sectors (%u read errors, %u unverified)",
                    static_cast<unsigned long long>(sector),
                    SLC_SECTOR_COUNT, read_errors, unverified_sectors);
            }
        }

        log_message(
            "SLCCMPT dump finished: %u read errors, %u unverified sectors%s",
            read_errors, unverified_sectors, aborted ? " (ABORTED)" : "");

        // Also sent after an abort, so the receiver can say how much is missing.
        if (connection_ok)
        {
            connection_ok = send_file_end(socket_fd, file_crc, read_errors, unverified_sectors);
            if (!connection_ok)
            {
                log_message("SLCCMPT dump: network send failed (file end)");
            }
        }
    }

    if (raw_device_handle >= 0)
    {
        in_buf[0] = static_cast<uint32_t>(fsa_handle);
        in_buf[1] = static_cast<uint32_t>(raw_device_handle);
        result = IOS_Ioctl(iosuhax, IOCTL_FSA_RAW_CLOSE, in_buf, 8, out_buf, 4);
        log_message("SLCCMPT dump: raw close -> %d", result);
    }

    if (fsa_open)
    {
        in_buf[0] = static_cast<uint32_t>(fsa_handle);
        result = IOS_Ioctl(iosuhax, IOCTL_FSA_CLOSE, in_buf, 4, out_buf, 4);
        log_message("SLCCMPT dump: FSA close -> %d", result);
    }

    return connection_ok;
}


static bool dump_slc(IOSHandle iosuhax, int socket_fd)
{
    int fsa_handle = -1;
    int raw_device_handle = -1;
    bool fsa_open = false;
    bool connection_ok = true;

    memset(out_buf, 0, sizeof(out_buf));
    IOSError result = IOS_Ioctl(iosuhax, IOCTL_FSA_OPEN, NULL, 0, out_buf, 4);
    fsa_handle = static_cast<int>(out_buf[0]);
    if (result < 0 || fsa_handle < 0)
    {
        log_message("SLC dump: FSA open failed: ioctl=%d handle=%d", result, fsa_handle);
    }
    else
    {
        fsa_open = true;
    }

    if (fsa_open)
    {
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
            8);
        int raw_result = static_cast<int>(out_buf[0]);
        raw_device_handle = static_cast<int>(out_buf[1]);
        if (result < 0 || raw_result < 0)
        {
            log_message(
                "SLC dump: raw open failed: ioctl=%d result=%d; WPS raw reads are disabled "
                "in favor of the native IOS-FS network dumper",
                result,
                raw_result);
            raw_device_handle = -1;
        }
    }

    if (raw_device_handle >= 0)
    {
        connection_ok = send_file_header(
            socket_fd, "slc.bin",
            static_cast<uint64_t>(SLC_SECTOR_COUNT) * SLC_BYTES_PER_SECTOR,
            CHUNK_SECTORS * SLC_BYTES_PER_SECTOR);
        if (!connection_ok)
        {
            log_message("SLC dump: network send failed (file header)");
        }
    }

    if (raw_device_handle >= 0 && connection_ok)
    {
        log_message(
            "SLC dump: reading %u data sectors (%u bytes each; spare area excluded)",
            SLC_SECTOR_COUNT, SLC_BYTES_PER_SECTOR);
        uint64_t sector = 0;
        uint32_t read_errors = 0;
        uint32_t unverified_sectors = 0;
        bool aborted = false;
        uint32_t file_crc = 0;

        while (sector < SLC_SECTOR_COUNT && !aborted)
        {
            uint32_t remaining = SLC_SECTOR_COUNT - sector;
            uint32_t count = remaining < CHUNK_SECTORS ? remaining : CHUNK_SECTORS;

            RawReadDiagnostics read_diagnostics = {};
            uint32_t chunk_attempts = 0;
            int read_result = read_with_retry(
                iosuhax, fsa_handle, raw_device_handle, sector, count,
                &read_diagnostics, &chunk_attempts);

            if (read_result >= 0)
            {
                // Whole chunk read fine
                size_t bytes = static_cast<size_t>(count) * SLC_BYTES_PER_SECTOR;
                if (!send_chunk(socket_fd, sector * SLC_BYTES_PER_SECTOR,
                                raw_read_output + 0x40, static_cast<uint32_t>(bytes),
                                &file_crc))
                {
                    log_message("SLC dump: network send failed at sector %llu",
                                static_cast<unsigned long long>(sector));
                    connection_ok = false;
                    break;
                }
            }
            else
            {
                // Chunk failed after retries
                log_message("SLC dump: chunk at sector %llu failed after %d attempts "
                            "(ioctl=%d, raw_status=%d%s), reading one sector at a time",
                            static_cast<unsigned long long>(sector),
                            chunk_attempts,
                            read_diagnostics.ioctl_result,
                            read_diagnostics.raw_status,
                            read_diagnostics.ioctl_result < 0
                                ? ", raw status may be invalid"
                                : "");

                for (uint32_t i = 0; i < count; i++)
                {
                    RawReadDiagnostics sector_diagnostics = {};
                    uint32_t sector_attempts = 0;
                    int r = read_with_retry(
                        iosuhax, fsa_handle, raw_device_handle, sector + i, 1,
                        &sector_diagnostics, &sector_attempts);

                    const uint8_t *src;
                    if (r >= 0)
                    {
                        src = raw_read_output + 0x40;
                    }
                    else if (r == FSA_ERROR_DATA_CORRUPTED)
                    {
                        unverified_sectors++;
                        src = raw_read_output + 0x40;
                        log_message(
                            "SLC dump: sector %llu reported DATA_CORRUPTED after %d attempts "
                            "(ioctl=%d, raw_status=%d); preserving returned bytes as unverified",
                            static_cast<unsigned long long>(sector + i),
                            sector_attempts,
                            sector_diagnostics.ioctl_result,
                            sector_diagnostics.raw_status);
                    }
                    else if (r == FSA_ERROR_OUT_OF_RANGE)
                    {
                        log_message(
                            "SLC dump: raw device reports out of range at sector %llu; "
                            "stopping without fabricating the remaining data",
                            static_cast<unsigned long long>(sector + i));
                        aborted = true;
                        break;
                    }
                    else
                    {
                        read_errors++;
                        src = zero_sector; // keeps later data at the right offset
                        log_message("SLC dump: read error at sector %llu failed after %d attempts "
                                    "(ioctl=%d, raw_status=%d%s; zero-filled)",
                                    static_cast<unsigned long long>(sector + i),
                                    sector_attempts,
                                    sector_diagnostics.ioctl_result,
                                    sector_diagnostics.raw_status,
                                    sector_diagnostics.ioctl_result < 0
                                        ? ", raw status may be invalid"
                                        : "");
                    }

                    if (!send_chunk(socket_fd, (sector + i) * SLC_BYTES_PER_SECTOR,
                                    src, SLC_BYTES_PER_SECTOR, &file_crc))
                    {
                        log_message("SLC dump: network send failed at sector %llu",
                                    static_cast<unsigned long long>(sector + i));
                        connection_ok = false;
                        aborted = true;
                        break;
                    }

                    if (read_errors >= MAX_READ_ERRORS)
                    {
                        log_message("SLC dump: too many read errors, aborting");
                        aborted = true;
                        break;
                    }
                }
                if (aborted)
                {
                    break;
                }
            }

            sector += count;
            if (sector % 0x1000 == 0 || sector == SLC_SECTOR_COUNT)
            {
                log_message(
                    "SLC dump progress: %llu / %u sectors (%u read errors, %u unverified)",
                    static_cast<unsigned long long>(sector),
                    SLC_SECTOR_COUNT, read_errors, unverified_sectors);
            }
        }

        log_message(
            "SLC dump finished: %u read errors, %u unverified sectors%s",
            read_errors, unverified_sectors, aborted ? " (ABORTED)" : "");

        // Also sent after an abort, so the receiver can say how much is missing.
        if (connection_ok)
        {
            connection_ok = send_file_end(socket_fd, file_crc, read_errors, unverified_sectors);
            if (!connection_ok)
            {
                log_message("SLC dump: network send failed (file end)");
            }
        }
    }

    if (raw_device_handle >= 0)
    {
        in_buf[0] = static_cast<uint32_t>(fsa_handle);
        in_buf[1] = static_cast<uint32_t>(raw_device_handle);
        result = IOS_Ioctl(iosuhax, IOCTL_FSA_RAW_CLOSE, in_buf, 8, out_buf, 4);
        log_message("SLC dump: raw close -> %d", result);
    }

    if (fsa_open)
    {
        in_buf[0] = static_cast<uint32_t>(fsa_handle);
        result = IOS_Ioctl(iosuhax, IOCTL_FSA_CLOSE, in_buf, 4, out_buf, 4);
        log_message("SLC dump: FSA close -> %d", result);
    }

    return connection_ok;
}

static void run_network_dump(void)
{
    crc32_init();

    IOSHandle iosuhax = IOS_Open("/dev/iosuhax", (IOSOpenMode)0);
    if (iosuhax < 0)
    {
        log_message("Network dump: iosuhax open failed: %d", iosuhax);
        return;
    }

    int socket_fd = connect_to_receiver();
    if (socket_fd < 0)
    {
        log_message("Network dump: start the Mac receiver, then retry");
        IOS_Close(iosuhax);
        return;
    }

    bool connection_ok = true;
#if DUMP_OTP
    connection_ok = dump_otp(iosuhax, socket_fd);
#endif
#if DUMP_SLC
    if (connection_ok)
    {
        connection_ok = dump_slc(iosuhax, socket_fd);
    }
#endif
#if DUMP_SEEPROM
    if (connection_ok)
    {
        connection_ok = dump_seeprom(socket_fd);
    }
#endif
#if DUMP_SLCCMPT
    if (connection_ok)
    {
        connection_ok = dump_slccmpt(iosuhax, socket_fd);
    }
#endif

    if (connection_ok && send_all(socket_fd, "DONE", 4))
    {
        log_message("Network dump: finished, DONE sent (the receiver prints what arrived)");
    }
    else
    {
        log_message("Network dump: connection lost, the receiver will report it as incomplete");
    }

    shutdown(socket_fd, SHUT_WR);
    close(socket_fd);
    IOS_Close(iosuhax);
}

static void __attribute__((unused)) run_raw_open_test(void)
{
    static const char *devices[] = {
        "/dev/slc01",
        "/dev/slccmpt01",
        "/dev/mlc01",
    };

    IOSHandle iosuhax = IOS_Open("/dev/iosuhax", (IOSOpenMode)0);
    if (iosuhax < 0)
    {
        log_message("iosuhax unavailable: %d", iosuhax);
        return;
    }

    memset(out_buf, 0, sizeof(out_buf));
    IOSError result = IOS_Ioctl(iosuhax, IOCTL_FSA_OPEN, NULL, 0, out_buf, 4);
    int fsa = (int)out_buf[0];
    if (result < 0 || fsa < 0)
    {
        log_message("FSA open failed: ioctl=%d fsa=%d", result, fsa);
        IOS_Close(iosuhax);
        return;
    }
    log_message("FSA handle: %d", fsa);

    for (unsigned int i = 0; i < sizeof(devices) / sizeof(devices[0]); i++)
    {
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
            8);
        int raw_result = (int)out_buf[0];
        int handle = (int)out_buf[1];
        log_message(
            "%s -> ioctl=%d res=%d handle=%d\n",
            devices[i],
            result,
            raw_result,
            handle);

        if (result >= 0 && raw_result >= 0)
        {
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
         i++)
    {
        if (title_id == system_menu_title_ids[i])
        {
            is_system_menu = true;
            break;
        }
    }

    if (!is_system_menu || test_started)
    {
        return;
    }

    test_started = true;
    run_network_dump();
    log_message(
        "System Menu detected; WPS does not read the NAND. IOS-FS should start the native "
        "SLC dump; start the TCP receiver on port 4406 before launching nanddumper.");
}

INITIALIZE_PLUGIN()
{
    initialize_logging();
    run_test_for_current_title();
}

ON_APPLICATION_START()
{
    run_test_for_current_title();
}

DEINITIALIZE_PLUGIN()
{
    if (module_logging)
    {
        WHBLogModuleDeinit();
    }
    else
    {
        if (cafe_logging)
        {
            WHBLogCafeDeinit();
        }
        if (udp_logging)
        {
            WHBLogUdpDeinit();
        }
    }
}
