with open('circle/Rules.mk', 'rb') as f:
    content = f.read()

target = b'no-warn-rwx-segments'
if target not in content:
    print('patch applied: False')
    print('WARNING: no-warn-rwx-segments not found in Rules.mk at all')
else:
    # Find and replace the entire ifneq line containing the shell pipe
    lines = content.split(b'\n')
    out = []
    patched = False
    for line in lines:
        if target in line and line.lstrip().startswith(b'ifneq'):
            print(f'patching line: {repr(line)}')
            out.append(b'ifneq (1,0)')
            patched = True
        else:
            out.append(line)
    result = b'\n'.join(out)
    with open('circle/Rules.mk', 'wb') as f:
        f.write(result)
    print('patch applied:', patched)
    if not patched:
        print('WARNING: line with no-warn-rwx-segments found but did not start with ifneq')
        for i, line in enumerate(lines):
            if target in line:
                print(f'  {i+1}: {repr(line)}')

# Verify
print('=== lines 230-245 after patch ===')
with open('circle/Rules.mk', 'rb') as f:
    for i, line in enumerate(f):
        if 229 <= i <= 244:
            print(f'  {i+1}: {repr(line.rstrip())}')
