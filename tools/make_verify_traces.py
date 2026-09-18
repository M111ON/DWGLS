# make_verify_traces.py -- 100 math fix-traces: wrong->verify->fix (50) + right->verify->confirm (50)
# ChatML JSONL for Qwen2.5-Instruct SFT. Deterministic (seeded). Verifies by a DIFFERENT method.
import json, random, sys

SEED = int(sys.argv[2]) if len(sys.argv) > 2 else 20260918
N_FIX, N_OK = 50, 50
OUT = sys.argv[1] if len(sys.argv) > 1 else "verify_traces.jsonl"

SYS = ("You are a careful math assistant. Solve the problem, then ALWAYS verify "
       "your answer by recomputing with a different method before giving the final answer.")

def plausible_wrong(ans, rng):
    c = rng.choice(["offby1", "offby10", "digitswap", "sign"])
    s = str(abs(ans))
    if c == "offby1":
        return ans + rng.choice([-1, 1])
    if c == "offby10":
        return ans + rng.choice([-10, 10])
    if c == "digitswap" and len(s) >= 2:
        i = rng.randrange(len(s) - 1)
        t = list(s)
        t[i], t[i + 1] = t[i + 1], t[i]
        v = int("".join(t))
        return v if ans >= 0 else -v
    return -ans

def gen_add(rng):
    a, b = rng.randint(11, 999), rng.randint(11, 999)
    return (f"What is {a} + {b}?", a + b,
            f"{a} + {b} = {a + b}.",
            f"Check by subtraction: {a + b} - {a} = {b}. Correct.")

def gen_sub(rng):
    a, b = rng.randint(100, 999), rng.randint(11, 99)
    return (f"What is {a} - {b}?", a - b,
            f"{a} - {b} = {a - b}.",
            f"Check by addition: {a - b} + {b} = {a}. Correct.")

def gen_mul(rng):
    a, b = rng.randint(12, 99), rng.randint(3, 19)
    return (f"What is {a} x {b}?", a * b,
            f"{a} x {b} = {a * b}.",
            f"Check by division: {a * b} / {b} = {a}. Correct.")

def gen_div(rng):
    b = rng.randint(3, 19)
    q = rng.randint(12, 99)
    return (f"What is {b * q} / {b}?", q,
            f"{b * q} / {b} = {q}.",
            f"Check by multiplication: {q} x {b} = {b * q}. Correct.")

def gen_word(rng):
    n = rng.randint(3, 9)
    price = rng.randint(5, 50)
    total = n * price
    paid = ((total // 100) + 1) * 100
    return (f"A shopper buys {n} items at ${price} each and pays ${paid}. What is the change?",
            paid - total,
            f"Total = {n} x {price} = {total}. Change = {paid} - {total} = {paid - total}.",
            f"Check: change + total = {paid - total} + {total} = {paid}, matches amount paid. Correct.")

def gen_pct(rng):
    p = rng.choice([10, 15, 20, 25, 50])
    base = rng.choice([80, 120, 200, 240, 400, 600])
    return (f"What is {p}% of {base}?", base * p // 100,
            f"{p}% of {base} = {base * p // 100}.",
            f"Check by fraction: {p}/100 x {base} = {base * p // 100}. Correct.")

GENS = [gen_add, gen_sub, gen_mul, gen_div, gen_word, gen_pct]

def trace_fix(prob, ans, attempt_show, verify_show, rng):
    wrong = plausible_wrong(ans, rng)
    return (f"My first attempt: {attempt_show} So the answer is {wrong}.\n"
            f"Let me verify with a different method. {verify_show}\n"
            f"My first attempt was wrong. Final answer: {ans}")

def trace_ok(prob, ans, attempt_show, verify_show):
    return (f"My first attempt: {attempt_show} So the answer is {ans}.\n"
            f"Let me verify with a different method. {verify_show}\n"
            f"Verified. Final answer: {ans}")

def main():
    rng = random.Random(SEED)
    recs, seen = [], set()
    jobs = (["fix"] * N_FIX + ["ok"] * N_OK)
    rng.shuffle(jobs)
    gi = 0
    for kind in jobs:
        for _ in range(100):  # retry on dup problem
            g = GENS[gi % len(GENS)]; gi += 1
            prob, ans, attempt, verify = g(rng)
            if prob not in seen:
                seen.add(prob)
                break
        body = trace_fix(prob, ans, attempt, verify, rng) if kind == "fix" else trace_ok(prob, ans, attempt, verify)
        recs.append({"messages": [
            {"role": "system", "content": SYS},
            {"role": "user", "content": prob},
            {"role": "assistant", "content": body}]})
    with open(OUT, "w", encoding="utf-8") as f:
        for r in recs:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")
    nfix = sum(1 for r in recs if "was wrong" in r["messages"][2]["content"])
    print(f"wrote {OUT}: total={len(recs)} fix={nfix} ok={len(recs) - nfix} dup_problems={N_FIX + N_OK - len(seen)}")

if __name__ == "__main__":
    main()
