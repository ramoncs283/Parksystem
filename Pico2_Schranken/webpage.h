#pragma once

// Weboberflaeche (wird vom Pico ausgeliefert)
const char WEBPAGE[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Parksystem</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#f2f4f7;color:#1d2433}
.pico{float:right;font-size:14px;font-weight:500;padding:3px 10px;border-radius:12px;background:#6b7280}
.pico.an{background:#1f8a4c}.pico.aus{background:#c0392b}
header{background:#1d3a6e;color:#fff;padding:14px 20px;font-size:20px;font-weight:600}
nav{display:flex;flex-wrap:wrap;gap:4px;padding:10px 20px;background:#fff;border-bottom:1px solid #d8dde6}
nav button{border:0;background:#e8ecf3;padding:8px 14px;border-radius:6px;cursor:pointer;font-size:15px}
nav button.aktiv{background:#1d3a6e;color:#fff}
main{max-width:640px;margin:20px auto;padding:0 16px}
section{display:none;background:#fff;border-radius:10px;padding:20px;box-shadow:0 1px 3px #0001}
section.aktiv{display:block}
input{font-size:18px;padding:8px 10px;width:100%;box-sizing:border-box;border:1px solid #b9c2d0;border-radius:6px;margin:6px 0 12px}
.knoepfe{display:flex;flex-wrap:wrap;gap:8px}
.knoepfe button{font-size:16px;padding:10px 16px;border:0;border-radius:6px;cursor:pointer;color:#fff;background:#1d3a6e}
.gruen{background:#1f8a4c!important}.rot{background:#c0392b!important}.grau{background:#6b7280!important}
pre{white-space:pre-wrap;background:#f6f7f9;border-left:4px solid #9aa4b5;padding:12px;font-size:15px;min-height:1.2em}
pre.ok{border-color:#1f8a4c}pre.fehler{border-color:#c0392b}
.schranken{display:flex;gap:12px;margin-top:14px}
.schranken div{flex:1;text-align:center;padding:10px;border-radius:8px;background:#eef1f5;font-weight:600}
</style></head><body>
<header>Pico 2 – Schranken <span id="pico" class="pico">Kasse: ?</span></header>
<nav>
<button data-s="kamera" class="aktiv">Kamera (Ein/Aus)</button>
</nav>
<main>
<section id="kamera" class="aktiv">
<h3>Kamera-Simulation</h3>
<label>Kennzeichen</label><input id="kz_kamera" placeholder="ZH 123456">
<div class="knoepfe">
<button class="gruen" onclick="api('einfahrt',{kz:v('kz_kamera')},'out_kamera')">Einfahrt</button>
<button class="rot" onclick="api('ausfahrt',{kz:v('kz_kamera')},'out_kamera')">Ausfahrt</button>
<button class="grau" onclick="api('hilfe',{},'out_kamera')">Hilfe / Klingel</button>
</div>
<pre id="out_kamera"></pre>
<div class="schranken"><div id="s_ein">Einfahrt: ?</div><div id="s_aus">Ausfahrt: ?</div></div>
</section>
</main>
<script>
document.querySelectorAll('nav button').forEach(b=>b.onclick=()=>{
  document.querySelectorAll('nav button,section').forEach(e=>e.classList.remove('aktiv'));
  b.classList.add('aktiv');document.getElementById(b.dataset.s).classList.add('aktiv');
  if(b.dataset.s==='uebersicht')api('uebersicht',{},'out_uebersicht');
});
function v(id){return document.getElementById(id).value}
function zeit(){return {t:Math.floor(Date.now()/1000),tz:-new Date().getTimezoneOffset()*60}}
async function api(pfad,args,out){
  const q=new URLSearchParams({...args,...zeit()});
  const el=document.getElementById(out);
  try{
    const r=await (await fetch('/api/'+pfad+'?'+q)).json();
    el.textContent=r.meldung;el.className=r.ok?'ok':'fehler';
  }catch(e){el.textContent='Keine Verbindung zum Pico.';el.className='fehler'}
  status();
}
async function status(){
  try{
    const s=await (await fetch('/api/status')).json();
    document.getElementById('s_ein').textContent='Einfahrt (grün): '+(s.einfahrt?'OFFEN':'ZU');
    document.getElementById('s_aus').textContent='Ausfahrt (rot): '+(s.ausfahrt?'OFFEN':'ZU');
    const p=document.getElementById('pico');
    p.textContent=s.kasse?'Kasse verbunden':'Kasse nicht erreichbar';p.className='pico '+(s.kasse?'an':'aus');
  }catch(e){}
}
status();setInterval(status,1000);
</script>
</body></html>)HTML";
