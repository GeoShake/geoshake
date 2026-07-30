// GeoShake DIY — embedded web UI (HTML/CSS/JS in flash)
// Copyright (C) 2024-2026 Appflows Teknoloji A.S.
// SPDX-License-Identifier: GPL-3.0-or-later
//
// One self-contained page, no CDN, no build step — the device serves this from
// flash and it works with no internet at all. That matters: a seismic station
// that needs the cloud to show you its own data is the wrong design.

#ifndef GEOSHAKE_DIY_WEB_PAGE_H
#define GEOSHAKE_DIY_WEB_PAGE_H

#include <Arduino.h>

static const char PAGE_MAIN[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>GeoShake DIY — live</title>
<style>
:root{
  --bg:#0d0f12; --surface:#161a1f; --surface2:#1e232a; --line:#2a3038;
  --ink:#e8eaed; --dim:#8b9099; --faint:#5a616b;
  --teal:#1ee3e0; --x:#4A87E0; --y:#E8710A; --z:#2ECFC3;
  --ok:#2bd96a; --warn:#f5d000; --fire:#ff4d3d;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);overflow-x:hidden;
  font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
.mono{font-family:ui-monospace,"SF Mono",Menlo,monospace}
header{padding:18px 22px;border-bottom:1px solid var(--line);
  display:flex;align-items:center;gap:14px;flex-wrap:wrap}
/* Flex children default to min-width:auto, so a long unbreakable string (the
   IP + RSSI line) refuses to shrink and widens the whole page on a phone. */
header>div{min-width:0;flex:1 1 auto}
.sub{overflow-wrap:anywhere}
.card,.panel{min-width:0;max-width:100%}
.logo{width:9px;height:9px;border-radius:50%;background:var(--teal);
  box-shadow:0 0 0 0 rgba(30,227,224,.6);animation:p 2.4s infinite}
@keyframes p{0%{box-shadow:0 0 0 0 rgba(30,227,224,.55)}70%{box-shadow:0 0 0 11px rgba(30,227,224,0)}100%{box-shadow:0 0 0 0 rgba(30,227,224,0)}}
h1{font-size:15px;font-weight:700;margin:0;letter-spacing:.01em}
.sub{color:var(--dim);font-size:12px}
.spacer{flex:1}
.pill{font-size:11.5px;font-weight:600;padding:4px 11px;border-radius:999px;
  border:1px solid var(--line);color:var(--dim)}
.pill.armed{color:var(--ok);border-color:rgba(43,217,106,.35)}
.pill.warm{color:var(--warn);border-color:rgba(245,208,0,.35)}
.pill.fire{color:#fff;background:var(--fire);border-color:var(--fire)}
main{padding:20px 22px 60px;max-width:1180px;margin:0 auto}
/* minmax(0,…) not minmax(168px,…): the latter refuses to shrink below 168px
   and pushes the whole row off a narrow phone screen. */
.grid{display:grid;gap:14px;margin:0 0 16px;
  grid-template-columns:repeat(auto-fit,minmax(min(168px,100%),1fr))}
.card{background:var(--surface);border:1px solid var(--line);border-radius:12px;padding:15px 17px}
.k{color:var(--dim);font-size:11px;text-transform:uppercase;letter-spacing:.07em;font-weight:600}
.v{font-size:25px;font-weight:700;margin:5px 0 0;letter-spacing:-.02em}
.v small{font-size:13px;font-weight:600;color:var(--dim);margin-left:3px}
.panel{background:var(--surface);border:1px solid var(--line);border-radius:12px;
  padding:16px 18px;margin:0 0 14px}
.ptitle{display:flex;align-items:baseline;gap:10px;margin:0 0 12px}
.ptitle h2{font-size:13.5px;font-weight:700;margin:0}
.ptitle span{color:var(--dim);font-size:11.5px}
canvas{width:100%;display:block;border-radius:8px;background:var(--bg)}
.legend{display:flex;gap:16px;margin:9px 0 0;font-size:11.5px;color:var(--dim)}
.legend i{display:inline-block;width:11px;height:2.5px;border-radius:2px;margin-right:5px;vertical-align:middle}
table{width:100%;border-collapse:collapse;font-size:13px}
th{text-align:left;color:var(--dim);font-size:11px;text-transform:uppercase;
  letter-spacing:.06em;padding:0 0 8px;font-weight:600}
td{padding:9px 0;border-top:1px solid var(--line)}
th.n,td.n{text-align:right}
th:first-child,td:first-child{width:42px}
th:nth-child(2),td:nth-child(2){width:auto}
th:nth-child(3),td:nth-child(3),th:nth-child(4),td:nth-child(4){width:110px}
th:last-child,td:last-child{width:88px}
a.dl{color:var(--teal);text-decoration:none;font-size:12px;font-weight:600}
a.dl:hover{text-decoration:underline}
.empty{color:var(--faint);font-size:13px;padding:14px 0}
footer{color:var(--faint);font-size:11.5px;text-align:center;padding:0 0 30px}
footer a{color:var(--dim)}
@media(max-width:640px){
  main{padding:14px 12px 50px}
  header{padding:14px 14px;gap:10px}
  .v{font-size:20px}
  .grid{gap:9px;grid-template-columns:repeat(2,minmax(0,1fr))}
  .card{padding:11px 12px;min-width:0}
  .k{font-size:10px}
  .sub{font-size:10.5px}
  .panel{padding:13px 13px}
  table{font-size:12px}
  th:first-child,td:first-child{width:26px}
  th:nth-child(3),td:nth-child(3),th:nth-child(4),td:nth-child(4){width:auto}
  th:last-child,td:last-child{width:52px}
  th{font-size:10px}
}
</style></head><body>

<header>
  <span class="logo"></span>
  <div><h1>GeoShake DIY</h1><div class="sub mono" id="hdr">connecting…</div></div>
  <span class="spacer"></span>
  <span class="pill" id="state">—</span>
</header>

<main>
  <div class="grid">
    <div class="card"><div class="k">STA / LTA</div><div class="v" id="ratio">—</div></div>
    <div class="card"><div class="k">Noise floor</div><div class="v" id="noise">—<small>m/s²</small></div></div>
    <div class="card"><div class="k">Sample rate</div><div class="v" id="rate">—<small>Hz</small></div></div>
    <div class="card"><div class="k">Triggers</div><div class="v" id="trigs">—</div></div>
    <div class="card"><div class="k">Uptime</div><div class="v" id="up">—</div></div>
  </div>

  <div class="panel">
    <div class="ptitle"><h2>Ground motion</h2><span>gravity removed · live</span></div>
    <canvas id="wave" height="230"></canvas>
    <div class="legend">
      <span><i style="background:var(--x)"></i>X</span>
      <span><i style="background:var(--y)"></i>Y</span>
      <span><i style="background:var(--z)"></i>Z</span>
    </div>
  </div>

  <div class="panel">
    <div class="ptitle"><h2>Detector</h2><span id="thr">threshold —</span></div>
    <canvas id="ratioC" height="130"></canvas>
  </div>

  <div class="panel">
    <div class="ptitle"><h2>Detections</h2><span id="trigNote">nothing yet</span></div>
    <div id="trigWrap"><p class="empty">No detections since boot. Knock on the desk once the detector is armed.</p></div>
  </div>

  <footer>
    <span id="dev" class="mono"></span><br>
    Served from the device · <a href="https://geoshake.org/diy">build guide</a>
  </footer>
</main>

<script>
const $=id=>document.getElementById(id);
const CAP=900;                 // samples held for the scrolling view
let buf=[], seq=0, thr=6, lastTrigCount=-1;

function css(n){return getComputedStyle(document.documentElement).getPropertyValue(n).trim()}
function fit(c){const r=c.getBoundingClientRect(),d=window.devicePixelRatio||1;
  if(c.width!==Math.round(r.width*d)){c.width=Math.round(r.width*d);c.height=Math.round(c.height*d/(c.dataset.done?1:1));}
  const x=c.getContext('2d');x.setTransform(d,0,0,d,0,0);return {x,w:r.width,h:c.height/d};}

function grid(x,w,h,rows){
  x.clearRect(0,0,w,h);x.strokeStyle=css('--line');x.lineWidth=1;
  for(let i=1;i<rows;i++){const y=h*i/rows;x.beginPath();x.moveTo(0,y+.5);x.lineTo(w,y+.5);x.stroke();}
}
function series(x,w,h,vals,color,lo,hi){
  if(vals.length<2)return;
  x.strokeStyle=color;x.lineWidth=1.6;x.lineJoin='round';x.beginPath();
  const n=vals.length;
  for(let i=0;i<n;i++){
    const px=w*i/(n-1), py=h-((vals[i]-lo)/(hi-lo))*h;
    i?x.lineTo(px,py):x.moveTo(px,py);
  }
  x.stroke();
}

function drawWave(){
  const c=$('wave'),{x,w,h}=fit(c);
  grid(x,w,h,4);
  if(!buf.length)return;
  let m=1e-4;
  for(const s of buf)m=Math.max(m,Math.abs(s.x),Math.abs(s.y),Math.abs(s.z));
  m*=1.2;
  series(x,w,h,buf.map(s=>s.x),css('--x'),-m,m);
  series(x,w,h,buf.map(s=>s.y),css('--y'),-m,m);
  series(x,w,h,buf.map(s=>s.z),css('--z'),-m,m);
  x.fillStyle=css('--faint');x.font='10px ui-monospace,monospace';
  x.fillText('±'+m.toFixed(3)+' m/s²',6,12);
}

function drawRatio(){
  const c=$('ratioC'),{x,w,h}=fit(c);
  grid(x,w,h,3);
  if(!buf.length)return;
  const vals=buf.map(s=>s.r);
  const top=Math.max(thr*1.6,...vals)*1.05;
  // threshold line
  const ty=h-(thr/top)*h;
  x.strokeStyle=css('--fire');x.globalAlpha=.55;x.setLineDash([5,4]);x.lineWidth=1;
  x.beginPath();x.moveTo(0,ty);x.lineTo(w,ty);x.stroke();
  x.setLineDash([]);x.globalAlpha=1;
  x.fillStyle=css('--fire');x.font='10px ui-monospace,monospace';
  x.fillText('trigger '+thr,6,Math.max(10,ty-5));
  series(x,w,h,vals,css('--teal'),0,top);
}

function fmtUp(s){
  const d=Math.floor(s/86400),hh=Math.floor(s%86400/3600),mm=Math.floor(s%3600/60);
  if(d)return d+'d '+hh+'h'; if(hh)return hh+'h '+mm+'m';
  if(mm)return mm+'m '+(s%60|0)+'s'; return (s|0)+'s';
}

async function poll(){
  try{
    const r=await fetch('/api/live?since='+seq,{cache:'no-store'});
    if(!r.ok)throw 0;
    const d=await r.json();
    seq=d.seq; thr=d.thr;

    for(const s of d.s)buf.push({x:s[0],y:s[1],z:s[2],r:s[3]});
    if(buf.length>CAP)buf=buf.slice(buf.length-CAP);

    $('ratio').textContent=d.ratio.toFixed(2);
    $('noise').innerHTML=d.noise.toFixed(5)+'<small>m/s²</small>';
    $('rate').innerHTML=d.rate.toFixed(1)+'<small>Hz</small>';
    $('trigs').textContent=d.trigs;
    $('up').textContent=fmtUp(d.up);
    $('thr').textContent='threshold '+d.thr;
    $('hdr').textContent=d.ip+'  ·  '+d.sensors+' sensor'+(d.sensors>1?'s':'')+'  ·  RSSI '+d.rssi+' dBm';
    $('dev').textContent=d.part+'  ·  golden vectors '+(d.golden?'PASS':'FAIL');

    const p=$('state');
    if(d.firing){p.className='pill fire';p.textContent='DETECTING';}
    else if(d.armed){p.className='pill armed';p.textContent='ARMED';}
    else{p.className='pill warm';p.textContent='WARMING UP '+d.warm+'%';}

    if(d.trigs!==lastTrigCount){lastTrigCount=d.trigs;loadTriggers();}
    drawWave();drawRatio();
  }catch(e){
    $('state').className='pill';$('state').textContent='OFFLINE';
  }
  setTimeout(poll,220);
}

async function loadTriggers(){
  try{
    const r=await fetch('/api/triggers',{cache:'no-store'});
    const d=await r.json();
    if(!d.length){return;}
    $('trigNote').textContent=d.length+' recorded';
    let h='<table><thead><tr><th>#</th><th>When</th><th class="n">STA/LTA</th><th class="n">PGA m/s²</th><th class="n">Waveform</th></tr></thead><tbody>';
    for(const t of d.slice().reverse()){
      h+='<tr><td class="mono">'+t.id+'</td><td class="mono">'+fmtUp(t.age)+' ago</td>'+
         '<td class="n mono">'+t.ratio.toFixed(2)+'</td><td class="n mono">'+t.pga.toFixed(4)+'</td>'+
         '<td class="n"><a class="dl" href="/api/trigger?id='+t.id+'">CSV ↓</a></td></tr>';
    }
    $('trigWrap').innerHTML=h+'</tbody></table>';
  }catch(e){}
}

addEventListener('resize',()=>{drawWave();drawRatio()});
poll();
</script>
</body></html>)HTML";

// Setup page, shown when the device has no Wi-Fi credentials and is running as
// its own access point.
static const char PAGE_SETUP[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>GeoShake DIY — setup</title>
<style>
:root{--bg:#0d0f12;--surface:#161a1f;--line:#2a3038;--ink:#e8eaed;--dim:#8b9099;--teal:#1ee3e0}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);display:flex;min-height:100vh;
  align-items:center;justify-content:center;padding:22px;
  font:15px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
.box{background:var(--surface);border:1px solid var(--line);border-radius:14px;
  padding:30px 28px;max-width:400px;width:100%}
h1{font-size:20px;margin:0 0 6px}
p{color:var(--dim);font-size:14px;margin:0 0 22px}
label{display:block;font-size:12px;font-weight:600;color:var(--dim);
  text-transform:uppercase;letter-spacing:.06em;margin:0 0 6px}
input{width:100%;padding:12px 14px;font-size:15px;margin:0 0 16px;
  background:var(--bg);color:var(--ink);border:1px solid var(--line);border-radius:9px}
input:focus,select:focus{outline:2px solid var(--teal);outline-offset:1px}
select{width:100%;padding:12px 14px;font-size:15px;margin:0 0 10px;
  background:var(--bg);color:var(--ink);border:1px solid var(--line);border-radius:9px}
button{width:100%;padding:13px;font-size:15px;font-weight:700;cursor:pointer;
  background:var(--teal);color:#04211f;border:0;border-radius:999px}
.note{font-size:12.5px;color:var(--dim);margin:18px 0 0;text-align:center}
.scan{font-size:12.5px;color:var(--teal);cursor:pointer;background:none;border:0;
  width:auto;padding:0;margin:0 0 14px;font-weight:600}
.hint{font-size:12px;color:var(--dim);margin:-6px 0 14px}
</style></head><body>
<form class="box" method="POST" action="/setup">
  <h1>Connect to Wi-Fi</h1>
  <p>The sensor needs your 2.4 GHz network. It will remember this and reconnect on its own.</p>

  <label for="pick">Networks in range</label>
  <select id="pick"><option value="">— scanning… —</option></select>
  <button type="button" class="scan" id="rescan">Scan again</button>

  <label for="s">Network name</label>
  <input id="s" name="ssid" required autocapitalize="off" autocomplete="off">
  <p class="hint" id="hint">Pick one above, or type it if your network is hidden.</p>

  <label for="p">Password</label>
  <input id="p" name="pass" type="password" autocomplete="off">
  <p class="hint"><label style="display:inline;font-size:12px;text-transform:none;letter-spacing:0;color:var(--dim)">
    <input type="checkbox" id="show" style="width:auto;margin:0 6px 0 0;vertical-align:middle"> show password</label></p>
  <button type="submit">Save and restart</button>
  <p class="note" id="status" style="display:none"></p>
  <p class="note">Only 2.4 GHz works — the ESP32 radio has no 5 GHz.<br>
    Phone hotspots often default to 5 GHz (iPhone: turn on <b>Maximise Compatibility</b>).</p>
</form>
<script>
const pick=document.getElementById('pick'),ssid=document.getElementById('s'),hint=document.getElementById('hint');
async function scan(){
  pick.innerHTML='<option value="">— scanning… —</option>';
  try{
    const r=await fetch('/api/scan',{cache:'no-store'});
    const list=await r.json();
    if(!list.length){pick.innerHTML='<option value="">— nothing found —</option>';
      hint.textContent='No 2.4 GHz networks in range. If yours is 5 GHz, switch it to 2.4.';return;}
    pick.innerHTML='<option value="">— choose —</option>'+list.map(n=>{
      const bars=n.rssi>-60?'●●●':n.rssi>-72?'●●○':'●○○';
      return '<option value="'+n.ssid.replace(/"/g,'&quot;')+'">'+bars+'  '+n.ssid+
             (n.open?'  (open)':'')+'</option>';}).join('');
    hint.textContent=list.length+' network'+(list.length>1?'s':'')+' found. Only 2.4 GHz ones appear here.';
  }catch(e){pick.innerHTML='<option value="">— scan failed —</option>';}
}
pick.addEventListener('change',()=>{if(pick.value){ssid.value=pick.value;document.getElementById('p').focus();}});
document.getElementById('rescan').addEventListener('click',scan);
document.getElementById('show').addEventListener('change',e=>{
  document.getElementById('p').type=e.target.checked?'text':'password';});

// Submit by hand instead of letting the browser do a normal form POST.
// In form encoding a "+" in your password arrives as a space, which then looks
// exactly like a wrong password — encodeURIComponent sends it as %2B instead.
document.querySelector('form').addEventListener('submit',async e=>{
  e.preventDefault();
  const st=document.getElementById('status');
  const body='ssid='+encodeURIComponent(ssid.value)+
             '&pass='+encodeURIComponent(document.getElementById('p').value);
  st.style.display='block';st.textContent='Saving…';
  try{
    const r=await fetch('/setup',{method:'POST',
      headers:{'Content-Type':'application/x-www-form-urlencoded'},body});
    st.innerHTML=r.ok
      ? 'Saved — the sensor is restarting.<br>Reconnect to your normal Wi-Fi, then open <b>http://geoshake-diy.local</b>'
      : 'Could not save. Try again.';
  }catch(err){
    st.innerHTML='Saved — the sensor is restarting.<br>Reconnect to your normal Wi-Fi, then open <b>http://geoshake-diy.local</b>';
  }
});
scan();
</script>
</body></html>)HTML";

#endif  // GEOSHAKE_DIY_WEB_PAGE_H
