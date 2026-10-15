with open("slc.bin", "rb") as src, open("slc_stripped.bin", "wb") as dst:
    while sector := src.read(2112):
        if len(sector) != 2112:
            raise ValueError("Incomplete sector")
        dst.write(sector[:2048])
print("Done")