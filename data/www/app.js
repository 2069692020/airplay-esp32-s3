/* AirPlay 接收器 —— 主页应用逻辑
 *
 * 页面是单页四视图（播放 / 音乐 / 设置 / 系统）+ 常驻播放条。
 * 所有取值链路和上一版一致：接口、节流、重入保护都没改，改的只有渲染目标。
 *
 * ⚠️ 轮询节流的三条硬约束（动之前先读，都是实测换来的）：
 *   1. 设备是单核跑 httpd + 音频解码，请求发太密会把音频线程饿死（听感就是爆音）。
 *   2. 每个轮询函数自带"还在跑就跳过"的重入标志，否则设备一慢请求就成串堆积。
 *   3. radio/sd 的状态轮询各自只能有一条链 —— 见下面 schedule 的 clearTimeout。
 */

/* ---------------- 视图路由 ---------------- */
var VIEWS=['play','music','set','sys'];
var curView='play';

function go(v){
  if(VIEWS.indexOf(v)<0)v='play';
  curView=v;
  for(var i=0;i<VIEWS.length;i++){
    var el=document.getElementById('v-'+VIEWS[i]);
    el.hidden=(VIEWS[i]!==v);
    el.classList.toggle('on',VIEWS[i]===v);
  }
  var tabs=document.querySelectorAll('.nav-i');
  for(var j=0;j<tabs.length;j++)tabs[j].classList.toggle('on',tabs[j].dataset.view===v);
  if(history.replaceState)history.replaceState(null,'','#'+v);else location.hash=v;
  renderNow();                 /* 迷你条在播放页要收起 */
  window.scrollTo(0,0);
}

function musicTab(which){
  var sd=(which!=='radio');
  document.getElementById('mtab-sd').hidden=!sd;
  document.getElementById('mtab-radio').hidden=sd;
  document.getElementById('seg-sd').classList.toggle('on',sd);
  document.getElementById('seg-radio').classList.toggle('on',!sd);
}

/* ---------------- 通用小工具 ---------------- */
/* 提示条：txt 一律 textContent。调用方喂进来的 d.error 是服务器回显的原始
   字符串（含 SD 卡文件名 / 电台 URL），走 innerHTML 就是 XSS 入口。 */
function msg(id,txt,type){var e=document.getElementById(id);if(!e)return;
  var d=document.createElement('div');d.className='msg '+(type||'info');
  d.textContent=txt;e.textContent='';e.appendChild(d);
  setTimeout(function(){if(d.parentNode===e)e.textContent='';},4000);}
/* 外部数据（曲名 / SSID / 文件名）进 innerHTML 前必须过这里 */
function esc(s){return String(s).replace(/[&<>"']/g,function(c){
  return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];});}
function setText(id,v){var e=document.getElementById(id);if(e)e.textContent=(v===0?v:(v||'-'));}
function dash(v){return (v===0?v:(v||'-'));}
function fmtUptime(s){if(s==null)return '-';
  var d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);
  if(d)return d+'d '+h+'h';
  if(h)return h+'h '+m+'m';
  return m+'m '+(s%60)+'s';}
function fmtTime(s){s=Math.max(0,Math.floor(s||0));
  var m=Math.floor(s/60);
  return (m<10?'0':'')+m+':'+((s%60)<10?'0':'')+(s%60);}
function fmtBytes(n){
  if(n===null||n===undefined||isNaN(n))return '';
  var u=['B','KB','MB','GB','TB'],i=0;
  while(n>=1024&&i<u.length-1){n/=1024;i++;}
  return (i===0?String(n):n.toFixed(n<10?1:0))+' '+u[i];}

/* ---------------- 全局状态 ---------------- */
var playbackSource='none';     /* /api/system/info: playback_source
                                  = 谁占着音频通路（airplay 服务在 / 蓝牙占用 / 无）。
                                  ⚠️ 它不代表"手机正在推流"，别拿来当播放状态显示。 */
var apNow=null;                /* /api/system/info 里的 AirPlay 曲目快照:
                                  {state:'standby'|'connected'|'playing'|'paused',
                                   title,artist,album,position,duration}
                                  state 取自设备侧 OLED 同源的那份缓存
                                  （display_get_now_playing），所以它是真判据；
                                  standby = 没有活的 AirPlay 会话。 */
var ethConnected=false;        /* 有线优先显示在顶栏的接入方式里 */
var sdLast=null;               /* /api/sd/state 最后一次响应 */
var radioLast=null;            /* /api/music 最后一次响应 */
var staConnected=false;
var lastWifiSig='';
var dualDevices=1, dualMode=0, dualLoaded=false;
var chanLocked=false, chanDsp=false;
var sdDir='/', sdPlaying='', sdBusy=false;
var sdTimer=null, radioTimer=null;

/* 当前该显示哪一路：SD / 网络音乐各有自己的 active 标志。
   AirPlay 这一路以前判定不了 —— `audio_receiver_is_playing()` 读的是
   `timing.playing`，而 `audio_timing_init()` 把它初始化成 true，开机没会话时也
   报"在放"（§30.4 记的就是这个，别再用它）。现在改用面板那份快照的 state
   （设备侧 display_get_now_playing → /api/system/info 的 airplay_state），
   standby 才是真的"没有活的会话"，所以手机推流时这里也能显示曲目。 */
function activeSrc(){
  if(sdLast&&sdLast.active)return 'sd';
  if(radioLast&&radioLast.active)return 'radio';
  /* AirPlay 排最后：本地两路是这台设备自己能控的，手机那一路只是"看得见"。 */
  if(apNow&&apNow.state!=='standby'&&apNow.title)return 'airplay';
  return null;
}

