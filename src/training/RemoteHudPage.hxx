#pragma once
#include <string>
#include "RemotePage.hxx"

// The HUD layout tab of the remote page (docs/design/HUD_EDITOR.md). Desktop browsers only:
// the script adds the tab unless the browser is a phone or its main pointer is a finger, and the
// server refuses the routes to the rest. Spliced into Page when it is served, so the
// phone's page itself stays as small as it was.
namespace sf4e { namespace training { namespace remote {
constexpr const char* HudCss =
R"HTML(.tabs.wide{grid-template-columns:repeat(auto-fit,minmax(0,1fr))}
#t-layout{height:100%;gap:8px}
#t-layout .bar{display:flex;flex-wrap:wrap;align-items:center;gap:6px}
#t-layout .bar .act{min-height:34px;padding:0 10px}
#t-layout .bar select{min-height:34px;background:var(--panel);color:var(--ink);border:1px solid var(--line);border-radius:5px;padding:0 8px}
#t-layout .ed{display:grid;grid-template-columns:200px minmax(0,1fr) 220px;gap:8px;flex:1;min-height:0}
#t-layout .ed>div{min-height:0;min-width:0}
#lyList{display:flex;flex-direction:column;gap:2px;overflow-y:auto}
#lyList button{display:flex;justify-content:space-between;gap:6px;text-align:left;padding:5px 8px;background:none;border:0;border-radius:4px;color:var(--ink)}
#lyList button.on{background:var(--accent);color:var(--on)}#lyList button i{font-style:normal;opacity:.6;font-size:11px}
#lyList button.kid{padding-left:18px}
#lyStage{position:relative;aspect-ratio:16/9;width:100%;margin:0 auto}
#lyCanvas{width:100%;height:100%;touch-action:none;cursor:default}
#lyInsp{display:flex;flex-direction:column;gap:8px;padding:8px}
#lyInsp label{display:flex;flex-direction:column;gap:2px;font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.04em}
#lyInsp input[type=number]{min-height:32px;padding:0 6px;background:var(--bg);border:1px solid var(--line);border-radius:4px}
#lyInsp input[type=number]{width:100%;min-width:0}
#t-layout .act:disabled{opacity:.4}
#lyInsp .two{display:grid;grid-template-columns:1fr 1fr;gap:6px}
#lyImgs{display:flex;flex-direction:column;gap:6px;overflow-y:auto;max-height:36vh}
#lyImgs .im{display:flex;flex-direction:column;gap:4px;padding:6px;border:1px solid var(--line);border-radius:5px}
#lyImgs .im small{color:var(--dim);overflow-wrap:anywhere}#lyImgs .im .bar .act{min-height:30px;padding:0 8px;font-size:12px}
#lySaveName{min-height:34px;width:11em;padding:0 8px;background:var(--panel);border:1px solid var(--line);border-radius:5px;color:var(--ink)}
#lyMsg{font-size:12px;color:var(--dim)}#lyMsg.bad{color:var(--bad)}
)HTML";

constexpr const char* HudHtml =
R"HTML(<section id="t-layout" hidden>
<div class="bar"><select id="lyTheme" aria-label="Theme"></select>
<button class="act" id="lyUndo">Undo</button><button class="act" id="lyRedo">Redo</button>
<button class="act" id="lyCx">Centre X</button><button class="act" id="lyCy">Centre Y</button>
<button class="act" id="lyReset">Reset element</button><button class="act" id="lyFit">Fit view</button>
<label class="bar"><input type="checkbox" id="lyArt" checked> Art</label><select id="lyFighter" aria-label="Fighter for names and portraits"></select>
<label class="bar"><input type="checkbox" id="lyAuto" checked> Auto-apply</label>
<input id="lySaveName" placeholder="New theme name" maxlength="60" aria-label="Name for a copy of the theme"><button class="act" id="lySave">Save as new theme</button>
<button class="act primary" id="lyApply" disabled>Apply 0 changes</button><span class="chip" id="lyChip">Applied</span></div>
<div class="ed"><div class="card" id="lyList" role="listbox" aria-label="HUD elements"></div>
<div id="lyStage" class="card"><canvas id="lyCanvas" tabindex="0" aria-label="HUD layout, 1280 by 720"></canvas></div>
<div class="card" id="lyInsp"><b id="lyName">Nothing selected</b>
<div class="two"><label>X<input type="number" id="lyX" step="1"></label><label>Y<input type="number" id="lyY" step="1"></label></div>
<div class="two"><label>Rotation<input type="number" id="lyRot" step="1"></label><label>Scale<input type="number" id="lyS" step="0.01" min="0.05"></label></div>
<label class="bar"><input type="checkbox" id="lyHide"> Hidden</label>
<p class="note">Drag to move. Arrows nudge 1 px, Shift 10 px. Wheel zooms, drag empty space pans. Ctrl+Z undoes. Names and portraits show after a game restart.</p>
<b>Images</b><div id="lyImgs"></div><input type="file" id="lyFile" accept="image/*" hidden>
<span id="lyMsg"></span></div></div></section>
)HTML";

constexpr const char* HudJs1 =
R"HTML((function(){
"use strict";
if(/Mobi|Android|iPhone|iPad|iPod|Tablet|Silk/i.test(navigator.userAgent)||(navigator.maxTouchPoints>0&&matchMedia("(pointer:coarse)").matches))return;   // a phone or a touch tablet (an iPad says Mac): desktop only
tabs.push(["layout","Layout"]);
const g=id=>document.getElementById(id),DY=21,GO=[-59.2,55.6];
let artSet=new Set(),upFor=null,sprites=[],sheets={},fighter="RYU",loadId=0,theme="",els=[],byId={},lay={},undo=[],redo=[],sel="",pending=0,V={z:1,ox:0,oy:0},guide=null,drag=null,saveT=0;
let auto=true;try{auto=localStorage.getItem("hudAuto")!=="0"}catch(e){}
function say(t,bad){g("lyMsg").textContent=t||"";g("lyMsg").className=bad?"bad":""}
async function call(path,body){
 try{const r=await fetch(path,{method:body?"POST":"GET",headers:{"X-Pin":pin},body:body?JSON.stringify(body):undefined});
  const t=await r.text();
  if(r.status==403&&t=="pin"){needPin("Wrong PIN.");return null}
  let j={};try{j=JSON.parse(t)}catch(e){}
  if(!r.ok){say(j.error=="off"?"Turn it on in the game: Training > Phone remote > Edit the HUD from a PC.":j.error=="desktop"?"The HUD editor needs a desktop browser.":(j.error||"Failed"),true);return null}
  return j}catch(e){say("No connection.",true);return null}}
function chainOf(id){const o=[];for(let i=id;i;i=(byId[i]||{}).parent){if(lay[i]&&byId[i])o.push([lay[i],byId[i],i])}return o}
function disp(id){
 const el=byId[id];let x=el.cx,y=el.cy,rot=0,s=1,hide=false;
 for(const [e,i] of chainOf(id)){const r=e.rot*Math.PI/180,c=Math.cos(r),n=Math.sin(r),dx=e.s*(x-i.cx),dy=e.s*(y-i.cy);x=e.x+dx*c-dy*n;y=e.y+dx*n+dy*c;rot+=e.rot;s*=e.s;hide=hide||e.hide}
 const m=/^p([12])_(super|ultra|ultradiv|ultranum)$/.exec(id);let gx=0,gy=0;if(m){gx=m[1]=="1"?GO[0]:-GO[0];gy=GO[1]}
 return {x:x+gx,y:y+gy-DY,w:el.w*s,h:el.h*s,rot,hide}}
function box(id){
 if(!byId[id])return null;
 if(!byId[id].group)return disp(id);
 const k=els.filter(e=>e.parent==id&&!e.group).map(e=>disp(e.id));if(!k.length)return disp(id);
 let a=1e9,b=1e9,c=-1e9,d=-1e9;for(const q of k){a=Math.min(a,q.x-q.w/2);b=Math.min(b,q.y-q.h/2);c=Math.max(c,q.x+q.w/2);d=Math.max(d,q.y+q.h/2)}
 return {x:(a+c)/2,y:(b+d)/2,w:c-a,h:d-b,rot:0,hide:false}}
function ensure(id){if(!lay[id])lay[id]={x:byId[id].cx,y:byId[id].cy,rot:0,s:1,hide:false};return lay[id]}
// A screen-space move, taken back through the groups around the element so it lands in the element's own space.
function moveBy(id,dx,dy){
 const e=ensure(id);let x=dx,y=dy;
 for(const [o,,i] of chainOf(id)){if(i==id)continue;const r=-o.rot*Math.PI/180,c=Math.cos(r),n=Math.sin(r);[x,y]=[(x*c-y*n)/o.s,(x*n+y*c)/o.s]}
 e.x+=x;e.y+=y}
function snap(id){
 guide=null;const b=box(id);if(!b)return;
 if(Math.abs(b.x-640)<6){moveBy(id,640-b.x,0);guide="x"}
 if(Math.abs(b.y-360)<6){moveBy(id,0,360-b.y);guide=guide?"xy":"y"}}
)HTML";

