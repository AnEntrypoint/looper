with open('circle/Rules.mk', 'rb') as f:
    lines = f.readlines()

out = []
i = 0
patched_rwx = False

while i < len(lines):
    line = lines[i]
    stripped = line.rstrip(b'\r\n')

    # Remove the no-warn-rwx-segments ifneq/LDFLAGS/endif trio, emit LDFLAGS unconditionally
    if b'no-warn-rwx-segments' in stripped and stripped.lstrip().startswith(b'ifneq'):
        print(f'patching rwx line {i+1}: {repr(stripped)}')
        out.append(lines[i+1])  # emit LDFLAGS line unconditionally
        patched_rwx = True
        i += 3  # skip ifneq + LDFLAGS + endif

    else:
        out.append(line)
        i += 1

with open('circle/Rules.mk', 'wb') as f:
    f.writelines(out)

print('patch_rwx:', patched_rwx)

print('=== lines 230-244 after patch ===')
with open('circle/Rules.mk', 'rb') as f:
    for j, line in enumerate(f):
        if 229 <= j <= 243:
            print(f'  {j+1}: {repr(line.rstrip())}')
