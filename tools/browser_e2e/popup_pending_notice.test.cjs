'use strict';
// Popup-only deterministic test; it does not replace real Chrome/Edge Windows E2E.
const {test}=require('node:test');
const assert=require('node:assert/strict');
const fs=require('node:fs');
const path=require('node:path');
const vm=require('node:vm');
const source=fs.readFileSync(path.join(__dirname,'../../browser-extension/popup.js'),'utf8');
const html=fs.readFileSync(path.join(__dirname,'../../browser-extension/popup.html'),'utf8');

function exercise(response){
    const nodes=new Map(), actions=[];
    function node(){
        return {
            hidden:true, textContent:'', className:'', title:'',
            appendChild(child){this.lastChild=child;this.children=(this.children||[]).concat(child);},
            addEventListener(){},
        };
    }
    const document={
        addEventListener(){},
        getElementById(id){
            if(!nodes.has(id)) nodes.set(id,node());
            return nodes.get(id);
        },
        createElement(){return node();}
    };
    const ctx=vm.createContext({
        console, document, window:{matchMedia:null}, mockResponse:response, actions,
        setTimeout(){},clearTimeout(){}
    });
    vm.runInContext(source,ctx,{filename:'popup.js'});
    vm.runInContext(`
        send=(msg,done)=>{actions.push(msg.action);done(mockResponse);};
        loadRecentHandoffs();
    `,ctx);
    return {
        section:nodes.get('recentHandoffs'),
        notice:nodes.get('pendingHandoffsNotice'),
        rows:nodes.get('recentHandoffsList'),
        actions
    };
}
test('unresolved requests stay visible without confirmed tasks or native add replay',()=>{
    assert.match(html,/id="pendingHandoffsNotice"/);
    const ui=exercise({success:true,items:[],unresolved:2});
    assert.equal(ui.section.hidden,false);
    assert.equal(ui.notice.hidden,false);
    assert.match(ui.notice.textContent,/2 个下载请求结果待确认/);
    assert.match(ui.notice.textContent,/请勿重复提交/);
    assert.deepEqual(ui.actions,['getRecentHandoffs']);
});
test('no pending requests and no handoffs keeps the history section hidden',()=>{
    const ui=exercise({success:true,items:[],unresolved:0});
    assert.equal(ui.section.hidden,true);
    assert.equal(ui.notice.hidden,true);
    assert.equal(ui.notice.textContent,'');
});
test('confirmed handoff appears without pending warning',()=>{
    const ui=exercise({success:true,items:[{
        taskId:42,filename:'demo.m3u8',sessionId:'11111111-1111-4111-8111-111111111111'
    }],unresolved:0});
    assert.equal(ui.section.hidden,false);
    assert.equal(ui.notice.hidden,true);
    assert.equal(ui.rows.children.length,1);
    assert.equal(ui.rows.children[0].children[0].textContent,'#42 · demo.m3u8');
    assert.deepEqual(ui.actions,['getRecentHandoffs']);
});
test('failed background read never reports an unresolved count as confirmed',()=>{
    const ui=exercise({success:false,items:[],unresolved:8});
    assert.equal(ui.section.hidden,true);
    assert.equal(ui.notice.hidden,true);
});