constexpr const char* HudJs2 =
R"HTML(
const cv=g("lyCanvas"),cx=cv.getContext("2d");
function base(){return cv.width/1280}
function toScr(px,py){const k=base()*V.z;return [px*k+V.ox,py*k+V.oy]}
function fromScr(px,py){const k=base()*V.z;return [(px-V.ox)/k,(py-V.oy)/k]}
const SKIP=/blink|thunder|fire|on2|text|num|_ef\d|efc|_color|time_loop|_on_|_on\d|_loop|_half|_time|combo|ex_on|uc_max|revenge_uc|revenge_max/,KEEP=/revenge_num1$|sc_max_super|sc_part\d$|vital_max$|sc_max_gauge$|(?<!num1)(?<!num2)(?<!num3)_max$/,WIN=/^[12]_winmark_(\d+)$/;
function drawArt(){
 cx.imageSmoothingEnabled=true;
 for(const sp of sprites){
  const img=sheets[sp.f+":"+sp.t];if(!img||!byId[sp.e])continue;
  const w=WIN.exec(sp.n);if(w?+w[1]>2:(SKIP.test(sp.n)&&!KEEP.test(sp.n)))continue;
  let x=sp.p[0],y=sp.p[1],rot=sp.rot,sc=1,hide=false;
  for(const [e,i] of chainOf(sp.e)){const r=e.rot*Math.PI/180,c=Math.cos(r),n=Math.sin(r),dx=e.s*(x-i.cx),dy=e.s*(y-i.cy);x=e.x+dx*c-dy*n;y=e.y+dx*n+dy*c;rot+=e.rot;sc*=e.s;hide=hide||e.hide}
  if(hide)continue;
  const m=/^([12])_(sc|revenge|uc_|combo$|ex_on$)/.exec(sp.n);if(m){x+=m[1]=="1"?GO[0]:-GO[0];y+=GO[1]}
  y-=DY;
  const a=sp.r.map(v=>v*sc),w2=Math.abs(a[2]-a[0]),h2=Math.abs(a[3]-a[1]);if(w2<.5||h2<.5||w2>3000||h2>3000)continue;
  cx.save();cx.translate(x,y);cx.rotate(rot*Math.PI/180);cx.translate((a[0]+a[2])/2,(a[1]+a[3])/2);cx.scale(a[2]<a[0]?-1:1,a[3]<a[1]?-1:1);
  cx.globalCompositeOperation=sp.add?"lighter":"source-over";
  cx.drawImage(img,sp.uv[0],sp.uv[1],sp.uv[2]-sp.uv[0],sp.uv[3]-sp.uv[1],-w2/2,-h2/2,w2,h2);cx.restore()}
 cx.globalCompositeOperation="source-over"}
