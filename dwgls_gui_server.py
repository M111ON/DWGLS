#!/usr/bin/env python3
"""
DWGLS GUI Server — local HTTP backend
═════════════════════════════════════
Serves HTML + API that calls DWGLS CLI tools.
File path: user types path OR browser uploads file via /api/upload.
"""

import json, os, socket, subprocess, sys, threading, time, urllib.parse, uuid
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
# DWGLS executables in build/
BUILD_DIR = SCRIPT_DIR / "build"
UPLOAD_DIR = SCRIPT_DIR / "uploads"
UPLOAD_DIR.mkdir(exist_ok=True)
PORT = 8081  # Different from FGLS (8080)

MIME = {'.html':'text/html; charset=utf-8','.css':'text/css; charset=utf-8',
        '.js':'application/javascript','.png':'image/png','.ico':'image/x-icon',
        '.json':'application/json','.svg':'image/svg+xml'}

def find_exec(name):
    c = BUILD_DIR / name
    return str(c) if c.is_file() else None

def fmt_size(n):
    if n < 1024: return f"{n:,} B"
    if n < 1048576: return f"{n/1024:.1f} KB"
    return f"{n/1048576:.2f} MB"

def resolve_path(p):
    """Resolve a path — could be upload-relative or absolute."""
    p = p.strip().strip('"').strip("'")
    if (UPLOAD_DIR / p).is_file(): return str(UPLOAD_DIR / p)
    if Path(p).is_file(): return p
    if (SCRIPT_DIR / p).is_file(): return str(SCRIPT_DIR / p)
    return p  # let the caller fail with a clear error

def run_cli(cmd, timeout=300):
    try:
        p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=str(SCRIPT_DIR))
        stdout, stderr = p.communicate(timeout=timeout)
        ok = (p.returncode == 0)
        return ok, stdout.decode('utf-8',errors='replace').strip(), stderr.decode('utf-8',errors='replace').strip()
    except subprocess.TimeoutExpired: p.kill(); return False, '', f'Timed out ({timeout}s)'
    except FileNotFoundError: return False, '', f'Not found: {cmd[0]}'

# ── API handlers ──

def _do_upload(args, body_bytes):
    """Save uploaded file bytes to uploads/ dir."""
    ct = args.get('_content_type', '')
    boundary = ''
    if 'boundary=' in ct:
        boundary = ct.split('boundary=')[1].split(';')[0].strip()
        if boundary.startswith('"') and boundary.endswith('"'): boundary = boundary[1:-1]
    if not boundary:
        fname = f"upload_{uuid.uuid4().hex[:12]}"
        dst = UPLOAD_DIR / fname
        dst.write_bytes(body_bytes)
        return {'ok': True, 'path': fname, 'size': len(body_bytes)}
    
    parts = body_bytes.split(b'--' + boundary.encode())
    saved = None
    for part in parts:
        if b'Content-Disposition' not in part: continue
        hdr_end = part.find(b'\r\n\r\n')
        if hdr_end < 0: continue
        headers_raw = part[:hdr_end].decode('utf-8', errors='replace')
        data = part[hdr_end+4:]
        if data.endswith(b'\r\n'): data = data[:-2]
        if data.endswith(b'--'): data = data[:-2]
        if data.endswith(b'\r\n'): data = data[:-2]
        fname = None
        if 'filename="' in headers_raw:
            fname = headers_raw.split('filename="')[1].split('"')[0]
        if not fname:
            fname = f"upload_{uuid.uuid4().hex[:12]}"
        fname = Path(fname).name
        dst = UPLOAD_DIR / fname
        if dst.exists():
            stem = dst.stem
            dst = UPLOAD_DIR / f"{stem}_{uuid.uuid4().hex[:6]}{dst.suffix}"
        dst.write_bytes(data)
        saved = {'ok': True, 'path': str(dst.relative_to(UPLOAD_DIR)), 'size': len(data), 'name': fname}
        break
    if saved: return saved
    return {'ok': False, 'error': 'No file data found in upload'}

def _do_rid_graft(args):
    """rid-graft: RID slots -> DtSlotRegion -> llama.cpp inference"""
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('geo_rid_graft.exe')
    if not exe: return {'ok': False, 'error': 'geo_rid_graft.exe not found in build/'}
    prompt = args.get('prompt', 'The capital of France is')
    tokens = args.get('tokens', '40')
    ok, out, err = run_cli([exe, src, prompt, tokens], timeout=600)
    return {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}

def _do_geofs_rid(args):
    """geofs-rid: GeoFS on RID slot region"""
    exe = find_exec('geofs_rid.exe')
    if not exe: return {'ok': False, 'error': 'geofs_rid.exe not found in build/'}
    ok, out, err = run_cli([exe], timeout=300)
    return {'ok': ok, 'output': out, 'error': err}

