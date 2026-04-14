with open('circle/Rules.mk') as f:
    content = f.read()

# Find and print the shell call lines for diagnosis
lines = content.split('\n')
print('=== lines containing shell or rwx before patch ===')
for i, line in enumerate(lines):
    if '$(shell' in line or 'rwx' in line or 'warn-rwx' in line:
        print(f'  {i+1}: {repr(line)}')

# Replace the hanging shell pipe with a hardcoded 1
# The original: ifneq ($(strip $(shell $(LD) --help | grep -F no-warn-rwx-segments | wc -l)),0)
# We unconditionally add --no-warn-rwx-segments since ld on Ubuntu 24.04 GCC 13 supports it
patched = False
out = []
for line in lines:
    stripped = line.rstrip('\r\n')
    if 'no-warn-rwx-segments' in stripped and stripped.lstrip().startswith('ifneq'):
        print(f'patching line: {repr(stripped)}')
        out.append('ifneq (1,0)')
        patched = True
    else:
        out.append(stripped)

with open('circle/Rules.mk', 'w') as f:
    f.write('\n'.join(out))

print('patch applied:', patched)
if not patched:
    print('WARNING: target line not found')

# Verify
print('=== lines 230-245 after patch ===')
with open('circle/Rules.mk') as f:
    for i, line in enumerate(f):
        if 229 <= i <= 244:
            print(f'  {i+1}: {repr(line.rstrip())}')
