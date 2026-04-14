with open('circle/Rules.mk', 'rb') as f:
    content = f.read()

target = b'no-warn-rwx-segments'
if target not in content:
    print('patch applied: False')
    print('WARNING: no-warn-rwx-segments not found in Rules.mk at all')
else:
    # Replace the 3-line block:
    #   ifneq ($(strip $(shell $(LD) --help | grep -F no-warn-rwx-segments | wc -l)),0)
    #   LDFLAGS\t+= --no-warn-rwx-segments
    #   endif
    # With just:
    #   LDFLAGS\t+= --no-warn-rwx-segments
    # (unconditionally, since Ubuntu 24.04 GCC 13 ld always supports this flag)
    import re
    patched = re.sub(
        rb'ifneq \(\$\(strip \$\(shell[^\n]*no-warn-rwx-segments[^\n]*\)\n(LDFLAGS[^\n]*--no-warn-rwx-segments[^\n]*)\nendif',
        rb'\1',
        content
    )
    if patched != content:
        with open('circle/Rules.mk', 'wb') as f:
            f.write(patched)
        print('patch applied: True')
    else:
        print('patch applied: False')
        print('WARNING: regex did not match - printing context lines')
        for i, line in enumerate(content.split(b'\n')):
            if target in line or b'shell' in line:
                print(f'  {i+1}: {repr(line)}')

# Verify
print('=== lines 230-245 after patch ===')
with open('circle/Rules.mk', 'rb') as f:
    for i, line in enumerate(f):
        if 229 <= i <= 244:
            print(f'  {i+1}: {repr(line.rstrip())}')
