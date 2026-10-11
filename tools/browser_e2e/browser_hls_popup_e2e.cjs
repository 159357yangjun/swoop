'use strict';
// Packaged Windows real-browser acceptance. No fake native responses or GUI state.
// Popup is loaded as an extension-owned tab to automate its actual DOM.
const assert=require('node:assert/strict');
const fs=require('node:fs'),os=require('node:os'),path=require('node:path'),http=require('node:http');
const cp=require('node:child_process'),crypto=require('node:crypto');
const {chromium}=require('playwright-core');
const {Browser,detectBrowserPlatform,resolveBuildId,install}=require('@puppeteer/browsers');
assert.equal(process.platform,'win32','Windows runtime required');
const ext=path.resolve(__dirname,'../../browser-extension'), id='fmnehlhldcoknecihnlbbcmhpaaapmbp';
const hostName='com.tencent.idm_next';
const ix=process.argv.indexOf('--dist');
assert.ok(ix>=0&&process.argv[ix+1],'Pass --dist installer/dist');
const dist=path.resolve(process.argv[ix+1]), gui=path.join(dist,'idm-next.exe'), host=path.join(dist,'idm-next-host.exe');
assert.ok(fs.existsSync(gui)&&fs.existsSync(host),'Packaged executable missing');
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
function cmd(bin,args,extra={}){
  return cp.execFileSync(bin,args,{windowsHide:true,timeout:45000,...extra});
}
async function until(fn,description,timeout=20000){
  const stop=Date.now()+timeout;let last=null;
  while(Date.now()<stop){try{last=await fn();if(last)return last;}catch(e){last=e.message;}await sleep(200);}
  throw Error(description+' timed out: '+String(last));
}
function hostPing(){
  const request=Buffer.from(JSON.stringify({action:'ping'}));
  const size=Buffer.alloc(4);size.writeUInt32LE(request.length);
  const response=cp.spawnSync(host,[],{cwd:dist,input:Buffer.concat([size,request]),
    windowsHide:true,timeout:9000,maxBuffer:1024*1024});
  assert.equal(response.status,0,'Native host ping failed: '+String(response.stderr||response.error));
  const bytes=response.stdout;
  assert.ok(bytes.length>=4&&bytes.readUInt32LE(0)===bytes.length-4,'Malformed native frame');
  return JSON.parse(bytes.subarray(4).toString('utf8'));
}
function fixture(root){
  const media=path.join(root,'media'),high=path.join(media,'high');
  fs.mkdirSync(high,{recursive:true});
  cmd('ffmpeg',['-v','error','-nostdin','-y','-f','lavfi','-i',
    'testsrc2=size=320x192:rate=10','-f','lavfi','-i','sine=frequency=880:sample_rate=48000',
    '-t','3','-c:v','libx264','-preset','ultrafast','-pix_fmt','yuv420p',
    '-g','10','-c:a','aac','-b:a','64k','-f','hls','-hls_time','1',
    '-hls_playlist_type','vod','-hls_segment_filename',path.join(high,'seg%03d.ts'),
    path.join(high,'index.m3u8')]);
  fs.writeFileSync(path.join(media,'master.m3u8'),
    '#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=800000,RESOLUTION=320x192\nhigh/index.m3u8\n');
  fs.writeFileSync(path.join(media,'broken.m3u8'),
    '#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXTINF:2,\nmissing.ts\n#EXT-X-ENDLIST\n');
  return media;
}
function register(root){
  const manifest=path.join(root,'native.json');
  fs.writeFileSync(manifest,JSON.stringify({name:hostName,path:host,type:'stdio',
    description:'Real browser end-to-end test',allowed_origins:['chrome-extension://'+id+'/']}));
  const keys=['Google\\Chrome','Microsoft\\Edge'].map(x=>
    'HKCU\\Software\\'+x+'\\NativeMessagingHosts\\'+hostName);
  const done=[];
  try{
    for(const key of keys){
      const old=cp.spawnSync('reg.exe',['query',key,'/ve'],{windowsHide:true});
      assert.notEqual(old.status,0,'Refusing to overwrite existing native host key: '+key);
      cmd('reg.exe',['add',key,'/ve','/t','REG_SZ','/d',manifest,'/f']);
      done.push(key);
    }
  }catch(e){
    for(const key of done)cp.spawnSync('reg.exe',['delete',key,'/f'],{windowsHide:true});
    throw e;
  }
  return ()=>{for(const key of done){
    const r=cp.spawnSync('reg.exe',['delete',key,'/f'],{windowsHide:true});
    assert.equal(r.status,0,'Could not clean native host key: '+key);
  }};
}
async function browsers(){
  const roots=[process.env['PROGRAMFILES(X86)'],process.env.ProgramFiles,
    process.env.LOCALAPPDATA].filter(Boolean);
  const edge=roots.map(x=>path.join(x,'Microsoft/Edge/Application/msedge.exe')).find(fs.existsSync);
  assert.ok(edge,'Real Edge missing');
  const platform=detectBrowserPlatform();
  assert.ok(platform,'Cannot identify Chrome platform');
  const version=await resolveBuildId(Browser.CHROME,platform,'stable');
  const chrome=await install({browser:Browser.CHROME,buildId:version,platform,
    cacheDir:path.join(os.tmpdir(),'idm-next-cft-cache')});
  assert.ok(fs.existsSync(chrome.executablePath),'Chrome for Testing missing');
  return [['Edge',edge],['Chrome for Testing',chrome.executablePath]];
}
function output(root,name){
  const matches=[],dirs=[[path.join(root,'downloads'),0]];
  while(dirs.length){
    const [dir,level]=dirs.shift();if(!fs.existsSync(dir))continue;
    for(const entry of fs.readdirSync(dir,{withFileTypes:true})){
      const full=path.join(dir,entry.name);
      if(entry.isFile()&&entry.name===name)matches.push(full);
      if(entry.isDirectory()&&level<2)dirs.push([full,level+1]);
    }
  }
  assert.ok(matches.length<=1,'Duplicate final files: '+matches);
  return matches[0]||null;
}
function frameHash(file){
  const raw=cmd('ffmpeg',['-v','error','-xerror','-nostdin','-i',file,
    '-map','0:v:0','-pix_fmt','yuv420p','-f','rawvideo','-'],{maxBuffer:24*1024*1024});
  assert.ok(raw.length>0,'Decoded zero frames');
  return crypto.createHash('sha256').update(raw).digest('hex');
}
async function scenario(label,executable,base,root,broken){
  const token=(broken?'bad':'ok')+crypto.randomBytes(5).toString('hex');
  const prefix=broken?'popup-broken-':'popup-hls-';
  const name=prefix+token+'.m3u8',finalName=name+'.mp4';
  assert.equal(output(root,finalName),null,'Target must not exist before download');
  const profile=fs.mkdtempSync(path.join(root,'profile-'));
  let browser;
  try{
    browser=await chromium.launchPersistentContext(profile,{executablePath:executable,
      headless:true,acceptDownloads:true,args:['--headless=new','--disable-extensions-except='+ext,
        '--load-extension='+ext,'--no-first-run','--no-default-browser-check','--disable-gpu']});
    const page=await browser.newPage();
    await page.goto(base+'/watch.html?token='+token+(broken?'&broken=1':''),{waitUntil:'domcontentloaded'});
    await page.waitForFunction(()=>window.mediaFetched===true,{timeout:15000});
    await page.locator('#idm-next-tokens').waitFor({state:'attached',timeout:15000});
    const popup=await browser.newPage();
    await popup.goto('chrome-extension://'+id+'/popup.html',{waitUntil:'domcontentloaded'});
    assert.equal(await popup.evaluate(()=>chrome.runtime.id),id,'Unexpected extension ID');
    const matches=await until(()=>popup.evaluate(async token=>{
      const tabs=await chrome.tabs.query({});
      const source=tabs.find(x=>x.url?.includes('/watch.html?token='+token));
      if(!source)return null;
      const data=await chrome.storage.session.get('tab_'+source.id);
      return (data['tab_'+source.id]||[]).filter(x=>
        new URL(x.url).pathname.endsWith((token.startsWith('bad')?
          '/popup-broken-':'/popup-hls-')+token+'.m3u8'));
    },token).then(items=>items?.length===1?items:null),label+' media discovery');
    assert.equal(matches.length,1,'Media URL not deduplicated');
    await page.bringToFront();
    // Load the same real resource listing as opening the toolbar Popup.
    await popup.evaluate(()=>loadResources());
    const row=popup.locator('#resourceList .row').filter({hasText:name});
    await row.waitFor({timeout:15000});
    await row.locator('input[type=checkbox]').check();
    await popup.locator('#downloadSelBtn').click();
    await popup.locator('#resourceList .row.sent').waitFor({timeout:18000});
    // Reopening the actual Popup reloads the persistent task journal.
    await popup.reload({waitUntil:'domcontentloaded'});
    const handoff=await until(()=>popup.evaluate(()=>new Promise(resolve=>
      chrome.runtime.sendMessage({action:'getRecentHandoffs'},resolve)))
      .then(r=>r?.items?.find(x=>x.filename===name)||null),
      label+' session-bound handoff');
    assert.ok(Number.isSafeInteger(handoff.taskId)&&handoff.taskId>0,'Missing task ID');
    assert.ok(handoff.sessionId&&handoff.sessionId.length>=32,'Missing session ID');
    console.log('[HANDOFF] '+label+' task='+handoff.taskId+' session_present=true');
    const state=await until(async()=>{
      const r=await popup.evaluate(taskId=>new Promise(resolve=>
        chrome.runtime.sendMessage({action:'getHandoffStatus',taskId},resolve)),handoff.taskId);
      assert.ok(r?.success,'Read-only task status failed: '+JSON.stringify(r));
      assert.equal(r.taskId,handoff.taskId);
      assert.equal(r.sessionId,handoff.sessionId);
      if(r.state===5)throw Error('Unexpected canceled task');
      return r.state===(broken?4:3)?r:null;
    },label+' terminal task state',95000);
    await popup.locator('#recentHandoffs .browser-capture-send').first().click();
    await until(async()=>{
      const title=await popup.locator('#recentHandoffs .browser-capture-name').first().getAttribute('title');
      return title?.includes(broken?'下载失败':'桌面端显示任务已完成')?title:null;
    },label+' user-visible status');
    if(broken){
      assert.ok(state.error,'Missing server failure reason');
      assert.equal(output(root,finalName),null,'Broken HLS falsely published MP4');
      console.log('[PASS] '+label+' Popup -> Native Host -> GUI -> failed HLS; no final file');
      return;
    }
    const final=await until(()=>output(root,finalName),label+' MP4 final file');
    const info=JSON.parse(cmd('ffprobe',['-v','error','-show_entries',
      'stream=codec_type,width,height','-show_entries','format=duration','-of','json',final]).toString());
    assert.ok(info.streams?.some(s=>s.codec_type==='video'&&s.width===320&&s.height===192));
    assert.ok(info.streams?.some(s=>s.codec_type==='audio'));
    assert.ok(Number(info.format?.duration)>2.7&&Number(info.format?.duration)<3.5);
    cmd('ffmpeg',['-v','error','-xerror','-nostdin','-i',final,'-f','null','-']);
    assert.equal(frameHash(final),frameHash(path.join(root,'media/high/index.m3u8')),
      'Decoded final video differs from source HLS');
    console.log('[PASS] '+label+' Popup -> Native Host -> GUI -> MP4, task='+
      handoff.taskId+' SHA256(decoded)=match');
  }finally{if(browser)await browser.close();}
}
(async()=>{
  const root=fs.mkdtempSync(path.join(os.tmpdir(),'idm-popup-e2e-'));
  let server,guiProcess,unregister;
  try{
    const media=fixture(root), config=path.join(root,'config'),downloads=path.join(root,'downloads');
    fs.mkdirSync(config);fs.mkdirSync(downloads);
    const ffmpegExe=cmd('where.exe',['ffmpeg'],{encoding:'utf8'}).trim().split(/\r?\n/)[0];
    assert.ok(ffmpegExe&&fs.existsSync(ffmpegExe),'FFmpeg executable not found for packaged GUI');
    fs.writeFileSync(path.join(config,'idm-next.ini'),
      '[General]\ndefaultSaveDir='+downloads.replace(/\\/g,'/')+
      '\nffmpegCustomPath='+ffmpegExe.replace(/\\/g,'/')+
      '\nautoDownloadFfmpeg=false\nautoDownloadYtDlp=false\nautoDownloadAria2=false\n');
    server=http.createServer((req,res)=>{
      const u=new URL(req.url,'http://127.0.0.1');
      if(u.pathname==='/watch.html'){
        const token=u.searchParams.get('token');
        assert.match(token,/^(ok|bad)[a-f0-9]{10}$/);
        const resource='/' +(u.searchParams.get('broken')==='1'?'popup-broken-':'popup-hls-')+token+'.m3u8';
        res.writeHead(200,{'Content-Type':'text/html','Cache-Control':'no-store'});
        res.end('<!doctype html><html><body>HLS resource fixture<script>'+
          'fetch('+JSON.stringify(resource)+').then(r=>{if(!r.ok)throw Error(r.status);return r.text()}).then(()=>window.mediaFetched=true)'+
          '.catch(e=>window.mediaError=String(e));</script></body></html>');
        return;
      }
      const file=u.pathname.startsWith('/popup-hls-')&&u.pathname.endsWith('.m3u8')
        ?path.join(media,'master.m3u8')
        :u.pathname.startsWith('/popup-broken-')&&u.pathname.endsWith('.m3u8')
        ?path.join(media,'broken.m3u8'):path.join(media,u.pathname.slice(1));
      if(!file.startsWith(media+path.sep)||!fs.existsSync(file)){
        res.writeHead(404);res.end('missing');return;
      }
      res.writeHead(200,{'Content-Type':file.endsWith('.m3u8')
        ?'application/vnd.apple.mpegurl':'video/mp2t','Cache-Control':'no-store'});
      fs.createReadStream(file).pipe(res);
    });
    await new Promise((r,reject)=>{server.once('error',reject);server.listen(0,'127.0.0.1',r);});
    unregister=register(root);
    const stdout=fs.openSync(path.join(root,'gui.out'),'w');
    const stderr=fs.openSync(path.join(root,'gui.err'),'w');
    guiProcess=cp.spawn(gui,[],{cwd:dist,windowsHide:true,stdio:['ignore',stdout,stderr],
      env:{...process.env,IDM_DATA_DIR:config,
        PATH:path.dirname(ffmpegExe)+path.delimiter+process.env.PATH,
        QT_FORCE_STDERR_LOGGING:'1'}});
    fs.closeSync(stdout);fs.closeSync(stderr);
    await until(()=>{
      if(guiProcess.exitCode!==null)throw Error('Packaged GUI exited');
      try{return hostPing().success===true;}catch{return false;}
    },'GUI Native Host ping',45000);
    const base='http://127.0.0.1:'+server.address().port;
    for(const [name,exe] of await browsers()){
      await scenario(name,exe,base,root,false);
      if(name==='Edge')await scenario(name,exe,base,root,true);
    }
  }catch(e){
    for(const f of ['gui.out','gui.err']){
      const file=path.join(root,f);
      if(fs.existsSync(file))console.error('[GUI LOG] '+f+': '+fs.readFileSync(file,'utf8').slice(-3000));
    }
    throw e;
  }finally{
    try{
      if(guiProcess){
        guiProcess.kill();
        await until(()=>guiProcess.exitCode!==null||guiProcess.signalCode!==null,
          'GUI process teardown',5000);
      }
    }finally{
      if(unregister)unregister();
      if(server)await new Promise(r=>server.close(r));
      fs.rmSync(root,{recursive:true,force:true});
    }
  }
})().catch(e=>{console.error(e.stack||e);process.exitCode=1;});
