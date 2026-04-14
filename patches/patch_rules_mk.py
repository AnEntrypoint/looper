with open('circle/Rules.mk') as f:
    lines = f.readlines()

out = []
patched = False
for line in lines:
    stripped = line.rstrip('\r\n')
    if 'no-warn-rwx-segments' in stripped and stripped.lstrip().startswith('ifneq'):
        print(f'patching line: {repr(stripped)}')
        out.append('ifeq (0,1)\n')
        patched = True
    else:
        out.append(line)

with open('circle/Rules.mk', 'w') as f:
    f.writelines(out)

print('patch applied:', patched)

# Verify: print all shell lines after patching
print('=== shell/rwx lines after patch ===')
with open('circle/Rules.mk') as f:
    for i, line in enumerate(f):
        if 'shell' in line or 'rwx' in line or 'warn-rwx' in line:
            print(f'  {i+1}: {repr(line.rstrip())}')

if not patched:
    print('WARNING: target line not found')
