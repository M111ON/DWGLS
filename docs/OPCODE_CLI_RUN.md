# Running opencode CLI headless (non-interactive)

Verified working invocation for spawning a NEW opencode process from a shell
(proved 2026-08-20 for cross-session handoff).

## Why the naive invocation hangs

`opencode run` from a shell fails or hangs with these root causes:

1. **Missing auth** — the CLI defaults to providers that are not configured.
   - `zen` provider URL in `opencode.jsonc` is `https://opencode.ai/zen/v1/chat/completions`
     which double-appends `/chat/completions` → **404**.
   - The `$OPENCODE_API_KEY` / `$OPENCODE_KEY` env vars are not set in the shell
     context → **401 Invalid API key**.
   - The env `gemini` key is present but invalid → **400 API key not valid**.
   - Local providers (`agentrouter` :8080, `local` :8082, llama :11434) are usually
     not running → connection refused.
2. **MCP server startup is slow** — loading `playwright` (`npx -y @playwright/mcp`)
   and `colab` (`uvx git+...`) can take minutes on first spawn, so the run appears
   to hang even when the model itself would answer instantly.
3. **Desktop instance lock** — if OpenCode Desktop is already running, a second
   `opencode` process can conflict on the shared `opencode.db`, so the run never
   produces output.

## Working invocation (proved)

```powershell
$exe = "C:\nvm4w\nodejs\node_modules\opencode-ai\bin\opencode.exe"

# 1. Point at the Desktop OAuth credentials (this is what actually works)
$env:OPENCODE_AUTH = "I:\opencode\auth.json"

# 2. Use a minimal config (no heavy MCP servers)
$env:OPENCODE_CONFIG = "C:\Users\ADMINI~1.AVE\AppData\Local\Temp\opencode\minimal.jsonc"

# 3. Run headless with a free-plan model
$p = Start-Process -FilePath $exe -ArgumentList `
  'run','--pure','--auto','--format','json','-m','openai/gpt-5.5',"`"YOUR PROMPT`"" `
  -WorkingDirectory "I:\DWGLS-native-fs" `
  -RedirectStandardOutput $log -RedirectStandardError "$log.err" -PassThru -WindowStyle Hidden
```

The `minimal.jsonc` config used:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "zen": {
      "name": "OpenCode Zen",
      "npm": "@ai-sdk/openai-compatible",
      "options": {
        "baseURL": "https://opencode.ai/zen/v1",
        "apiKey": "$OPENCODE_API_KEY"
      },
      "models": { "deepseek-v4-flash-free": { "name": "DeepSeek V4 Flash Free" } }
    }
  }
}
```

## Key flags

| Flag | Purpose |
|------|---------|
| `--pure` | skip plugins (faster, less noise) |
| `--auto` | auto-approve permissions (non-interactive) |
| `--format json` | machine-readable JSON events on stdout |
| `-m openai/gpt-5.5` | free-plan OpenAI model (gpt-5.1/5.3-codex rejected on free plan) |
| `-c/--continue` / `-s --session` | continue an existing session id |

## Cross-session handoff pattern (proved)

Two separate `opencode run` processes continue one work unit through the
cloud workspace state (`ws_resume` reads `.workspace/current`):

- **Session A** (fresh process): writes section 1 + records marker file.
- **Session B** (another fresh process): calls `ws_resume` → loads state →
  sees "Session A wrote section 1" → appends section 2 → writes marker.

No chat history is copied between processes; the workspace state is the bridge.
Proof artifacts: `docs/OPCODE_CROSS_SESSION.md` (commit `b2fcc2e`).