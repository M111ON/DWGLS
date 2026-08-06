#!/usr/bin/env python3
"""
FGLS_UI — DWGLS Geometry Console (visualizer + interactive UI)

Usage:
    python tools/fgls_vis.py [port] [gguf_path]
    → http://127.0.0.1:5001

Endpoints:
    /               HTML console
    /api/info       ?path=   tensor list
    /api/tensor     ?path=&idx=&n=   weight stats
    /api/roundtrip  ?path=&idx=&n=   adaptive store roundtrip
"""
import sys, os, json, subprocess, urllib.parse
from http.server import HTTPServer, BaseHTTPRequestHandler

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 5001
DEFAULT_GGUF = sys.argv[2] if len(sys.argv) > 2 else ""
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build")
MODEL_DIR = os.environ.get("MODEL_DIR", "I:/model")

# ── Geometry Types (from geo_param_grid.h) ──────────────
GEO_TYPES = [
    {"id": 0,  "name": "GEO_DODEC_BASE",      "verts": 20,   "edges": 30,   "faces": 12,   "cells": 1,   "note": "dodecahedron (root)"},
    {"id": 1,  "name": "GEO_ICO_BASE",         "verts": 20,   "edges": 30,   "faces": 20,   "cells": 1,   "note": "icosahedron (dual)"},
    {"id": 2,  "name": "GEO_COMPOUND_24",      "verts": 24,   "edges": 48,   "faces": 24,   "cells": 6,   "note": "inverted dodeca compound"},
    {"id": 3,  "name": "GEO_DODEC_EDGES",      "verts": 30,   "edges": 60,   "faces": 32,   "cells": 1,   "note": "edge-based"},
    {"id": 4,  "name": "GEO_COMPOUND_60",      "verts": 60,   "edges": 90,   "faces": 32,   "cells": 1,   "note": "pentakis dodeca"},
    {"id": 5,  "name": "GEO_PENTAKIS_72",      "verts": 72,   "edges": 90,   "faces": 32,   "cells": 1,   "note": "12 base + 60 pyramids"},
    {"id": 6,  "name": "GEO_GOLDBERG_92",      "verts": 92,   "edges": 270,  "faces": 92,   "cells": 1,   "note": "goldberg dual"},
    {"id": 7,  "name": "GEO_COMP_SPIKE_120",   "verts": 120,  "edges": 180,  "faces": 62,   "cells": 1,   "note": "spike compound"},
    {"id": 8,  "name": "GEO_GOLDBERG_132",     "verts": 132,  "edges": 270,  "faces": 92,   "cells": 1,   "note": "goldberg level 2"},
    {"id": 9,  "name": "GEO_COMPOUND_144",     "verts": 144,  "edges": 576,  "faces": 576,  "cells": 144, "note": "★ 6ico = 18tes (protagonist)"},
    {"id": 10, "name": "GEO_GOLDBERG_192",     "verts": 192,  "edges": 270,  "faces": 92,   "cells": 1,   "note": "goldberg level 3"},
]
GEAR_GEO_FULL = 20736

# ── gguf_tool backend ────────────────────────────────────
def ensure_tool():
    cand = [os.path.join(BUILD, "gguf_tool.exe"), os.path.join(BUILD, "gguf_tool"),
            os.path.join(ROOT, "build", "gguf_tool.exe")]
    for p in cand:
        if os.path.exists(p):
            return p
    src = os.path.join(ROOT, "tools", "gguf_tool.c")
    hdr_dir = os.path.join(ROOT, ".hermes", "desktop-attachments")
    os.makedirs(BUILD, exist_ok=True)
    out = os.path.join(BUILD, "gguf_tool.exe")
    r = subprocess.run(
        ["gcc", "-O2", f"-I{ROOT}", f"-I{ROOT}/core", f"-I{ROOT}/core/infra",
         f"-I{hdr_dir}", "-o", out, src, "-lm"],
        capture_output=True, text=True)
    return out if os.path.exists(out) else None

TOOL = ensure_tool()

def run_tool(mode, path, idx="0", n="1024"):
    if not TOOL:
        return {"error": "gguf_tool not built"}
    try:
        r = subprocess.run([TOOL, mode, path, str(idx), str(n)],
                           capture_output=True, text=True, timeout=60)
        return json.loads(r.stdout) if r.stdout.strip() else {"error": "empty output"}
    except Exception as e:
        return {"error": str(e)}

def list_models():
    if not os.path.isdir(MODEL_DIR):
        return []
    return sorted(os.path.join(MODEL_DIR, f) for f in os.listdir(MODEL_DIR)
                  if f.endswith(".gguf"))