/* ---------------- 顶栏 / 播放条 / 音源行 ---------------- */
function renderChrome(){
  var lamp=document.getElementById('conn-lamp');
  var chip=document.getElementById('bar-net');
  lamp.className='lamp '+(staConnected?'ok':'err');
  chip.className='chip '+(staConnected?'ok':'err');
  chip.textContent=ethConnected?'有线':(staConnected?'WiFi':'未连接');

  var s=activeSrc();
  /* 三行同一套语义：绿=这一路**正在出声**；蓝=AirPlay 服务就绪但没在推；
     琥珀=音频通路被蓝牙占走。AirPlay 的"在不在推"用 apNow.state（面板同源），
     不用 playback_source —— 后者只说谁占着通路，服务一起来就是 airplay。 */
  var apRow=document.getElementById('src-airplay');
  var apSt=document.getElementById('st-airplay');
  apRow.classList.remove('ready','held','live');
  var apPlaying=!!(apNow&&apNow.state==='playing');
  if(apPlaying){
    /* 手机真的在推流 —— 和 SD / 网络音乐一样用绿色这盏"正在出声"灯。 */
    apRow.classList.add('live');apSt.textContent='播放中';
  }else if(apNow&&apNow.state==='paused'){
    apRow.classList.add('ready');apSt.textContent='已暂停';
  }else if(playbackSource==='bluetooth'){
    apRow.classList.add('held');apSt.textContent='蓝牙占用';
  }else if(playbackSource==='airplay'){
    apRow.classList.add('ready');apSt.textContent='服务就绪';
  }else apSt.textContent='未启动';
  document.getElementById('st-sd').textContent=sdLabel();
  document.getElementById('src-sd').classList.toggle('live',!!(sdLast&&sdLast.active));
  document.getElementById('st-radio').textContent=radioLabel();
  document.getElementById('src-radio').classList.toggle('live',!!(radioLast&&radioLast.active));

  var app=document.querySelector('.app');
  var mini=document.getElementById('mini');
  var show=!!s&&curView!=='play';
  mini.classList.toggle('on',show);
  app.classList.toggle('has-mini',show);
  if(show){
    var d=(s==='sd')?sdLast:radioLast;
    mini.classList.toggle('playing',!!(d&&d.state==='playing'));
    var info=npInfo(s);
    document.getElementById('mini-t').textContent=info.title;
    document.getElementById('mini-s').textContent=info.meta;
    document.getElementById('mini-src').textContent=info.src;
  }
  renderNavMark();
}

function sdLabel(){
  if(!sdLast)return '—';
  if(!sdLast.mounted)return '未插卡';
  if(sdLast.state==='error')return '出错';
  if(sdLast.state==='playing')return '播放中'+idx(sdLast);
  return '空闲';
}
function radioLabel(){
  if(!radioLast)return '—';
  if(radioLast.state==='error')return '出错';
  if(radioLast.state==='connecting')return '连接中';
  if(radioLast.state==='playing')return '播放中'+idx(radioLast);
  return '空闲';
}
function idx(d){return (d.count>1&&d.index>0)?(' '+d.index+'/'+d.count):'';}

/* 正在播放的三个字段：标题 / 副标题 / 源标签 */
function npInfo(s){
  if(s==='airplay'){
    if(!apNow)return {title:'—',meta:'',src:'AIRPLAY'};
    var am=[];
    if(apNow.artist)am.push(apNow.artist);
    if(apNow.album)am.push(apNow.album);
    if(apNow.state==='paused')am.push('手机上已暂停');
    return {title:apNow.title||'（手机没给曲名）',meta:am.join(' · '),src:'AIRPLAY'};
  }
  var d=(s==='sd')?sdLast:radioLast;
  if(!d)return {title:'—',meta:'',src:''};
  var meta=[];
  if(d.artist)meta.push(d.artist);
  if(d.album)meta.push(d.album);
  if(d.count>1&&d.index>0)meta.push(d.index+'/'+d.count);
  var fallback=(s==='sd')?shortName(d.path):shortName(d.cur_url||d.url);
  return {title:d.title||fallback||'（无标签）',
          meta:meta.join(' · '),src:(s==='sd'?'SD':'NET')};
}
function shortName(p){if(!p)return '';var i=p.lastIndexOf('/');return i<0?p:p.substring(i+1);}

/* 播放页的大块 */
function renderNow(){
  var s=activeSrc();
  var np=document.getElementById('np'), empty=document.getElementById('np-empty');
  if(!s){np.hidden=true;empty.hidden=false;renderChrome();return;}
  np.hidden=false;empty.hidden=true;
  var info=npInfo(s);
  document.getElementById('np-title').textContent=info.title;
  document.getElementById('np-meta').textContent=info.meta;
  document.getElementById('np-src').textContent=info.src;
  var clk='';
  var tr=document.getElementById('np-tr');
  if(s==='airplay'){
    /* 本机控不了手机那一路（AirPlay 2 没有 DACP 通路），所以整排按钮
       **藏掉**而不是置灰 —— 灰掉的按钮会让人以为点得动。 */
    if(tr)tr.hidden=true;
    if(apNow&&apNow.duration){
      clk=fmtTime(apNow.position||0)+' / '+fmtTime(apNow.duration);
    }
    document.getElementById('np-clock').textContent=clk;
    document.getElementById('np-hint').textContent=
      '这一路的播放/暂停/切曲都在手机上，本机只能显示收到的曲目信息。';
    renderChrome();
    return;
  }
  if(tr)tr.hidden=false;
  if(s==='sd'&&sdLast&&sdLast.state==='playing'){
    clk=fmtTime(sdLast.elapsed);
    if(sdLast.sample_rate)clk+=' · '+(sdLast.sample_rate/1000).toFixed(sdLast.sample_rate%1000?1:0)+' kHz';
  }
  document.getElementById('np-clock').textContent=clk;
  /* 上一条/下一条只在歌单模式下有效 —— 与音乐页那组按钮用同一个判据。 */
  var d=(s==='sd')?sdLast:radioLast;
  var multi=!!(d&&d.count>1);
  document.getElementById('tr-prev').disabled=!multi;
  document.getElementById('tr-next').disabled=!multi;
  document.getElementById('tr-stop').disabled=false;
  document.getElementById('np-hint').textContent=multi?'':'「上一条 / 下一条」只在歌单模式下有效。';
  renderChrome();
}

function trStop(){if(activeSrc()==='sd')sdStop();else if(activeSrc()==='radio')radioStop();}
function trSkip(ep){if(activeSrc()==='sd')sdSkip(ep);else if(activeSrc()==='radio')radioSkip(ep);}

/* ---------------- 轮询调度 ---------------- */
var inflight={info:false,vol:false,bri:false,chan:false,wifi:false};
function guard(key,fn){
  return async function(){
    if(inflight[key])return;
    inflight[key]=true;
    try{await fn();}finally{inflight[key]=false;}
  };
}
setInterval(guard('info',loadInfo),5000);
setInterval(guard('vol',loadVolume),5000);
setInterval(guard('bri',loadBrightness),5000);
setInterval(guard('chan',loadChannel),5000);
/* WiFi 扫描只在 **STA 已断开** 时自动做 —— 已连上时扫描没意义，
   而且扫描期间 WiFi 会短暂断流（实测：会打断正在播的流）。 */
