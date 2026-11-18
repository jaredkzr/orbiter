#!/usr/bin/env python3
# Copyright (c) Martin Schweiger
# Licensed under the MIT License

"""A small web viewer for the base screenshots (out/baseshots, made by
shots.py): pick a shot, then two labels to compare side by side, with a wipe
slider, or flipped in place; or all labels of a shot at once. The folder is
read on every request, so new renders show up on a reload.

    shotview.py [--port 8766] [--bind 0.0.0.0] [--dir out/baseshots]

A view as a link (the address bar keeps it): http://<host>:8766/#<shot>&a=<label>&b=<label>&m=<side|wipe|flip|all>
"""

import argparse, http.server, json, os, re, socketserver, urllib.parse

ROOT = os.path.normpath (os.path.join (os.path.dirname (os.path.abspath (__file__)), '..', '..'))

PAGE = r'''<!DOCTYPE html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Base shots</title>
<style>
:root { --bg:#111418; --panel:#1a1f26; --line:#2a313b; --fg:#d8dee6; --dim:#8a94a3; --acc:#5fa8ff; }
* { box-sizing:border-box; }
body { margin:0; background:var(--bg); color:var(--fg); font:14px/1.4 system-ui, sans-serif; display:flex; height:100vh; height:100dvh; }
#side { width:250px; flex:none; background:var(--panel); border-right:1px solid var(--line); overflow:auto; }
#side h1 { font-size:15px; margin:12px 14px 6px; }
#side .grp { color:var(--dim); font-size:12px; margin:12px 14px 4px; text-transform:uppercase; letter-spacing:.05em; }
#side a { display:block; padding:5px 14px; color:var(--fg); text-decoration:none; cursor:pointer; }
#side a:hover { background:#232a33; }
#side a.on { background:#25364d; color:#fff; }
#side a small { color:var(--dim); float:right; }
#main { flex:1; display:flex; flex-direction:column; min-width:0; }
#bar { display:flex; gap:10px; align-items:center; flex-wrap:wrap; padding:8px 12px; border-bottom:1px solid var(--line); background:var(--panel); }
#bar select, #bar button { background:#232a33; color:var(--fg); border:1px solid var(--line); border-radius:4px; padding:4px 8px; font:inherit; }
#bar button.on { background:var(--acc); color:#000; border-color:var(--acc); }
#bar .sp { flex:1; }
#bar .hint { color:var(--dim); font-size:12px; }
#view { flex:1; overflow:auto; padding:10px; }
.pair { display:flex; gap:8px; }
.pair figure, .grid figure { margin:0; flex:1; min-width:0; }
figure img { width:100%; display:block; border:1px solid var(--line); cursor:zoom-in; }
figcaption { color:var(--dim); font-size:12px; padding:3px 0; }
.grid { display:grid; grid-template-columns:repeat(auto-fill, minmax(min(560px, 100%), 1fr)); gap:10px; }
.wipe { position:relative; user-select:none; cursor:ew-resize; max-width:100%; touch-action:pan-y; }
.wipe img { width:100%; display:block; pointer-events:none; }
.wipe .top { position:absolute; inset:0; overflow:hidden; }
.wipe .top img { width:auto; height:100%; max-width:none; }
.wipe .line { position:absolute; top:0; bottom:0; width:2px; background:#fff; box-shadow:0 0 4px #000; }
.tag { position:absolute; top:8px; background:#000a; padding:2px 8px; border-radius:3px; font-weight:600; }
.flip { position:relative; cursor:pointer; }
.flip img { width:100%; display:block; }
#zoom { position:fixed; inset:0; background:#000e; display:none; overflow:auto; cursor:zoom-out; z-index:9; }
#zoom img { display:block; margin:auto; }
.empty { color:var(--dim); padding:30px; }
#mhead, #shade { display:none; }
/* phones and narrow windows: the shot list becomes a drawer, a header with the shot and prev/next */
@media (max-width: 1000px) {
  body { font-size:15px; }
  #side { position:fixed; top:0; bottom:0; left:0; width:min(80vw, 300px); z-index:8; transform:translateX(-100%);
          transition:transform .2s; padding-bottom:env(safe-area-inset-bottom); }
  #side.open { transform:none; box-shadow:4px 0 20px #000a; }
  #side a { padding:10px 16px; }
  #shade.open { display:block; position:fixed; inset:0; background:#0008; z-index:7; }
  #mhead { display:flex; align-items:center; gap:8px; width:100%; }
  #mhead b { flex:1; text-align:center; font-size:16px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  #bar { gap:8px; padding:8px 10px; padding-top:max(8px, env(safe-area-inset-top)); }
  #bar select, #bar button { padding:8px 10px; min-height:40px; }
  #bar .ab { display:flex; gap:8px; width:100%; }
  #bar .ab span { flex:1; display:flex; align-items:center; gap:6px; }
  #bar .ab select { flex:1; min-width:0; }
  #bar .modes { display:flex; gap:6px; width:100%; }
  #bar .modes button { flex:1; padding:8px 4px; }
  #bar .sp, #bar .hint { display:none; }
  #view { padding:8px 6px; padding-bottom:max(8px, env(safe-area-inset-bottom)); }
}
@media (max-width: 1000px) and (orientation: portrait) {
  .pair { flex-direction:column; }
}
@media (max-width: 1000px) and (orientation: landscape) {
  #bar { flex-wrap:nowrap; }
  #mhead { width:auto; flex:1; min-width:0; }
  #bar .ab, #bar .modes { width:auto; }
  #bar .ab select { width:90px; }
  #bar .modes button { padding:8px 10px; white-space:nowrap; }
  #bar .lg { display:none; }
}
</style></head><body>
<nav id="side"><h1>Base shots</h1><div id="list"></div></nav>
<div id="shade"></div>
<div id="main">
 <div id="bar">
  <div id="mhead"><button id="menu" aria-label="Shots">&#9776;</button><button id="prev" aria-label="Previous">&#8249;</button><b id="title"></b><button id="next" aria-label="Next">&#8250;</button></div>
  <div class="ab"><span>A <select id="a"></select></span>
  <span>B <select id="b"></select></span></div>
  <div class="modes"><button data-m="side">Side<span class="lg"> by side</span></button>
  <button data-m="wipe">Wipe</button>
  <button data-m="flip">Flip</button>
  <button data-m="all">All</button></div>
  <span class="sp"></span>
  <span class="hint" id="hint"></span>
 </div>
 <div id="view"></div>
</div>
<div id="zoom"><img></div>
<script>
let D = null, shot = null, mode = localStorage.mode || 'side';
const $ = s => document.querySelector(s);
const url = (l, s) => 'img/' + encodeURIComponent(l) + '/' + encodeURIComponent(s) + '.png?' + D.stamp;
const pretty = s => s.replace(/^base_/, '').replace(/_(day|night)$/, ' · $1').replace(/_/g, ' ');

async function load () {
  D = await (await fetch('api/index')).json();
  const groups = {};
  for (const s of Object.keys(D.shots).sort ()) {
    const g = s.replace(/^base_/, '').split('_')[0];
    (groups[g] = groups[g] || []).push(s);
  }
  let h = '';
  for (const g of Object.keys(groups).sort ()) {
    h += '<div class="grp">' + g + '</div>';
    for (const s of groups[g]) h += '<a data-s="' + s + '">' + pretty(s).replace(g, '').trim().replace(/^· /, '') + '<small>' + D.shots[s].length + '</small></a>';
  }
  $('#list').innerHTML = h || '<div class="empty">No shots yet</div>';
  document.querySelectorAll('#side a').forEach(a => a.onclick = () => { pick(a.dataset.s); drawer(false); });
  // (a link: #<shot>&a=<label>&b=<label>&m=<side|wipe|flip|all>)
  const [hs, ...hp] = location.hash.slice(1).split('&');
  const q = new URLSearchParams(hp.join('&'));
  if (q.get('a')) localStorage.laba = q.get('a');
  if (q.get('b')) localStorage.labb = q.get('b');
  if (q.get('m')) mode = q.get('m');
  const want = decodeURIComponent(hs) || localStorage.shot;
  pick(D.shots[want] ? want : Object.keys(D.shots).sort()[0]);
}

function pick (s) {
  if (!s) return;
  shot = s; localStorage.shot = s;
  document.querySelectorAll('#side a').forEach(a => a.classList.toggle('on', a.dataset.s === s));
  $('#title').textContent = pretty(s);
  const labels = D.shots[s];
  for (const id of ['a', 'b']) {
    const sel = $('#' + id), prev = localStorage['lab' + id];
    sel.innerHTML = labels.map(l => '<option>' + l + '</option>').join('');
    sel.value = labels.includes(prev) ? prev : (id === 'a' ? labels[0] : labels[labels.length - 1]);
  }
  draw();
}

function draw () {
  document.querySelectorAll('#bar button[data-m]').forEach(b => b.classList.toggle('on', b.dataset.m === mode));
  const A = $('#a').value, B = $('#b').value, v = $('#view');
  localStorage.laba = A; localStorage.labb = B; localStorage.mode = mode;
  history.replaceState(null, '', '#' + encodeURIComponent(shot) + '&a=' + encodeURIComponent(A) + '&b=' + encodeURIComponent(B) + '&m=' + mode); // (the view, as a link)
  $('#hint').textContent = {side:'click an image to zoom', wipe:'drag or move across the image', flip:'click or press space to flip', all:'oldest first'}[mode];
  if (mode === 'side') {
    v.innerHTML = '<div class="pair">' + [A, B].map(l => '<figure><figcaption>' + l + '</figcaption><img src="' + url(l, shot) + '"></figure>').join('') + '</div>';
  } else if (mode === 'all') {
    v.innerHTML = '<div class="grid">' + D.shots[shot].map(l => '<figure><figcaption>' + l + '</figcaption><img src="' + url(l, shot) + '"></figure>').join('') + '</div>';
  } else if (mode === 'wipe') {
    v.innerHTML = '<div class="wipe"><img src="' + url(B, shot) + '"><div class="top"><img src="' + url(A, shot) + '"></div><div class="line"></div>'
      + '<span class="tag" style="left:8px">' + A + '</span><span class="tag" style="right:8px">' + B + '</span></div>';
    const w = v.querySelector('.wipe'), top = w.querySelector('.top'), line = w.querySelector('.line'), ti = top.querySelector('img');
    const set = f => { const W = w.clientWidth; top.style.width = (f * W) + 'px'; line.style.left = (f * W - 1) + 'px'; ti.style.width = W + 'px'; ti.style.height = 'auto'; };
    const mv = e => { const r = w.getBoundingClientRect(); const x = (e.touches ? e.touches[0].clientX : e.clientX) - r.left; set(Math.min(1, Math.max(0, x / r.width))); };
    w.addEventListener('mousemove', mv); w.addEventListener('touchstart', mv, {passive:true});
    w.addEventListener('touchmove', e => { mv(e); e.preventDefault(); }, {passive:false});
    w.querySelector('img').onload = () => set(0.5); set(0.5);
  } else {
    let cur = 0; const ls = [A, B];
    v.innerHTML = '<div class="flip"><img src="' + url(A, shot) + '"><span class="tag" style="left:8px">' + A + '</span></div>';
    new Image().src = url(B, shot);
    const f = v.querySelector('.flip');
    window.flip = () => { cur ^= 1; f.querySelector('img').src = url(ls[cur], shot); f.querySelector('.tag').textContent = ls[cur]; };
    f.onclick = window.flip;
  }
  v.querySelectorAll('figure img').forEach(i => i.onclick = () => { const z = $('#zoom'); z.querySelector('img').src = i.src; z.style.display = 'block'; });
}

$('#zoom').onclick = () => $('#zoom').style.display = 'none';
const drawer = on => { $('#side').classList.toggle('open', on); $('#shade').classList.toggle('open', on); };
const step = d => { const ks = [...document.querySelectorAll('#side a')].map(a => a.dataset.s), j = ks.indexOf(shot) + d; if (j >= 0 && j < ks.length) pick(ks[j]); };
$('#menu').onclick = () => { drawer(true); const on = $('#side a.on'); if (on) on.scrollIntoView({block:'center'}); };
$('#shade').onclick = () => drawer(false);
$('#prev').onclick = () => step(-1); $('#next').onclick = () => step(1);
$('#a').onchange = $('#b').onchange = draw;
document.querySelectorAll('#bar button[data-m]').forEach(b => b.onclick = () => { mode = b.dataset.m; draw(); });
document.addEventListener('keydown', e => {
  if (e.key === ' ' && mode === 'flip' && window.flip) { window.flip(); e.preventDefault(); }
  if (e.key === 'Escape') $('#zoom').style.display = 'none';
  if (e.key === 'ArrowDown' || e.key === 'ArrowUp') { step(e.key === 'ArrowDown' ? 1 : -1); e.preventDefault(); }
});
load();
</script></body></html>
'''