def _do_kv_rid(args):
    """kv-rid: llama KV/state <-> RID slot region"""
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('kv_rid_serve.exe')
    if not exe: return {'ok': False, 'error': 'kv_rid_serve.exe not found in build/'}
    prompt = args.get('prompt', 'The capital of France is')
    tokens = args.get('tokens', '24')
    ok, out, err = run_cli([exe, src, prompt, tokens], timeout=600)
    return {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}

def _do_gguf_roundtrip(args):
    """gguf-roundtrip: full GGUF file roundtrip through RID slots"""
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('gguf_roundtrip.exe')
    if not exe: return {'ok': False, 'error': 'gguf_roundtrip.exe not found in build/'}
    ok, out, err = run_cli([exe, src], timeout=600)
    return {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}

def _do_graft_belt(args):
    """graft-belt: token+logits stream -> field (+37 belt)"""
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('gguf_graft_belt.exe')
    if not exe: return {'ok': False, 'error': 'gguf_graft_belt.exe not found in build/'}
    prompt = args.get('prompt', 'The capital of France is')
    tokens = args.get('tokens', '40')
    ok, out, err = run_cli([exe, src, prompt, tokens], timeout=600)
    return {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}

def _do_graft_page(args):
    """graft-page: tokenizer KV -> field, graft header 5.9MB -> ~20KB"""
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('gguf_graft_page.exe')
    if not exe: return {'ok': False, 'error': 'gguf_graft_page.exe not found in build/'}
    prompt = args.get('prompt', 'The capital of France is')
    tokens = args.get('tokens', '40')
    ok, out, err = run_cli([exe, src, prompt, tokens], timeout=600)
    return {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}

def _do_lazy_serve(args):
    """lazy-serve: KV in memory, field windows mmap'd on demand"""
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('gguf_lazy_serve.exe')
    if not exe: return {'ok': False, 'error': 'gguf_lazy_serve.exe not found in build/'}
    prompt = args.get('prompt', 'The capital of France is')
    tokens = args.get('tokens', '40')
    ok, out, err = run_cli([exe, src, prompt, tokens], timeout=600)
    return {'ok': ok, 'output': out, 'error': err, 'input_file': os.path.basename(src), 'input_size': os.path.getsize(src)}

def _do_vis(args):
    """vis: FGLS_vis geometry visualizer + console"""
    # This launches the Python visualizer - run in background
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    # Just return info - actual vis runs separately
    return {'ok': True, 'output': f'Visualizer for: {src}\nRun: python tools/fgls_vis.py 5001 {src}', 'error': ''}

def _do_geo_kv_bench(args):
    """geo_kv_bench: KV benchmark with real model"""
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('geo_kv_real_bench.exe')
    if not exe: return {'ok': False, 'error': 'geo_kv_real_bench.exe not found in build/'}
    ok, out, err = run_cli([exe, src], timeout=600)
    return {'ok': ok, 'output': out, 'error': err}

def _do_geo_bench(args):
    """geo_bench: 3-way speed comparison (MAP vs Classic vs RAM)"""
    exe = find_exec('geo_speed_bench.exe')
    if not exe: return {'ok': False, 'error': 'geo_speed_bench.exe not found in build/'}
    ok, out, err = run_cli([exe], timeout=300)
    return {'ok': ok, 'output': out, 'error': err}

def _do_info(args):
    """info: dump GGUF model info"""
    src = resolve_path(args.get('path', ''))
    if not os.path.isfile(src): return {'ok': False, 'error': f'File not found: {src}'}
    exe = find_exec('gguf_dump.exe')
    if not exe: return {'ok': False, 'error': 'gguf_dump.exe not found in build/'}
    ok, out, err = run_cli([exe, src])
    return {'ok': ok, 'output': out, 'error': err, 'file': os.path.basename(src), 'size': os.path.getsize(src)}

def _do_ping(args):
    return {'ok': True, 'version': 'DWGLS GUI v1.0.0', 'pid': os.getpid()}

def _do_download(args):
    """Serve a file from uploads/ for download."""
    path = args.get('path', '')
    if not path: return {'ok': False, 'error': 'No path specified'}
    f = UPLOAD_DIR / Path(path).name
    if not f.is_file(): return {'ok': False, 'error': f'File not found: {f}'}
    return {'ok': True, '_file': str(f)}

ROUTES = {
    'upload': _do_upload, 'download': _do_download,
    'ping': _do_ping, 'info': _do_info,
    'rid_graft': _do_rid_graft, 'geofs_rid': _do_geofs_rid,
    'kv_rid': _do_kv_rid, 'gguf_roundtrip': _do_gguf_roundtrip,
    'graft_belt': _do_graft_belt, 'graft_page': _do_graft_page,
    'lazy_serve': _do_lazy_serve, 'vis': _do_vis,
    'geo_kv_bench': _do_geo_kv_bench, 'geo_bench': _do_geo_bench,
}