setInterval(function(){if(!staConnected)scanWiFi(true);},15000);

/* ---------------- 系统信息 ---------------- */
async function loadInfo(){
  try{var r=await fetch('/api/system/info');var d=await r.json();
    if(!d.success)return;var i=d.info;
    staConnected=!!(i.eth_connected||i.wifi_connected);
    ethConnected=!!i.eth_connected;
    playbackSource=i.playback_source||'none';
    apNow={state:i.airplay_state||'standby',title:i.airplay_title||'',
           artist:i.airplay_artist||'',album:i.airplay_album||'',
           position:i.airplay_position_s,duration:i.airplay_duration_s};
    otaEnabled=(i.ota_enabled===true);
    renderOta();
    setText('info-src',{airplay:'AirPlay 服务',bluetooth:'蓝牙',none:'空闲'}[playbackSource]||playbackSource);

    var nm=document.getElementById('bar-name');
    nm.textContent=i.device_name||'AirPlay 接收器';
    nm.classList.toggle('is-def',!i.device_name);

    setText('info-ip',i.ip);
    setText('info-mac',i.mac);
    setText('info-name',i.device_name);
    setText('info-heap',i.free_heap?Math.round(i.free_heap/1024)+' KB':'-');
    setText('info-fw',i.firmware_version);
    setText('info-slot',i.fw_slot);
    setText('rst-uptime',fmtUptime(i.uptime_s));   /* 挪进了「上次复位」框 */
    setText('info-via',ethConnected?'有线以太网':(i.wifi_connected?'WiFi 客户端':'未接入'));
    /* 热点：开着就顺带给固定入口地址，DHCP 换 IP 时不必去路由器后台翻 */
    var ap=document.getElementById('info-ap');
    ap.textContent=(i.ap_open===undefined)?'-':
      (i.ap_open?('开'+(i.ap_stations?(' · '+i.ap_stations+' 台'):' · 无人连')):'关');
    ap.title=i.ap_url||'';
    ap.className='v '+(i.ap_open?'ok':'');
    document.getElementById('info-name').title=dash(i.device_name);
    document.getElementById('info-ip').title=dash(i.ip)+'（点按复制）';

    /* 复位原因：固件发的是小写英文码（web_server.c: reset_reason_str），
       直接显示 code 看不出所以然。unknown 不算异常 —— 冷启动烧录后芯片
       读不到上一次的原因，染红会误导。 */
    var rz=document.getElementById('info-reset');
    var rc=(i.reset_reason||'').toLowerCase();
    var RZ={poweron:['上电',1],external:['复位键',1],software:['软重启',1],
      deepsleep:['深睡眠',1],sdio:['SDIO',1],panic:['崩溃',0],int_wdt:['看门狗',0],
      task_wdt:['任务看门狗',0],other_wdt:['看门狗',0],brownout:['欠压',0]};
    var e=RZ[rc];
    rz.textContent=e?e[0]:(rc||'-');
    rz.title=dash(i.reset_reason);
    rz.className='v '+(e?(e[1]?'ok':'err'):'');

    setText('info-ssid',i.wifi_ssid);
    setText('info-rssi',i.wifi_rssi==null?'-':(i.wifi_rssi+' dBm'));
    setText('info-chan',i.wifi_channel);
    setText('info-phy',i.wifi_phy);
    setText('info-bssid',i.wifi_bssid);
    document.getElementById('info-refresh').textContent=
      '更新于 '+new Date().toLocaleTimeString('zh-CN',{hour:'2-digit',minute:'2-digit'});
    document.getElementById('dev-name').placeholder=i.device_name||'';
    document.getElementById('wifi-tag').textContent=staConnected?'已连接':'未连接';

    if(i.eq_supported||i.sub_supported){document.getElementById('eq-link').style.display='';}
    /* 只有 TAS58xx 构建才会注册 /api/audio/dual，贸然请求会让设备在每次
       打开页面时都打一条警告日志。 */
    if(i.dual_supported&&!dualLoaded){dualLoaded=true;loadDual();}
    renderEnv(i);
    renderNow();
  }catch(e){}}
/* ---------------- 温湿度 / 系统时间 ----------------
 * 两个特性都是"编译期可选"的：字段不在（未启用对应 Kconfig）时整块隐藏，
 * 字段在但值为空（传感器没接 / 还没对上位）时只隐藏那一格。
 * 任何情况下都不拿 0 或 1970 填空 —— 那种值在页面上长得跟真读数一样。 */
function renderEnv(i){
  var chip=document.getElementById('bar-env');
  var grp=document.getElementById('env-grp');
  var sensor=(i.sensor_ok===true);
  var hasTime=(i.time_synced!==undefined);

  document.getElementById('env-temp-cell').hidden=!sensor;
  document.getElementById('env-hum-cell').hidden=!sensor;
  if(sensor){
    setText('env-temp',(Math.round(i.temperature_c*10)/10).toFixed(1)+' °C');
    setText('env-hum',(Math.round(i.humidity_pct*10)/10).toFixed(1)+' %RH');
    var age=i.sensor_age_s||0;
    /* 不用 setText()：它把空值写成 "-"，挂在标题旁边像个坏掉的读数。
       新鲜读数时这里就该是空的。 */
    document.getElementById('env-age').textContent=age>15?('读数 '+age+' 秒前'):'';
    if(chip){chip.textContent=(Math.round(i.temperature_c*10)/10).toFixed(1)
      +'° '+Math.round(i.humidity_pct)+'%';chip.hidden=false;}
  }else if(chip){chip.hidden=true;}

  if(hasTime){
    setText('env-clock',i.time_local||'—');
    var n=document.getElementById('env-ntp');
    n.textContent=i.time_synced?'已同步':'未同步';
    n.className='v '+(i.time_synced?'ok':'warn');
    var rt=document.getElementById('rst-time');
    if(i.boot_time_local){rt.textContent=i.boot_time_local;rt.className='v';}
    else{rt.textContent='待时间同步后显示';rt.className='v warn';}
  }
  grp.hidden=!(sensor||hasTime);
}