function colour(id){return id.startsWith("p1")?"#ff7a2e":id.startsWith("p2")?"#5aa9ff":"#b9b4ad"}
function draw(){
 const st=g("lyStage");if(st.hidden||!st.offsetWidth)return;
 const w=Math.round(cv.clientWidth*devicePixelRatio),h=Math.round(cv.clientHeight*devicePixelRatio);
 if(cv.width!=w||cv.height!=h){cv.width=w;cv.height=h}
 const k=base()*V.z;cx.setTransform(1,0,0,1,0,0);cx.fillStyle="#0e0e10";cx.fillRect(0,0,cv.width,cv.height);
 cx.setTransform(k,0,0,k,V.ox,V.oy);cx.fillStyle="#17171a";cx.fillRect(0,0,1280,720);
 cx.lineWidth=1/k;cx.strokeStyle="#ffffff12";cx.beginPath();for(let i=80;i<1280;i+=80){cx.moveTo(i,0);cx.lineTo(i,720)}for(let i=80;i<720;i+=80){cx.moveTo(0,i);cx.lineTo(1280,i)}cx.stroke();
 cx.strokeStyle=guide&&guide.includes("x")?"#52c27a":"#ffffff30";cx.beginPath();cx.moveTo(640,0);cx.lineTo(640,720);cx.stroke();
 cx.strokeStyle=guide&&guide.includes("y")?"#52c27a":"#ffffff30";cx.beginPath();cx.moveTo(0,360);cx.lineTo(1280,360);cx.stroke();
 cx.strokeStyle="#ffffff60";cx.strokeRect(0,0,1280,720);
 if(g("lyArt").checked)drawArt();
 cx.font=(11/k)+"px system-ui";
 const art=g("lyArt").checked&&sprites.length;
 for(const e of els){
  if(e.group&&e.id!=sel)continue;
  const b=box(e.id);if(!b)continue;
  cx.save();cx.translate(b.x,b.y);cx.rotate(b.rot*Math.PI/180);
  cx.globalAlpha=b.hide?.25:art&&e.id!=sel?.35:1;cx.lineWidth=(e.id==sel?2.5:1)/k;cx.strokeStyle=colour(e.id);cx.fillStyle=colour(e.id)+(art?(e.id==sel?"20":"00"):(e.id==sel?"40":"18"));
  if(b.hide)cx.setLineDash([4/k,3/k]);
  cx.fillRect(-b.w/2,-b.h/2,b.w,b.h);cx.strokeRect(-b.w/2,-b.h/2,b.w,b.h);cx.setLineDash([]);
  if(e.id==sel||k>1.4){cx.fillStyle="#f0eeeb";cx.fillText(e.label,-b.w/2+2/k,-b.h/2-3/k)}
  cx.restore()}}
