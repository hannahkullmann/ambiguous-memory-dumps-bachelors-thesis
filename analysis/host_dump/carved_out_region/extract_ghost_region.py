start = 0x339c00000
size = 3460300800

with open(r"C:\Users\hakul\Documents\Ambiguous_Memory_Dumps\vboxmanage\vboxmanage_ambig_dump.elf", "rb") as fin:
    fin.seek(start)
    data = fin.read(size)

with open(r"C:\Users\hakul\Documents\Ambiguous_Memory_Dumps\vboxmanage\extracted_ghost_cage_region.raw", "wb") as fout:
    fout.write(data)