/* 点 IP 复制 —— 省得对着一串数字手抄到电脑上 */
document.addEventListener('click',function(ev){
  var t=ev.target;
  if(!t||t.id!=='info-ip')return;
  var v=t.textContent;
  if(!v||v==='-')return;
  function done(ok){t.textContent=ok?'已复制':'复制失败';
    /* 失败提示要留够时间被看见；成功后 1.2 秒交回给下一轮刷新。 */
    setTimeout(loadInfo,ok?1200:3000);}
  if(navigator.clipboard&&navigator.clipboard.writeText){
    navigator.clipboard.writeText(v).then(function(){done(true);},function(){done(false);});
  }else{
    var ta=document.createElement('textarea');ta.value=v;ta.style.position='fixed';ta.style.opacity='0';
    document.body.appendChild(ta);ta.select();
    var ok=false;try{ok=document.execCommand('copy');}catch(e){}
    document.body.removeChild(ta);done(ok);}
});

/* ---------------- 音量 ----------------
 * 拖动要"跟手"又不打爆设备，三个约束下的折中：
 *  1. 拖的时候只改本地 DOM，一个请求都不发 → 拖动不掉帧。
 *  2. POST 最快每 120ms 一次 → 每个 input 事件都发会把音频线程饿死。
 *  3. 轮询在拖动中必须跳过 → 否则设备还没处理的旧值会写回滑块，看起来"跳回去"。
 *     同理 POST 的响应也不回写。change 事件再补一次最终值。
 */
var volDragging=false, volLastSent=0, volLastValue=50;
var volMuted=false, volBeforeMute=50;

function paintRange(el){
  var min=Number(el.min)||0, max=Number(el.max)||100;
  var pct=max>min?(Number(el.value)-min)/(max-min)*100:0;
  el.style.backgroundImage='linear-gradient(90deg,var(--accent) '+pct+'%,var(--line) '+pct+'%)';
}
function volPaint(v){
  var el=document.getElementById('volume');
  if(Number(el.value)!==Number(v))el.value=v;
  document.getElementById('vol-pct').innerHTML=Math.round(v)+'<i>%</i>';
  paintRange(el);
}
function volSend(v,force){
  var now=Date.now();
  if(!force&&now-volLastSent<120)return Promise.resolve();
  volLastSent=now;
  return fetch('/api/volume',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({percent:Math.round(v)})}).catch(function(e){console.warn('音量上报失败',e);});
}
function onVolumeInput(v){
  volDragging=true;volLastValue=Number(v);
  volMuted=false;document.getElementById('mute-btn').classList.remove('on');
  volPaint(v);volSend(v,false);
}
function onVolumeCommit(v){
  volLastValue=Number(v);
  volSend(v,true).then(function(){volDragging=false;},function(){volDragging=false;});
}
function toggleMute(){
  volMuted=!volMuted;
  document.getElementById('mute-btn').classList.toggle('on',volMuted);
  if(volMuted){volBeforeMute=Number(document.getElementById('volume').value);volPaint(0);volSend(0,true);}
  else{volPaint(volBeforeMute);volSend(volBeforeMute,true);}
}
async function loadVolume(){
  if(volDragging)return;
  try{var r=await fetch('/api/volume');var d=await r.json();
    if(d.success&&!volDragging){volLastValue=Number(d.percent);volPaint(d.percent);}
  }catch(e){}}

/* ---------------- LED 亮度 ---------------- */
function onBrightnessInput(el){
  document.getElementById('led-pct').textContent=Math.round(el.value/255*100)+'%';
  paintRange(el);
}
async function loadBrightness(){
  try{var r=await fetch('/api/led/brightness');var d=await r.json();
    if(d.success){var s=document.getElementById('led-brightness');s.value=d.brightness;
      document.getElementById('led-pct').textContent=Math.round(d.brightness/255*100)+'%';
      paintRange(s);}
  }catch(e){console.warn('无法读取 LED 亮度',e);}}
async function saveBrightness(){
  var v=Number.parseInt(document.getElementById('led-brightness').value,10);
  try{var r=await fetch('/api/led/brightness',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({brightness:v})});
    var d=await r.json();if(d.success)msg('brightness-msg','已保存！','ok');
    else msg('brightness-msg',d.error,'err');
  }catch(e){msg('brightness-msg','出错了','err');console.error('保存亮度失败',e);}}

/* ---------------- 声道 / 双 DAC ---------------- */
async function loadChannel(){
  try{var r=await fetch('/api/audio/channel');var d=await r.json();
    if(d.success){document.getElementById('chan-mode').value=String(d.mode);
      chanLocked=!!d.locked;chanDsp=!!d.dsp;}
  }catch(e){console.warn('无法读取声道模式',e);}
  applyChanLabels();applyAmpLayout();}
/* 双线分音(Bi-Amp)会把两路输出交叉接到同一只音箱上，所以这里选的是
   喂给它哪一路输入——没有立体声可以直通。 */
function applyChanLabels(){
  document.getElementById('chan-label').textContent=chanDsp?'输入声道':'输出声道';
  var o=document.getElementById('chan-mode').options;
  o[0].text=chanDsp?'两路混合 (L+R)/2':'立体声';
  o[3].text=chanDsp?'两路混合 (L+R)/2':'单声道 (L+R)';
}
var DUAL_WARN={
  0:'切换到 2.1（卫星箱 / 低音炮）？\n\n第二路功放会被重新配置为 PBTL 单声道：它的 OUT_A 与 OUT_B 端子必须短接在一起，再接到一只低音炮上。\n\n请先改好接线再重启。',
  1:'切换到双线分音 (Bi-Amp)？\n\n两路功放都工作在立体声 BTL 模式，OUT_A 推低音单元、OUT_B 推高音单元，各自驱动一只音箱。\n\n如果 OUT_A 和 OUT_B 目前仍为 PBTL 低音炮短接在一起，必须先断开短接——保持短接会让两路输出互相对冲，可能烧毁功放及其输出滤波器。'
};
/* 双功放板卡的 DAC 配置本身已经决定了走线，所以单路声道选择没有意义。 */
function applyAmpLayout(){
  document.getElementById('chan-group').style.display=
    (chanLocked||dualDevices>1)?'none':'';
}
async function saveChannel(){
  var v=Number.parseInt(document.getElementById('chan-mode').value,10);
  try{var r=await fetch('/api/audio/channel',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({mode:v})});
    var d=await r.json();
    if(d.success){msg('chan-msg','已保存！','ok');
      /* 固件有可能不采纳请求，仍保持立体声。 */
      if(typeof d.mode==='number')document.getElementById('chan-mode').value=String(d.mode);
      chanLocked=!!d.locked;applyAmpLayout();}
    else msg('chan-msg',d.error,'err');
  }catch(e){msg('chan-msg','出错了','err');console.error('保存声道模式失败',e);}}