def index (d):
	"""{shot: [labels, oldest first]} of the render sets (not the compare_ sheets)."""
	shots, newest = {}, 0
	labels = [l for l in os.listdir (d) if os.path.isdir (os.path.join (d, l)) and not l.startswith ('compare_')]
	labels.sort (key=lambda l: os.path.getmtime (os.path.join (d, l)))
	for l in labels:
		for f in sorted (os.listdir (os.path.join (d, l))):
			if f.endswith ('.png'):
				shots.setdefault (f[:-4], []).append (l)
				newest = max (newest, os.path.getmtime (os.path.join (d, l, f)))
	return {'shots': shots, 'stamp': int (newest)}


def handler (d):
	class H (http.server.BaseHTTPRequestHandler):
		def send (self, code, body, ctype):
			self.send_response (code)
			self.send_header ('Content-Type', ctype)
			self.send_header ('Content-Length', str (len (body)))
			self.send_header ('Cache-Control', 'no-cache' if ctype != 'image/png' else 'max-age=3600')
			self.end_headers ()
			self.wfile.write (body)

		def do_GET (self):
			p = urllib.parse.unquote (urllib.parse.urlparse (self.path).path)
			if p in ('/', '/index.html'):
				return self.send (200, PAGE.encode (), 'text/html; charset=utf-8')
			if p == '/api/index':
				return self.send (200, json.dumps (index (d)).encode (), 'application/json')
			m = re.fullmatch (r'/img/([^/]+)/([^/]+\.png)', p)
			if m and '..' not in p:
				f = os.path.join (d, m.group (1), m.group (2))
				if os.path.isfile (f):
					return self.send (200, open (f, 'rb').read (), 'image/png')
			self.send (404, b'not found', 'text/plain')

		def log_message (self, *a): pass
	return H


def main ():
	ap = argparse.ArgumentParser (description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument ('--port', type=int, default=8766)
	ap.add_argument ('--bind', default='0.0.0.0')
	ap.add_argument ('--dir', default=os.path.join (ROOT, 'out', 'baseshots'))
	a = ap.parse_args ()
	socketserver.ThreadingTCPServer.allow_reuse_address = True
	with socketserver.ThreadingTCPServer ((a.bind, a.port), handler (os.path.abspath (a.dir))) as s:
		print ('serving %s on http://%s:%d/' % (a.dir, a.bind, a.port), flush=True)
		s.serve_forever ()


if __name__ == '__main__':
	main ()
