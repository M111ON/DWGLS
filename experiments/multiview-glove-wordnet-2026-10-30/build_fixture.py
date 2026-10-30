#!/usr/bin/env python3
"""build_fixture.py — GloVe vectors + WordNet relations as one multi-view corpus.

Why this dataset: the earlier multi-view test (ann_eval_multiview.py) used two kmeans
partitions of the SAME vectors as its two "views", so the views could not disagree and
the relation result was an artifact. Here the two views are genuinely different:

  value view    : GloVe 50d vector, similarity = cosine
  relation view : WordNet graph edge (hypernym / hyponym / similar_to / part_of)

Ground truth for each class is objective and independent of the routing:
  value    query -> exact cosine top-k over every item
  relation query -> the WordNet neighbour set of the query word

Writes data/glove_wordnet.npz and prints the overlap between the two answer sets —
if that overlap is high the corpus cannot separate the classes and the test is vacuous.

    python build_fixture.py [--max-words 20000] [--dim 50]
"""
import argparse
import os
import zipfile

import numpy as np
from nltk.corpus import wordnet as wn

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "data")
ZIP = os.path.join(DATA, "glove.6B.zip")


def glove_path(dim):
    """data/glove.6B.{dim}d.txt, unzipped from the 6B archive on first use."""
    p = os.path.join(DATA, "glove.6B.%dd.txt" % dim)
    if not os.path.exists(p):
        zipfile.ZipFile(ZIP).extract("glove.6B.%dd.txt" % dim, DATA)
    return p


def out_path(dim):
    """50d keeps its original filename so existing runs stay valid."""
    return os.path.join(DATA, "glove_wordnet.npz" if dim == 50 else "glove_wordnet_%dd.npz" % dim)

REL_POINTERS = {"@": "hypernym", "~": "hyponym", "&": "similar_to", "%": "part_of"}


def load_glove(path, keep):
    vec, words = [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            parts = line.rstrip().split(" ")
            w = parts[0]
            if w not in keep:
                continue
            vec.append(np.asarray(parts[1:], dtype=np.float32))
            words.append(w)
    X = np.vstack(vec)
    X /= np.maximum(np.linalg.norm(X, axis=1, keepdims=True), 1e-9)
    return X, words


def wordnet_edges(vocab):
    """Undirected lemma->lemma edges from WordNet, restricted to vocab."""
    edges = set()
    for w in vocab:
        for syn in wn.synsets(w):
            lemmas = {l.name().lower() for l in syn.lemmas()}
            for lemma in lemmas:
                if lemma == w or lemma not in vocab:
                    continue
                edges.add((w, lemma))  # same synset = synonym edge
            for p in syn.hypernyms() + syn.hyponyms() + syn.similar_tos():
                for l in p.lemmas():
                    t = l.name().lower()
                    if t != w and t in vocab:
                        edges.add((w, t))
    return edges


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--max-words", type=int, default=20000)
    ap.add_argument("--dim", type=int, default=50, help="GloVe dimension: 50/100/200/300")
    ap.add_argument("--nq", type=int, default=500, help="queries per class")
    ap.add_argument("--k", type=int, default=10)
    args = ap.parse_args()

    glove = glove_path(args.dim)
    out = out_path(args.dim)

    # 1. candidate vocabulary: alphabetic GloVe words that WordNet knows
    keep = set()
    with open(glove, encoding="utf-8") as f:
        for line in f:
            w = line.split(" ", 1)[0]
            if w.isalpha() and w.islower():
                keep.add(w)
    wn_lemmas = {l.lower() for l in wn.all_lemma_names()}
    keep &= wn_lemmas
    print("GloVe alphabetic words that WordNet also has: %d" % len(keep))

    # 2. load vectors for a deterministic slice of that vocabulary
    ordered = sorted(keep)[: args.max_words]
    X, words = load_glove(glove, set(ordered))
    idx = {w: i for i, w in enumerate(words)}
    print("items with vectors: %d  dim=%d" % (len(words), X.shape[1]))

    # 3. relation graph
    edges = [(idx[a], idx[b]) for a, b in wordnet_edges(set(words)) if a in idx and b in idx]
    nbr = [[] for _ in words]
    for a, b in edges:
        nbr[a].append(b)
        nbr[b].append(a)
    deg = np.array([len(n) for n in nbr])
    print("edges: %d  mean degree: %.2f  isolated: %d"
          % (len(edges), deg.mean(), int((deg == 0).sum())))

    # 4. query sets
    rng = np.random.default_rng(20261030)
    eligible_rel = np.where(deg >= 3)[0]
    rel_q = rng.choice(eligible_rel, size=min(args.nq, len(eligible_rel)), replace=False)
    val_q = rng.choice(len(words), size=min(args.nq, len(words)), replace=False)

    # 5. how much do the two answer sets overlap? high overlap => test is vacuous
    ov = []
    for qi in val_q[:200]:
        cos = X @ X[qi]
        top = set(np.argsort(-cos)[: args.k])
        rel = set(nbr[qi])
        if rel:
            ov.append(len(top & rel) / len(top))
    print("overlap(cosine top-%d, wordnet neighbours) mean = %.3f" % (args.k, float(np.mean(ov))))

    np.savez_compressed(
        out,
        X=X.astype(np.float32),
        words=np.array(words),
        edges=np.array(edges, dtype=np.int32),
        deg=deg.astype(np.int32),
        value_queries=val_q.astype(np.int32),
        relation_queries=rel_q.astype(np.int32),
        k=np.int32(args.k),
    )
    print("wrote", out)


if __name__ == "__main__":
    main()
