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
// A new native process for READ-ONLY reconciliation; never resubmit add_download.
function nativeRequest(request){
  const bytes=Buffer.from(JSON.stringify(request));
  const length=Buffer.alloc(4);length.writeUInt32LE(bytes.length);
  const child=cp.spawnSync(host,[],{cwd:dist,input:Buffer.concat([length,bytes]),
    windowsHide:true,timeout:9000,maxBuffer:1024*1024});
  assert.equal(child.status,0,'Read-only Native Host request failed');
  const result=child.stdout;
  assert.ok(result.length>=4&&result.readUInt32LE(0)===result.length-4,
    'Malformed read-only Native Host frame');
  return JSON.parse(result.subarray(4).toString('utf8'));
}
function persistedHistoryCount(root,filename){
  const file=path.join(root,'config','history.db');
  if(!fs.existsSync(file))return -1;
  const query='import sqlite3,sys; db=sqlite3.connect(sys.argv[1],timeout=2);'+
    "print(db.execute('SELECT COUNT(*) FROM downloads WHERE filename=?',"+
    '(sys.argv[2],)).fetchone()[0])';
  return Number(cmd('python',['-c',query,file,filename],{encoding:'utf8'}).trim());
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
async function scenario(label,executable,base,root,broken,ackDelay=false){
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
    if(ackDelay){
      // Production behavior under the same genuine 4200ms DB stall:
      // no retry of add_download, only a read-only UUID lookup of its outcome.
      await popup.locator('#resourceList .row.sent').waitFor({timeout:18000});
      const recoveryNotice=await popup.locator('#toast').textContent();
      assert.match(recoveryNotice||'',/已核对原请求/,
        'The Popup must identify this as a recovered (not a resent) handoff');
      await popup.reload({waitUntil:'domcontentloaded'});
      const journal=await popup.evaluate(async()=>(
        (await chrome.storage.local.get('recentHandoffs')).recentHandoffs||[]));
      assert.equal(journal.length,1,'Recovered add should persist exactly one handoff');
      assert.ok(Number.isSafeInteger(journal[0].taskId)&&journal[0].taskId>0);
      assert.ok(journal[0].sessionId&&journal[0].sessionId.length>=32);
      // Reconcile state only by read-only queries after the delayed GUI operation.
      const state=await until(()=>{
        const result=nativeRequest({action:'get_status'});
        if(!result.success||!result.running)return null;
        const message=result.message||'';
        const rows=message.split(/\r?\n/).filter(x=>/^#\d+\t/.test(x));
        return rows.length===1&&rows[0].includes(name)
          ?{rows:rows.length,taskIds:rows.map(x=>Number(x.match(/^#(\d+)/)[1]))}
          :null;
      },label+' single GUI task read-only reconciliation',16000);
      assert.equal(state.rows,1);
      assert.equal(new Set(state.taskIds).size,1);
      const history=await until(()=>{
        const count=persistedHistoryCount(root,name);
        return count===1?count:null;
      },label+' single persisted SQLite record',15000);
      assert.equal(history,1,'History must contain exactly one creation');
      const final=await until(()=>output(root,finalName),
        label+' final file after unconfirmed ACK',75000);
      cmd('ffmpeg',['-v','error','-xerror','-nostdin','-i',final,'-f','null','-']);
      assert.equal(frameHash(final),frameHash(path.join(root,'media/high/index.m3u8')),
        'HLS output changed under injected SQLite delay');
      // Last independent snapshot: no hidden automatic resubmission on completion.
      const last=nativeRequest({action:'get_status'});
      const lines=(last.message||'').split(/\r?\n/).filter(x=>/^#\d+\t/.test(x));
      assert.equal(lines.length,1,'GUI contains duplicate tasks after timeout');
      assert.equal(persistedHistoryCount(root,name),1,'SQLite contains duplicated add');
      const guiLog=fs.readFileSync(path.join(root,'gui.err'),'utf8');
      assert.equal((guiLog.match(/HISTORY_WRITE_FAULT_INJECTION_BEGIN delay_ms=4200/g)||[]).length,1,
        'The opted-in SQLite latency injection must execute exactly once');
      assert.equal((guiLog.match(/HISTORY_WRITE_FAULT_INJECTION_END delay_ms=4200/g)||[]).length,1,
        'The injected SQLite write must reach the finish boundary exactly once');
      console.log('[PASS] SQLite delay unknown-ACK reconciled by request ID: '+
        'Popup tracked, GUI tasks=1, history records=1, no resend, final decoded SHA256=match');
      return;
    }
    try {
      await popup.locator('#resourceList .row.sent').waitFor({timeout:18000});
    } catch(error) {
      // Observe state once, WITHOUT resubmitting the irreversible add.
      // Keep the same 18-second strict failure boundary. Do not log URLs or paths.
      let snapshot={unavailable:true};
      try {
        snapshot=await popup.evaluate(async()=>{
          const rowNodes=[...document.querySelectorAll('#resourceList .row')];
          const toasts=[...document.querySelectorAll('.toast')];
          const raw=await chrome.storage.local.get('recentHandoffs');
          const journal=Array.isArray(raw.recentHandoffs)?raw.recentHandoffs:[];
          return {
            rowCount:rowNodes.length,
            sentRows:rowNodes.filter(x=>x.classList.contains('sent')).length,
            selectedRows:rowNodes.filter(x=>x.classList.contains('sel')).length,
            buttonDisabled:document.getElementById('downloadSelBtn')?.disabled,
            toastText:toasts.map(x=>x.textContent?.slice(0,160)).slice(0,2),
            journalCount:journal.length,
            journalHeadHasIdentity:!!(journal[0]?.taskId&&journal[0]?.sessionId)
          };
        });
      } catch(snapshotError) {
        snapshot={error:String(snapshotError).slice(0,180)};
      }
      console.error('[POPUP_UNCONFIRMED] '+label+' '+JSON.stringify(snapshot));
      throw error;
    }
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
async function scenarioMidflightWorkerStop(executable,base,root){
  // Full Edge MV3 + packaged Native Host/GUI: prove real CDP stopped-state
  // DURING a known in-progress 4200ms SQLite history write. No fake callbacks.
  const token='ok'+crypto.randomBytes(5).toString('hex');
  const name='popup-hls-'+token+'.m3u8',finalName=name+'.mp4';
  const profile=fs.mkdtempSync(path.join(root,'profile-midflight-'));
  let browser;
  try{
    browser=await chromium.launchPersistentContext(profile,{executablePath:executable,
      headless:true,acceptDownloads:true,args:['--headless=new',
        '--disable-extensions-except='+ext,'--load-extension='+ext,
        '--no-first-run','--no-default-browser-check','--disable-gpu']});
    const page=await browser.newPage();
    await page.goto(base+'/watch.html?token='+token,{waitUntil:'domcontentloaded'});
    await page.waitForFunction(()=>window.mediaFetched===true,{timeout:15000});
    await page.locator('#idm-next-tokens').waitFor({state:'attached',timeout:15000});
    const popup=await browser.newPage();
    await popup.goto('chrome-extension://'+id+'/popup.html',{waitUntil:'domcontentloaded'});
    assert.equal(await popup.evaluate(()=>chrome.runtime.id),id);
    // Like the established positive E2E, select the actual media page as the
    // source tab. Otherwise Chrome treats the extension-owned Popup tab as
    // active and returns an empty resource list.
    await page.bringToFront();
    await popup.evaluate(()=>loadResources());
    const row=popup.locator('#resourceList .row').filter({hasText:name});
    await row.waitFor({timeout:15000});
    await row.locator('input[type=checkbox]').check();

    const cdp=await browser.newCDPSession(popup);
    const workerUrl='chrome-extension://'+id+'/background.js';
    let activeVersion=null,sawStopped=false,sawRestarted=false;
    cdp.on('ServiceWorker.workerVersionUpdated',msg=>{
      for(const v of msg.versions||[]){
        if(v.scriptURL!==workerUrl)continue;
        if(v.runningStatus==='running'){
          activeVersion=v.versionId;
          if(sawStopped)sawRestarted=true;
        }
        if(v.runningStatus==='stopped')sawStopped=true;
      }
    });
    await cdp.send('ServiceWorker.enable');
    await until(()=>activeVersion,'real original MV3 service worker',15000);
    await popup.locator('#downloadSelBtn').click();

    const pending=await until(()=>popup.evaluate(async()=>{
      const stored=await chrome.storage.local.get('pendingAddRequests');
      const list=stored.pendingAddRequests||[];
      return list.length===1&&list[0].requestId?list[0]:null;
    }),'persisted pending UUID before worker stop',14000);
    assert.match(pending.requestId,/^[a-f0-9-]{36}$/i);
    assert.equal(pending.filename,name);
    assert.ok(!Object.hasOwn(pending,'url'),
      'Pending request journal must not expose source URL or credentials');

    const guiLog=path.join(root,'gui.err');
    await until(()=>fs.existsSync(guiLog)&&
      fs.readFileSync(guiLog,'utf8').includes('HISTORY_WRITE_FAULT_INJECTION_BEGIN delay_ms=4200'),
      'GUI inside actual 4200ms SQLite injection',12000);
    await cdp.send('ServiceWorker.stopWorker',{versionId:activeVersion});
    await until(()=>sawStopped,'real worker stopped-state event during add',10000);
    assert.equal(fs.readFileSync(guiLog,'utf8')
      .includes('HISTORY_WRITE_FAULT_INJECTION_END delay_ms=4200'),false,
      'Worker was not stopped while the original GUI add remained in flight');
    const pendingAfterStop=await popup.evaluate(async()=>{
      const data=await chrome.storage.local.get('pendingAddRequests');
      return (data.pendingAddRequests||[]).length;
    });
    assert.equal(pendingAfterStop,1,'Pending UUID disappeared when worker stopped');

    // Wait for GUI's first add to finish WITHOUT sending anything to the worker.
    await until(()=>fs.readFileSync(guiLog,'utf8')
      .includes('HISTORY_WRITE_FAULT_INJECTION_END delay_ms=4200'),
      'original GUI add finished after worker stop',15000);

    // Reopening the extension Popup wakes a fresh MV3 worker; its only recovery
    // action is get_request_status using the persisted token, never another add.
    await popup.reload({waitUntil:'domcontentloaded'});
    const journal=await until(()=>popup.evaluate(()=>new Promise(resolve=>
      chrome.runtime.sendMessage({action:'getRecentHandoffs'},resolve)))
      .then(x=>x?.items?.length===1?x.items[0]:null),
      'session-bound original handoff recovered after real worker kill',22000);
    await until(()=>sawRestarted,'real worker restarted-state event',10000);
    assert.ok(Number.isSafeInteger(journal.taskId)&&journal.taskId>0);
    assert.ok(typeof journal.sessionId==='string'&&journal.sessionId.length>=32);
    const pendingRemaining=await popup.evaluate(async()=>{
      const r=await chrome.storage.local.get('pendingAddRequests');
      return (r.pendingAddRequests||[]).length;
    });
    assert.equal(pendingRemaining,0,'Verified UUID should be cleared after recovery');
    const status=nativeRequest({action:'get_request_status',requestId:pending.requestId});
    assert.equal(status.found,true,'GUI must retain same request ID until query');
    assert.equal(status.taskId,journal.taskId);
    assert.equal(status.sessionId,journal.sessionId);
    const list=nativeRequest({action:'get_status'});
    const tasks=(list.message||'').split(/\r?\n/).filter(x=>/^#\d+\t/.test(x));
    assert.equal(tasks.length,1,'MV3 restart caused a duplicate GUI task');
    assert.equal(persistedHistoryCount(root,name),1,'MV3 restart caused duplicate SQLite row');
    const final=await until(()=>output(root,finalName),
      'MP4 after real MV3 worker restart',75000);
    cmd('ffmpeg',['-v','error','-xerror','-nostdin','-i',final,'-f','null','-']);
    assert.equal(frameHash(final),frameHash(path.join(root,'media/high/index.m3u8')),
      'Final video frames differ after worker was killed mid-request');
    const log=fs.readFileSync(guiLog,'utf8');
    assert.equal((log.match(/HISTORY_WRITE_FAULT_INJECTION_BEGIN delay_ms=4200/g)||[]).length,1);
    assert.equal((log.match(/HISTORY_WRITE_FAULT_INJECTION_END delay_ms=4200/g)||[]).length,1);
    console.log('[PASS] Real MV3 stopped mid Native add and restarted: '+
      'persisted UUID recovered read-only, GUI tasks=1, history=1, '+
      'no replay, decoded video SHA256=match');
  }finally{if(browser)await browser.close();}
}

(async()=>{
  const root=fs.mkdtempSync(path.join(os.tmpdir(),'idm-popup-e2e-'));
  let server,guiProcess,unregister;
  const midflight=process.argv.includes('--mv3-midflight');
  const ackDelay=process.argv.includes('--ack-delay')||midflight;
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
        QT_FORCE_STDERR_LOGGING:'1',
        ...(ackDelay?{
          IDM_IPC_TIMING_TRACE:'1',
          IDM_E2E_HISTORY_WRITE_DELAY_MS:'4200'
        }:{})}});
    fs.closeSync(stdout);fs.closeSync(stderr);
    await until(()=>{
      if(guiProcess.exitCode!==null)throw Error('Packaged GUI exited');
      try{return hostPing().success===true;}catch{return false;}
    },'GUI Native Host ping',45000);
    const base='http://127.0.0.1:'+server.address().port;
    if(midflight){
      const installed=await browsers();
      await scenarioMidflightWorkerStop(installed[0][1],base,root);
      return;
    }
    if(process.argv.includes('--request-lifecycle')){
      // Real Native Host / GUI protocol lifecycle. A second add below is an
      // explicit same-token idempotence assertion, NOT a timeout retry.
      const requestId=crypto.randomUUID(), unknownId=crypto.randomUUID();
      const url=base+'/broken.m3u8';
      const filename='request-lifecycle-'+crypto.randomBytes(5).toString('hex')+'.m3u8';
      const first=nativeRequest({action:'add_download',requestId,url,filename});
      assert.equal(first.success,true,'First actual GUI creation failed');
      assert.ok(Number.isSafeInteger(first.taskId)&&first.taskId>0);
      assert.ok(first.sessionId&&first.sessionId.length>=32);
      const verified=nativeRequest({action:'get_request_status',requestId});
      assert.equal(verified.found,true,'Committed request is not queryable');
      assert.equal(verified.taskId,first.taskId);
      assert.equal(verified.sessionId,first.sessionId);
      const duplicate=nativeRequest({action:'add_download',requestId,url,filename});
      assert.equal(duplicate.success,true,'Same request token must be idempotent');
      assert.equal(duplicate.taskId,first.taskId);
      assert.equal(duplicate.sessionId,first.sessionId);
      const invalid=nativeRequest({action:'add_download',
        requestId:'not-a-uuid',url,filename});
      assert.equal(invalid.success,false,'Malformed token must be rejected');
      const missing=nativeRequest({action:'get_request_status',requestId:unknownId});
      assert.equal(missing.success,false,'Unknown UUID cannot be treated as acceptance');
      assert.equal(missing.found,false);
      const listing=nativeRequest({action:'get_status'});
      const rows=(listing.message||'').split(/\r?\n/).filter(x=>/^#\d+\t/.test(x));
      assert.equal(rows.length,1,'Duplicate or invalid token must not create tasks');
      assert.ok(rows[0].includes(filename),'Original task missing from GUI listing');
      const oldGui=guiProcess;
      oldGui.kill();
      await until(()=>oldGui.exitCode!==null||oldGui.signalCode!==null,
        'First GUI process exit before restart',7000);
      // Keep the isolated database but create a new GUI session. Opaque tokens
      // are deliberately NOT durable; a stale request lookup must fail closed.
      const outFd=fs.openSync(path.join(root,'gui.out'),'a');
      const errFd=fs.openSync(path.join(root,'gui.err'),'a');
      guiProcess=cp.spawn(gui,[],{cwd:dist,windowsHide:true,
        stdio:['ignore',outFd,errFd],
        env:{...process.env,IDM_DATA_DIR:config,
          PATH:path.dirname(ffmpegExe)+path.delimiter+process.env.PATH,
          QT_FORCE_STDERR_LOGGING:'1'}});
      fs.closeSync(outFd);fs.closeSync(errFd);
      await until(()=>{
        if(guiProcess.exitCode!==null)throw Error('Restarted GUI exited');
        try{return hostPing().success===true;}catch{return false;}
      },'Restarted GUI Native Host ping',45000);
      const restartBefore=nativeRequest({action:'get_status'});
      const stale=nativeRequest({action:'get_request_status',requestId});
      const restartAfter=nativeRequest({action:'get_status'});
      assert.equal(stale.success,false,'Old GUI session token must not be accepted');
      assert.equal(stale.found,false,'Old GUI token lookup must be unconfirmed');
      const ids=response=>(response.message||'').split(/\r?\n/)
        .filter(line=>/^#\d+\t/.test(line))
        .map(line=>Number(line.match(/^#(\d+)/)[1])).sort((a,b)=>a-b);
      assert.deepEqual(ids(restartAfter),ids(restartBefore),
        'Read-only stale lookup must not create or remove GUI tasks');
      console.log('[PASS] Request lifecycle: same UUID creates one task; '+
        'invalid/unknown IDs fail closed; stale UUID after GUI restart stays unknown');
      return;
    }
    const installed=await browsers();
    if(ackDelay){
      await scenario('Edge SQLite delayed write',installed[0][1],base,root,false,true);
    }else{
      for(const [name,exe] of installed){
        await scenario(name,exe,base,root,false);
        if(name==='Edge')await scenario(name,exe,base,root,true);
      }
    }
  }catch(e){
    for(const f of ['gui.out','gui.err']){
      const file=path.join(root,f);
      if(fs.existsSync(file))console.error('[GUI LOG] '+f+': '+fs.readFileSync(file,'utf8').slice(-3000));
    }
    throw e;
  }finally{
    // Emit only opt-in phase durations on EVERY outcome, including green.
    // The GUI log itself contains request URLs/paths: NEVER print raw lines.
    // This runs after the strict Popup success/failure gate and never retries.
    try {
      if(process.env.IDM_IPC_TIMING_TRACE==='1'){
        const file=path.join(root,'gui.err');
        const raw=fs.existsSync(file)?fs.readFileSync(file,'utf8'):'';
        const markers=[...raw.matchAll(/HLS_ADD_TIMING phase=([a-z_]+) elapsed_ms=(\d+)/g)]
          .map(m=>({phase:m[1],elapsedMs:Number(m[2])}));
        console.log('[IPC_PHASE_SUMMARY] '+JSON.stringify(markers));
        if(!markers.length)console.error('[IPC_PHASE_SUMMARY] no HLS timing markers captured');
      }
    }catch(traceError){
      console.error('[IPC_PHASE_SUMMARY_ERROR] '+String(traceError).slice(0,160));
    }
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
