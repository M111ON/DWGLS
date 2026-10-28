from pathlib import Path
p=Path('test_prompt.txt')
s=p.read_text(encoding='utf-8-sig')
p.write_bytes(s.encode('utf-8'))
