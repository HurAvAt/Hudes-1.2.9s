'use strict';
const host = window.chrome.webview;
const tabs = new Map();
let active = 0;
let appearance = {fontFamily:'Consolas',fontSize:14,theme:{background:'#121923',foreground:'#E5EBF2'}};
function send(type, id, value) { host.postMessage(JSON.stringify({type,id,value})); }
function fit() {
    const tab = tabs.get(active);
    if (tab && tab.node.clientWidth && tab.node.clientHeight) tab.fit.fit();
}
function select(id) {
    active = id;
    for (const [key,tab] of tabs) tab.node.hidden = key !== id;
    fit();
    tabs.get(id)?.term.focus();
}
function add(id) {
    const node = document.createElement('div');
    node.className = 'session';
    document.querySelector('main').append(node);
    const term = new Terminal({...appearance,cursorBlink:true,scrollback:3000,allowProposedApi:false});
    const adapter = new FitAddon.FitAddon();
    term.loadAddon(adapter);
    term.open(node);
    tabs.set(id,{term,node,fit:adapter});
    term.onData(input=>send('input',id,input));
    term.onResize(size=>send('size',id,size.cols+','+size.rows));
    select(id);
}
host.addEventListener('message',({data})=>{
    const {id,type,value}=data;
    const tab=tabs.get(id);
    switch(type) {
    case 'add': add(id); break;
    case 'select': select(id); break;
    case 'close': if(tab){tab.term.dispose();tab.node.remove();tabs.delete(id);} break;
    case 'out': if(tab) tab.term.write(Uint8Array.from(atob(value),c=>c.charCodeAt(0))); break;
    case 'end': if(tab) tab.term.writeln('\r\n[Session ended]'); break;
    case 'style':
        appearance=JSON.parse(value);
        document.body.style.backgroundColor=appearance.theme.background;
        for(const t of tabs.values()) Object.assign(t.term.options,appearance);
        fit();
        break;
    }
});
new ResizeObserver(fit).observe(document.body);
document.addEventListener('pointerenter',()=>send('enter',active,''));
document.addEventListener('pointerleave',()=>send('leave',active,''));
document.addEventListener('pointerdown',()=>send('inside',active,''));
send('ready',0,'');
