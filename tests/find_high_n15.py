import json, re, numpy as np

def parse(text):
    parts = re.split(r'}\s*{', text)
    results = []
    for i, part in enumerate(parts):
        if i == 0: part += '}'
        elif i == len(parts) - 1: part = '{' + part
        else: part = '{' + part + '}'
        try: results.append(json.loads(part))
        except: pass
    return results

def count_n15(grid, mc):
    n = grid.shape[0]
    count = 0
    for i in range(n):
        if abs(np.sum(grid[i]) - mc) < 1e-6: count += 1
    for j in range(n):
        if abs(np.sum(grid[:, j]) - mc) < 1e-6: count += 1
    if abs(np.trace(grid) - mc) < 1e-6: count += 1
    if abs(np.trace(np.fliplr(grid)) - mc) < 1e-6: count += 1
    return count

def rank_order(values, n):
    needed = n * n
    chunk = np.array(values[:needed])
    sorted_idx = np.argsort(chunk)
    ranks = np.empty_like(sorted_idx, dtype=np.float64)
    ranks[sorted_idx] = np.arange(1, needed + 1)
    return ranks.reshape(n, n)

t1 = parse(open('I:/DWGLS-native-fs/tests/weights_raw.json').read())
t2 = parse(open('I:/DWGLS-native-fs/tests/kokoro_all.json').read())
all_t = t1 + t2

print(f'Total tensors: {len(all_t)}')
for t in all_t:
    w = t['weights']
    grid = rank_order(w, 3)
    n15 = count_n15(grid, 15)
    if n15 >= 3:
        print(f'{t["name"]}: n15={n15}')
        print(grid.astype(int))
        print()