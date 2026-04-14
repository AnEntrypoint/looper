import re
with open('circle/Rules.mk') as f:
    content = f.read()
patched = re.sub(r'ifneq \(\$\(strip \$\(shell \$\(LD\).*no-warn-rwx-segments.*', 'ifeq (0,1)', content)
with open('circle/Rules.mk', 'w') as f:
    f.write(patched)
print('patch applied:', 'no-warn-rwx-segments' not in patched)
