---
title: "Torus knot is a constrained route template, not a shape"
author: M111ON
date: 2026-11-23
branch: feat/geo-native-fs
id: l-765017d0
scope: repo
applies: github.com/m111on/dwgls
status: active
src: agent
---

## Decided
- File: experiments/torus_knot_test.c (candidate route test).
- Torus knot (p,q) on 144x144 is a 144-cell bijection/route template, not a materialized shape; field 20736 is a logical address space, not a container that exists fully from the start (MAP not COMPRESS).
- Suitable only for value-blind constrained paths: KV cold prefetch, storage traversal, phase round counter, rail switching, tombstone/roster evidence. Not for ANN retrieval (torus wrap makes score non-monotonic and candidate set noisy).
- No primitive/test exists yet in repo; do not treat as shape/geometry.

## Tried and ruled out
- Using torus knot to replace ANN anchor route: rejected because ANN needs similarity-faithful candidates; wrap-around breaks monotonicity.

## Watch out
- Do not materialize owner drawings as files/HTML; translate to code only when asked.
- Any use as route needs topology roundtrip test before trust.

