"""Local API server that proxies to the Molab-hosted Ripple-65B model.

Runs an OpenAI-compatible API on localhost, forwarding requests to the
Molab instance via the marimo execute API (since the Molab container's
port 8080 is not directly reachable from outside).

Usage:
    python local_api_server.py
    # Then use: curl http://localhost:8000/v1/chat/completions ...

Or set API_ENDPOINT env var to a directly reachable llama-server URL.
"""
import json
import os
import subprocess
import sys
import urllib.request
import urllib.error
from http.server import HTTPServer, BaseHTTPRequestHandler

DEFAULT_PORT = int(os.environ.get("LOCAL_API_PORT", "8000"))
DEFAULT_ENDPOINT = os.environ.get(
    "API_ENDPOINT", "http://sb-3d3b98dce2d8b194.sb.molab.run:8080"
)
EXECUTE_SCRIPT = os.path.join(
    os.path.dirname(__file__), ".agents", "skills", "marimo-pair", "scripts", "execute-code.sh"
)
# Fallback path for execute script
if not os.path.exists(EXECUTE_SCRIPT):
    EXECUTE_SCRIPT = r"C:\Users\Administrator.AVENTADOR\.agents\skills\marimo-pair\scripts\execute-code.sh"


def call_molab_api(messages, model="ripple", max_tokens=256, temperature=0.7):
    """Call the Molab-hosted API via the marimo execute API."""
    code = f'''
import json, urllib.request
payload = {{"model": {json.dumps(model)}, "messages": {json.dumps(messages)}, "max_tokens": {max_tokens}, "temperature": {temperature}, "stream": False}}
data = json.dumps(payload).encode()
req = urllib.request.Request("http://127.0.0.1:8080/v1/chat/completions", data=data, headers={{"Content-Type": "application/json"}})
try:
    with urllib.request.urlopen(req, timeout=300) as r:
        print(r.read().decode())
except Exception as e:
    print(json.dumps({{"error": str(e)}}))
'''
    # Write code to temp file
    import tempfile
    with tempfile.NamedTemporaryFile(mode="w", suffix=".py", delete=False) as f:
        f.write(code)
        tmp_path = f.name

    try:
        git_bash = r"C:\Program Files\Git\bin\bash.exe"
        result = subprocess.run(
            [git_bash, EXECUTE_SCRIPT, "--url",
             "https://sb-3d3b98dce2d8b194.sb.molab.run/", tmp_path],
            capture_output=True, text=True, timeout=360,
        )
        # Parse output - find JSON in stdout
        output = result.stdout.strip()
        # The output may contain other lines; find the JSON
        for line in output.splitlines():
            line = line.strip()
            if line.startswith("{"):
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    continue
        # Try parsing entire output
        try:
            return json.loads(output)
        except json.JSONDecodeError:
            return {"error": f"No valid JSON in output: {output[:500]}"}
    finally:
        os.unlink(tmp_path)


def call_direct_api(endpoint, messages, model="ripple", max_tokens=256, temperature=0.7):
    """Call a directly reachable llama-server API."""
    url = f"{endpoint.rstrip('/')}/v1/chat/completions"
    payload = {
        "model": model,
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
        "stream": False,
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data, headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=300) as resp:
            return json.loads(resp.read())
    except Exception as e:
        return {"error": str(e)}


class APIHandler(BaseHTTPRequestHandler):
    def do_POST(self):
        if self.path == "/v1/chat/completions":
            content_length = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(content_length)
            try:
                payload = json.loads(body)
            except json.JSONDecodeError:
                self.send_error(400, "Invalid JSON")
                return

            messages = payload.get("messages", [])
            model = payload.get("model", "ripple")
            max_tokens = payload.get("max_tokens", 256)
            temperature = payload.get("temperature", 0.7)

            # Try direct endpoint first, fall back to Molab
            endpoint = os.environ.get("API_ENDPOINT")
            if endpoint:
                result = call_direct_api(endpoint, messages, model, max_tokens, temperature)
            else:
                result = call_molab_api(messages, model, max_tokens, temperature)

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(result).encode())
        else:
            self.send_error(404, "Not Found")

    def do_GET(self):
        if self.path in ("/", "/health", "/v1/models"):
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps({
                "status": "ok",
                "endpoint": os.environ.get("API_ENDPOINT", "molab-execute"),
            }).encode())
        else:
            self.send_error(404, "Not Found")

    def log_message(self, format, *args):
        print(f"[{self.log_date_time_string()}] {format % args}")


def main():
    port = DEFAULT_PORT
    print(f"Starting local API server on port {port}...")
    print(f"Mode: {'direct (' + os.environ.get('API_ENDPOINT', '') + ')' if os.environ.get('API_ENDPOINT') else 'molab-execute'}")
    print(f"Test: curl http://localhost:{port}/health")
    print()
    server = HTTPServer(("0.0.0.0", port), APIHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutting down.")
        server.shutdown()


if __name__ == "__main__":
    main()