# ── HTML page ────────────────────────────────────────────
def gen_html():
    models = list_models()
    current = DEFAULT_GGUF if DEFAULT_GGUF and os.path.exists(DEFAULT_GGUF) else (models[0] if models else "")
    current_n = current.replace("\\", "/")
    model_opts = "".join(
        f'<option value="{m}"{" selected" if m.replace(chr(92), "/") == current_n else ""}>{os.path.basename(m)} ({os.path.getsize(m)/1048576:.0f}MB)</option>'
        for m in models)

    geo_rows = ""
    for g in GEO_TYPES:
        bar_w = int(g["verts"] / 20 * 60)
        cls = ' class="protagonist"' if g["id"] == 9 else ""
        geo_rows += (f'<tr{cls}><td>{g["id"]}</td><td class="mono">{g["name"]}</td>'
                     f'<td>{g["verts"]}</td><td>{g["edges"]}</td><td>{g["faces"]}</td>'
                     f'<td>{g["cells"]}</td><td><div class="bar" style="width:{bar_w}px"></div></td>'
                     f'<td>{g["note"]}</td></tr>')

    grid_cells = ""
    for idx in range(20736):
        if idx < 144: color = "#4ecdc4"
        elif idx < 576: color = "#45b7d1"
        else:
            t = (idx - 576) / (20736 - 576)
            color = f"rgb({int(50+t*150)},{int(80+t*100)},{int(200-t*100)})"
        grid_cells += f'<div class="grid-cell" style="background:{color}" title="addr {idx}"></div>'

    css = """
:root{--bg:#0a0e17;--fg:#e0e0e0;--accent:#4ecdc4;--accent2:#ff6b6b;--card:#131a2a;--border:#1e2a3a;--ok:#2ecc71}
*{margin:0;padding:0;box-sizing:border-box}
body{background:var(--bg);color:var(--fg);font-family:'Inter',-apple-system,sans-serif}
.header{padding:16px 30px;border-bottom:1px solid var(--border);display:flex;align-items:center;gap:20px;flex-wrap:wrap}
.header h1{font-size:1.3em}.header h1 span{color:var(--accent)}.subtitle{color:#888;font-size:0.85em}
.tabs{display:flex;gap:4px;padding:0 30px;margin-top:16px}
.tab{padding:8px 16px;background:var(--card);border:1px solid var(--border);border-radius:6px 6px 0 0;cursor:pointer;font-size:0.85em}
.tab.active{background:var(--accent);color:#000;font-weight:600}
.container{padding:20px 30px}.panel{display:none}.panel.active{display:block}
.card{background:var(--card);border:1px solid var(--border);border-radius:8px;padding:16px;margin-bottom:16px}
.card h3{color:var(--accent);margin-bottom:10px;font-size:0.95em}
table{width:100%;border-collapse:collapse;font-size:0.82em}
th{text-align:left;padding:8px 10px;background:var(--card);color:var(--accent);border-bottom:2px solid var(--accent);position:sticky;top:0}
td{padding:6px 10px;border-bottom:1px solid var(--border)}
tr:hover{background:rgba(78,205,196,0.05)}tr.protagonist{background:rgba(78,205,196,0.12)}
.mono{font-family:Consolas,monospace;font-size:0.9em}
.bar{height:14px;background:var(--accent);border-radius:2px;min-width:2px}
.stat{display:inline-block;margin-right:24px}.stat .val{font-size:1.8em;font-weight:700;color:var(--accent);font-family:Consolas,monospace}
.stat .lbl{font-size:0.75em;color:#888}.scroll-table{max-height:500px;overflow-y:auto}
select,button,input{background:var(--card);color:var(--fg);border:1px solid var(--border);border-radius:6px;padding:8px 12px;font-size:0.85em}
button{cursor:pointer}button:hover{border-color:var(--accent)}
button.primary{background:var(--accent);color:#000;font-weight:600;border:none}
.toolbar{display:flex;gap:10px;align-items:center;flex-wrap:wrap}
#log{font-family:Consolas,monospace;font-size:0.8em;color:#9f9;background:#0d1210;border:1px solid var(--border);border-radius:6px;padding:10px;height:260px;overflow-y:auto;white-space:pre-wrap}
#log .fail{color:#f66}
#tensor-filter{width:240px}
.tensor-row{cursor:pointer}.tensor-row.selected{background:rgba(78,205,196,0.15)}
.grid-container{display:grid;grid-template-columns:repeat(144,1fr);gap:0}
.grid-cell{aspect-ratio:1;cursor:crosshair}.grid-cell:hover{transform:scale(2.5);z-index:10;outline:2px solid #fff}
#addr-tooltip{position:fixed;background:#000;color:#fff;padding:6px 10px;border-radius:4px;font-family:Consolas,monospace;font-size:0.8em;pointer-events:none;display:none;z-index:100}
.pill{display:inline-block;padding:2px 10px;border-radius:12px;font-size:0.75em;font-weight:600}
.pill.ok{background:rgba(46,204,113,.2);color:var(--ok)}.pill.fail{background:rgba(255,107,107,.2);color:var(--accent2)}
.kis-bar{display:flex;height:40px;border-radius:4px;overflow:hidden;margin:10px 0}
.kis-seg{display:flex;align-items:center;justify-content:center;font-size:0.7em;font-weight:600}
pre{color:#aaa;font-size:0.8em;line-height:1.6}
"""

    js = r"""
let CURRENT_MODEL=''; let CURRENT_IDX=0; let TENSOR_LIST=[];
function esc(s){return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');}
function showTab(el){
  document.querySelectorAll('.panel').forEach(p=>p.classList.remove('active'));
  document.querySelectorAll('.tab').forEach(t=>t.classList.remove('active'));
  document.getElementById('panel-'+el.dataset.tab).classList.add('active');
  el.classList.add('active');
}
function log(msg,cls=''){
  const l=document.getElementById('log');const d=document.createElement('div');
  d.className=cls;d.textContent='['+new Date().toLocaleTimeString()+'] '+msg;
  l.appendChild(d);l.scrollTop=l.scrollHeight;
}
function loadModel(){
  const sel=document.getElementById('model-select');CURRENT_MODEL=sel.value;CURRENT_IDX=0;
  log('Loading model: '+CURRENT_MODEL);
  fetch('/api/info?path='+encodeURIComponent(CURRENT_MODEL))
    .then(r=>r.json()).then(d=>{
      if(d.error){log('Load failed: '+d.error,'fail');return}
      TENSOR_LIST=d.tensors||[];
      renderTensors(TENSOR_LIST);
      const mb=d.file_size?(d.file_size/1048576).toFixed(1):'?';
      document.getElementById('model-stats').textContent=mb+' MB · '+TENSOR_LIST.length+' tensors';
      log('Loaded '+TENSOR_LIST.length+' tensors');
    }).catch(e=>log('Load failed: '+e,'fail'));
}
function renderTensors(tensors){
  const w=document.getElementById('tensor-tbody');w.innerHTML='';
  tensors.forEach((t,i)=>{
    const tr=document.createElement('tr');tr.className='tensor-row';tr.dataset.idx=t.idx;
    tr.innerHTML='<td class="mono">'+t.idx+'</td><td class="mono">'+esc(t.name)+
      '</td><td>'+(t.size/1048576).toFixed(1)+' MB</td><td id="st-'+t.idx+'">—</td>';
    tr.onclick=()=>selectTensor(t.idx,tr);
    w.appendChild(tr);
  });
}
function filterTensors(){
  const q=document.getElementById('tensor-filter').value.toLowerCase();
  document.querySelectorAll('#tensor-tbody tr').forEach(r=>{
    r.style.display=r.children[1].textContent.toLowerCase().includes(q)?'':'none';
  });
}
function selectTensor(idx,tr){
  document.querySelectorAll('.tensor-row.selected').forEach(s=>s.classList.remove('selected'));
  tr.classList.add('selected');CURRENT_IDX=idx;
}
function analyzeTensor(){
  log('Analyze tensor #'+CURRENT_IDX);
  fetch('/api/tensor?path='+encodeURIComponent(CURRENT_MODEL)+'&idx='+CURRENT_IDX+'&n=1024')
    .then(r=>r.json()).then(d=>{
      if(d.error){log('Error: '+d.error,'fail');return}
      document.getElementById('analysis-result').innerHTML=
        '<div class="stat"><div class="val">'+d.distinct_buckets+'</div><div class="lbl">distinct ×100-buckets</div></div>'+
        '<div class="stat"><div class="val">'+(+d.min).toFixed(4)+'</div><div class="lbl">min</div></div>'+
        '<div class="stat"><div class="val">'+(+d.max).toFixed(4)+'</div><div class="lbl">max</div></div>'+
        '<div class="stat"><div class="val">'+(+d.mean).toFixed(5)+'</div><div class="lbl">mean</div></div>'+
        '<div class="stat"><div class="val">'+(+d.spread).toFixed(4)+'</div><div class="lbl">spread</div></div>'+
        '<p style="margin-top:8px;font-size:0.8em;color:#888">'+esc(d.name)+' · decoded '+d.decoded+' weights</p>';
      log('Analyzed: '+d.name+' min='+d.min+' max='+d.max);
    }).catch(e=>log('Analyze failed: '+e,'fail'));
}
function roundtripTensor(){
  log('Roundtrip tensor #'+CURRENT_IDX);
  const btn=document.getElementById('btn-roundtrip');btn.disabled=true;
  fetch('/api/roundtrip?path='+encodeURIComponent(CURRENT_MODEL)+'&idx='+CURRENT_IDX+'&n=256')
    .then(r=>r.json()).then(d=>{
      if(d.error){log('Error: '+d.error,'fail');return}
      const ok=d.lossless==1&&d.container_ok==1;
      document.getElementById('analysis-result').innerHTML=
        '<div class="stat"><div class="val">'+d.weights+'</div><div class="lbl">weights</div></div>'+
        '<div class="stat"><div class="val">'+d.entropy+'</div><div class="lbl">entropy</div></div>'+
        '<div class="stat"><div class="val">'+d.container_size+'</div><div class="lbl">container bytes</div></div>'+
        '<div class="stat"><div class="val"><span class="pill '+(ok?'ok':'fail')+'">'+(ok?'LOSSLESS':'FAIL')+'</span></div>'+
        '<div class="lbl">write='+d.write+' verify='+d.verify+' read='+d.read+'</div></div>';
      const cell=document.getElementById('st-'+CURRENT_IDX);
      if(cell)cell.innerHTML='<span class="pill ok">ok</span>';
      log('Roundtrip: lossless='+ok,ok?'':'fail');
    }).catch(e=>log('Roundtrip failed: '+e,'fail'))
    .finally(()=>btn.disabled=false);
}
function randomTensor(){
  if(!TENSOR_LIST.length){log('No model loaded','fail');return}
  const t=TENSOR_LIST[Math.floor(Math.random()*TENSOR_LIST.length)];
  CURRENT_IDX=t.idx;
  document.querySelectorAll('.tensor-row').forEach(tr=>{
    tr.classList.toggle('selected',tr.dataset.idx==t.idx);
  });
  analyzeTensor();
}
document.addEventListener('DOMContentLoaded',()=>{
  const tt=document.getElementById('addr-tooltip');
  document.querySelectorAll('.grid-cell').forEach((cell,i)=>{
    cell.addEventListener('mouseenter',()=>{
      const x=i%144,y=Math.floor(i/144);
      tt.textContent='['+x+','+y+'] addr='+i;tt.style.display='block';
    });
    cell.addEventListener('mousemove',e=>{tt.style.left=(e.clientX+12)+'px';tt.style.top=(e.clientY-30)+'px'});
    cell.addEventListener('mouseleave',()=>tt.style.display='none');
  });
  const sel=document.getElementById('model-select');
  if(sel.value)loadModel();
});
"""

    return f"""<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>FGLS_UI — DWGLS Geometry Console</title>
<style>{css}</style></head>
<body>
<div class="header">
  <h1>FGLS<span>_UI</span></h1>
  <div class="subtitle">DWGLS 4D Geometry + KIS Timeline — Console</div>
  <div style="margin-left:auto;font-size:0.8em;color:#666">GEAR_GEO_FULL = {GEAR_GEO_FULL}</div>
</div>
<div class="tabs">
  <div class="tab active" data-tab="console" onclick="showTab(this)">Console</div>
  <div class="tab" data-tab="geometry" onclick="showTab(this)">Geometry</div>
  <div class="tab" data-tab="grid" onclick="showTab(this)">20736 Grid</div>
  <div class="tab" data-tab="kis" onclick="showTab(this)">KIS Timeline</div>
</div>
<div class="container">

<div id="panel-console" class="panel active">
  <div class="card"><h3>Model</h3>
    <div class="toolbar">
      <select id="model-select" onchange="loadModel()">{model_opts}</select>
      <span id="model-stats" style="color:#888;font-size:0.8em"></span>
    </div>
  </div>
  <div class="card"><h3>Tensors <span style="color:#888;font-weight:normal">(click to select)</span></h3>
    <div class="toolbar" style="margin-bottom:10px">
      <input id="tensor-filter" placeholder="filter tensor name..." oninput="filterTensors()">
    </div>
    <div class="scroll-table" style="max-height:280px">
      <table><thead><tr><th>#</th><th>Name</th><th>Size</th><th>Status</th></tr></thead>
      <tbody id="tensor-tbody"></tbody></table>
    </div>
  </div>
  <div class="card"><h3>Analysis</h3>
    <div class="toolbar">
      <button id="btn-tensor" class="primary" onclick="analyzeTensor()">Analyze weights</button>
      <button id="btn-roundtrip" onclick="roundtripTensor()">Roundtrip verify (lossless)</button>
      <button id="btn-random" onclick="randomTensor()">Random tensor</button>
    </div>
    <div id="analysis-result" style="margin-top:12px"></div>
  </div>
  <div class="card"><h3>Log</h3><div id="log"></div></div>
</div>

<div id="panel-geometry" class="panel">
  <div class="card"><h3>Parameterized Geometry — Dodeca Root Family</h3>
    <div class="scroll-table"><table>
      <tr><th>ID</th><th>Name</th><th>Verts</th><th>Edges</th><th>Faces</th><th>Cells</th><th>Scale</th><th>Note</th></tr>
      {geo_rows}
    </table></div>
  </div>
  <div class="card"><h3>Key Numbers</h3>
    <ul style="color:#aaa;font-size:0.85em;line-height:1.8;padding-left:20px">
      <li>12 × 12 = 144 (protagonist verts)</li><li>144 × 144 = 20736 (full address space)</li>
      <li>128 × 162 = 20736</li><li>6ico = 18 tesseracts (144÷8)</li>
      <li>Stride-37: gcd(37,v)=1 for ALL GeoTypes — 0 collisions</li>
    </ul></div>
</div>

<div id="panel-grid" class="panel">
  <div class="card"><h3>20736 Address Space (144×144)</h3>
    <div style="display:flex;gap:16px;font-size:0.8em;margin-bottom:10px">
      <span style="color:#4ecdc4">■ row 0 protagonist</span>
      <span style="color:#45b7d1">■ edges 0-575</span>
      <span style="color:rgb(50,80,200)">■ interior gradient</span>
    </div>
    <div class="grid-container">{grid_cells}</div>
  </div>
</div>

<div id="panel-kis" class="panel">
  <div class="card"><h3>KIS Timeline — Balance Scale</h3>
    <div class="kis-bar">
      <div class="kis-seg" style="flex:1;background:#ff6b6b">∞ contraction</div>
      <div class="kis-seg" style="flex:.5;background:#ffd93d">← 0 →</div>
      <div class="kis-seg" style="flex:1;background:#4ecdc4">expansion ∞</div>
    </div>
    <pre>
Enter ANYWHERE on 0-20736
Forward  = expansion (spike → more vertices)
Backward = contraction (seal → fewer vertices)
6 values same position = 6 data points, different topology
Direction = value = path data came from</pre>
  </div>
  <div class="card"><h3>Adaptive Store Tiers</h3>
    <pre>
Tier 0 (seal):   tick 12  — frozen
Tier 1 (normal): tick 1-11 — active
Tier 2 (spike):  tick 0    — expansion entry
Tier 3 (bridge): overflow  — 324 blocks max
FRAME_SIZE = 1440 (10 ticks × 144 slots)
FS_PIPES   = 1728 (12³ = pipe connection)
FS_TICKS   = 12</pre>
  </div>
</div>
</div>
<div id="addr-tooltip"></div>
<script>{js}</script>
</body></html>"""

