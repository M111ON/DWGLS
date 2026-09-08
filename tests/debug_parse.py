#!/usr/bin/env python3
import re, json

for fname in ['smollm_all.json', 'qwen3_all.json', 'kokoro_all.json']:
    path = f'I:/DWGLS-native-fs/tests/{fname}'
    with open(path) as f:
        raw = f.read()
    lines = raw.strip().split('\n')
    # count JSON objects via regex
    count = len(re.findall(r'\{"name":', raw))
    print(f'{fname}: {len(lines)} lines, {len(raw)} chars, {count} JSON objects')

# Now test: what does json.loads on the full string do?
path = 'I:/DWGLS-native-fs/tests/smollm_all.json'
with open(path) as f:
    raw = f.read()
# Try splitting by {"name":
parts = raw.split('{"name":')
print(f'\nsmollm split by {{\"name\": : {len(parts)} parts')
# Each part after split needs {"name": prepended to be valid JSON
count = 0
for i, part in enumerate(parts):
    if not part.strip():
        continue
    try:
        obj = json.loads('{"name":' + part.split('}{"name":')[0] + '}')
        count += 1
    except:
        pass
print(f'Parseable objects: {count}')
