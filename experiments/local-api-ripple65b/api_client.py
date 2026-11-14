"""Local API client for the Ripple-65B model served via Molab/llama.cpp.

Usage:
    python api_client.py "Hello, how are you?"
    python api_client.py --interactive
    python api_client.py --endpoint http://localhost:8080 "test"

Configure endpoint via env var API_ENDPOINT or --endpoint flag.
Default: http://sb-3d3b98dce2d8b194.sb.molab.run:8080
"""
import argparse
import json
import os
import sys
import urllib.request
import urllib.error

DEFAULT_ENDPOINT = os.environ.get(
    "API_ENDPOINT", "http://sb-3d3b98dce2d8b194.sb.molab.run:8080"
)


def chat_completion(endpoint, messages, model="ripple", max_tokens=256, temperature=0.7, stream=False):
    url = f"{endpoint.rstrip('/')}/v1/chat/completions"
    payload = {
        "model": model,
        "messages": messages,
        "max_tokens": max_tokens,
        "temperature": temperature,
        "stream": stream,
    }
    data = json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(
        url, data=data, headers={"Content-Type": "application/json"}
    )
    try:
        with urllib.request.urlopen(req, timeout=300) as resp:
            body = json.loads(resp.read())
            return body["choices"][0]["message"]["content"]
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode()}", file=sys.stderr)
        return None
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        return None


def interactive_mode(endpoint):
    print(f"Connected to: {endpoint}")
    print("Type 'quit' to exit.\n")
    history = []
    while True:
        try:
            user_input = input("You: ").strip()
        except (EOFError, KeyboardInterrupt):
            print("\nBye!")
            break
        if user_input.lower() in ("quit", "exit", "q"):
            print("Bye!")
            break
        if not user_input:
            continue
        history.append({"role": "user", "content": user_input})
        print("Assistant: ", end="", flush=True)
        response = chat_completion(endpoint, history)
        if response:
            print(response)
            history.append({"role": "assistant", "content": response})
        else:
            print("(no response)")


def main():
    parser = argparse.ArgumentParser(description="Local API client for Ripple-65B")
    parser.add_argument("prompt", nargs="?", help="Single prompt to send")
    parser.add_argument("--endpoint", default=DEFAULT_ENDPOINT, help="API endpoint URL")
    parser.add_argument("--interactive", "-i", action="store_true", help="Interactive chat mode")
    parser.add_argument("--max-tokens", type=int, default=256)
    parser.add_argument("--temperature", type=float, default=0.7)
    parser.add_argument("--model", default="ripple")
    args = parser.parse_args()

    if args.interactive:
        interactive_mode(args.endpoint)
    elif args.prompt:
        messages = [{"role": "user", "content": args.prompt}]
        response = chat_completion(
            args.endpoint, messages, model=args.model,
            max_tokens=args.max_tokens, temperature=args.temperature,
        )
        if response:
            print(response)
    else:
        parser.print_help()


if __name__ == "__main__":
    main()