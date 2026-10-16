/* LinTOS UI 设计器 — 核心逻辑 (400x300 单色屏)
 * 数据模型: state.els = [ {...} ] 每个元件就是一个对象, type 决定渲染与属性.
 */
(() => {
"use strict";

/* =========================================================================
 * 1. 元件库定义 — 丰富、可灵活组合
 * ========================================================================= */
const PAL = {
  struct: [
    {type:"statusbar", name:"信息条", ico:"▬", defs:[
      {k:"text",name:"标题",type:"text",def:"状态栏"},
      {k:"right",name:"右侧",type:"text",def:"状态"},
      {k:"h",name:"高",type:"num",def:24,min:16,max:60},
      {k:"invert",name:"反色",type:"bool",def:true},
    ]},
    {type:"divider", name:"分隔线", ico:"━", defs:[
      {k:"thick",name:"线粗",type:"num",def:2,min:1,max:6},
      {k:"x0",name:"起点x",type:"num",def:0,min:0,max:399},
      {k:"len",name:"长度",type:"num",def:400,min:8,max:400},
      {k:"vert",name:"竖线",type:"bool",def:false},
    ]},
    {type:"panel", name:"面板/框", ico:"▭", defs:[
      {k:"label",name:"标签",type:"text",def:"面板"},
      {k:"fill",name:"填充",type:"bool",def:false},
      {k:"border",name:"边框",type:"bool",def:true},
      {k:"borderw",name:"框粗",type:"num",def:1,min:1,max:6},
    ]},
  ],
  text: [
    {type:"text", name:"文本", ico:"文", defs:[
      {k:"text",name:"内容",type:"text",def:"Hello"},
      {k:"size",name:"字号",type:"seg",def:"m",opts:[["s","小"],["m","中"],["l","大"]]},
      {k:"invert",name:"反色",type:"bool",def:false},
      {k:"align",name:"对齐",type:"seg",def:"c",opts:[["l","左"],["c","中"],["r","右"]]},
    ]},
    {type:"clock", name:"时钟(冒号)", ico:"◷", defs:[
      {k:"fmt",name:"格式",type:"text",def:"12:45"},
      {k:"size",name:"字号",type:"seg",def:"l",opts:[["m","中"],["l","大"],["xl","特大"]]},
      {k:"invert",name:"反色",type:"bool",def:false},
    ]},
    {type:"numbers", name:"数字分组", ico:"123", defs:[
      {k:"text",name:"内容",type:"text",def:"08"},
      {k:"size",name:"字号",type:"seg",def:"l",opts:[["m","中"],["l","大"],["xl","特大"]]},
      {k:"spacing",name:"字距",type:"num",def:2,min:0,max:12},
      {k:"invert",name:"反色",type:"bool",def:false},
    ]},
    {type:"label", name:"小标签", ico:"标签", defs:[
      {k:"text",name:"内容",type:"text",def:"小字说明"}, {k:"invert",name:"反色",type:"bool",def:false},
    ]},
  ],
  ctrl: [
    {type:"button", name:"按键", ico:"▢", defs:[
      {k:"text",name:"文字",type:"text",def:"确定"},
      {k:"invert",name:"反色",type:"bool",def:false},
    ]},
    {type:"keypad3", name:"九宫格(数字盘)", ico:"#", defs:[
      {k:"cols",name:"列数",type:"num",def:3,min:2,max:3},
      {k:"rows",name:"行数",type:"num",def:3,min:1,max:4},
      {k:"start",name:"起始值",type:"num",def:1,min:0,max:9},
      {k:"sel",name:"选中格",type:"num",def:5,min:-1,max:9},
    ]},
    {type:"progress", name:"进度条", ico:"▰", defs:[
      {k:"pct",name:"进度%",type:"num",def:60,min:0,max:100},
      {k:"thick",name:"条高",type:"num",def:8,min:2,max:24},
      {k:"text",name:"文本",type:"text",def:""},
    ]},
    {type:"ring", name:"圆环", ico:"◎", defs:[
      {k:"pct",name:"进度%",type:"num",def:70,min:0,max:100},
      {k:"thick",name:"环宽",type:"num",def:8,min:2,max:24},
    ]},
    {type:"toggle", name:"开关", ico:"⇄", defs:[
      {k:"on",name:"状态",type:"bool",def:true},
      {k:"label",name:"标签",type:"text",def:"开关"},
    ]},
  ],
  shape: [
    {type:"rect", name:"实心矩形", ico:"■", defs:[]},
    {type:"rectout", name:"空心矩形", ico:"□", defs:[{k:"borderw",name:"框粗",type:"num",def:1,min:1,max:6}]},
    {type:"circle", name:"实心圆", ico:"●", defs:[]},
    {type:"circleout", name:"空心圆", ico:"○", defs:[{k:"borderw",name:"框粗",type:"num",def:1,min:1,max:6}]},
    {type:"hline", name:"横线", ico:"─", defs:[{k:"thick",name:"线粗",type:"num",def:1,min:1,max:6}]},
    {type:"vline", name:"竖线", ico:"│", defs:[{k:"thick",name:"线粗",type:"num",def:1,min:1,max:6}]},
    {type:"dot", name:"圆点", ico:"•", defs:[]},
  ],
  adv: [
    {type:"statusbarList", name:"列表+顶栏", ico:"≡", defs:[
      {k:"title",name:"标题",type:"text",def:"列表"},
      {k:"rows",name:"行数",type:"num",def:4,min:1,max:12},
      {k:"selrow",name:"选中行",type:"num",def:0,min:-1,max:11},
      {k:"rowh",name:"行高",type:"num",def:32,min:16,max:60},
    ]},
    {type:"grid", name:"网格区域", ico:"▦", defs:[
      {k:"cols",name:"列",type:"num",def:4,min:1,max:8},
      {k:"rows",name:"行",type:"num",def:3,min:1,max:8},
      {k:"sel",name:"选中格",type:"num",def:0,min:-1,max:30},
      {k:"label",name:"图标文字",type:"text",def:"应用"},
    ]},
    {type:"keyboard", name:"键盘(多排)", ico:"⌨", defs:[
      {k:"rows",name:"排数",type:"num",def:3,min:1,max:5},
      {k:"chars1",name:"第1排",type:"text",def:"123456"},
      {k:"chars2",name:"第2排",type:"text",def:"7890AB"},
      {k:"chars3",name:"第3排",type:"text",def:"CORNMX"},
      {k:"selrow",name:"选中排",type:"num",def:0,min:-1,max:4},
    ]},
    {type:"slider", name:"滑块(调整)", ico:"⏶", defs:[
      {k:"val",name:"值%",type:"num",def:50,min:0,max:100},
      {k:"label",name:"标签",type:"text",def:"时长"},
    ]},
  ],
};
const DEF_W = {
  statusbar:[400,24], divider:[400,2], panel:[220,90],
  text:[140,39], clock:[120,59], numbers:[90,59], label:[120,20],
  button:[120,40], keypad3:[280,150], progress:[300,16], ring:[160,160], toggle:[120,30],
  rect:[100,60], rectout:[100,60], circle:[60,60], circleout:[60,60],
  hline:[180,2], vline:[2,120], dot:[8,8],
  statusbarList:[400,230], grid:[300,210], keyboard:[380,130], slider:[200,40],
};

/* =========================================================================
 * 2. 状态
 * ========================================================================= */
const state = { els:[], sel:null, zoom:2, nextId:1, savedName:null, marquee:null,
  _resizeStart:null, resizeDir:null, mode:null,
  hist:[], histIdx:-1 };   // 撤销/重做栈
const canvas = document.getElementById('screen');
const ctx = canvas.getContext('2d');

/* =========================================================================
 * 3. 基础单色绘制
 * ========================================================================= */
function fillRect(x0,y0,w,h,on){
  if (w<=0||h<=0) return;
  ctx.fillStyle = on ? '#000' : '#fff';
  ctx.fillRect(Math.round(x0),Math.round(y0),Math.round(w),Math.round(h));
}
function strokeRect(x0,y0,w,h,bw,on){
  if(!bw||bw<=0) return;
  fillRect(x0,y0,w,bw,on); fillRect(x0,y0+h-bw,w,bw,on);
  fillRect(x0,y0,bw,h,on); fillRect(x0+w-bw,y0,bw,h,on);
}
/* 5x7 点阵字形 (ASCII) */
const FONT5 = {
  '0':[0x0E,0x11,0x13,0x15,0x19,0x11,0x0E], '1':[0x04,0x0C,0x04,0x04,0x04,0x04,0x0E],
  '2':[0x0E,0x11,0x01,0x02,0x04,0x08,0x1F], '3':[0x1F,0x02,0x04,0x02,0x01,0x11,0x0E],
  '4':[0x02,0x06,0x0A,0x12,0x1F,0x02,0x02], '5':[0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E],
  '6':[0x06,0x08,0x10,0x1E,0x11,0x11,0x0E], '7':[0x1F,0x01,0x02,0x04,0x08,0x08,0x08],
  '8':[0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E], '9':[0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C],
  ':':[0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00], '.':[0x00,0x00,0x00,0x00,0x00,0x0C,0x0C],
  '-':[0x00,0x00,0x00,0x1F,0x00,0x00,0x00], 'A':[0x0E,0x11,0x11,0x1F,0x11,0x11,0x11],
  'B':[0x1E,0x11,0x1E,0x11,0x11,0x11,0x1E], 'C':[0x0E,0x11,0x10,0x10,0x10,0x11,0x0E],
  'D':[0x1E,0x11,0x11,0x11,0x11,0x11,0x1E], 'E':[0x1F,0x10,0x1E,0x10,0x10,0x10,0x1F],
  'F':[0x1F,0x10,0x1E,0x10,0x10,0x10,0x10], 'G':[0x0E,0x11,0x10,0x17,0x11,0x11,0x0F],
  'H':[0x11,0x11,0x1F,0x11,0x11,0x11,0x11], 'I':[0x0E,0x04,0x04,0x04,0x04,0x04,0x0E],
  'J':[0x03,0x02,0x02,0x02,0x12,0x12,0x0C], 'K':[0x11,0x12,0x14,0x18,0x14,0x12,0x11],
  'L':[0x10,0x10,0x10,0x10,0x10,0x10,0x1F], 'M':[0x11,0x1B,0x15,0x15,0x11,0x11,0x11],
  'N':[0x11,0x19,0x15,0x13,0x11,0x11,0x11], 'O':[0x0E,0x11,0x11,0x11,0x11,0x11,0x0E],
  'P':[0x1E,0x11,0x11,0x1E,0x10,0x10,0x10], 'Q':[0x0E,0x11,0x11,0x15,0x12,0x12,0x0D],
  'R':[0x1E,0x11,0x11,0x1E,0x14,0x12,0x11], 'S':[0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E],
  'T':[0x1F,0x04,0x04,0x04,0x04,0x04,0x04], 'U':[0x11,0x11,0x11,0x11,0x11,0x11,0x0E],
  'V':[0x11,0x11,0x11,0x11,0x11,0x0A,0x04], 'W':[0x11,0x11,0x11,0x15,0x15,0x15,0x0A],
  'X':[0x11,0x11,0x0A,0x04,0x0A,0x11,0x11], 'Y':[0x11,0x11,0x0A,0x04,0x04,0x04,0x04],
  'Z':[0x1F,0x01,0x02,0x04,0x08,0x10,0x1F], ' ':[0,0,0,0,0,0,0],
};
function textW(t,scale,sp){ return String(t).length*(5+(sp||0))*(scale||1); }
function paintText(t,x,y,scale,on,sp){
  t = String(t==null?'':t); const s=scale||1; const gap=sp||0;
  let cx=x;
  for(const ch of t){
    const g=FONT5[ch]||FONT5[' '];
    for(let r=0;r<7;r++){ const b=g[r];
      for(let c=0;c<5;c++) if(b&(0x10>>c)) fillRect(cx+c*s,y+r*s,s,s,on);
    }
    cx+=(5+gap)*s;
  }
}
function paintCentered(t,cx,cy,scale,on){
  const s=scale||1;
  paintText(t, cx-textW(t,s,0)/2, cy-((7*s)/2), s, on);
}

/* =========================================================================
 * 4. 每类元件渲染 (全部画在局部 0,0 原点)
 * ========================================================================= */
function renderElem(el){
  const {w=20,h=20}=el;
  const bool=v=>v===true;
  switch(el.type){
    case 'statusbar': {
      fillRect(0,0,400,el.h,bool(el.invert));
      paintText(el.text||'',6,(el.h-7)/2,1,bool(el.invert));
      const rw=textW(el.right||'',1,0);
      paintText(el.right||'',400-6-rw,(el.h-7)/2,1,bool(el.invert));
      break;
    }
    case 'divider': {
      if(bool(el.vert)) fillRect(el.x0,0,el.thick||2,el.len||300,true);
      else fillRect(el.x0,(el.thick||2)<2?0:1,el.len||400,el.thick||2,true);
      break;
    }
    case 'panel': {
      if(el.border!==false) strokeRect(0,0,w,h,el.borderw||1,true);
      if(bool(el.fill)) fillRect(1,1,w-2,h-2,true);
      break;
    }
    case 'text': {
      const s=el.size==='l'?2:(el.size==='s'?1:1.5);
      const tw=textW(el.text||'',s,0);
      let x0=0; if(el.align==='c')x0=(w-tw)/2; else if(el.align==='r')x0=w-tw;
      paintText(el.text||'',x0,(h-7*s)/2,s,bool(el.invert));
      break;
    }
    case 'clock': {
      const s=el.size==='l'?2:(el.size==='xl'?3:1.5);
      paintText(el.fmt||'12:45',(w-textW(el.fmt, s,0))/2,(h-7*s)/2,s,bool(el.invert));
      break;
    }
    case 'numbers': {
      const s=el.size==='l'?2:(el.size==='xl'?3:1.5);
      const tw=textW(el.text||'',s,el.spacing||0);
      paintText(el.text||'',(w-tw)/2,(h-7*s)/2,s,bool(el.invert),el.spacing||0);
      break;
    }
    case 'label': paintText(el.text||'',0,(h-7)/2,1,bool(el.invert)); break;
    case 'button': {
      strokeRect(0,0,w,h,1,true);
      if(bool(el.invert)) fillRect(1,1,w-2,h-2,true);
      const s=h<=24?1:(h<=40?1.5:2);
      paintCentered(el.text||'',w/2,h/2,s,bool(el.invert));
      break;
    }
    case 'keypad3': {
      const cols=el.cols||3, rows=el.rows||3;
      const gw=w/cols, gh=h/rows;
      for(let r=0;r<rows;r++)for(let c=0;c<cols;c++){
        const idx=r*cols+c, lab=String((el.start||1)+idx), on=el.sel===idx;
        if(on) fillRect(c*gw,r*gh,gw,gh,true);
        strokeRect(c*gw,r*gh,gw,gh,1,false);
        const s=gh<=30?1:(gh<=50?1.5:2);
        paintCentered(lab,c*gw+gw/2,r*gh+gh/2,s,on);
      }
      break;
    }
    case 'progress': {
      const th=el.thick||8, y=(h-th)/2;
      const pxw=Math.min(w-4,(w-8)*(el.pct/100));
      strokeRect(0,y,w,th,1,true);
      fillRect(2,y+1,Math.max(0,pxw),th-2,true);
      if(el.text) paintCentered(el.text,w/2,y-4,1,false);
      break;
    }
    case 'ring': {
      const cx=w/2, cy=h/2, R=Math.min(w,h)/2- (el.thick||8)/2, T=el.thick||8;
      ctx.strokeStyle='#000'; ctx.lineWidth=T;
      ctx.beginPath(); ctx.arc(cx,cy,R,-Math.PI/2,-Math.PI/2+2*Math.PI*((el.pct||0)/100)); ctx.stroke();
      break;
    }
    case 'toggle': {
      strokeRect(0,0,w,h,1,true);
      if(bool(el.on)) fillRect(1,1,w-2,h-2,true);
      paintText(el.label||'',w+4,(h-7)/2,1,false);
      break;
    }
    case 'rect': fillRect(0,0,w,h,true); break;
    case 'rectout': strokeRect(0,0,w,h,el.borderw||1,true); break;
    case 'circle': { ctx.fillStyle='#000'; ctx.beginPath(); ctx.arc(w/2,h/2,Math.min(w,h)/2,0,6.283); ctx.fill(); break; }
    case 'circleout': { ctx.strokeStyle='#000'; ctx.lineWidth=(el.borderw||1); ctx.beginPath(); ctx.arc(w/2,h/2,Math.min(w,h)/2-(el.borderw||1)/2,0,6.283); ctx.stroke(); break; }
    case 'hline': fillRect(0,(h-(el.thick||1))/2,w,el.thick||1,true); break;
    case 'vline': fillRect((w-(el.thick||1))/2,0,el.thick||1,h,true); break;
    case 'dot': fillRect(w/2-1,h/2-1,2,2,true); break;
    case 'statusbarList': {
      fillRect(0,0,w,22,true);
      paintText(el.title||'列表',6,(22-7)/2,1,true);
      const rowh=el.rowh||32;
      for(let i=0;i<(el.rows||4);i++){
        const y0=22+i*rowh;
        const on=el.selrow===i;
        if(on) fillRect(0,y0,w,rowh,true);
        paintText('● '+(i+1),(el.selrow===i)?12:16,y0+(rowh-7)/2,1,on);
        if(i<(el.rows||4)-1) fillRect(0,y0+rowh-1,w,1,false);
      }
      break;
    }
    case 'grid': {
      const cols=el.cols||4, rows=el.rows||3, cellw=w/cols, cellh=h/rows;
      for(let r=0;r<rows;r++)for(let c=0;c<cols;c++){
        const idx=r*cols+c, on=el.sel===idx;
        const cx=c*cellw+cellw/2, cy=r*cellh+cellh/2 + cellh*0.15;
        if(on) fillRect(c*cellw,r*cellh,cellw,cellh,true);
        paintCentered('◆',cx,cy,1,false);
        paintCentered(el.label||'App',cx,cy+6,1,on);
      }
      break;
    }
    case 'keyboard': {
      const rows=el.rows||3, rowh=h/rows;
      const lines=[el.chars1,el.chars2,el.chars3];
      for(let r=0;r<rows;r++){
        const chs=lines[r]||'', n=chs.length||1, kw=w/n;
        for(let c=0;c<n;c++){
          const on=el.selrow===r;
          if(on) fillRect(c*kw,r*rowh,kw,rowh,true);
          strokeRect(c*kw,r*rowh,kw,rowh,1,false);
          paintCentered(chs[c],c*kw+kw/2,r*rowh+rowh/2,rowh<=24?1:1.2,on);
        }
      }
      break;
    }
    case 'slider': {
      const th=6, y=(h-th)/2;
      strokeRect(0,y,w,th,1,true);
      const pos=w*(el.val||0)/100;
      fillRect(3,y,Math.max(0,pos-3),th,true);
      paintText(el.label||'',0,y-12,1,false);
      break;
    }
    default: strokeRect(0,0,w,h,1,true);
  }
}

/* =========================================================================
 * 5. 主渲染
 * ========================================================================= */
function render(){
  ctx.fillStyle='#fff'; ctx.fillRect(0,0,400,300);
  canvas.style.width=(400*state.zoom)+'px';
  canvas.style.height=(300*state.zoom)+'px';
  for(const el of state.els){
    const px=el.x||0, py=el.y||0;
    ctx.save(); ctx.translate(px,py);
    renderElem(el);
    ctx.restore();
    if(state.sel===el.id){
      ctx.strokeStyle='#ff6b3d'; ctx.lineWidth=1.5;
      ctx.strokeRect(px-1,py-1,(el.w||20)+2,(el.h||20)+2);
      drawHandles(px,py,el.w||20,el.h||20);
    }
  }
  if(state.marquee){
    ctx.strokeStyle='#3b82f6'; ctx.lineWidth=1; ctx.setLineDash([4,3]);
    const m=state.marquee;
    ctx.strokeRect(Math.min(m.x,m.x+m.w),Math.min(m.y,m.y+m.h),Math.abs(m.w),Math.abs(m.h));
    ctx.setLineDash([]);
  }
  document.getElementById('stats-info').textContent='元件: '+state.els.length;
}
function drawHandles(x,y,w,h){
  const pts=[[x,y],[x+w/2,y],[x+w,y],[x,y+h/2],[x+w,y+h/2],[x,y+h],[x+w/2,y+h],[x+w,y+h]];
  ctx.fillStyle='#3b82f6';
  for(const [hx,hy] of pts) ctx.fillRect(hx-3,hy-3,6,6);
}

/* =========================================================================
 * 6. 元件库
 * ========================================================================= */
function buildPallery(){
  const map={struct:'pal-struct',text:'pal-text',ctrl:'pal-ctrl',shape:'pal-shape',adv:'pal-adv'};
  for(const cat in PAL){
    const wrap=document.getElementById(map[cat]);
    for(const item of PAL[cat]){
      const b=document.createElement('div');
      b.className='pal-item';
      b.innerHTML=`<span class="pi-ico">${item.ico}</span>${item.name}`;
      b.addEventListener('click',()=> addEl(item.type, 10+Math.floor(Math.random()*120), 10+Math.floor(Math.random()*80)));
      wrap.appendChild(b);
    }
  }
}

/* =========================================================================
 * 7. 交互
 * ========================================================================= */
function canvasToLocal(ex,ey){
  const r=canvas.getBoundingClientRect();
  return {x:Math.round((ex-r.left)/state.zoom), y:Math.round((ey-r.top)/state.zoom)};
}
function byId(id){ return state.els.find(e=>e.id===id); }
function elAt(mx,my){
  for(let i=state.els.length-1;i>=0;i--){
    const el=state.els[i], w=el.w||20, h=el.h||20;
    if(mx>=el.x&&mx<=el.x+w&&my>=el.y&&my<=el.y+h) return el;
  }
  return null;
}
function hitHandle(el,mx,my){
  const {x,y}=el, w=el.w||20, h=el.h||20, c=5;
  const t=[['nw',x-c,y-c,x+c,y+c],['n',x+w/2-c,y-c,x+w/2+c,y+c],['ne',x+w-c,y-c,x+w+c,y+c],
    ['e',x+w-c,y+h/2-c,x+w+c,y+h/2+c],['se',x+w-c,y+h-c,x+w+c,y+h+c],['s',x+w/2-c,y+h-c,x+w/2+c,y+h+c],
    ['sw',x-c,y+h-c,x+c,y+h+c],['w',x-c,y+h/2-c,x+c,y+h/2+c]];
  for(const [k,a,b,c2,d] of t){ if(mx>=a&&mx<=c2&&my>=b&&my<=d) return k; }
  return null;
}
const DIR={'nw':[-1,-1],'n':[0,-1],'ne':[1,-1],'e':[1,0],'se':[1,1],'s':[0,1],'sw':[-1,1],'w':[-1,0]};

function installMouse(){
  let down=false, mode=null, selEl=null;
  const onDown=(ev)=>{
    ev.preventDefault();
    const {x:mx,y:my}=canvasToLocal(ev.clientX,ev.clientY);
    down=true;
    state._resizeStart=null;
    if(state.sel){ const el=byId(state.sel); if(el){ const hh=hitHandle(el,mx,my); if(hh){ mode='resize'; selEl=el; state._resizeStart={x:el.x,y:el.y,w:el.w,h:el.h}; state.resizeDir=hh; return; } } }
    const hit=elAt(mx,my);
    if(hit){ select(hit.id); mode='move'; selEl=hit; bumpInner(hit, mx-hit.x, my-hit.y); return; }
    select(null); mode='marquee'; state.marquee={x:mx,y:my,w:0,h:0};
  };
  const onMove=(ev)=>{
    if(!down) return;
    const {x:mx,y:my}=canvasToLocal(ev.clientX,ev.clientY);
    document.getElementById('stats-pos').textContent=`坐标: ${mx},${my}`;
    if(mode==='move'&&selEl){
      selEl.x=Math.max(0,Math.min(400-(selEl.w||20), selEl.x+(mx-startMx)));
      // 移动用增量: 已在 mousedown 存了基线到 selEl._bx
      selEl.x=Math.max(0,Math.min(400-(selEl.w||20), (selEl._bx||0)+(mx-startMx)));
      selEl.y=Math.max(0,Math.min(300-(selEl.h||20), (selEl._by||0)+(my-startMy)));
      snapMove(selEl);
      render(); if(state.sel)buildProps();
    }
    else if(mode==='resize'&&selEl){ doResize(selEl, mx,my); }
    else if(state.marquee){ state.marquee.w=mx-startMx; state.marquee.h=my-startMy; render(); }
  };
  const onUp=()=>{
    if(selEl){ const a=selEl; const r=state._resizeStart;
      const changed = (mode==='move' && (a.x!==a._bx||a.y!==a._by)) ||
                      (mode==='resize' && r && (a.x!==r.x||a.y!==r.y||a.w!==r.w||a.h!==r.h));
      if(changed) pushHistory();
    }
    if(mode==='move'&&selEl){ delete selEl._bx; delete selEl._by; }
    down=false; mode=null; state.marquee=null; state.resizeDir=null; state._resizeStart=null; selEl=null;
    render(); if(state.sel) buildProps();
  };
  let startMx=0, startMy=0;
  canvas.addEventListener('mousedown',(ev)=>{
    const {x,y}=canvasToLocal(ev.clientX,ev.clientY); startMx=x; startMy=y;
    if(ev.target===canvas){ /* 记录移动基线 */ }
    // 移动基线在 elAt 命中后设置
    onDown(ev);
    const hit=elAt(x,y); if(hit){ hit._bx=hit.x; hit._by=hit.y; }
  });
  window.addEventListener('mousemove',onMove);
  window.addEventListener('mouseup',onUp);
}
function snapMove(el){
  const T=4;
  let best={dx:0,dy:0};
  for(const o of state.els){ if(o===el) continue;
    const myX=el.x+(el.w||20)/2, myY=el.y+(el.h||20)/2;
    for(const myv of [el.x, myX, el.x+(el.w||20)]){
      for(const ov of [o.x, o.x+(o.w||20)/2, o.x+(o.w||20)]){
        const dx=ov-myv;
        if(best.dx===0 || Math.abs(dx)<Math.abs(best.dx)){ if(Math.abs(dx)<=T) best.dx=dx; }
      }
    }
    for(const myv of [el.y, myY, el.y+(el.h||20)]){
      for(const ov of [o.y, o.y+(o.h||20)/2, o.y+(o.h||20)]){
        const dy=ov-myv;
        if(best.dy===0 || Math.abs(dy)<Math.abs(best.dy)){ if(Math.abs(dy)<=T) best.dy=dy; }
      }
    }
  }
  if(Math.abs(best.dx)<=T) el.x+=best.dx;
  if(Math.abs(best.dy)<=T) el.y+=best.dy;
}
function doResize(el,mx,my){
  const r=state._resizeStart; if(!r) return;
  const x=r.x, y=r.y, w=r.w, h=r.h;
  let nx=x, ny=y, nw=w, nh=h;
  const d=state.resizeDir;
  if(d.includes('e')) nw=Math.max(4, mx-x);
  if(d.includes('s')) nh=Math.max(4, my-y);
  if(d.includes('w')){ const ww=Math.max(4, x+w-mx); nx=x+w-ww; nw=ww; }
  if(d.includes('n')){ const hh=Math.max(4, y+h-my); ny=y+h-hh; nh=hh; }
  el.x=Math.max(0,nx); el.y=Math.max(0,ny); el.w=Math.max(4,nw); el.h=Math.max(4,nh);
  render(); if(state.sel) buildProps();
}
function bumpInner(el,lx,ly){
  // 复合组件点内部选中
  if(el.type==='statusbarList'){ const rowh=el.rowh||32; if(ly>=22){ el.selrow=Math.min((el.rows||4)-1,Math.floor((ly-22)/rowh)); buildProps(); render(); } }
  if(el.type==='grid'){ const c=Math.floor(lx/(el.w/(el.cols||4))), r=Math.floor(ly/(el.h/(el.rows||3))); if(r>=0&&r<(el.rows||3)&&c>=0&&c<(el.cols||4)){ el.sel=r*(el.cols||4)+c; buildProps(); render(); } }
  if(el.type==='keyboard'){ const r=Math.floor(ly/(el.h/(el.rows||3))); if(r>=0&&r<(el.rows||3)){ el.selrow=r; buildProps(); render(); } }
}

/* =========================================================================
 * 8. 元件增删 / 属性面板
 * ========================================================================= */
function addEl(type,x,y){
  const meta=Object.values(PAL).flat().find(p=>p.type===type);
  const el={id:state.nextId++,type,x,y,w:20,h:20};
  if(meta) for(const d of meta.defs) el[d.k]=d.def;
  const dw=DEF_W[type]; if(dw){ el.w=dw[0]; el.h=dw[1]; }
  pushHistory();
  state.els.push(el); select(el.id); return el;
}
/* ===== 撤销/重做 ===== */
function snapshot(){ return JSON.parse(JSON.stringify(state.els)); }
function pushHistory(){
  const s=snapshot();
  state.hist.splice(state.histIdx+1);
  state.hist.push(s);
  if(state.hist.length>80) state.hist.shift();
  state.histIdx=state.hist.length-1;
}
function applyHistory(s){ state.els=JSON.parse(JSON.stringify(s)); if(state.sel&&!byId(state.sel)) state.sel=null; render(); buildProps(); }
function undo(){ if(state.histIdx>0){ state.histIdx--; applyHistory(state.hist[state.histIdx]); } else if(state.histIdx===0){ state.histIdx=-1; state.els=snapshot(); } }
function redo(){ if(state.histIdx<state.hist.length-1){ state.histIdx++; applyHistory(state.hist[state.histIdx]); } }

function select(id){ state.sel=id; buildProps(); render(); }
window.DL={
  del(){
    if(!state.sel){ alert('未选中元件: 请先在画布中点选一个元件再删除'); return; }
    const nm=byId(state.sel); if(nm) alert('已删除: '+(nm.text||nm.label||nm.type||'element'));
    pushHistory();
    state.els=state.els.filter(e=>e.id!==state.sel);
    state.sel=null; render(); buildProps();
  },
  dup(){
    if(!state.sel)return;
    const o=byId(state.sel); const n=JSON.parse(JSON.stringify(o));
    n.id=state.nextId++; n.x=Math.min(360,o.x+10); n.y=Math.min(260,o.y+10);
    pushHistory();
    state.els.push(n); select(n.id);
  },
  top(){
    if(!state.sel)return;
    const el=byId(state.sel); state.els=state.els.filter(e=>e!==el); state.els.push(el);
    pushHistory(); render();
  },
  undo(){ undo(); },
  redo(){ redo(); },
};
function buildProps(){
  const body=document.getElementById('prop-body');
  const el=state.sel?byId(state.sel):null;
  if(!el){ body.innerHTML='<div class="prop-empty">← 在画布中点选元件<br>或从左侧点按加入元件</div>'; return; }
  const meta=Object.values(PAL).flat().find(p=>p.type===el.type);
  let h=`<div class="prop-group"><div class="prop-group-title">${meta?meta.name:el.type}</div></div>`;
  h+=`<div class="prop-group"><div class="prop-group-title">位置/尺寸</div>
    ${rN('x','x',el.x,0,390)}${rN('y','y',el.y,0,290)}
    ${rN('w','w',el.w,4,400)}${rN('h','h',el.h,4,300)}</div>`;
  if(meta){ h+=`<div class="prop-group"><div class="prop-group-title">属性</div>`;
    for(const d of meta.defs){
      if(d.type==='text') h+=rT(d.name,d.k,el[d.k]);
      else if(d.type==='num') h+=rR(d.name,d.k,el[d.k],d);
      else if(d.type==='bool') h+=rB(d.name,d.k,el[d.k]);
      else if(d.type==='seg') h+=rS(d.name,d.k,el[d.k],d.opts);
    }
    h+='</div>';
  }
  h+=`<div class="prop-actions"><button class="dup" data-act="dup">复制</button>
    <button class="dup" data-act="top">置顶</button>
    <button class="del" data-act="del" type="button">删除</button></div>`;
  body.innerHTML=h;
  body.querySelectorAll('[data-k]').forEach(inp=>{
    const k=inp.dataset.k;
    inp.addEventListener(inp.type==='checkbox'?'change':'input',()=>{
      const e2=byId(state.sel); if(!e2)return;
      if(inp.type==='checkbox') e2[k]=inp.checked;
      else if(k==='x'||k==='y') e2[k]=Math.max(0,+inp.value||0);
      else if(k==='w'||k==='h') e2[k]=Math.max(4,+inp.value||4);
      else e2[k]=inp.value;
      render(); if(state.sel) buildProps();
    });
  });
  // 分段(seg)按钮
  body.querySelectorAll('[data-seg]').forEach(seg=>{
    const k=seg.dataset.seg;
    seg.querySelectorAll('button').forEach(btn=>{
      btn.addEventListener('click',()=>{
        const e2=byId(state.sel); if(!e2)return;
        e2[k]=btn.dataset.v;
        seg.querySelectorAll('button').forEach(x=>x.classList.toggle('active',x===btn));
        render();
      });
    });
  });
}
const rT=(n,k,v)=>`<div class="prop-row"><label>${n}</label><input data-k="${k}" type="text" value="${esc(v??'')}"></div>`;
const rN=(n,k,v,mn,mx)=>`<div class="prop-row"><label>${n}</label><input data-k="${k}" type="number" value="${v}" min="${mn}" max="${mx}"></div>`;
const rR=(n,k,v,d)=>`<div class="range-row"><label>${n}</label><input data-k="${k}" type="range" min="${d.min??0}" max="${d.max??100}" value="${v}"><output>${v}</output></div>`;
const rB=(n,k,v)=>`<div class="prop-row"><label>${n}</label><input data-k="${k}" type="checkbox" ${v?'checked':''}></div>`;
const rS=(n,k,v,opts)=>`<div class="prop-row"><label>${n}</label><div class="seg" data-seg="${k}">${opts.map(([val,lab])=>`<button data-v="${val}" class="${String(val)===String(v)?'active':''}">${lab}</button>`).join('')}</div></div>`;
function esc(s){ return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;'); }

/* =========================================================================
 * 9. 顶栏
 * ========================================================================= */
function bindTopbar(){
  document.querySelectorAll('#zoom-grp button').forEach(b=> b.addEventListener('click',()=>{
    state.zoom=+b.dataset.zoom; document.querySelectorAll('#zoom-grp button').forEach(x=>x.classList.toggle('active',x===b)); render();
  }));
  document.getElementById('btn-new').onclick=()=>{ if(!confirm('清空当前画布?'))return; pushHistory(); state.els=[]; state.sel=null; state.savedName=null; render(); buildProps(); };
  document.getElementById('btn-save').onclick=saveDesign;
  document.getElementById('btn-load').onclick=openLoad;
  document.getElementById('btn-export-c').onclick=exportC;
  document.getElementById('btn-preview').onclick=openPreview;
  document.addEventListener('keydown',(e)=>{
    const inField=/INPUT|TEXTAREA|SELECT/.test(document.activeElement.tagName);
    const mod=e.metaKey||e.ctrlKey;
    if(mod && (e.key==='z'||e.key==='Z')){ e.preventDefault(); mod&&e.shiftKey?DL.redo():DL.undo(); }
    else if(mod && (e.key==='y'||e.key==='Y')){ e.preventDefault(); DL.redo(); }
    else if(e.key==='Delete'){ if(state.sel&&!inField) DL.del(); }
  });
}
function saveDesign(){
  const name=prompt('方案文件名 (保存到项目 designs/):', state.savedName|| ('design_'+(new Date().toISOString().slice(0,16).replace(/[:T]/g,'-'))));
  if(!name)return;
  const data={meta:{name,w:400,h:300,created:new Date().toISOString()},els:state.els.map(e=>({...e}))};
  fetch('/save/'+encodeURIComponent(name),{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)})
    .then(r=>r.json()).then(res=>{ state.savedName=name; alert('已保存到:\n'+res.file); })
    .catch(()=>alert('保存失败: 请先运行 server.py'));
}
function openLoad(){ fetch('/designs').then(r=>r.json()).then(list=>{
    const box=document.getElementById('load-list'); box.innerHTML='';
    if(!list.length) box.innerHTML='<div class="prop-empty">暂无已保存方案</div>';
    list.forEach(f=>{ const d=document.createElement('div'); d.className='load-item';
      d.innerHTML=`<span class="li-name">${esc(f.replace('.json',''))}</span><span class="li-time">↧ 载入</span>`;
      d.onclick=()=>{ loadByName(f.replace('.json','')); document.getElementById('modal-load').hidden=true; };
      box.appendChild(d); });
    document.getElementById('modal-load').hidden=false;
  }).catch(()=>alert('无法列出方案: 请先运行 server.py'));
}
function loadByName(name){ fetch('/designs/'+encodeURIComponent(name)).then(r=>r.json()).then(data=>{
    if(data.els){ state.els=data.els; state.sel=null; state.savedName=name; render(); buildProps(); alert('已载入: '+name); }
  }).catch(()=>alert('载入失败')); }
document.getElementById('ml-cancel').onclick=()=>{ document.getElementById('modal-load').hidden=true; };

function openPreview(){ render();
  const pv=document.getElementById('screen-preview').getContext('2d');
  pv.imageSmoothingEnabled=false; pv.fillStyle='#fff'; pv.fillRect(0,0,1600,1200);
  pv.drawImage(canvas,0,0,1600,1200);
  document.getElementById('modal-preview').hidden=false;
}
document.getElementById('mp-close').onclick=()=>{ document.getElementById('modal-preview').hidden=true; };

function exportC(){
  const lines=[]; lines.push('// LinTOS UI 设计器 导出 (400x300 单色, 按元件顺序)');
  lines.push('// 在页面 render 中自行 st7305_clear(ST7305_COLOR_WHITE) 打底后调用:');
  lines.push('');
  for(const el of state.els) lines.push(ccode(el));
  const w=document.createElement('div'); w.className='modal';
  w.innerHTML=`<div class="modal-box"><h3>导出 C 代码片段</h3><textarea id="export-text" readonly>${esc(lines.join('\n'))}</textarea>
  <div class="modal-actions"><button id="export-copy">复制</button><button onclick="this.remove()">关闭</button></div></div>`;
  document.body.appendChild(w);
  w.querySelector('#export-copy').onclick=()=>{ const t=w.querySelector('#export-text'); t.select(); navigator.clipboard&&navigator.clipboard.writeText(t.value); };
}
function ccode(el){
  const {type,x=0,y=0,w=20,h=20}=el; const inv=el.invert?'true':'false';
  switch(type){
    case 'text': return `draw_text(lcd, ${x}, ${y}, "${esc(el.text||'')}", ${inv});`;
    case 'label': return `draw_text(lcd, ${x}, ${y}, "${esc(el.text||'')}", ${inv}); /* 小字 */`;
    case 'statusbar': return `fill_rect(lcd, 0, 0, 399, ${(el.h||24)-1}, ST7305_COLOR_BLACK); // 信息条 "${esc(el.text)}" / "${esc(el.right)}"`;
    case 'button': return `draw_rect_outline(lcd, ${x}, ${y}, ${x+w-1}, ${y+h-1}, ST7305_COLOR_BLACK);\n  draw_text_centered(lcd, ${y+(h-24)/2}, "${esc(el.text)}", ${inv});`;
    case 'rect': return `fill_rect(lcd, ${x}, ${y}, ${x+w-1}, ${y+h-1}, ST7305_COLOR_BLACK);`;
    case 'rectout': return `draw_rect_outline(lcd, ${x}, ${y}, ${x+w-1}, ${y+h-1}, ST7305_COLOR_BLACK);`;
    case 'hline': return `draw_hline(lcd, ${x}, ${x+w-1}, ${y+(h/2)}, ST7305_COLOR_BLACK);`;
    case 'vline': return `draw_vline(lcd, ${x+w/2}, ${y}, ${y+h-1}, ST7305_COLOR_BLACK);`;
    case 'progress': return `// 进度条 ${el.pct}%\n  fill_rect(lcd, ${x+2}, ${y+(h-(el.thick||8))/2}, ${Math.max(x+2, x+2+(w-8)*((el.pct||0)/100)-1)}, ${y+(h-(el.thick||8))/2+(el.thick||8)-2}, ST7305_COLOR_BLACK);`;
    case 'button2': break;
    default: return `/* ${type} @ ${x},${y} ${w}x${h} */`;
  }
}

/* =========================================================================
 * 10. 启动
 * ========================================================================= */
function loadSample(){
  addEl('statusbar',0,0);
  addEl('text',120,70);
  addEl('button',80,240);
  addEl('progress',30,180);
  state.sel=null;
}
/* 属性面板操作按钮: 事件委托 (重建 innerHTML 后仍有效, 不依赖 getElementById) */
function bindPropsDelegate(){
  const box=document.getElementById('prop-body');
  box.addEventListener('click',(e)=>{
    const b=e.target.closest('[data-act]');
    if(!b) return;
    e.stopPropagation();
    const act=b.dataset.act;
    if(act==='del') DL.del(); else if(act==='dup') DL.dup(); else if(act==='top') DL.top();
  });
}

function init(){ buildPallery(); installMouse(); bindTopbar(); bindPropsDelegate(); loadSample(); render(); buildProps(); }
window.addEventListener('DOMContentLoaded', init);
})();