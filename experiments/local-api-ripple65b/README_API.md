# Local API Access for Ripple-65B

## Files

- `api_client.py` — CLI client to call the API
- `local_api_server.py` — OpenAI-compatible local proxy server

## Usage

### Option 1: CLI client (direct)

```bash
# Single prompt
python api_client.py "Hello, how are you?"

# Interactive chat
python api_client.py --interactive

# Custom endpoint (if you have direct access)
python api_client.py --endpoint http://localhost:8080 "test"
```

### Option 2: Local API server (OpenAI-compatible)

```bash
# Start the local server (default port 8000)
python local_api_server.py

# Then use with any OpenAI-compatible client:
curl http://localhost:8000/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":100}'

# Health check
curl http://localhost:8000/health
```

### Environment variables

| Variable | Description | Default |
|----------|-------------|---------|
| `API_ENDPOINT` | Direct llama-server URL (if accessible) | Molab execute API |
| `LOCAL_API_PORT` | Local server port | 8000 |

### How it works

The Molab instance runs llama-server on port 8080 internally, but this port
is not directly accessible from outside. The local API server uses the
marimo execute API to run chat completion requests on the Molab instance
and return results locally.

If you have direct access to the llama-server (e.g., via SSH tunnel or
port forwarding), set `API_ENDPOINT` to the direct URL for faster responses.

### Model download status

The Ripple-65B model (38.8 GB) is still downloading on Molab.
Check progress in the notebook's `ripple_download` cell.
Once complete, run the `ripple_serve` cell to start llama-server.
```

print("README created")