async function loadDual(){
  try{var r=await fetch('/api/audio/dual');if(!r.ok)return;var d=await r.json();
    if(d.success&&d.devices>1){dualDevices=d.devices;dualMode=d.mode;
      var sel=document.getElementById('dual-mode');
      sel.value=String(d.mode);
      if(!d.biamp){
        sel.querySelector('option[value="1"]').disabled=true;
        sel.querySelector('option[value="1"]').textContent='双线分音 (Bi-Amp)（不可用）';}
      document.getElementById('dual-group').style.display='';
      if(d.restart_required)msg('dual-msg','重启后新接线才会生效。','err');}
  }catch(e){console.warn('无法读取双 DAC 模式',e);}
  applyAmpLayout();}
async function saveDual(){
  var sel=document.getElementById('dual-mode');
  var v=Number.parseInt(sel.value,10);
  if(v!==dualMode&&!confirm(DUAL_WARN[v])){sel.value=String(dualMode);return;}
  try{var r=await fetch('/api/audio/dual',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({mode:v})});
    var d=await r.json();
    if(d.success){dualMode=v;msg('dual-msg','已保存！请改好音箱接线，然后重启生效。','ok');}
    else{sel.value=String(dualMode);msg('dual-msg',d.error,'err');}
  }catch(e){sel.value=String(dualMode);msg('dual-msg','出错了','err');console.error('保存 DAC 配置失败',e);}}

/* ---------------- 设备名 / 重启 ---------------- */
async function saveName(){
  var n=document.getElementById('dev-name').value.trim();
  if(!n){msg('name-msg','请输入名称','err');return;}
  try{var r=await fetch('/api/device/name',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({name:n})});
    var d=await r.json();if(d.success){msg('name-msg','已保存！重启后生效。','ok');loadInfo();}
    else msg('name-msg',d.error,'err');
  }catch(e){msg('name-msg','出错了','err');}}
async function restart(){
  if(!confirm('确定要重启设备吗？'))return;
  try{await fetch('/api/system/restart',{method:'POST'});}catch(e){}}

/* ---------------- 固件升级 (OTA) ----------------
 * 和设备侧 ota_update_handler / ota_password_handler 一一对应的三条规则：
 *   1. 口令为空 = 接口 403（默认关闭），页面上也就是"未启用"；
 *   2. 上传固件把口令放在 X-OTA-Password 头里；
 *   3. 改口令要带旧口令，从没设过的时候不需要（否则永远打不开）。
 * 口令**不往页面里任何地方存**（不用 localStorage / 不回显），刷新后要重填。
 */
var otaEnabled=false;
var otaFile=null;

function renderOta(){
  var st=document.getElementById('ota-state');
  var cur=document.getElementById('ota-cur-row');
  var ds=document.getElementById('ota-desc');
  var lb=document.getElementById('ota-new-label');
  if(!st)return;
  st.textContent=otaEnabled?'已启用':'未启用';
  st.style.color=otaEnabled?'var(--ok-tx)':'';
  if(cur)cur.hidden=!otaEnabled;
  if(lb)lb.textContent=otaEnabled?'改成新口令（留空 = 关闭升级）':'设置升级口令';
  if(ds)ds.textContent=otaEnabled
    ?'选一个 app 镜像（airplay2-receiver.bin）上传即可。写的是另一个槽，镜像验不过不会切过去；成功后设备自动重启。'
    :'网页升级目前是关闭的 —— 设一个口令才会打开。不想用就照旧 USB 烧，这条接口会一直回 403。';
  updateOtaBtn();
}

function updateOtaBtn(){
  var b=document.getElementById('ota-btn');
  if(!b)return;
  var pw=document.getElementById('ota-cur');
  var havePw=!otaEnabled||(pw&&pw.value);
  b.disabled=!(otaFile&&havePw);
}

async function otaSavePw(){
  var ne=document.getElementById('ota-new');
  var cu=document.getElementById('ota-cur');
  var pw=ne?ne.value:'';
  var cur=cu?cu.value:'';
  if(pw.length>64){msg('ota-pw-msg','口令太长（上限 64 字节）','err');return;}
  try{
    var r=await fetch('/api/ota/password',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({password:pw,current:cur})});
    var d=await r.json().catch(function(){return null;});
    if(!r.ok){
      msg('ota-pw-msg',(d&&(d.error||d.message))||('HTTP '+r.status),'err');
      return;
    }
    if(d&&d.success){
      msg('ota-pw-msg',pw?'口令已保存 —— 网页升级已启用':'口令已清空 —— 网页升级已关闭','ok');
      if(ne)ne.value='';
      if(cu)cu.value='';
      otaEnabled=!!d.ota_enabled;
      renderOta();
      loadInfo();
    }else msg('ota-pw-msg',(d&&d.error)||'失败','err');
  }catch(e){msg('ota-pw-msg','请求失败：'+e,'err');}
}

async function startOTA(){
  if(!otaFile){msg('ota-msg','先选一个 .bin 文件','err');return;}
  var cu=document.getElementById('ota-cur');
  var pw=cu?cu.value:'';
  if(otaEnabled&&!pw){msg('ota-msg','先填当前口令','err');return;}
  if(!confirm('用 '+otaFile.name+'（'+fmtBytes(otaFile.size)+'）升级？\n'+
              '设备会重启，正在播放的会断。'))return;
  var b=document.getElementById('ota-btn');
  if(b)b.disabled=true;
  msg('ota-msg','上传中… '+fmtBytes(otaFile.size)+'，几十秒到一两分钟','info');
  try{
    var r=await fetch('/api/ota/update',{method:'POST',
      headers:{'Content-Type':'application/octet-stream','X-OTA-Password':pw},
      body:otaFile});
    var t=await r.text();
    if(r.ok){
      /* 成功即重启：这条响应之后连接就断了，别再发请求问结果。 */
      msg('ota-msg','升级完成，设备正在重启 —— 约 15 秒后刷新页面','ok');
      otaFile=null;
      setText('ota-file-name','');
    }else{
      msg('ota-msg','升级被拒绝：'+(t||('HTTP '+r.status)),'err');
    }
  }catch(e){
    msg('ota-msg','连接中断：'+e+'（若刚上传完，可能已在重启，等十几秒刷新看版本）','err');
  }
  updateOtaBtn();
}

