import json,time,urllib.request
p=open('test_prompt.txt',encoding='utf-8').read(); body=json.dumps({'prompt':p,'max_tokens':1}).encode(); t=time.perf_counter(); r=urllib.request.urlopen(urllib.request.Request('http://127.0.0.1:18105/v1/completions',data=body,headers={'Content-Type':'application/json'}),timeout=600); r.read(); print('wall_ms',round((time.perf_counter()-t)*1000))