function hit(px,py){
 const [x,y]=fromScr(px,py);let best=null,area=1e18;
 for(const e of els){if(e.group)continue;const b=box(e.id);if(!b)continue;
  const r=-b.rot*Math.PI/180,dx=x-b.x,dy=y-b.y,u=dx*Math.cos(r)-dy*Math.sin(r),v=dx*Math.sin(r)+dy*Math.cos(r);
  if(Math.abs(u)<=b.w/2+3&&Math.abs(v)<=b.h/2+3&&b.w*b.h<area){best=e.id;area=b.w*b.h}}
 return best}
function fit(){V={z:1,ox:0,oy:0};draw()}
function list(){
 const l=g("lyList");l.innerHTML="";
 for(const e of els){const b=document.createElement("button");b.className=(e.id==sel?"on ":"")+(e.parent?"kid":"");b.role="option";
  b.innerHTML="";const t=document.createElement("span");t.textContent=e.label;const i=document.createElement("i");i.textContent=lay[e.id]?"edited":"";b.append(t,i);
  b.onclick=()=>{sel=e.id;paint()};l.append(b)}}
function stem(sp){return sp.f+(sp.f=="chara"||sp.f=="uc"?"."+fighter:"")+"-"+sp.t+"-"+sp.uv.join("-")}
function images(){
 const box=g("lyImgs");box.innerHTML="";if(!sel)return;
 const mine=new Set([sel,...els.filter(e=>e.parent==sel).map(e=>e.id)]),seen=new Set();
 for(const sp of sprites){
  if(!mine.has(sp.e))continue;const k=stem(sp);if(seen.has(k))continue;seen.add(k);
  const d=document.createElement("div");d.className="im";
  const t=document.createElement("small");t.textContent=sp.n+" - "+(sp.uv[2]-sp.uv[0])+" x "+(sp.uv[3]-sp.uv[1])+(artSet.has(k)?" - replaced":"");d.append(t);
  const row=document.createElement("div");row.className="bar";
  for(const [name,fn] of [["Export",()=>exportImage(sp)],["Replace...",()=>{upFor=sp;g("lyFile").click()}],...(artSet.has(k)?[["Restore",()=>restoreImage(sp)]]:[])]){
   const b=document.createElement("button");b.className="act";b.textContent=name;b.onclick=fn;row.append(b)}
  d.append(row);box.append(d)}}
