#pragma once
// The phone's page. Portrait: tabs, one screen, no scrolling, every option drawn from
// /api/options. Landscape: the classic frame meter full screen on a canvas with the
// Data panel beside or above it. Live data arrives on /api/stream (Server-Sent Events).
// It is three literals because a compiler caps the length of one.
namespace sf4e { namespace training { namespace remote {
constexpr const char* Page =
R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Ember Training Remote</title><style>
:root{color-scheme:dark;--bg:#131315;--panel:#1d1d20;--ink:#f0eeeb;--dim:#9b968f;--line:#2e2d30;--accent:#ff7a2e;--on:#1a0e05;--good:#52c27a;--bad:#ff7d73}
*{box-sizing:border-box}html{height:100%;overflow:hidden}body{height:100vh;height:100dvh;height:var(--vh,100dvh)}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.3 system-ui,sans-serif;display:flex;flex-direction:column;gap:8px;overflow:hidden;
padding:max(8px,env(safe-area-inset-top)) max(12px,env(safe-area-inset-right)) max(8px,env(safe-area-inset-bottom)) max(12px,env(safe-area-inset-left))}
button,input{font:inherit;color:inherit}button:focus-visible,input:focus-visible{outline:2px solid var(--accent);outline-offset:1px}
[hidden]{display:none!important}
.top{display:flex;align-items:center;gap:8px;min-width:0}h1{font-size:15px;margin:0;white-space:nowrap}
.chip{font-size:11px;padding:2px 7px;border:1px solid var(--good);color:var(--good);border-radius:3px;white-space:nowrap}.chip.off{border-color:var(--bad);color:var(--bad)}
main{flex:1;min-height:0;overflow-y:auto;overflow-x:hidden;-webkit-overflow-scrolling:touch;overscroll-behavior:contain;touch-action:pan-y}
section{display:flex;flex-direction:column;gap:12px;padding-bottom:8px}
.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:10px}.g3{grid-template-columns:repeat(3,minmax(0,1fr))}.g5{grid-template-columns:repeat(5,minmax(0,1fr))}.wide{grid-column:1/-1}
.tile{display:flex;min-height:56px;background:var(--panel);border:1px solid var(--line);border-radius:5px;overflow:hidden;min-width:0}
.tile .st{flex:none;width:48px;border:0;background:none;color:var(--dim);font-size:18px}.tile .st:active{background:var(--line)}
.tile .v{flex:1;min-width:0;display:flex;flex-direction:column;justify-content:center;text-align:center}
.tile small{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.tile b{font-size:15px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.tog{display:flex;align-items:center;justify-content:space-between;gap:8px;min-height:56px;padding:0 12px;background:var(--panel);border:1px solid var(--line);border-radius:5px;text-align:left;min-width:0;font-weight:560}
.tog span{white-space:nowrap;overflow:hidden;text-overflow:ellipsis;min-width:0}
.tog i{flex:none;width:48px;height:28px;border-radius:14px;background:var(--line);position:relative}
.tog i::after{content:"";position:absolute;top:3px;left:3px;width:22px;height:22px;border-radius:50%;background:var(--panel);transition:left .12s}
.tog[aria-pressed=true]{border-color:var(--accent)}.tog[aria-pressed=true] i{background:var(--accent)}.tog[aria-pressed=true] i::after{left:23px}.tog b{flex:none;font-size:12px;color:var(--dim);min-width:2.2em;text-align:right}.tog[aria-pressed=true] b{color:var(--accent)}
.act{min-height:48px;border:1px solid var(--line);background:var(--panel);border-radius:5px;font-weight:600;min-width:0;padding:0 4px;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.act.rec{color:var(--bad);border-color:var(--bad)}.act.primary,.act.on{background:var(--accent);border-color:var(--accent);color:var(--on)}
.act{white-space:normal;overflow:visible;text-overflow:clip;line-height:1.15;padding:4px 6px;overflow-wrap:break-word;hyphens:auto}.tog span,.tile b{white-space:normal;overflow:visible;text-overflow:clip;line-height:1.15;overflow-wrap:break-word;hyphens:auto}
.seg{display:flex;flex-wrap:wrap;gap:8px}.seg .act{flex:1 1 4.5em;min-height:48px;font-size:14px}.seg .act.on{box-shadow:inset 0 0 0 2px var(--on)}
.opt{display:flex;flex-direction:column;gap:6px;grid-column:1/-1}.opt>small{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}
.tabs{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:4px;border-top:1px solid var(--line);padding-top:8px}
.tabs button{min-height:48px;border:0;background:none;color:var(--dim);font-size:12px;font-weight:600;border-radius:5px;padding:0 2px}
.tabs button[aria-selected=true]{background:var(--panel);color:var(--accent);box-shadow:inset 0 -3px 0 var(--accent)}
.cpad{position:relative;height:30vh;min-height:150px;touch-action:none;display:flex;align-items:center;justify-content:center;user-select:none}
.sl{display:flex;flex-direction:column;gap:2px;padding:6px 10px;background:var(--panel);border:1px solid var(--line);border-radius:5px;min-width:0}
.sl label{display:flex;justify-content:space-between;font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}.sl output{color:var(--ink)}
.sl input{width:100%;min-height:32px;accent-color:var(--accent)}
.cpad .hint{font-size:12px;color:var(--dim);pointer-events:none;text-align:center;padding:0 12px}
.keys{display:flex;flex-wrap:wrap;gap:6px}.keys .act{min-height:40px;flex:0 0 auto;padding:0 12px}
.card{background:var(--panel);border:1px solid var(--line);border-radius:5px;min-width:0}
canvas{display:block;width:100%;background:#0e0e10;border-radius:5px}
.data{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:2px 10px;padding:6px 8px;font-size:11px;font-variant-numeric:tabular-nums;line-height:1.25}
.data .p{min-width:0}.data b{display:block;color:var(--accent);font-size:11px}.data span{display:block;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.hist{padding:6px 10px;font:12px ui-monospace,monospace;display:flex;gap:10px;flex-wrap:wrap}
.note{font-size:11px;color:var(--dim);margin:0}
#login{display:flex;gap:8px;align-items:center;flex-wrap:wrap}#login input{flex:1;min-width:8em;min-height:52px;padding:0 10px;background:var(--panel);border:1px solid var(--line);border-radius:5px;font-size:20px}
#land{display:none}
.crow{display:flex;flex-direction:column;gap:4px;padding:6px 8px;background:var(--panel);border:1px solid var(--line);border-radius:5px;min-width:0}
.crow b{white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.crow small{color:var(--dim);font-size:11px}
.crow .seg .act{min-height:48px;font-size:16px;display:flex;align-items:center;justify-content:center;text-decoration:none;color:inherit}
.crow .act.sure{background:var(--bad);border-color:var(--bad);color:var(--on)}
.row2{display:flex;gap:6px;min-width:0}.row2 input,#paste{flex:1;min-width:0;min-height:48px;padding:0 10px;background:var(--panel);border:1px solid var(--line);border-radius:5px}
#paste{padding:8px 10px;font:12px ui-monospace,monospace;resize:none;height:9em}
.row2 select{flex:1;min-width:0;min-height:48px;padding:0 8px;background:var(--panel);color:var(--ink);border:1px solid var(--line);border-radius:5px;font-size:16px}
.flist{display:flex;flex-direction:column;gap:8px}
.fm{display:flex;flex-direction:column;gap:6px;padding:8px 10px;background:var(--panel);border:1px solid var(--line);border-radius:5px;min-width:0;text-align:left;width:100%}
.fm .h{display:flex;gap:8px;align-items:baseline;min-width:0}.fm .h b{flex:1;min-width:0;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;font-size:15px}.fm .h small{flex:none;color:var(--dim);font-size:11px}
.fm .s{display:grid;grid-template-columns:repeat(6,minmax(0,1fr));gap:4px;text-align:center}
.fm .s div{min-width:0}.fm .s small{display:block;font-size:10px;color:var(--dim);text-transform:uppercase}.fm .s span{display:block;font-size:15px;font-weight:600;font-variant-numeric:tabular-nums;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.fm .pos{color:var(--good,#4caf50)}.fm .neg{color:var(--bad)}
.fm .more{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:2px 10px;font-size:12px;border-top:1px solid var(--line);padding-top:6px}.fm .more div{min-width:0;overflow-wrap:anywhere}.fm .more small{color:var(--dim)}
.pager{display:flex;align-items:center;justify-content:space-between;gap:6px;font-size:12px;color:var(--dim)}.pager .act{flex:none;width:70px;min-height:48px}
/* Landscape: the classic meter fills the screen. The Data panel sits above it on a wide, short screen and beside it when there is width to spare. */
@media (orientation:landscape) and (max-height:540px){
 body{padding-top:max(4px,env(safe-area-inset-top));padding-bottom:max(4px,env(safe-area-inset-bottom));gap:0}
 .top,main,.tabs,#login{display:none!important}
 #land{display:grid;flex:1;min-height:0;gap:6px;grid-template-areas:"data" "meter";grid-template-rows:auto 1fr;position:relative}
 #land .data{grid-area:data;grid-template-columns:repeat(2,minmax(0,1fr));padding:2px 4px;font-size:10px}
 #land #bigwrap{grid-area:meter;min-height:0;position:relative}
 #land #bigwrap{overflow:hidden}
 #land canvas{position:absolute;inset:0;width:100%;height:100%}
 #land .hint{position:absolute;right:6px;bottom:4px;font-size:10px;color:var(--dim);pointer-events:none}
}
@media (orientation:landscape) and (max-height:540px) and (min-aspect-ratio:21/10){
 #land{grid-template-areas:"meter data";grid-template-columns:1fr minmax(150px,24%);grid-template-rows:1fr}
 #land .data{grid-template-columns:1fr;align-content:start;overflow:hidden}
}
</style></head><body>
<div class="top"><h1>Ember Remote</h1><span class="chip off" id="conn">Connecting</span></div>
<div id="login" hidden><span>Type the PIN shown in the game.</span><input id="pin" inputmode="numeric" maxlength="6" autocomplete="off" aria-label="PIN"><button class="act primary" id="go" style="flex:none;padding:0 16px">Connect</button></div>
<main id="main" hidden>
<section id="t-live"><div class="card"><canvas id="small"></canvas></div><div class="card data" id="data"></div><div class="card hist" id="hist"></div><p class="note" id="liveNote"></p></section>
<section id="t-dummy" hidden><div class="grid g5" id="acts"></div><div class="seg" id="seg-dummy"></div><div class="grid" id="o-dummy"></div></section>
<section id="t-reply" hidden><div id="rbox"></div><div class="seg" id="seg-reply"></div><div class="grid" id="o-reply"></div></section>
<section id="t-hud" hidden><div class="seg" id="seg-hud"></div><div class="grid" id="o-hud"></div></section>
<section id="t-frames" hidden><div class="row2"><select id="fFighter" aria-label="Fighter"></select><input id="fSearch" type="search" placeholder="Search moves" aria-label="Search moves" autocomplete="off"></div><div id="fList" class="flist"></div><button class="act" id="fMore" hidden></button><p class="note" id="fNote"></p></section>
<section id="t-tools" hidden><div class="grid g3" id="toolacts"></div><div class="seg" id="seg-tools"></div><div class="grid" id="o-tools"></div></section>
<section id="t-camera" hidden><div class="card cpad" id="cpad" role="application" aria-label="Camera pad: drag to orbit, two fingers to zoom and pan"><span class="hint">Drag: orbit · two fingers: pinch zoom, slide pan · mouse: wheel zoom, middle/right/Shift drag pan</span></div>
<div class="grid" id="csl"></div>
<div class="grid"><button class="act" id="mTog"></button><button class="act" id="mZero">Zero here</button></div>
<div class="grid"><button class="act" id="mFlip"></button><button class="act" id="mRoll"></button></div>
<div class="sl"><label for="mSens">Motion strength<output id="mSensO"></output></label><input type="range" id="mSens" min="0.3" max="3" step="0.1" value="1"></div>
<p class="note" id="mNote"></p>
<div class="sl"><label for="vSecs">Glide time<output id="vSecsO"></output></label><input type="range" id="vSecs" min="0.3" max="10" step="0.1"></div>
<div class="grid"><button class="act primary" id="vTour">▶ Tour</button><button class="act" id="vLoop"></button></div>
<div class="keys" id="vList"></div>
<div class="grid g3" id="vFx"></div><button class="act" id="vStop">■ Stop move</button>
<div class="grid"><button class="act" id="cFree"></button><button class="act primary" id="cReset">Reset camera</button></div>
<div class="grid g3"><button class="act" id="kAdd">Add key</button><button class="act primary" id="kPlay"></button><button class="act" id="kClear">Clear keys</button></div>
<button class="act primary" id="kRender"></button>
<p class="note" id="kNote"></p><div class="keys" id="kList"></div></section>
<section id="t-combos" hidden>
<div class="row2"><button class="act rec" id="cRec" style="flex:none;width:96px"></button><input id="cName" maxlength="60" aria-label="Combo name"><button class="act primary" id="cSave" style="flex:none;width:84px"></button></div>
<p class="note" id="cNote"></p><div id="cList" style="display:flex;flex-direction:column;gap:6px"></div>
<div class="pager"><button class="act" id="cPrev">‹</button><span id="cPage"></span><button class="act" id="cNext">›</button></div>
<div class="row2"><button class="act" id="cFile" style="flex:1"></button><button class="act" id="cPasteBtn" style="flex:1"></button><button class="act" id="cBack" style="flex:none;width:70px"></button></div>
<input type="file" id="cPick" accept=".json,application/json" hidden>
</section>
<section id="t-text" hidden><textarea id="paste" aria-label="Combo text"></textarea><p class="note" id="tNote"></p>
<div class="row2"><button class="act" id="tAll" style="flex:1"></button><button class="act primary" id="tImport" style="flex:1"></button><button class="act" id="tClose" style="flex:none;width:70px"></button></div></section>
</main>
<nav class="tabs" id="tabs" role="tablist" aria-label="Sections" hidden></nav>
<div id="land"><div class="card data" id="data2"></div><div id="bigwrap" class="card"><canvas id="big"></canvas><span class="hint">Turn the phone upright to change settings</span></div></div>
)HTML"
R"HTML(<script>
"use strict";
const $=id=>document.getElementById(id);
let pin=sessionStorage.getItem("pin")||"",opts=[],vals={},es=null;
const q=new URLSearchParams(location.search).get("pin");
if(q){pin=q;sessionStorage.setItem("pin",q);history.replaceState(null,"",location.pathname)}
async function api(path,body){
 const r=await fetch(path,{method:body?"POST":"GET",headers:{"X-Pin":pin},body:body?JSON.stringify(body):undefined});
 if(r.status==403||r.status==429){needPin(r.status==429?"Too many tries, wait 30 seconds.":"Wrong PIN.");return null}
 return r.json()}
function needPin(text){if(es){es.close();es=null}$("login").hidden=false;$("main").hidden=true;$("tabs").hidden=true;
 $("conn").textContent=text||"PIN needed";$("conn").className="chip off"}
const tabs=[["live","Live"],["dummy","Dummy"],["reply","Reply"],["hud","HUD"],["frames","Frames"],["tools","Tools"],["camera","Camera"]];

const RAD=Math.PI/180;
const camSl=[["yaw","Orbit",-180,180,1,RAD,"°"],["pitch","Tilt",-80,80,1,RAD,"°"],["zoom","Zoom",0.2,4,0.05,1,"×"],["panX","Pan side",-6,6,0.1,1,""],["panY","Pan up",-6,6,0.1,1,""],["roll","Roll",-180,180,1,RAD,"°"],["fov","Lens",-30,45,1,RAD,"°"]];
let cam={live:{yaw:0,pitch:0,zoom:1,panX:0,panY:0,roll:0,fov:0},free:false,dolly:false,keys:[],frame:-1},camTimer=0,camPoll=0;
let camPend=null;
function camSend(body){camPend=Object.assign(camPend||{},body);if(camTimer)return;camTimer=setTimeout(async()=>{camTimer=0;const b=camPend;camPend=null;const r=await api("/api/camera",b);if(r&&r.live){cam=r;camPaint()}else if(r&&r.error)$("kNote").textContent=r.error},40)}
function camLive(){camSend({live:cam.live})}
function camPaint(){
 for(const [id,,,,,k,u] of camSl){const i=$("cs-"+id);if(i&&document.activeElement!==i){i.value=cam.live[id]/k;$("co-"+id).textContent=(+(cam.live[id]/k).toFixed(2))+u}}
 $("cFree").textContent="Free camera: "+(cam.free?"on":"off");$("cFree").classList.toggle("on",cam.free);
 $("kPlay").textContent=cam.dolly?"■ Stop shot":"▶ Play shot";$("kPlay").classList.toggle("on",cam.dolly);$("kPlay").disabled=!cam.keys.length;
 const R=["Watch a replay from Ember\u2019s Replays screen to render a shot","Render shot to MP4","Leaving the replay…","Opening the replay again…","Playing up to the first key…","Recording the shot…","Saving the video…","Video saved next to the replay","Render failed"],rn=cam.render|0;
 $("kRender").textContent=rn==1?"● Render shot to MP4":rn==2?"Stop: "+R[2]:R[rn]||R[0];$("kRender").disabled=(rn!=1&&rn!=2)||(rn==1&&cam.keys.length<2);
 viewsPaint();
 $("kNote").textContent=(cam.frame<0?"Not in a fight.":"Frame "+cam.frame+" · "+cam.keys.length+" key"+(cam.keys.length==1?"":"s"))+(rn>=2&&rn<=6?" · "+R[rn]:"");
 const l=$("kList");l.innerHTML="";
 for(const k of cam.keys){const b=document.createElement("button");b.className="act";b.textContent="f"+k.frame;
  b.setAttribute("aria-label","Key at frame "+k.frame+", tap to remove");b.onclick=()=>camSend({keys:cam.keys.filter(x=>x!==k)});l.append(b)}}
const camFx=[["Push in","push"],["Pull out","pull"],["Orbit 360","orbit"],["Crash zoom","crash"],["Vertigo","vertigo"],["Dutch tilt","dutch"]];
let camLoop=false;
function viewsPaint(){
 const l=$("vList");l.innerHTML="";const v=cam.views||[];
 v.forEach((x,i)=>{const g=document.createElement("button");g.className="act";g.textContent=(i+1)+" · "+(x.used?x.name:"empty");g.disabled=!x.used;g.setAttribute("aria-label",x.used?"Glide to "+x.name:"View "+(i+1)+" is empty");g.onclick=()=>camSend({goView:i});
  const sv=document.createElement("button");sv.className="act";sv.textContent="Save";sv.style.flex="0 0 60px";sv.setAttribute("aria-label","Save the camera as view "+(i+1));sv.onclick=()=>camSend({saveView:i});
  const x2=document.createElement("button");x2.className="act";x2.textContent="×";x2.disabled=!x.used;x2.style.flex="0 0 40px";x2.setAttribute("aria-label","Delete view "+(i+1));x2.onclick=()=>camSend({deleteView:i});l.append(g,sv,x2)});
 $("vTour").disabled=v.filter(x=>x.used).length<2;$("vLoop").textContent="Loop: "+(camLoop?"on":"off");$("vLoop").classList.toggle("on",camLoop);
 const secs=(cam.glideMs||2000)/1000;if(document.activeElement!==$("vSecs"))$("vSecs").value=secs;$("vSecsO").textContent=secs+" s";$("vStop").disabled=!cam.gliding}
function viewsBuild(){
 for(const [name,id] of camFx){const b=document.createElement("button");b.className="act";b.textContent=name;b.onclick=()=>camSend({effect:id});$("vFx").append(b)}
 $("vTour").onclick=()=>camSend({tour:true,loop:camLoop});
 $("vLoop").onclick=()=>{camLoop=!camLoop;viewsPaint()};$("vStop").onclick=()=>camSend({stopGlide:true});
 $("vSecs").oninput=e=>{$("vSecsO").textContent=e.target.value+" s";camSend({glideMs:Math.round(+e.target.value*1000)})};viewsPaint()}
const mo={on:false,zero:null,flip:false,roll:false,k:1};
const wrap=d=>((d+540)%360)-180;
function motionPaint(){$("mTog").textContent="Motion: "+(mo.on?"on":"off");$("mTog").classList.toggle("on",mo.on);$("mZero").disabled=!mo.on;
 $("mFlip").textContent="Direction: "+(mo.flip?"flipped":"normal");$("mRoll").textContent="Roll: "+(mo.roll?"on":"off");$("mRoll").classList.toggle("on",mo.roll);$("mSensO").textContent="×"+mo.k}
function motionEv(e){
 if(e.alpha==null||e.beta==null){$("mNote").textContent=window.isSecureContext?"This phone gives no motion data.":"No motion data: browsers only give it to https pages.";return}
 $("mNote").textContent="";
 if(!mo.zero)mo.zero={a:e.alpha,b:e.beta,g:e.gamma||0,base:Object.assign({},cam.live)};
 const z=mo.zero,L=cam.live,f=(mo.flip?-1:1)*mo.k*RAD;
 L.yaw=z.base.yaw-wrap(e.alpha-z.a)*f;
 L.pitch=Math.max(-1.4,Math.min(1.4,z.base.pitch+wrap(e.beta-z.b)*f));
 if(mo.roll)L.roll=z.base.roll-wrap((e.gamma||0)-z.g)*f;
 camPaint();camLive()}
async function motionToggle(){
 if(mo.on){mo.on=false;removeEventListener("deviceorientation",motionEv);motionPaint();return}
 if(typeof DeviceOrientationEvent!="undefined"&&DeviceOrientationEvent.requestPermission){
  const r=await DeviceOrientationEvent.requestPermission().catch(()=>"denied");if(r!="granted"){$("mNote").textContent="Motion permission was refused.";return}}
 mo.on=true;mo.zero=null;addEventListener("deviceorientation",motionEv);motionPaint();
 if(!window.isSecureContext)$("mNote").textContent="This page is not https, so the phone may give no motion."}
function camBuild(){
 viewsBuild();motionPaint();
 $("mTog").onclick=motionToggle;$("mZero").onclick=()=>{mo.zero=null};$("mFlip").onclick=()=>{mo.flip=!mo.flip;mo.zero=null;motionPaint()};$("mRoll").onclick=()=>{mo.roll=!mo.roll;mo.zero=null;motionPaint()};$("mSens").oninput=e=>{mo.k=+e.target.value;mo.zero=null;motionPaint()};
 const el=$("csl");el.innerHTML="";
 for(const [id,name,min,max,step,k,u] of camSl){const d=document.createElement("div");d.className="sl";
  d.innerHTML='<label for="cs-'+id+'">'+name+'<output id="co-'+id+'"></output></label><input type="range" id="cs-'+id+'" min="'+min+'" max="'+max+'" step="'+step+'">';
  const i=d.querySelector("input");i.oninput=()=>{cam.live[id]=+i.value*k;$("co-"+id).textContent=(+(+i.value).toFixed(2))+u;camLive()};el.append(d)}
 $("cReset").onclick=()=>{cam.live={yaw:0,pitch:0,zoom:1,panX:0,panY:0,roll:0,fov:0};camSend({reset:true})};
 $("cFree").onclick=()=>camSend({free:!cam.free});
 $("kAdd").onclick=()=>camSend({addKey:{at:cam.live,ease:1}});
 $("kRender").onclick=()=>camSend((cam.render|0)==1?{render:true}:{cancelRender:true});
 $("kPlay").onclick=()=>camSend({dolly:!cam.dolly});$("kClear").onclick=()=>camSend({clearKeys:true,dolly:false});
 const pad=$("cpad"),pts=new Map();let last=null;
 const mid=()=>{let x=0,y=0,d=0;const a=[...pts.values()];for(const p of a){x+=p.x/a.length;y+=p.y/a.length}if(a.length>1)d=Math.hypot(a[0].x-a[1].x,a[0].y-a[1].y);return {x,y,d,n:a.length}};
 pad.onpointerdown=e=>{if(e.pointerType=="mouse"){camBusy=true;pad.setPointerCapture(e.pointerId);mz={x:e.clientX,y:e.clientY,pan:e.button>0||e.shiftKey};e.preventDefault();return}camBusy=true;pad.setPointerCapture(e.pointerId);pts.set(e.pointerId,{x:e.clientX,y:e.clientY});last=mid()};
 let mz=null;const cl=(v,l,h)=>Math.max(l,Math.min(h,v));
 pad.onpointermove=e=>{if(mz){const dx=e.clientX-mz.x,dy=e.clientY-mz.y,L=cam.live;mz.x=e.clientX;mz.y=e.clientY;
  if(mz.pan){L.panX=cl(L.panX-dx*0.01,-6,6);L.panY=cl(L.panY+dy*0.01,-6,6)}else{L.yaw=cl(L.yaw-dx*0.008,-Math.PI,Math.PI);L.pitch=cl(L.pitch+dy*0.008,-1.4,1.4)}
  camPaint();camLive();return}
  if(!pts.has(e.pointerId))return;pts.set(e.pointerId,{x:e.clientX,y:e.clientY});const m=mid();
  if(last&&m.n==last.n){const dx=m.x-last.x,dy=m.y-last.y,L=cam.live;
   if(m.n==1){L.yaw=Math.max(-Math.PI,Math.min(Math.PI,L.yaw-dx*0.008));L.pitch=Math.max(-1.4,Math.min(1.4,L.pitch+dy*0.008))}
   else{L.zoom=Math.max(0.2,Math.min(4,L.zoom*(last.d/Math.max(m.d,1))));L.panX=Math.max(-6,Math.min(6,L.panX-dx*0.01));L.panY=Math.max(-6,Math.min(6,L.panY+dy*0.01))}
   camPaint();camLive()}
  last=m};
 pad.onwheel=e=>{e.preventDefault();cam.live.zoom=cl(cam.live.zoom*Math.exp(e.deltaY*0.0015),0.2,4);camPaint();camLive()};
 pad.oncontextmenu=e=>e.preventDefault();
 pad.onpointerup=pad.onpointercancel=e=>{if(mz){mz=null;camBusy=false;return}pts.delete(e.pointerId);last=pts.size?mid():null;camBusy=pts.size>0};
 pad.ondblclick=()=>$("cReset").click()}
let camBusy=false;
async function camLoad(){const r=await api("/api/camera");if(r&&!camBusy&&!camTimer&&!mo.on){cam=r;camPaint()}else if(r){cam.frame=r.frame;cam.keys=r.keys;cam.dolly=r.dolly;cam.gliding=r.gliding;cam.views=r.views;cam.glideMs=r.glideMs;viewsPaint()}}
const section={};
function show(id){$("t-combos").hidden=$("t-text").hidden=true;for(const [t] of tabs)$("t-"+t).hidden=t!=id;
 for(const b of $("tabs").children)b.setAttribute("aria-selected",b.dataset.id==id);if(id=="frames")loadFrames();clearInterval(camPoll);if(id=="camera"){camLoad();camPoll=setInterval(camLoad,300)}draw()}
async function set(id,value){vals[id]=value;paintValues();await api("/api/cmd",{set:id,value})}
async function cmd(c,extra){await api("/api/cmd",Object.assign({cmd:c},extra||{}))}
function actionButtons(el,list){el.innerHTML="";for(const [c,name,cls] of list){const b=document.createElement("button");b.className="act "+(cls||"");b.textContent=name;b.onclick=()=>cmd(c);el.append(b)}}
function build(){
 const nav=$("tabs");nav.innerHTML="";
 for(const [id,name] of tabs){const b=document.createElement("button");b.textContent=name;b.role="tab";b.dataset.id=id;b.onclick=()=>show(id);nav.append(b)}
 actionButtons($("acts"),[["record","● Rec","rec"],["play","▶ Play","primary"],["stop","■ Stop"],["clear","Clear"],["mirror","Mirror"]]);
 actionButtons($("toolacts"),[["save","Save pos"],["restore","Reset pos"],["comboRecord","Record combo"],["comboStart","Check others","primary"],["comboStop","Stop check"],["stop","Stop"]]);
 const cb=document.createElement("button");cb.className="act primary";cb.textContent=S.combos;cb.onclick=openCombos;$("toolacts").append(cb);
 for(const tab of ["dummy","reply","hud","tools"]){
  const list=opts.filter(o=>o.tab==tab),names=[...new Set(list.map(o=>o.section))];
  section[tab]=section[tab]||names[0];
  const seg=$("seg-"+tab);seg.innerHTML="";seg.hidden=names.length<2;
  for(const n of names){const b=document.createElement("button");b.className="act";b.textContent=n;b.dataset.s=n;
   b.onclick=()=>{section[tab]=n;paintTab(tab,list)};seg.append(b)}
  paintTab(tab,list)}
 replyBuild();camBuild();show("live")}
function paintTab(tab,list){
 for(const b of $("seg-"+tab).children)b.classList.toggle("on",b.dataset.s==section[tab]);
 const el=$("o-"+tab);el.innerHTML="";
 for(const o of list.filter(o=>o.section==section[tab])){
  let node;
  if(o.switch){node=document.createElement("button");node.className="tog";node.role="switch";node.innerHTML="<span></span><b></b><i></i>";node.firstChild.textContent=o.label;
   node.onclick=()=>set(o.id,vals[o.id]?0:1)}
  else if(o.choices.length<=5){node=document.createElement("div");node.className="opt";node.role="group";
   const label=document.createElement("small");label.textContent=o.label;const seg=document.createElement("div");seg.className="seg";
   for(const c of o.choices){const b=document.createElement("button");b.className="act";b.textContent=c[1];b.dataset.v=c[0];b.onclick=()=>set(o.id,c[0]);seg.append(b)}
   node.append(label,seg)}
  else{node=document.createElement("div");node.className="tile";
   node.innerHTML='<button class="st" aria-label="Previous">‹</button><div class="v"><small></small><b></b></div><button class="st" aria-label="Next">›</button>';
   node.querySelector("small").textContent=o.label;
   const step=d=>{const i=o.choices.findIndex(c=>c[0]==vals[o.id]);set(o.id,o.choices[((i<0?0:i)+d+o.choices.length)%o.choices.length][0])};
   node.children[0].onclick=()=>step(-1);node.children[2].onclick=()=>step(1)}
  node.dataset.id=o.id;el.append(node)}
 paintValues()}
function paintValues(){
 for(const n of document.querySelectorAll("#main [data-id]")){
  const o=opts.find(x=>x.id==n.dataset.id);if(!o)continue;const v=vals[o.id];
  if(o.switch){n.setAttribute("aria-pressed",v==1);n.setAttribute("aria-checked",v==1);n.querySelector("b").textContent=v==1?S.on:S.off}
  else if(o.choices.length<=5)for(const b of n.querySelectorAll("button")){const on=b.dataset.v==v;b.classList.toggle("on",on);b.setAttribute("aria-pressed",on)}
  else{const c=o.choices.find(c=>c[0]==v);n.querySelector("b").textContent=c?c[1]:"–"}}
 $("liveNote").textContent=vals.available?"":"Start a Training battle to see live data.";
 replyPaint();draw()}
)HTML"
R"HTML(
// ---- live data ----
const L={cells:[new Map(),new Map()],length:0,hits:[],lines:["",""],data:null,live:null};
const KIND=["#969ca2","#7896a0","#3cc46e","#ec3860","#3c78e8","#ff7a2e","#f4de60","#f6b030","#965cc4","#d8b8f0","#dccea6","#ff48d6","#303236"];
const BUT=[0x10,0x20,0x400,0x40,0x80,0x800],NAME=["LP","MP","HP","LK","MK","HK"],STR=["#ffe08a","#ffb347","#ff7a2e"];
function onMessage(m){
 if(m.values)Object.assign(vals,m.values),paintValues();
 if(m.meter){const x=m.meter;if(x.reset){L.cells=[new Map(),new Map()];L.hits=[]}
  x.cells.forEach((c,i)=>{const a=x.from+i;for(let s=0;s<2;s++)L.cells[s].set(a,[c[s*3],c[s*3+1],c[s*3+2]])});
  L.length=x.length;L.hits=L.hits.filter(h=>h[0]<x.from).concat(x.hits);
  for(const mp of L.cells)for(const k of mp.keys())if(k<x.length-480)mp.delete(k)}
 if(m.lines)L.lines=m.lines;
 if("data" in m)L.data=m.data;
 if(m.live)L.live=m.live;
 info();draw();if(m.live&&!$("t-combos").hidden)paintCombos()}
function dir(i){const x=(i&8?1:0)-(i&4?1:0),y=(i&1?1:0)-(i&2?1:0);return [["↙","↓","↘"],["←","","→"],["↖","↑","↗"]][y+1][x+1]}
function btns(b){return NAME.filter((n,i)=>b&BUT[i]).join("+")}
function info(){
 const on=k=>vals[k]!=0,d=L.data;let html="";
 if(d&&vals.data!=0){
  const side=vals.dataFighters|0,names=["P1","P2"];
  d.sides.forEach((f,s)=>{if(!f.valid||(side==1&&s)||(side==2&&!s))return;html+=dataCard(d,f,s,on)})}
 for(const id of ["data","data2"]){$(id).innerHTML=html;$(id).hidden=!html}
 const h=L.live?L.live.history:[];
 $("hist").innerHTML=h.map((runs,s)=>"<span>P"+(s+1)+": "+runs.slice(-4).map(r=>r[0]+"f "+(dir(r[1])+btns(r[1])||"·")).join(" · ")+"</span>").join("");
}
let queued=false;function draw(){if(queued)return;queued=true;requestAnimationFrame(()=>{queued=false;for(const c of [$("big"),$("small")])if(c.offsetParent)meter(c)})}
function meter(canvas){
 const dpr=window.devicePixelRatio||1,box=canvas.parentElement,W=box.clientWidth,big=canvas.id=="big";
 const H=big?box.clientHeight:Math.min(190,Math.round(W*.42));
 if(!big)canvas.style.height=H+"px";canvas.width=W*dpr;canvas.height=H*dpr;
 const g=canvas.getContext("2d");g.setTransform(dpr,0,0,dpr,0,0);g.clearRect(0,0,W,H);
 const shown=vals.shown||60,smooth=vals.scroll==1,bar=vals.barHeight||16,slantOn=vals.classic_slant!=0;
 const parts={data:vals.classic_data!=0,inputs:vals.classic_inputs!=0,colors:vals.classic_colors!=0,extras:vals.classic_extras!=0};
 const len=L.length,pad=big?8:6,usable=W-2*pad;
 const cells=Math.max(10,Math.min(shown,Math.floor(usable/4/5)*5));
 const cell=usable/cells;
 let unit=Math.max(.6,cell/5),font=Math.max(9,Math.min(big?15:11,cell*1.1)),lineH=font*1.3;
 const need=u=>(parts.data?lineH:0)+2*bar*u+(parts.inputs?2*(9*u+1):0)+12*u+(parts.extras?lineH*1.1:0)+6;
 while(need(unit)>H&&unit>.35)unit*=.92;
 const h=bar*unit,slant=slantOn?h*.35:0,dots=12*unit,lane=9*unit,x0=pad+slant;
 const cw=(usable-slant)/cells;
 let y=2;
 if(parts.extras){const gr=g.createLinearGradient(pad,0,W-pad,0);gr.addColorStop(0,"#ff7a2e");gr.addColorStop(1,"#ffd648");g.fillStyle=gr;g.fillRect(pad,0,W-2*pad,2);y=4}
 g.font=font+"px system-ui";g.textBaseline="top";
 if(parts.data){
  for(let s=0;s<2;s++){const txt=(L.lines[s]||"").split("\n").map(l=>l.split(": "));let x=pad+s*W/2;
   g.fillStyle=s?"#78beff":"#ff786e";g.fillText("P"+(s+1),x,y);x+=g.measureText("P1").width+8;
   for(const [k,v] of txt){if(v===undefined)continue;const lab=k.length>9?k.slice(0,9):k;g.fillStyle="#ff7a2e";g.font=font*.72+"px system-ui";g.fillText(lab,x,y+font*.3);x+=g.measureText(lab).width+3;
    g.font=font+"px system-ui";g.fillStyle=/^[+]/.test(v)?"#52c27a":/^[-−]/.test(v)?"#ff7d73":"#fff";g.fillText(v,x,y);x+=g.measureText(v).width+8;if(x>pad+(s+1)*W/2-30)break}}
  y+=lineH}
 const start=smooth?Math.max(0,len-cells):Math.max(0,Math.floor((len-1)/cells))*cells;
 const at=i=>start+i;
 const quad=(x,w,yy,c)=>{g.fillStyle=c;g.beginPath();g.moveTo(x+slant,yy);g.lineTo(x+w+slant,yy);g.lineTo(x+w,yy+h);g.lineTo(x,yy+h);g.fill()};
 const inputs=(s,yy)=>{let cursor=x0,before=0;g.font="bold "+lane*.8+"px system-ui";g.textBaseline="middle";
  for(let i=0;i<cells;i++){const c=L.cells[s].get(at(i));if(!c)continue;const now=c[2],pos=x0+i*cw;
   const way=now&15;if(way&&way!=(before&15)){const d=dir(way);if(d){const x=Math.max(pos,cursor);g.fillStyle="#ebebeb";g.fillText(d,x,yy+lane/2);cursor=x+lane}}
   BUT.forEach((m,b)=>{if((now&m)&&!(before&m)){const x=Math.max(pos,cursor);g.fillStyle=b<3?STR[b%3]:["#9fd8ff","#5fb4f0","#3a8fe0"][b%3];
    g.beginPath();g.arc(x+lane/2,yy+lane/2,lane/2,0,7);g.fill();g.fillStyle="#111";g.font="bold "+lane*.55+"px system-ui";g.textAlign="center";g.fillText(b<3?"P":"K",x+lane/2,yy+lane/2);g.textAlign="left";g.font="bold "+lane*.8+"px system-ui";cursor=x+lane+1}});
   before=now}
  g.textBaseline="top"};
 for(let s=0;s<2;s++){
  if(parts.inputs&&s==0){inputs(0,y);y+=lane+1}
  const ox=s?pad:x0;quad(ox,cells*cw,y,"#0c0c0e");
  let runKind=-1,runFrom=0;
  const flush=(end)=>{if(runKind<0)return;const n=end-runFrom;if(n*cw>font*1.4&&[2,3,4,6,7].includes(runKind)){g.fillStyle="#fff";g.font="bold "+Math.min(h*.8,font)+"px system-ui";g.textAlign="center";g.textBaseline="middle";
    g.fillText(n,ox+slant/2+(runFrom+n/2)*cw,y+h/2);g.textAlign="left";g.textBaseline="top"}};
  for(let i=0;i<cells;i++){const c=L.cells[s].get(at(i));
   if(!c||at(i)>=len){flush(i);runKind=-1;quad(ox+i*cw,cw-1,y,"#303236");continue}
   const kind=parts.colors&&c[1]&1?11:c[0];
   quad(ox+i*cw,cw-(cw>=3?1:0),y,KIND[kind]||KIND[12]);
   if(kind!=runKind){flush(i);runKind=kind;runFrom=i}}
  flush(cells);
  y+=h;
  if(s==0){const mid=y+dots/2;
   for(let i=0;i<cells;i++){const f=(i+1)%5==0;g.fillStyle=f?"#ffd648":"rgba(200,200,200,.6)";g.beginPath();g.arc(x0+(i+.5)*cw,mid,(f?1.6:.9)*Math.max(1,unit),0,7);g.fill()}
   g.font="bold "+Math.min(dots*.9,font)+"px system-ui";g.textBaseline="middle";g.textAlign="center";
   for(const [a,side,adv] of L.hits){const i=a-start;if(i<0||i>=cells)continue;g.fillStyle=adv>=0?"#52c27a":"#ff7d73";g.fillText((adv>0?"+":"")+adv,x0+(i+.5)*cw,mid-dots*.05)}
   g.textAlign="left";g.textBaseline="top";y+=dots}
  if(parts.inputs&&s==1){inputs(1,y);y+=lane+1}}
 if(parts.extras){
  const key=[["Startup",2],["Active",3],["Recovery",4],["Hit",7],["Block",6],["Down",8],["Move",1],["Throw",10],["Meaty",11]];
  g.font=font*.72+"px system-ui";let x=pad;const ky=Math.min(y+2,H-lineH);
  for(const [n,k] of key){const w=g.measureText(n).width+font*.55+10;if(x+w>W-pad)break;g.fillStyle=KIND[k];g.fillRect(x,ky+2,font*.5,font*.5);g.fillStyle="#d8d4cf";g.fillText(n,x+font*.5+3,ky+1);x+=w}}
}
)HTML"
R"HTML(
// ---- saved combos: everything the page says about them is in S ----
const S={on:"On",off:"Off",combos:"Combos",record:"● Record",recording:"● Recording…",save:"Save last",rename:"Rename",namePh:"Combo name",file:"Import file",paste:"Paste…",back:"Back",
 none:"No saved combos yet.",last:(f,h)=>"Last combo: "+f+" f, "+h+" hits",nolast:"Record a combo: press Record, then play it.",saved:"Saved.",
 play:"Play on the dummy",edit:"Rename",down:"Download",copy:"Show as text",del:"Delete",sure:"Sure?",page:(a,b)=>"Page "+a+" of "+b,
 selectAll:"Select all",importText:"Import",close:"Close",pasteNote:"Paste a combo file here.",copyNote:"Select all, then copy.",imported:"Imported.",failed:"Could not do that: "};
let combos=[],cPage=0,editing=null,armed=null;
const esc=t=>String(t).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
function cNote(t){$("cNote").textContent=t}
async function loadCombos(){const r=await api("/api/combos");if(r){combos=r.combos;cPage=Math.min(cPage,Math.max(0,Math.ceil(combos.length/perPage())-1));paintCombos()}}
function perPage(){return Math.max(2,Math.floor((innerHeight-330)/76))}
function openCombos(){for(const [t] of tabs)$("t-"+t).hidden=true;for(const b of $("tabs").children)b.setAttribute("aria-selected","false");
 $("t-text").hidden=true;$("t-combos").hidden=false;loadCombos()}
function paintCombos(){
 const rec=L.live&&L.live.check,frames=rec?rec.frames:0,hits=rec?rec.hits:0;
 $("cRec").textContent=rec&&rec.recording?S.recording:S.record;$("cSave").textContent=editing?S.rename:S.save;$("cName").placeholder=S.namePh;
 $("cFile").textContent=S.file;$("cPasteBtn").textContent=S.paste;$("cBack").textContent=S.back;
 $("cSave").disabled=!editing&&!frames;
 if(!$("cNote").dataset.keep)cNote(frames?S.last(frames,hits):S.nolast);
 const n=perPage(),pages=Math.max(1,Math.ceil(combos.length/n)),list=$("cList");list.innerHTML="";
 if(!combos.length)list.innerHTML='<p class="note">'+esc(S.none)+"</p>";
 for(const c of combos.slice(cPage*n,cPage*n+n)){
  const row=document.createElement("div");row.className="crow";
  row.innerHTML="<b>"+esc(c.name)+"</b><small>"+esc(c.fighter||"?")+" · "+c.hits+" hits · "+c.length+" f</small>";
  const seg=document.createElement("div");seg.className="seg";
  const add=(label,title,fn,tag)=>{const b=document.createElement(tag||"button");b.className="act";b.textContent=label;b.title=title;b.setAttribute("aria-label",title);if(fn)b.onclick=fn;seg.append(b);return b};
  add("▶",S.play,()=>comboCmd("comboPlay",{id:c.id}));
  add("✎",S.edit,()=>{editing=editing==c.id?null:c.id;$("cName").value=editing?c.name:"";paintCombos();$("cName").focus()});
  const a=add("⬇",S.down,null,"a");a.href="/api/combos/"+c.id+"?pin="+encodeURIComponent(pin);a.download="ember-combo-"+c.id+".json";
  add("⧉",S.copy,async()=>{const r=await fetch("/api/combos/"+c.id,{headers:{"X-Pin":pin}});openText(await r.text(),S.copyNote,false)});
  const d=add(armed==c.id?S.sure:"✕",S.del,async()=>{if(armed!=c.id){armed=c.id;paintCombos();return}armed=null;await comboCmd("comboDelete",{id:c.id});loadCombos()});
  if(armed==c.id)d.classList.add("sure");
  list.append(row);row.append(seg)}
 $("cPage").textContent=S.page(cPage+1,pages);$("cPrev").disabled=cPage==0;$("cNext").disabled=cPage>=pages-1}
async function comboCmd(c,extra){const r=await api("/api/cmd",Object.assign({cmd:c},extra||{}));if(r&&!r.ok)cNote(S.failed+(r.error||"no"));return r}
$("cRec").onclick=()=>{const rec=L.live&&L.live.check&&L.live.check.recording;comboCmd(rec?"comboStop":"comboRecord")};
$("cSave").onclick=async()=>{const name=$("cName").value.trim();if(!name)return $("cName").focus();
 const r=editing?await comboCmd("comboRename",{id:editing,name}):await comboCmd("comboSave",{name});
 if(r&&r.ok){editing=null;$("cName").value="";cNote(S.saved);loadCombos()}};
$("cPrev").onclick=()=>{cPage--;paintCombos()};$("cNext").onclick=()=>{cPage++;paintCombos()};
$("cBack").onclick=()=>show("tools");
$("cFile").onclick=()=>$("cPick").click();
$("cPick").onchange=async e=>{const f=e.target.files[0];e.target.value="";if(f)await importText(await f.text())};
$("cPasteBtn").onclick=()=>openText("",S.pasteNote,true);
function openText(text,note,canImport){$("t-combos").hidden=true;$("t-text").hidden=false;$("paste").value=text;$("tNote").textContent=note;$("tImport").hidden=!canImport;
 $("tAll").textContent=S.selectAll;$("tImport").textContent=S.importText;$("tClose").textContent=S.close;$("paste").focus();$("paste").select()}
$("tAll").onclick=()=>{$("paste").focus();$("paste").select();try{document.execCommand("copy")}catch(e){}};
$("tClose").onclick=()=>{$("t-text").hidden=true;$("t-combos").hidden=false;loadCombos()};
$("tImport").onclick=()=>importText($("paste").value);
async function importText(text){
 const r=await fetch("/api/combos",{method:"POST",headers:{"X-Pin":pin},body:text}).then(r=>r.json()).catch(()=>({ok:false,error:"no answer"}));
 $("t-text").hidden=true;$("t-combos").hidden=false;
 if(r.ok){cNote(S.imported);$("cNote").dataset.keep="1";setTimeout(()=>{delete $("cNote").dataset.keep;paintCombos()},3000)}else cNote(S.failed+r.error);
 loadCombos()}
)HTML"
R"HTML(
// ---- frame data: Ember's own table, one fighter at a time ----
const FS={fighters:null,rows:[],cols:[],shown:30,name:""};
const FC=["startup","active","recovery","total","block_adv","hit_adv"],FL={startup:"Start",active:"Active",recovery:"Recov",total:"Total",block_adv:"Block",hit_adv:"Hit"};
const FX=["damage","stun","hit_level","cancel","inv","juggle","extra"],FXL={damage:"Damage",stun:"Stun",hit_level:"Level",cancel:"Cancel",inv:"Invincible",juggle:"Juggle",extra:"Notes"};
async function loadFrames(){
 if(!FS.fighters){const r=await api("/api/frames");if(!r)return;FS.fighters=r.fighters;
  const sel=$("fFighter");for(const f of FS.fighters){const o=document.createElement("option");o.value=f.name;o.textContent=f.name+" ("+f.moves+")";sel.append(o)}
  FS.name=sessionStorage.getItem("fighter")||(FS.fighters[0]||{}).name||"";sel.value=FS.name;
  sel.onchange=()=>{FS.name=sel.value;sessionStorage.setItem("fighter",FS.name);FS.rows=[];FS.shown=30;loadFrames()};
  $("fSearch").oninput=()=>{FS.shown=30;paintFrames()};
  $("fMore").onclick=()=>{FS.shown+=30;paintFrames()};
  if(!FS.fighters.length){$("fNote").textContent="No frame data is installed.";return}}
 if(!FS.name||FS.rows.length)return;
 const r=await api("/api/frames?fighter="+encodeURIComponent(FS.name));if(!r)return;
 FS.cols=r.cols;FS.rows=r.rows.map(x=>Object.fromEntries(r.cols.map((c,i)=>[c,x[i]])));paintFrames()}
function paintFrames(){
 const t=$("fSearch").value.trim().toLowerCase(),list=FS.rows.filter(r=>!t||(r.move+" "+r.strength+" "+r.section).toLowerCase().includes(t));
 const el=$("fList");el.innerHTML="";
 for(const r of list.slice(0,FS.shown)){
  const b=document.createElement("button");b.className="fm";
  const h=document.createElement("div");h.className="h";const n=document.createElement("b");n.textContent=(r.section?r.section+" ":"")+r.move;const k=document.createElement("small");k.textContent=r.strength;h.append(n,k);
  const g=document.createElement("div");g.className="s";
  for(const c of FC){const d=document.createElement("div"),l=document.createElement("small"),v=document.createElement("span");l.textContent=FL[c];v.textContent=r[c]||"–";
   if(c=="block_adv"||c=="hit_adv"){const x=parseInt(r[c]);if(x>0)v.className="pos";else if(x<0)v.className="neg"}d.append(l,v);g.append(d)}
  b.append(h,g);
  b.onclick=()=>{const old=b.querySelector(".more");if(old){old.remove();return}
   const m=document.createElement("div");m.className="more";
   for(const c of FX){if(!r[c]||r[c]=="-")continue;const d=document.createElement("div"),l=document.createElement("small");l.textContent=FXL[c]+" ";d.append(l,document.createTextNode(r[c]));m.append(d)}
   b.append(m)};
  el.append(b)}
 $("fMore").hidden=list.length<=FS.shown;$("fMore").textContent="Show more ("+(list.length-FS.shown)+")";
 $("fNote").textContent=list.length?"":"No moves match."}
)HTML"
R"HTML(
// ---- Data panel: what the game's own panel shows, per fighter ----
document.head.appendChild(Object.assign(document.createElement("style"),{textContent:
".dc{display:flex;flex-direction:column;gap:3px;padding:4px 0;min-width:0}"+
".dc h4{margin:0;display:flex;justify-content:space-between;font-size:12px;color:var(--accent)}.dc h4 small{color:var(--dim);font-weight:400}"+
".dc .sec{font-size:10px;letter-spacing:.08em;text-transform:uppercase;color:var(--dim);margin-top:3px}"+
"#data:has(.dc),#data2:has(.dc){grid-template-columns:minmax(0,1fr)!important;font-size:12px}"+
".dc .kv{display:flex;flex-wrap:wrap;justify-content:space-between;gap:0 8px}.dc .kv span{display:inline;white-space:normal;overflow:visible;text-overflow:clip;overflow-wrap:anywhere;text-align:right}.dc .kv em{font-style:normal;color:var(--dim)}"+
".db{position:relative;height:16px;background:#26262a;border-radius:2px;overflow:hidden}.db i{position:absolute;left:0;top:0;bottom:0}.db u{position:absolute;left:0;top:0;bottom:0;background:#fff3}"+
".db span{position:absolute;left:4px;right:4px;top:0;display:flex;justify-content:space-between;gap:6px;font-size:11px;line-height:16px;white-space:nowrap;text-shadow:0 0 2px #000}"+
".db.s4::after,.db.s2::after{content:'';position:absolute;inset:0;background:repeating-linear-gradient(90deg,transparent 0 calc(25% - 1px),#000a calc(25% - 1px) 25%)}.db.s2::after{background:repeating-linear-gradient(90deg,transparent 0 calc(50% - 1px),#000a calc(50% - 1px) 50%)}"+
".tl{position:relative;height:9px;background:#26262a;border-radius:2px}.tl i{position:absolute;top:0;bottom:0}.tl b{position:absolute;top:-2px;bottom:-2px;width:2px;background:#f2f2f2}.tl s{position:absolute;top:-1px;bottom:-1px;width:1px;background:#a8a8b0}"+
".jg{display:grid;grid-template-columns:repeat(4,1fr);gap:3px;text-align:center}.jg div{background:#26262a;border-radius:3px;padding:1px 0}.jg small{display:block;color:var(--dim);font-size:9px}"+
".chip2{display:inline-block;padding:0 6px;border-radius:3px;color:#fff;font-size:10px}"}));
const STANCE=[["Standing","#4b6b8a"],["Crouching","#a07a2a"],["Airborne","#3b7a5a"],["Down","#9a3a3a"]];
const RANGE=["","Close from ","Far within ","Close height from ","Far height within "];
function dataCard(d,f,s,on){
 const e=t=>String(t).replace(/&/g,"&amp;").replace(/</g,"&lt;"),n2=v=>(+v).toFixed(3),sec=t=>'<div class="sec">'+t+"</div>",
  kv=(a,b)=>'<div class="kv"><em>'+a+"</em><span>"+b+"</span></div>",
  bar=(label,v,max,col,txt,cls,rec)=>'<div class="db '+(cls||"")+'">'+(rec?'<u style="width:'+Math.min(100,rec/Math.max(max,1)*100)+'%"></u>':"")+'<i style="width:'+Math.min(100,v/Math.max(max,1)*100)+"%;background:"+col+'"></i><span><b style="color:#fff;font-weight:500">'+label+"</b><b style=\"color:#fff;font-weight:400\">"+txt+"</b></span></div>",
  m=f.script,st=STANCE[f.stance]||STANCE[0];
 let h='<div class="dc"><h4>P'+(s+1)+"<small>frame "+d.frame+"</small></h4>";
 if(on("data_move")){
  h+=sec("Move")+'<div class="kv"><span>'+e(m.name||("#"+m.id))+(m.name?' <em>#'+m.id+"</em>":"")+"</span><span>"+(Math.abs(m.frame-Math.round(m.frame))<.01?Math.round(m.frame):m.frame.toFixed(2))+(m.total?" / "+m.total:"")+"</span></div>";
  if(m.total){h+='<div class="tl">'+m.runs.map(r=>'<i style="left:'+r[0]/m.total*100+"%;width:"+Math.max((r[1]-r[0])/m.total*100,.8)+"%;background:"+(KIND[r[2]]||KIND[12])+'"></i>').join("")+
   (m.interruptible>0&&m.interruptible<m.total?'<s style="left:'+m.interruptible/m.total*100+'%"></s>':"")+'<b style="left:'+Math.min(99,m.frame/m.total*100)+'%"></b></div>'}
  h+=kv("Interruptible",m.interruptible>0?m.interruptible:"-")+kv("Advantage",f.advantage==null?"-":(f.advantage>0?"+":"")+f.advantage);
  if(f.range[0])h+=kv("Range",RANGE[f.range[0]]+n2(f.range[1]))}
 if(on("data_position"))h+=sec("Position")+kv("X",n2(f.x))+kv("Y",n2(f.y))+kv("Distance",n2(d.distance));
 if(on("data_state"))h+=sec("State")+'<div class="kv"><span class="chip2" style="background:'+st[1]+'">'+st[0]+"</span><em>status "+f.status+"</em></div>";
 if(on("data_gauges"))h+=sec("Gauges")+bar("Life",f.life[0],f.life[1],"#3cc46e",f.life[0],"",f.life[2])+bar("Stun",f.stun[0],f.stun[1],"#e8c23c",f.stun[0])+bar("Super",f.super[0],f.super[1],"#3c78e8",f.super[0],"s4")+bar("Revenge",f.revenge[0],f.revenge[1],"#ec3860",f.revenge[0],"s2");
 if(on("data_juggle")){const c=v=>v<0?"-":v;h+=sec("Juggle")+'<div class="jg"><div><small>Juggled</small>'+(f.juggled<0?"none":f.juggled)+"</div><div><small>Start</small>"+c(f.juggle[0])+"</div><div><small>Add</small>"+c(f.juggle[1])+"</div><div><small>Limit</small>"+c(f.juggle[2])+"</div></div>"}
 if(on("data_charge")){h+=sec("Charge");if(!f.charges.length)h+=kv("No charge moves","");
  for(const c of f.charges){const full=c.r>0&&c.h>=c.r,arrow=c.b&&c.d?"↙":c.b?"←":c.d?"↓":"",label=c.n||(c.b&&c.d?"Down + Back":c.b?"Back":"Down");
   h+=bar(arrow+" "+e(label),c.h,c.r,full?"#3cc46e":"#ff7a2e",c.h+" / "+c.r)}}
 return h+"</div>"}
)HTML"
R"HTML(
// ---- Reply: the moves it plays, and the recordings it can play ----
let presets=[];
function replyBuild(){
 const box=$("rbox");if(!box)return;box.innerHTML="";
 const head=t=>{const d=document.createElement("small");d.className="note";d.textContent=t;return d};
 const pick=document.createElement("div");pick.className="seg";pick.id="rpick";
 for(const [t,name] of [["","Recordings"],...presets.map(p=>[p,p])]){const b=document.createElement("button");b.className="act";b.textContent=name;b.dataset.t=t;b.onclick=()=>replyMoves(t);pick.append(b)}
 const row=document.createElement("div");row.className="seg";row.style.marginTop="8px";
 const inp=document.createElement("input");inp.id="rtext";inp.placeholder="Moves, e.g. 623HP or 2MK > 236P";inp.autocapitalize="characters";inp.autocomplete="off";inp.spellcheck=false;
 inp.style.cssText="flex:1 1 12em;min-height:48px;padding:0 10px;background:var(--panel);border:1px solid var(--line);border-radius:5px;font-size:16px;color:inherit";
 inp.onkeydown=e=>{if(e.key=="Enter")replyMoves(inp.value)};
 const go=document.createElement("button");go.className="act primary";go.textContent="Set moves";go.style.flex="0 0 7em";go.onclick=()=>replyMoves(inp.value);
 row.append(inp,go);
 const err=document.createElement("p");err.id="rerr";err.className="note";err.style.color="var(--bad)";
 const slots=document.createElement("div");slots.className="grid";
 for(const o of opts.filter(o=>/^slot\d+$/.test(o.id))){
  const n=document.createElement("button");n.className="tog";n.role="switch";n.innerHTML="<span></span><b></b><i></i>";n.firstChild.textContent=o.label;
  n.onclick=()=>set(o.id,vals[o.id]?0:1);n.dataset.id=o.id;slots.append(n)}
 box.append(head("Reply with"),pick,row,err,head("With Recordings: plays the recordings that are on"),slots)}
function replyPaint(){
 const pick=$("rpick");if(!pick)return;const now=vals.replyMoves||"";
 let hit=false;
 for(const b of pick.children){const on=b.dataset.t==now;hit|=on;b.classList.toggle("on",on);b.setAttribute("aria-pressed",on)}
 const t=$("rtext");if(t&&document.activeElement!=t)t.value=now;
 $("rerr").textContent=vals.replyError?"Not accepted: "+vals.replyError:""}
async function replyMoves(text){vals.replyMoves=text.trim();vals.replyError="";replyPaint();await cmd("replyMoves",{text})}
)HTML"
R"HTML(
// ---- connection ----
function connect(){
 if(es)es.close();
 es=new EventSource("/api/stream?pin="+encodeURIComponent(pin));
 let lost=false;
 es.onopen=()=>{if(lost)location.reload();$("conn").textContent=location.hostname;$("conn").className="chip"};
 es.onmessage=e=>onMessage(JSON.parse(e.data));
 es.onerror=async()=>{lost=true;$("conn").textContent="Reconnecting";$("conn").className="chip off";
  if(es.readyState==2||!(await fetch("/api/state",{headers:{"X-Pin":pin}}).then(r=>r.ok,()=>false)))needPin()}}
async function start(){
 if(!pin){needPin();return}
 const o=await api("/api/options");if(!o)return;
 opts=o.options.map(x=>Object.assign(x,{choices:x.choices}));presets=o.replyPresets||[];
 $("login").hidden=true;$("main").hidden=false;$("tabs").hidden=false;
 build();connect()}
$("go").onclick=()=>{pin=$("pin").value.trim();sessionStorage.setItem("pin",pin);start()};
// iOS browsers report a taller page than they show until the first scroll or resize; size the page to what is visible, now and whenever the bars move.
function fit(){const v=window.visualViewport;document.documentElement.style.setProperty("--vh",(v?v.height:innerHeight)+"px");scrollTo(0,0);draw()}
fit();addEventListener("resize",fit);addEventListener("orientationchange",()=>setTimeout(fit,200));addEventListener("pageshow",fit);if(window.visualViewport){visualViewport.addEventListener("resize",fit);visualViewport.addEventListener("scroll",fit)}
setTimeout(fit,300);setTimeout(fit,1000);
start();
</script></body></html>)HTML";
} } }
