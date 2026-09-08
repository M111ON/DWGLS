#!/usr/bin/env python3
"""
Lo Shu Tree Root — file system, formulas, routes.
Goal: define tree structure, find routes from root (n15=8) to leaf (n15=0).
"""

from itertools import permutations
from typing import List, Tuple, Dict, Set
import json

# ============================================================
# CORE: Grid operations
# ============================================================
def grid_to_tuple(grid) -> tuple:
    return tuple(tuple(row) for row in grid)

def tuple_to_grid(t) -> list:
    return [list(row) for row in t]

def magic_lines(grid, mc=15) -> int:
    n = len(grid)
    count = 0
    for i in range(n):
        if sum(grid[i]) == mc: count += 1
    for j in range(n):
        if sum(grid[i][j] for i in range(n)) == mc: count += 1
    if sum(grid[i][i] for i in range(n)) == mc: count += 1
    if sum(grid[i][n-1-i] for i in range(n)) == mc: count += 1
    return count

def single_swap(grid, r1, c1, r2, c2):
    """Swap two cells, return new grid."""
    g = [row[:] for row in grid]
    g[r1][c1], g[r2][c2] = g[r2][c2], g[r1][c1]
    return g

# ============================================================
# TREE: Node = grid state, Edge = single swap
# ============================================================
def build_tree_bfs(max_depth=8):
    """BFS from all 8 magic squares, track routes to n15=0."""
    mc = 15
    base = [[2,7,6],[9,5,1],[4,3,8]]
    
    # Generate all 8 D4 forms as roots
    def rot90(g): return [list(row) for row in zip(*g[::-1])]
    def rot180(g): return rot90(rot90(g))
    def rot270(g): return rot90(rot180(g))
    def mh(g): return [row[::-1] for row in g]
    def mv(g): return g[::-1]
    def md(g): return [list(row) for row in zip(*g)]
    def mad(g): return [list(row) for row in zip(*g[::-1])]
    
    roots = [base, rot90(base), rot180(base), rot270(base),
             mh(base), mv(base), md(base), mad(base)]
    
    root_keys = set()
    for r in roots:
        root_keys.add(grid_to_tuple(r))
    
    print(f"Roots: {len(root_keys)} magic squares (n15=8)")
    
    # BFS
    visited = {}  # grid_tuple -> (n15, parent_tuple, swap_op)
    queue = []
    
    for r in roots:
        key = grid_to_tuple(r)
        if key not in visited:
            visited[key] = (8, None, None)
            queue.append((r, key, 8, []))
    
    routes_to_zero = []
    depth_count = {8: len(root_keys)}
    
    print(f"Starting BFS from depth 0 (n15=8)...")
    
    while queue:
        grid, key, n15, route = queue.pop(0)
        
        if len(route) >= max_depth:
            continue
        
        # Generate all single swaps
        cells = [(i,j) for i in range(3) for j in range(3)]
        for idx1, (r1,c1) in enumerate(cells):
            for idx2, (r2,c2) in enumerate(cells):
                if idx1 >= idx2:
                    continue
                new_grid = single_swap(grid, r1, c1, r2, c2)
                new_key = grid_to_tuple(new_grid)
                new_n15 = magic_lines(new_grid, mc)
                
                if new_key not in visited:
                    new_route = route + [(r1,c1,r2,c2,n15,new_n15)]
                    visited[new_key] = (new_n15, key, (r1,c1,r2,c2))
                    
                    depth = len(new_route)
                    depth_count[depth] = depth_count.get(depth, 0) + 1
                    
                    if new_n15 == 0:
                        routes_to_zero.append(new_route)
                        if len(routes_to_zero) <= 5:
                            print(f"  Route to n15=0 found! depth={depth}, swaps={len(new_route)}")
                    
                    if new_n15 > 0 and depth < max_depth:
                        queue.append((new_grid, new_key, new_n15, new_route))
        
        if len(visited) % 1000 == 0:
            print(f"  visited={len(visited)}, queue={len(queue)}")
    
    return visited, routes_to_zero, depth_count

# ============================================================
# OUTPUT: Export tree structure to files
# ============================================================
def export_tree(visited, routes, depth_count):
    """Export tree structure to JSON files."""
    # Depth distribution
    print(f"\n{'='*60}")
    print("TREE STRUCTURE")
    print(f"{'='*60}")
    for d in sorted(depth_count.keys()):
        print(f"  Depth {d}: {depth_count[d]} nodes")
    print(f"  Total visited: {len(visited)}")
    print(f"  Routes to n15=0: {len(routes)}")
    
    # Show first 3 routes
    print(f"\n{'='*60}")
    print("SAMPLE ROUTES (root → leaf)")
    print(f"{'='*60}")
    for i, route in enumerate(routes[:3]):
        print(f"\nRoute {i+1}: {len(route)} swaps")
        for step, (r1,c1,r2,c2,n15_before,n15_after) in enumerate(route):
            print(f"  Step {step+1}: swap ({r1},{c1})↔({r2},{c2}) | n15: {n15_before}→{n15_after}")
    
    # Save to JSON
    tree_data = {
        "depth_distribution": depth_count,
        "total_nodes": len(visited),
        "routes_count": len(routes),
        "sample_routes": [
            [{"step": i+1, "swap": [r1,c1,r2,c2], "n15_before": nb, "n15_after": na}
             for i, (r1,c1,r2,c2,nb,na) in enumerate(route)]
            for route in routes[:10]
        ]
    }
    
    with open("tests/loshu_tree.json", "w") as f:
        json.dump(tree_data, f, indent=2)
    print(f"\nSaved: tests/loshu_tree.json")

# ============================================================
# FORMULA: Transition function
# ============================================================
def define_transition_formula():
    """Define the transition formula for the tree."""
    print(f"\n{'='*60}")
    print("TRANSITION FORMULA")
    print(f"{'='*60}")
    print("""
    State: 3×3 grid G, value set S = {a, a+d, ..., a+8d}
    Magic constant: MC = 3a + 12d
    
    Transition T(i,j,k,l): swap G[i][j] ↔ G[k][l]
    Result: G' = T(G), n15(G') = count_lines_sum_to_MC(G')
    
    Tree:
      Root: all G where n15(G) = 8 (magic squares, exactly 8 via D4)
      Node: any grid state G
      Edge: single swap T(i,j,k,l)
      Leaf: G where n15(G) = 0
    
    Route: sequence of swaps T₁,T₂,...,Tₙ such that
      n15(root) = 8 → n15(T₁(root)) → ... → n15(Tₙ(...)) = 0
    
    Branching factor: C(9,2) = 36 possible swaps per node
    """)

# ============================================================
# MAIN
# ============================================================
if __name__ == "__main__":
    define_transition_formula()
    visited, routes, depth_count = build_tree_bfs(max_depth=8)
    export_tree(visited, routes, depth_count)
