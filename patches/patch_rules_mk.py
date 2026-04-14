with open('circle/Rules.mk', 'rb') as f:
    lines = f.readlines()

out = []
i = 0
patched_rwx = False
patched_simply = 0

while i < len(lines):
    line = lines[i]

    # Remove the no-warn-rwx-segments ifneq/LDFLAGS/endif trio, emit LDFLAGS unconditionally
    if b'no-warn-rwx-segments' in line and line.lstrip().startswith(b'ifneq'):
        print(f'patching rwx line {i+1}')
        out.append(lines[i+1])
        patched_rwx = True
        i += 3
        continue

    # Change recursive = to simply-expanded := for lines with $(shell ...)
    # This prevents re-evaluation on every variable reference (critical for -j4)
    if b'$(shell' in line and b' = ' in line and not line.lstrip().startswith(b'#'):
        new_line = line.replace(b' = ', b' := ', 1)
        print(f'patching simply-expanded line {i+1}')
        out.append(new_line)
        patched_simply += 1
        i += 1
        continue

    out.append(line)
    i += 1

with open('circle/Rules.mk', 'wb') as f:
    f.writelines(out)

print(f'patch_rwx: {patched_rwx}')
print(f'patch_simply_expanded: {patched_simply}')

print('=== shell lines after patch ===')
with open('circle/Rules.mk', 'rb') as f:
    for j, line in enumerate(f):
        if b'$(shell' in line:
            print(f'  {j+1}: {repr(line.rstrip())}')
