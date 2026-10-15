import re
from collections import defaultdict

SECTOR_SIZE = 0x800

log_path = "Untitled.txt"
sd_path = "slc_stripped.bin"
net_path = "network-test.bin"

# Keep the last result for each exact read request.
requests = {}
unverified = set()

read_re = re.compile(
    r"READ CHECK sector=(\d+) count=(\d+) "
    r"attempt=(\d+) result=(-?\d+) ioctl=(-?\d+) raw=(-?\d+)"
)
unverified_re = re.compile(
    r"sector (\d+) reported DATA_CORRUPTED"
)

with open(log_path, "r", errors="replace") as f:
    for line in f:
        m = read_re.search(line)
        if m:
            sector, count, attempt, result, ioctl, raw = map(int, m.groups())
            key = (sector, count)
            if key not in requests or attempt >= requests[key][0]:
                requests[key] = (attempt, result, ioctl, raw)

        m = unverified_re.search(line)
        if m:
            unverified.add(int(m.group(1)))

failed_single = {
    sector for (sector, count), (_, result, _, _) in requests.items()
    if count == 1 and result < 0
}
successful_single = {
    sector for (sector, count), (_, result, _, _) in requests.items()
    if count == 1 and result >= 0
}

print("READ REQUEST SUMMARY")
print("Unique requests:", len(requests))
print("Single-sector requests ending negative:", len(failed_single))
print("Single-sector requests ending successful:", len(successful_single))
print("Sectors explicitly logged DATA_CORRUPTED:", len(unverified))

# Compare the reference and network dump sector by sector.
mismatches = set()
with open(sd_path, "rb") as sd, open(net_path, "rb") as net:
    sector = 0
    while True:
        a = sd.read(SECTOR_SIZE)
        b = net.read(SECTOR_SIZE)
        if not a or not b:
            break
        if a != b:
            mismatches.add(sector)
        sector += 1

print("\nDUMP COMPARISON")
print("Mismatching sectors:", len(mismatches))
print("Mismatches with failed single-sector reads:",
      len(mismatches & failed_single))
print("Mismatches with DATA_CORRUPTED log entries:",
      len(mismatches & unverified))
print("Mismatches with successful single-sector reads:",
      len(mismatches & successful_single))

def ranges(values):
    values = sorted(values)
    if not values:
        return []
    result = []
    start = previous = values[0]
    for value in values[1:]:
        if value != previous + 1:
            result.append((start, previous))
            start = value
        previous = value
    result.append((start, previous))
    return result

print("\nFirst 30 mismatching ranges:")
for start, end in ranges(mismatches)[:30]:
    print(f"{start}-{end} ({end - start + 1} sectors)")

print("\nFirst 30 failed single-sector read ranges:")
for start, end in ranges(failed_single)[:30]:
    print(f"{start}-{end} ({end - start + 1} sectors)")

print("\nFirst 30 unverified-sector ranges:")
for start, end in ranges(unverified)[:30]:
    print(f"{start}-{end} ({end - start + 1} sectors)")