function exportImage(sp){
 const img=sheets[sp.f+":"+sp.t];if(!img)return;
 const c=document.createElement("canvas");c.width=sp.uv[2]-sp.uv[0];c.height=sp.uv[3]-sp.uv[1];
 c.getContext("2d").drawImage(img,sp.uv[0],sp.uv[1],c.width,c.height,0,0,c.width,c.height);
 c.toBlob(b=>{const a=document.createElement("a");a.href=URL.createObjectURL(b);a.download=stem(sp)+".png";a.click();setTimeout(()=>URL.revokeObjectURL(a.href),1000)})}
g("lyFile").onchange=async e=>{
 const f=e.target.files[0];e.target.value="";if(!f||!upFor)return;const sp=upFor;
 try{const bmp=await createImageBitmap(f),c=document.createElement("canvas");c.width=sp.uv[2]-sp.uv[0];c.height=sp.uv[3]-sp.uv[1];
  const x=c.getContext("2d");x.imageSmoothingQuality="high";x.drawImage(bmp,0,0,c.width,c.height);
  const r=await call("/api/hud/image",{theme,file:sp.f,fighter:fighter,tex:sp.t,uv:sp.uv,png:c.toDataURL("image/png").split(",")[1]});
  if(r){artSet.add(stem(sp));say("Image replaced.");await art();changed()}}catch(x){say("That file is not a picture.",true)}};
async function restoreImage(sp){
 const r=await call("/api/hud/image-remove",{theme,name:stem(sp)});if(r){artSet.delete(stem(sp));await art();changed()}}
function inspector(){
 const e=byId[sel];g("lyName").textContent=e?e.label:"Nothing selected";
 for(const id of ["lyX","lyY","lyRot","lyS","lyHide"])g(id).disabled=!e;
 if(!e)return;const v=lay[sel]||{x:e.cx,y:e.cy,rot:0,s:1,hide:false};
 g("lyX").value=+v.x.toFixed(1);g("lyY").value=+v.y.toFixed(1);g("lyRot").value=+v.rot.toFixed(1);g("lyS").value=+v.s.toFixed(3);g("lyHide").checked=!!v.hide}
