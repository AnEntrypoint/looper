import re

with open('circle/Rules.mk', 'rb') as f:
    content = f.read()

original = content

# Replace the hanging shell pipe with a static true condition
# Target: ifneq ($(strip $(shell $(LD) --help | grep -F no-warn-rwx-segments | wc -l)),0)
# Replace: ifneq (1,0)
patched = re.sub(
    rb'ifneq\s*\(\$\(strip\s*\$\(shell.*?no-warn-rwx-segments.*?\)\)',
    b'ifneq (1,0)',
    content
)

if patched != original:
    with open('circle/Rules.mk', 'wb') as f:
        f.write(patched)
    print('patch applied: True')
else:
    print('patch applied: False')
    print('WARNING: target line not found')
    print('=== all lines with shell or rwx ===')
    for i, line in enumerate(content.split(b'\n')):
        if b'shell' in line or b'rwx' in line:
            print(f'  {i+1}: {repr(line)}')

# Always verify
print('=== lines 230-245 after patch ===')
with open('circle/Rules.mk', 'rb') as f:
    for i, line in enumerate(f):
        if 229 <= i <= 244:
            print(f'  {i+1}: {repr(line.rstrip())}')
