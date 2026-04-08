import os, sys
fw = sys.argv[1]
out = sys.argv[2]
files = [
    ('wlan_bin', 'brcmfmac43455-sdio.bin'),
    ('wlan_txt', 'brcmfmac43455-sdio.txt'),
    ('wlan_clm', 'brcmfmac43455-sdio.clm_blob'),
]
with open(out, 'w') as f:
    f.write('.section .rodata\n')
    for sym, fname in files:
        path = os.path.join(fw, fname)
        size = os.path.getsize(path)
        abspath = os.path.abspath(path)
        f.write('.global %s\n' % sym)
        f.write('.type %s, %%object\n' % sym)
        f.write('%s:\n' % sym)
        f.write('.incbin "%s"\n' % abspath)
        f.write('.size %s, %d\n' % (sym, size))
        f.write('.global %s_size\n' % sym)
        f.write('.type %s_size, %%object\n' % sym)
        f.write('%s_size:\n' % sym)
        f.write('.long %d\n' % size)
        f.write('.size %s_size, 4\n' % sym)
print('Generated', out)