function bar(){
 g("lyAuto").checked=auto;g("lyApply").disabled=auto||!pending;g("lyApply").textContent="Apply "+pending+" change"+(pending==1?"":"s");
 g("lyChip").textContent=auto?"Applied live":pending?pending+" pending":"Up to date";g("lyChip").className="chip"+(!auto&&pending?" off":"")}
function paint(){list();inspector();images();bar();draw()}
function snapshot(){undo.push(JSON.stringify(lay));if(undo.length>100)undo.shift();redo=[]}
function changed(){
 pending++;paint();clearTimeout(saveT);saveT=setTimeout(flush,500)}
async function flush(){
 const r=await call("/api/hud/layout",{theme,layout:lay});if(r&&auto)apply()}
async function apply(){
 clearTimeout(saveT);const r=await call("/api/hud/apply",{theme,layout:lay});if(r){pending=0;say("Applied.")}bar()}
function restore(from,to){if(!from.length)return;to.push(JSON.stringify(lay));lay=JSON.parse(from.pop());changed()}
async function open(){
 const r=await call("/api/hud/themes");if(!r)return;
 const s=g("lyTheme"),keep=theme;s.innerHTML="";for(const n of r.themes){const o=document.createElement("option");o.textContent=o.value=n;s.append(o)}
 if(!r.themes.length){say("No theme with a battle HUD in ember\\themes.",true);return}
 s.value=r.themes.includes(keep)?keep:r.themes[0];await load(s.value)}
async function load(name){
 const r=await call("/api/hud/theme?name="+encodeURIComponent(name));if(!r)return;
 theme=name;els=r.elements;byId={};for(const e of els)byId[e.id]=e;for(const e of els)e.group=els.some(o=>o.parent==e.id);
 artSet=new Set(r.art||[]);lay=r.layout;undo=[];redo=[];pending=0;sel=els.length?els.find(e=>!e.group).id:"";say("");paint();art()}
async function art(){
 const me=++loadId;say("Loading art...");
 const r=await call("/api/hud/scene?name="+encodeURIComponent(theme)+"&fighter="+encodeURIComponent(fighter));if(!r||me!=loadId)return;
 fighter=r.fighter;const f=g("lyFighter");if(f.options.length!=r.fighters.length){f.innerHTML="";for(const n of r.fighters){const o=document.createElement("option");o.textContent=o.value=n;f.append(o)}}f.value=fighter;
 const got={},jobs=[];
 for(const [file,list] of Object.entries(r.sheets))list.forEach((_,i)=>jobs.push((async()=>{
  try{const res=await fetch("/api/hud/sheet?name="+encodeURIComponent(theme)+"&file="+file+"&fighter="+fighter+"&index="+i,{headers:{"X-Pin":pin}});
   if(res.ok)got[file+":"+i]=await createImageBitmap(await res.blob())}catch(e){}})()));
 await Promise.all(jobs);if(me!=loadId)return;
 sheets=got;sprites=r.sprites.slice().sort((a,b)=>b.p[2]-a.p[2]);say("");images();draw()}
g("lyFighter").onchange=e=>{fighter=e.target.value;art()};g("lyArt").onchange=draw;
g("lyTheme").onchange=e=>load(e.target.value);
g("lyUndo").onclick=()=>restore(undo,redo);g("lyRedo").onclick=()=>restore(redo,undo);
g("lyFit").onclick=fit;
g("lyCx").onclick=()=>{if(sel){snapshot();const b=box(sel);moveBy(sel,640-b.x,0);changed()}};
g("lyCy").onclick=()=>{if(sel){snapshot();const b=box(sel);moveBy(sel,0,360-b.y);changed()}};
g("lyReset").onclick=()=>{if(sel&&lay[sel]){snapshot();delete lay[sel];changed()}};
g("lyApply").onclick=apply;
g("lySave").onclick=async()=>{
 const name=g("lySaveName").value.trim();if(!name){say("Type a name for the new theme first.",true);return}
 clearTimeout(saveT);await call("/api/hud/layout",{theme,layout:lay});
 const r=await call("/api/hud/save-as",{theme,name});
 if(r){say("Saved as "+name+". It shows in the game's theme list after a restart.");g("lySaveName").value="";theme=name;await open()}};