/* ---------------- WiFi ---------------- */
/* 重入保护只针对**后台**自动刷新：设备侧一次扫描要好几秒，而定时器每 15 秒
   无条件叫一次（掉线期间），叠上来的请求会把只有 3 个 socket 的 httpd 挤满
   —— 那正是"页面卡死"的成因。用户手点「扫描网络」不挡，但照样置标志，
   免得后台那一发在手动扫描中途叠进来。 */
async function scanWiFi(quiet){
  if(quiet&&inflight.wifi)return;
  inflight.wifi=true;
  try{await scanWiFiDo(quiet);}finally{inflight.wifi=false;}
}
async function scanWiFiDo(quiet){
  var l=document.getElementById('wifi-list');
  if(!l)return;
  /* quiet = 后台自动刷新：不改内容、不显示"正在扫描…" ——
     否则用户正在挑网络时列表会被反复清空重画，点都点不中。 */
  if(!quiet)l.innerHTML='<div class="empty">正在扫描…</div>';
  try{var r=await fetch('/api/wifi/scan');var d=await r.json();
    if(d.success&&d.networks.length>0){
      /* 第一项是**当前已连上的**网络（设备把 STA 排在最前）。 */
      var connected=d.networks[0].ssid;
      var sig=d.networks.map(function(n){return n.ssid+'|'+n.rssi;}).join(',');
      if(quiet&&sig===lastWifiSig)return;
      lastWifiSig=sig;
      if(quiet&&document.activeElement&&document.activeElement.id==='wifi-ssid')return;
      l.innerHTML='';
      d.networks.forEach(function(n){
        var i=document.createElement('div');i.className='li';
        i.innerHTML=(n.ssid===connected?'<span class="tag">已连接</span>':'')+
          '<span class="nm">'+esc(n.ssid)+'</span><span class="sz">'+n.rssi+' dBm</span>';
        i.onclick=function(){
          var all=document.querySelectorAll('#wifi-list .li');
          for(var k=0;k<all.length;k++)all[k].classList.remove('sel');
          i.classList.add('sel');
          document.getElementById('wifi-ssid').value=n.ssid;};
        l.appendChild(i);});}
    else if(!quiet)l.innerHTML='<div class="empty">未发现网络</div>';
  }catch(e){if(!quiet)l.innerHTML='<div class="msg err">扫描失败</div>';}}

async function saveWiFi(){
  var s=document.getElementById('wifi-ssid').value.trim();var p=document.getElementById('wifi-pass').value;
  if(!s){msg('wifi-msg','请输入网络名称','err');return;}
  try{var r=await fetch('/api/wifi/config',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({ssid:s,password:p})});
    var d=await r.json();if(d.success)msg('wifi-msg','已保存！正在重启…','ok');
    else msg('wifi-msg',d.error,'err');
  }catch(e){msg('wifi-msg','出错了','err');}}

/* ---------------- 网络音乐 ---------------- */
async function radioPlay(){
  var u=document.getElementById('radio-url').value.trim();
  if(!u){msg('radio-msg','请输入音频地址','err');return;}
  /* 兼容常见简写：没写协议就按 http:// 处理 */
  if(u.indexOf('://')<0){u='http://'+u;document.getElementById('radio-url').value=u;}
  try{var r=await fetch('/api/music/play',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({url:u})});
    var d=await r.json();
    if(d.success){msg('radio-msg','正在连接…','ok');radioSchedule(1200);}
    else msg('radio-msg',d.error||'播放失败','err');
  }catch(e){msg('radio-msg','出错了','err');}}
async function radioStop(){
  try{await fetch('/api/music/stop',{method:'POST'});
    msg('radio-msg','已停止','ok');radioSchedule(100);}
  catch(e){msg('radio-msg','出错了','err');}}
function radioNext(){radioSkip('next','下一条');}
function radioPrev(){radioSkip('prev','上一条');}
async function radioSkip(ep,name){
  try{await fetch('/api/music/'+ep,{method:'POST'});
    /* 切曲有网络往返 + 重新连接的时间，稍等一下再刷状态，
       否则会先显示上一曲的信息。 */
    radioSchedule(700);}
  catch(e){msg('radio-msg',(name||'切换')+'失败','err');}}
function radioSchedule(ms){
  if(radioTimer){clearTimeout(radioTimer);radioTimer=null;}
  radioTimer=setTimeout(radioState,ms);}
async function radioState(){
  if(radioTimer){clearTimeout(radioTimer);radioTimer=null;}
  try{var r=await fetch('/api/music');var d=await r.json();
    radioLast=d;
    var el=document.getElementById('radio-status');
    var labels={idle:'空闲',connecting:'正在连接…',playing:'播放中',error:'出错'};
    el.className='state '+(d.state||'');
    var txt=labels[d.state]||d.state;
    if(d.count>1&&d.index>0&&(d.state==='playing'||d.state==='connecting'))txt+=idx(d);
    if(d.state==='error'&&d.error)txt='出错：'+d.error;
    el.textContent=txt;
    /* 「上一条/下一条」只在歌单模式下有意义，单曲模式下半透明不可点。 */
    var multi=(d.count>1);
    ['radio-prev-btn','radio-next-btn'].forEach(function(id){
      var b=document.getElementById(id);
      b.disabled=!multi||!d.active;
      b.style.opacity=(multi&&d.active)?'':'0.45';});
    document.getElementById('radio-idx-note').style.display=multi?'':'none';
    renderNow();
    /* 播放中要继续轮询 —— 歌单会自动换曲，界面得跟上。
       空闲时放慢到 8 秒，别白耗。 */
    radioSchedule(d.state==='connecting'||d.active?2000:8000);
  }catch(e){}}

