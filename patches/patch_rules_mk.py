with open('circle/Rules.mk', 'rb') as f:
    lines = f.readlines()

target = b'no-warn-rwx-segments'
out = []
i = 0
patched = False
while i < len(lines):
    line = lines[i]
    stripped = line.rstrip(b'\r\n')
    # Match: ifneq (...shell...no-warn-rwx-segments...)
    # Remove this line + next LDFLAGS line + next endif line
    # Replace with just the unconditional LDFLAGS assignment
    if target in stripped and stripped.lstrip().startswith(b'ifneq'):
        print(f'patching line {i+1}: {repr(stripped)}')
        # peek at next two lines
        next1 = lines[i+1].rstrip(b'\r\n') if i+1 < len(lines) else b''
        next2 = lines[i+2].rstrip(b'\r\n') if i+2 < len(lines) else b''
        print(f'  next1: {repr(next1)}')
        print(f'  next2: {repr(next2)}')
        # Emit just the LDFLAGS line unconditionally (preserving its line ending)
        out.append(lines[i+1])
        patched = True
        i += 3  # skip ifneq + LDFLAGS + endif
    else:
        out.append(line)
        i += 1

with open('circle/Rules.mk', 'wb') as f:
    f.writelines(out)

print('patch applied:', patched)
if not patched:
    print('WARNING: target line not found')
    for j, line in enumerate(lines):
        if target in line:
            print(f'  {j+1}: {repr(line.rstrip())}')

print('=== lines 230-244 after patch ===')
with open('circle/Rules.mk', 'rb') as f:
    for j, line in enumerate(f):
        if 229 <= j <= 243:
            print(f'  {j+1}: {repr(line.rstrip())}')
