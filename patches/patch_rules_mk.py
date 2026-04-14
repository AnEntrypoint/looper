import re
with open('circle/Rules.mk') as f:
    content = f.read()
# Print all shell lines for diagnostics
for i, line in enumerate(content.split('\n')):
    if 'shell' in line or 'rwx' in line:
        print(f'line {i+1}: {repr(line)}')
# Patch: replace the hanging ld --help pipe check with always-false
patched = re.sub(r'ifneq[^\n]*no-warn-rwx-segments[^\n]*', 'ifeq (0,1)', content)
# Also patch all $(shell ...) lines that invoke the linker help
patched = re.sub(r'ifneq[^\n]*\$\(shell[^\n]*--help[^\n]*\)', 'ifeq (0,1)', patched)
with open('circle/Rules.mk', 'w') as f:
    f.write(patched)
remaining = [l for l in patched.split('\n') if 'no-warn-rwx-segments' in l and l.strip().startswith('ifneq')]
print('patch applied:', len(remaining) == 0)
print('remaining ifneq rwx lines:', remaining)