/* ---------------- SD 卡 ---------------- */
var SD_ICON_DIR="<svg viewBox='0 0 24 24'><path d='M3 7a2 2 0 0 1 2-2h4l2 2h8a2 2 0 0 1 2 2v8a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z'/></svg>";
var SD_ICON_AUDIO="<svg viewBox='0 0 24 24'><path d='M9 18V6l10-2v12'/><circle cx='6.5' cy='18' r='2.5'/><circle cx='16.5' cy='16' r='2.5'/></svg>";

function sdJoin(dir,name){return dir==='/'?('/'+name):(dir+'/'+name);}
function sdSchedule(ms){
  if(sdTimer){clearTimeout(sdTimer);sdTimer=null;}
  sdTimer=setTimeout(sdState,ms);}
/* ⚠️ 「重新挂载」**不在**禁用列表里 —— 没插卡的时候它恰恰是唯一该能点的
   按钮，跟着一起禁用就没法救回来了。 */
function sdSetEnabled(on){
  ['sd-playall-btn','sd-upload-btn','sd-mkdir-btn'].forEach(function(id){
    document.getElementById(id).disabled=!on;});
  if(!on)['sd-prev-btn','sd-next-btn','sd-stop-btn'].forEach(function(id){
    document.getElementById(id).disabled=true;});}

function sdRenderCrumb(dir){
  var box=document.getElementById('sd-crumb');
  box.innerHTML='';
  var parts=dir.split('/').filter(function(s){return s.length>0;});
  var acc='';
  function add(label,path,isLast){
    var a=document.createElement('a');
    a.textContent=label;
    if(isLast)a.className='last';
    a.addEventListener('click',function(){if(!isLast)sdList(path);});
    box.appendChild(a);}
  function sep(){var s=document.createElement('span');s.className='sep';s.textContent='/';box.appendChild(s);}
  add('根目录','/',parts.length===0);
  for(var i=0;i<parts.length;i++){acc+='/'+parts[i];sep();add(parts[i],acc,i===parts.length-1);}}

function sdHighlight(){
  var rows=document.querySelectorAll('#sd-list .li');
  for(var i=0;i<rows.length;i++){
    var r=rows[i];
    if(r.dataset.path===sdPlaying&&sdPlaying)r.classList.add('cur');
    else r.classList.remove('cur');}}

function sdRenderList(d){
  var box=document.getElementById('sd-list');
  box.innerHTML='';
  if(!d.entries||!d.entries.length){
    var e=document.createElement('div');e.className='empty';
    e.textContent='这个目录里没有文件';box.appendChild(e);
    if(d.truncated)sdNote(box);return;}
  d.entries.forEach(function(it){
    var full=sdJoin(d.dir,it.name);
    var playable=it.is_dir||it.audio;
    var row=document.createElement('div');
    row.dataset.path=full;
    row.className='li'+(it.is_dir?' dir':(it.audio?' audio':''))+(playable?'':' unsupported');
    row.innerHTML=(it.is_dir?SD_ICON_DIR:SD_ICON_AUDIO)+
      "<span class='nm'>"+esc(it.name)+"</span>"+
      "<span class='sz'>"+(it.is_dir?'':fmtBytes(it.size))+"</span>";
    if(playable)row.addEventListener('click',function(){
      if(it.is_dir)sdList(full);else sdPlay(full);});
    /* 删除按钮独立监听，并且 stopPropagation —— 否则点删除会顺带触发播放 */
    var rm=document.createElement('button');
    rm.className='rm';rm.type='button';rm.textContent='删除';rm.title='删除 '+it.name;
    rm.addEventListener('click',function(ev){ev.stopPropagation();sdDelete(full,it.name);});
    row.appendChild(rm);
    box.appendChild(row);});
  if(d.truncated)sdNote(box);}
function sdNote(box){
  var n=document.createElement('div');n.className='empty';
  n.textContent='（内容过多，只显示了前一部分。请分目录整理。）';box.appendChild(n);}

async function sdList(dir){
  if(dir!==undefined)sdDir=dir;
  var st=document.getElementById('sd-status');
  try{
    var r=await fetch('/api/sd/list?dir='+encodeURIComponent(sdDir));
    var d=await r.json();
    if(!d.mounted){
      st.className='state';st.textContent='未检测到 SD 卡';
      document.getElementById('sd-space').textContent='';
      document.getElementById('sd-crumb').innerHTML='';
      document.getElementById('sd-list').innerHTML=
        "<div class='empty'>插入格式化为 FAT32 的卡后点「重新挂载」，不用重启设备。</div>";
      sdSetEnabled(false);return;}
    sdSetEnabled(true);
    document.getElementById('sd-space').textContent=d.total?(fmtBytes(d.total-d.free)+' / '+fmtBytes(d.total)):'';
    if(!d.success){
      document.getElementById('sd-crumb').innerHTML='';
      document.getElementById('sd-list').innerHTML="<div class='empty'>"+esc(d.error||'目录打不开')+"</div>";
      return;}
    sdRenderCrumb(d.dir);sdRenderList(d);}
  catch(e){st.className='state error';st.textContent='读取失败';}}

async function sdPlay(path){
  msg('sd-msg','正在开始…','ok');
  try{var r=await fetch('/api/sd/play',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({path:path})});
    var d=await r.json();
    if(d.success){sdPlaying=path;sdHighlight();sdSchedule(600);}
    else msg('sd-msg',d.error||'播放失败','err');
  }catch(e){msg('sd-msg','出错了','err');}}
/* 播放本目录：固件收到目录路径会把它整个排成播放列表 */
function sdPlayAll(){if(sdDir)sdPlay(sdDir);}
async function sdStop(){
  try{await fetch('/api/sd/stop',{method:'POST'});
    msg('sd-msg','已停止','ok');sdSchedule(100);}
  catch(e){msg('sd-msg','出错了','err');}}
function sdNext(){sdSkip('next','下一条');}
function sdPrev(){sdSkip('prev','上一条');}
async function sdSkip(ep,name){
  /* 切曲要等固件丢掉缓冲 + 重开解码器，稍等一下再刷状态，
     否则会先显示上一曲的信息。 */
  try{await fetch('/api/sd/'+ep,{method:'POST'});sdSchedule(700);}
  catch(e){msg('sd-msg',(name||'切换')+'失败','err');}}
