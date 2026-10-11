'use strict';
// Additional fast regression for real Popup source semantics; the full
// packaged Windows / native host / GUI test remains mandatory.
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');

const source=fs.readFileSync(path.join(__dirname,'../../browser-extension/popup.js'),'utf8');

function exercise(items,queued,response) {
  const calls=[];
  let refreshes=0;
  const nodes=new Map();
  const ctx=vm.createContext({
    console,
    window:{matchMedia:null},
    document:{
      addEventListener(){},
      getElementById(id){
        if(!nodes.has(id))nodes.set(id,{disabled:false});
        return nodes.get(id);
      }
    },
    calls,
    mockResponse:response,
    sourceItems:items,
    refresh:()=>{refreshes++;},
    setTimeout(){},
    clearTimeout(){}
  });
  vm.runInContext(source,ctx,{filename:'popup.js'});
  vm.runInContext(`
    visible=sourceItems;
    for(const item of sourceItems) selected.add(item.url);
    render=()=>{};
    showToast=()=>{};
    pageToast=()=>{};
    loadRecentHandoffs=refresh;
    send=(msg,callback)=>{calls.push(msg);callback(mockResponse);};
    sendBatch(${queued?'true':'false'});
  `,ctx);
  return {calls,refreshes,
    sent:vm.runInContext('Array.from(sent)',ctx),
    selected:vm.runInContext('Array.from(selected)',ctx)};
}

const a={url:'https://example.test/one.m3u8',filename:'one.m3u8'};
const b={url:'https://example.test/two.m3u8',filename:'two.m3u8'};
test('single Popup Download uses identity-bearing operation, exactly once',()=>{
  const result=exercise([a],false,{success:true,taskId:2000001,
    sessionId:'11111111-1111-4111-8111-111111111111',tracked:true});
  assert.equal(result.calls.length,1);
  assert.equal(result.calls[0].action,'downloadWithOptions');
  assert.equal(result.calls[0].url,a.url);
  assert.equal(result.calls[0].queued,false);
  assert.equal(result.refreshes,1);
  assert.equal(result.sent.length,1);
  assert.equal(result.selected.length,0);
});
test('single Popup Queue retains queued=true and no batch submission',()=>{
  const result=exercise([a],true,{success:true,taskId:2000002,
    sessionId:'11111111-1111-4111-8111-111111111111',tracked:true});
  assert.equal(result.calls.length,1);
  assert.equal(result.calls[0].action,'downloadWithOptions');
  assert.equal(result.calls[0].queued,true);
});
test('failed single Popup submit does not mark sent or retry',()=>{
  const result=exercise([a],false,{success:false,message:'Native Host unavailable'});
  assert.equal(result.calls.length,1);
  assert.equal(result.sent.length,0);
  assert.equal(result.selected.length,1);
  assert.equal(result.refreshes,0);
});
test('multiple Popup items remain on existing batch protocol',()=>{
  const result=exercise([a,b],true,{success:2,failed:0,message:'added'});
  assert.equal(result.calls.length,1);
  assert.equal(result.calls[0].action,'batchDownload');
  assert.equal(result.calls[0].items.length,2);
  assert.ok(result.calls[0].items.every(x=>x.queued===true));
  assert.equal(result.refreshes,0);
});
