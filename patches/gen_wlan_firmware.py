import os, sys
fw = sys.argv[1]
out = sys.argv[2]
files = [
    ('wlan_bin', 'brcmfmac43455-sdio.bin'),
    ('wlan_txt', 'brcmfmac43455-sdio.txt'),
    ('wlan_clm', 'brcmfmac43455-sdio.clm_blob'),
]
with open(out, 'w') as f:
    f.write('#ifdef __cplusplus\nextern "C" {\n#endif\n')
    for sym, fname in files:
        data = open(os.path.join(fw, fname), 'rb').read()
        f.write('const unsigned char %s[] = {%s};\n' % (sym, ','.join(str(b) for b in data)))
        f.write('const unsigned long %s_size = %d;\n' % (sym, len(data)))
    f.write('#ifdef __cplusplus\n}\n#endif\n')
print('Generated', out, len(open(out).read()), 'bytes')