# ── HTTP ──

def handle_client(conn, addr):
    try:
        conn.settimeout(30)
        data = b''
        while b'\r\n\r\n' not in data:
            try:
                chunk = conn.recv(65536)
                if not chunk: break
                data += chunk
            except socket.timeout: break
            except: break
        if not data: conn.close(); return
        
        text = data.split(b'\r\n\r\n')[0].decode('utf-8', errors='replace')
        lines = text.split('\r\n')
        if not lines: conn.close(); return
        first = lines[0].split(' ')
        if len(first) < 2: conn.close(); return
        method, path_raw = first[0], first[1]
        parsed = urllib.parse.urlparse(path_raw)
        path = parsed.path.rstrip('/')
        
        cl = 0
        ct = ''
        for line in lines[1:]:
            l = line.lower()
            if l.startswith('content-length:'): cl = int(line.split(':')[1].strip())
            if l.startswith('content-type:'): ct = line.split(':',1)[1].strip()
        
        body = data.split(b'\r\n\r\n', 1)[1] if b'\r\n\r\n' in data else b''
        while len(body) < cl:
            try:
                chunk = conn.recv(65536)
                if not chunk: break
                body += chunk
            except: break
        
        if path.startswith('/api/'):
            endpoint = path[5:]
            qs = urllib.parse.parse_qs(parsed.query)
            args = {k: v[0] for k, v in qs.items()}
            if 'application/json' in ct and body:
                try: args.update(json.loads(body))
                except: pass
            elif ct: args['_content_type'] = ct
            
            handler = ROUTES.get(endpoint)
            if not handler:
                _send_json(conn, 404, {'error': f'Unknown: {endpoint}'})
                conn.close(); return
            
            try:
                if endpoint == 'upload':
                    result = handler(args, body)
                else:
                    result = handler(args)
                if '_file' in result:
                    fpath = result['_file']
                    try:
                        d = Path(fpath).read_bytes()
                        _send_raw(conn, 200, d, 'application/octet-stream')
                    except: _send_raw(conn, 404, b'Not Found')
                else:
                    _send_json(conn, 200, result)
            except Exception as e:
                _send_json(conn, 500, {'ok': False, 'error': str(e)})
            conn.close(); return
        
        if path == '' or path == '/': path = '/dwgls_gui.html'
        fpath = SCRIPT_DIR / path.lstrip('/')
        try:
            d = fpath.read_bytes()
            _send_raw(conn, 200, d, MIME.get(fpath.suffix.lower(), 'application/octet-stream'))
        except FileNotFoundError:
            _send_raw(conn, 404, b'Not Found')
        conn.close()
    except Exception as e:
        print(f'[ERR] {addr}: {e}')
        try: _send_raw(conn, 500, b'Internal Error')
        except: pass
        try: conn.close()
        except: pass

def _send_raw(conn, status, body, mime='application/octet-stream'):
    reasons = {200:'OK',400:'Bad Request',404:'Not Found',500:'Internal Server Error'}
    h = (f'HTTP/1.1 {status} {reasons.get(status,"?")}\r\n'
         f'Content-Type: {mime}\r\nContent-Length: {len(body)}\r\n'
         f'Access-Control-Allow-Origin: *\r\nConnection: close\r\n\r\n').encode()
    conn.sendall(h + body)

def _send_json(conn, status, data):
    _send_raw(conn, status, json.dumps(data, ensure_ascii=False).encode(), 'application/json; charset=utf-8')

def serve():
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(('127.0.0.1', PORT))
    s.listen(16)
    s.settimeout(1)
    print(f'DWGLS GUI at http://localhost:{PORT}')
    try:
        while True:
            try:
                conn, addr = s.accept()
                threading.Thread(target=handle_client, args=(conn, addr), daemon=True).start()
            except socket.timeout: continue
            except KeyboardInterrupt: break
    finally:
        s.close()
        print('Stopped.')

def open_browser():
    time.sleep(0.8)
    import webbrowser
    webbrowser.open(f'http://localhost:{PORT}')

if __name__ == '__main__':
    print('DWGLS GUI v1.0.0')
    for name in ('geo_rid_graft.exe', 'geofs_rid.exe', 'kv_rid_serve.exe', 'gguf_roundtrip.exe'):
        print(f'  {name:25s} {"✓" if (BUILD_DIR/name).is_file() else "✗"}')
    print()
    threading.Thread(target=open_browser, daemon=True).start()
    serve()