async function sdDelete(path,name){
  if(!confirm('删除「'+name+'」？\n\n删除后无法恢复。'))return;
  try{var r=await fetch('/api/sd/delete',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({path:path})});
    var d=await r.json();
    if(d.success)msg('sd-msg','已删除：'+name,'ok');
    else msg('sd-msg',d.error||'删除失败','err');
  }catch(e){msg('sd-msg','出错了','err');}
  sdList();}
async function sdMkdir(){
  var name=prompt('新文件夹的名字：');
  if(!name)return;
  if(name.indexOf('/')>=0||name.indexOf('\\')>=0){msg('sd-msg','名字里不能有斜杠','err');return;}
  try{var r=await fetch('/api/sd/mkdir',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({dir:sdJoin(sdDir,name)})});
    var d=await r.json();
    msg('sd-msg',d.success?('已创建：'+name):(d.error||'创建失败'),d.success?'ok':'err');}
  catch(e){msg('sd-msg','出错了','err');}
  sdList();}
async function sdRemount(){
  msg('sd-msg','正在重新挂载…','ok');
  try{await fetch('/api/sd/mount',{method:'POST'});}catch(e){}
  sdList();}
/* 上传：用 fetch 直接把 File 当 body 传（不是 multipart）—— 固件那边分块写
   文件，不整块进内存。文件名放 query，必须 encodeURIComponent，中文名编码后
   固件才能正确解回来。 */
async function sdUpload(){
  var inp=document.getElementById('sd-file');
  var f=inp.files&&inp.files[0];
  if(!f)return;
  if(f.size>32*1024*1024){msg('sd-msg','文件太大（上限 32MB）','err');inp.value='';return;}
  if(sdBusy)return;
  sdBusy=true;
  msg('sd-msg','正在上传 '+f.name+'（'+fmtBytes(f.size)+'）…','ok');
  try{var r=await fetch('/api/sd/upload?dir='+encodeURIComponent(sdDir)+
      '&name='+encodeURIComponent(f.name),{method:'POST',body:f});
    var d=await r.json();
    msg('sd-msg',d.success?('已上传：'+f.name):(d.error||'上传失败'),d.success?'ok':'err');}
  catch(e){msg('sd-msg','上传失败（连接中断？）','err');}
  sdBusy=false;
  inp.value='';   /* 清空，否则同一个文件再选一次不触发 onchange */
  sdList();}

async function sdState(){
  if(sdTimer){clearTimeout(sdTimer);sdTimer=null;}
  if(sdBusy){sdSchedule(2000);return;}   /* 上传中别抢 httpd */
  try{var r=await fetch('/api/sd/state');var d=await r.json();
    sdLast=d;
    var st=document.getElementById('sd-status');
    if(d.mounted===false){st.className='state';st.textContent='未检测到 SD 卡';}
    else{
      var labels={idle:'空闲',playing:'播放中',error:'出错'};
      st.className='state '+(d.state||'');
      var txt=labels[d.state]||d.state;
      if(d.count>1&&d.index>0&&d.state==='playing')txt+=idx(d);
      if(d.state==='error'&&d.error)txt='出错：'+d.error;
      st.textContent=txt;}
    var multi=(d.count>1);
    ['sd-prev-btn','sd-next-btn'].forEach(function(id){
      document.getElementById(id).disabled=!multi||!d.active;});
    document.getElementById('sd-stop-btn').disabled=!d.active;
    /* 自动播下一曲时高亮要跟上 */
    if(d.path&&d.path!==sdPlaying){sdPlaying=d.path;sdHighlight();}
    renderNow();
    sdSchedule(d.active?2000:8000);
  }catch(e){sdSchedule(8000);}}

/* 导航上的小绿点：本地两路任一在放就亮，用户在别的页也能知道"还在响" */
function renderNavMark(){
  var s=activeSrc();
  var n=document.querySelector('.nav-i[data-view="play"]');
  if(n)n.classList.toggle('busy',!!s);}

/* ---------------- 启动 ---------------- */
window.onload=function(){
  /* 深链：#music 之类直接进对应视图 */
  var h=(location.hash||'').replace(/^#/,'');
  go(VIEWS.indexOf(h)>=0?h:'play');
  /* 初始 paintRange：不调的话滑块轨道会是一整条灰色，看不出当前值 */
  paintRange(document.getElementById('volume'));
  paintRange(document.getElementById('led-brightness'));
  volPaint(50);
  loadInfo();loadVolume();loadBrightness();loadChannel();radioState();
  sdList('/');sdState();};

document.getElementById('mini').addEventListener('click',function(){go('play');});

/* 固件升级：文件选择 + 口令填了才允许点上传（设备侧还会再查一遍） */
(function(){
  var f=document.getElementById('ota-file');
  var c=document.getElementById('ota-cur');
  if(f)f.addEventListener('change',function(){
    otaFile=(this.files&&this.files.length)?this.files[0]:null;
    setText('ota-file-name',otaFile?('已选 '+otaFile.name+' · '+fmtBytes(otaFile.size)):'');
    updateOtaBtn();
  });
  if(c)c.addEventListener('input',updateOtaBtn);
  renderOta();   /* 第一次 loadInfo 之前先按"未知"画出来，别停在"读取中…" */
})();

/* 页面重新可见时立刻刷一次 —— 手机锁屏/切后台回来后不该显示旧数据。 */
document.addEventListener('visibilitychange',function(){
  if(!document.hidden)refreshAll();});
function refreshAll(){
  loadInfo();loadVolume();loadBrightness();loadChannel();radioState();
  /* SD 只刷状态，**不重拉目录** —— 用户可能正在列表里挑歌，重画会把他
     点到一半的列表刷掉。目录内容只在进目录/上传/删除后主动刷新。 */
  sdState();
  if(!staConnected)scanWiFi(true);}

/* 键盘：左右方向键切歌（播放页），1–4 切视图 —— 桌面端接键盘时省事 */
document.addEventListener('keydown',function(ev){
  if(ev.target&&/INPUT|SELECT|TEXTAREA/.test(ev.target.tagName))return;
  if(ev.key==='1')go('play');
  else if(ev.key==='2')go('music');
  else if(ev.key==='3')go('set');
  else if(ev.key==='4')go('sys');
  else if(ev.key==='ArrowRight'&&curView==='play')trSkip('next');
  else if(ev.key==='ArrowLeft'&&curView==='play')trSkip('prev');
});
