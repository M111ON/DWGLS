## 1. Cloud Workspace System — Overview (by codex)

- Workspace pool lives at `cloud-memory-worker` and tracks active work units for cross-agent handoff.
- `ws_resume` loads the current workspace by following the repo pointer at `.workspace/current`.
- Write operations use a heartbeat-backed lease so only one agent writes at a time; stale leases expire after 10 minutes.
- Three agents are connected in this proof flow: `opencode`, `claude`, and `codex`.

## 2. Write-Lease Concurrency (by opencode)

- WRITE ops (update/checkpoint) require a heartbeat-backed lease; stale leases auto-expire after 10 minutes allowing takeover.
- READ/CREATE are never locked — reading and creating new workspaces/files are always allowed.
- Heartbeat extends the lease only for its owner; a non-owner heartbeat is recorded but does not refresh the lock (prevents lock-jacking).
- Verified live: agent B blocked with 409 while agent A held the lease; after release agent B writes succeed.

## 3. ws_resume Pointer Auto-Load (by closing agent)

- `.workspace/current` is the repo-local pointer to the active cloud workspace id, so agents do not need to search or guess state.
- `ws_resume` reads that pointer and loads the full workspace bundle in one call: context, decisions, files, variables, and next steps.
- Any agent can call `ws_resume` at boot to recover the current handoff state before touching files or making write decisions.
- Because state loads from the shared workspace, Codex/OpenCode/Claude can continue the same work unit without copying chat history between tools.