# ── Server ────────────────────────────────────────────────
class VisHandler(BaseHTTPRequestHandler):
    def send_json(self, obj):
        body = json.dumps(obj).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        try:
            path = self.path.split("?")[0]
            q = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(self.path).query))

            if path in ("/", "/index.html"):
                body = gen_html().encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif path == "/api/info":
                p = q.get("path", "")
                info = run_tool("info", p)
                info["file_size"] = os.path.getsize(p) if os.path.exists(p) else 0
                self.send_json(info)
            elif path == "/api/tensor":
                self.send_json(run_tool("tensor", q.get("path", ""), q.get("idx", "0"), q.get("n", "1024")))
            elif path == "/api/roundtrip":
                self.send_json(run_tool("roundtrip", q.get("path", ""), q.get("idx", "0"), q.get("n", "256")))
            else:
                self.send_response(404)
                self.end_headers()
        except Exception as e:
            self.send_json({"error": str(e)})

    def log_message(self, fmt, *args):
        pass

if __name__ == "__main__":
    print(f"FGLS_UI → http://127.0.0.1:{PORT}")
    if DEFAULT_GGUF:
        print(f"GGUF: {DEFAULT_GGUF}")
    print(f"gguf_tool: {TOOL or 'NOT BUILT'}")
    HTTPServer(("127.0.0.1", PORT), VisHandler).serve_forever()