g("lyAuto").onchange=e=>{auto=e.target.checked;try{localStorage.setItem("hudAuto",auto?"1":"0")}catch(x){}if(auto&&pending)flush();bar()};
for(const [id,key] of [["lyX","x"],["lyY","y"],["lyRot","rot"],["lyS","s"]])g(id).oninput=e=>{
 const n=parseFloat(e.target.value);if(!sel||!isFinite(n)||(key=="s"&&n<=0))return;snapshot();ensure(sel)[key]=n;changed()};
g("lyHide").onchange=e=>{if(sel){snapshot();ensure(sel).hide=e.target.checked;changed()}};
cv.onpointerdown=e=>{
 cv.focus();const r=cv.getBoundingClientRect(),px=(e.clientX-r.left)*cv.width/r.width,py=(e.clientY-r.top)*cv.height/r.height,h=hit(px,py);
 cv.setPointerCapture(e.pointerId);
 if(h){sel=h;snapshot();drag={id:h,px,py,moved:false};paint()}else drag={pan:true,px,py,ox:V.ox,oy:V.oy}};
cv.onpointermove=e=>{
 if(!drag)return;const r=cv.getBoundingClientRect(),px=(e.clientX-r.left)*cv.width/r.width,py=(e.clientY-r.top)*cv.height/r.height;
 if(drag.pan){V.ox=drag.ox+px-drag.px;V.oy=drag.oy+py-drag.py;draw();return}
 const k=base()*V.z;moveBy(drag.id,(px-drag.px)/k,(py-drag.py)/k);drag.px=px;drag.py=py;drag.moved=true;snap(drag.id);changed()};
cv.onpointerup=()=>{if(drag&&!drag.pan&&!drag.moved)undo.pop();drag=null;guide=null;draw()};
cv.onwheel=e=>{
 e.preventDefault();const r=cv.getBoundingClientRect(),px=(e.clientX-r.left)*cv.width/r.width,py=(e.clientY-r.top)*cv.height/r.height;
 const z=Math.max(.5,Math.min(8,V.z*(e.deltaY<0?1.15:1/1.15))),f=z/V.z;V.ox=px-(px-V.ox)*f;V.oy=py-(py-V.oy)*f;V.z=z;draw()};
cv.onkeydown=e=>{
 if(e.ctrlKey&&(e.key=="z"||e.key=="y")){e.preventDefault();e.key=="z"?restore(undo,redo):restore(redo,undo);return}
 const d={ArrowLeft:[-1,0],ArrowRight:[1,0],ArrowUp:[0,-1],ArrowDown:[0,1]}[e.key];
 if(!d||!sel)return;e.preventDefault();snapshot();const n=e.shiftKey?10:1;moveBy(sel,d[0]*n,d[1]*n);guide=null;changed()};
const showOld=show;show=function(id){showOld(id);if(id=="layout"){g("lyStage").hidden=false;open();setTimeout(draw,50)}};
addEventListener("resize",draw);
addEventListener("beforeunload",e=>{if(!auto&&pending){e.preventDefault();e.returnValue=""}});
const nav=g("tabs");nav.classList.add("wide");
})();
)HTML";

// The remote's page with the HUD layout tab spliced in.
inline std::string PageWithHudEditor() {
    std::string html = Page;
    const auto put = [&html](const char* before, const std::string& text, bool last = false) {
        const auto at = last ? html.rfind(before) : html.find(before);
        if (at != std::string::npos) html.insert(at, text);
    };
    put("</style>", HudCss);
    put("</main>", HudHtml);
    put("\nstart();\n</script>", "\n" + std::string(HudJs1) + HudJs2, true);   // inside the page script, before it starts
    return html;
}
} } }
