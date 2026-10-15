// Shared production editor. Canvas is an immediate composition preview only;
// Save asks the native host (or controlled browser host) to emit an actual PNG.
(() => {
  const el=id=>document.getElementById(id),dialog=el('game-artwork-editor'),canvas=el('game-editor-canvas');
  const t=key=>window.NitLinkLocales?.[document.documentElement.lang]?.[key]||window.NitLinkLocales?.['en-US']?.[key]||'';
  let live={},editing={},source=null,mode='fit',x=.5,y=.5,zoom=1,drag=null,saving=false,timer,sourceTimer,loadSequence=0;
  const clamp=value=>Math.max(0,Math.min(1,value));
  const draw=()=>{
    const ctx=canvas.getContext('2d');ctx.clearRect(0,0,512,512);ctx.fillStyle='#161616';ctx.fillRect(0,0,512,512);
    document.querySelectorAll('[data-artwork-mode]').forEach(button=>button.setAttribute('aria-pressed',String(button.dataset.artworkMode===mode)));
    el('game-editor-help').textContent=t('game.edit.help.'+mode);el('game-editor-zoom-row').hidden=mode!=='crop';el('game-editor-zoom').value=zoom;
    el('game-editor-save').disabled=!source||saving;el('game-editor-canvas').style.cursor=mode==='crop'?'grab':'default';
    if(source){
      const cover=Math.max(512/source.naturalWidth,512/source.naturalHeight),fit=Math.min(512/source.naturalWidth,512/source.naturalHeight);
      if(mode==='fit'){
        ctx.save();ctx.filter='blur(12.5px)';ctx.drawImage(source,(512-source.naturalWidth*cover)/2,(512-source.naturalHeight*cover)/2,source.naturalWidth*cover,source.naturalHeight*cover);ctx.restore();
        ctx.fillStyle='rgba(0,0,0,.35)';ctx.fillRect(0,0,512,512);
      }
      const scale=mode==='crop'?cover*zoom:fit,w=source.naturalWidth*scale,h=source.naturalHeight*scale;
      ctx.drawImage(source,mode==='crop'?-(w-512)*x:(512-w)/2,mode==='crop'?-(h-512)*y:(512-h)/2,w,h);
    }
    for(const size of [84,64]){const target=el('game-editor-'+size),context=target.getContext('2d');context.clearRect(0,0,size,size);context.drawImage(canvas,0,0,size,size);}
    canvas.dataset.cropX=String(x);canvas.dataset.cropY=String(y);canvas.dataset.zoom=String(zoom);canvas.dataset.mode=mode;
  };
  const close=()=>{clearTimeout(timer);clearTimeout(sourceTimer);saving=false;++loadSequence;dialog.close();};
  el('game-editor-close').addEventListener('click',close);
  dialog.addEventListener('cancel',()=>{clearTimeout(timer);saving=false;++loadSequence;});
  document.querySelectorAll('[data-artwork-mode]').forEach(button=>button.addEventListener('click',()=>{mode=button.dataset.artworkMode;draw();}));
  el('game-editor-zoom').addEventListener('input',event=>{zoom=Number(event.target.value);draw();});
  el('game-editor-reset').addEventListener('click',()=>{x=y=.5;zoom=1;draw();});
  canvas.addEventListener('pointerdown',event=>{if(mode!=='crop'||!source||saving)return;canvas.setPointerCapture(event.pointerId);drag={pointer:event.pointerId,x,y,left:event.clientX,top:event.clientY};});
  canvas.addEventListener('pointermove',event=>{
    if(!drag||event.pointerId!==drag.pointer||!source)return;
    const ratio=512/canvas.getBoundingClientRect().width,scale=Math.max(512/source.naturalWidth,512/source.naturalHeight)*zoom;
    const dx=source.naturalWidth*scale-512,dy=source.naturalHeight*scale-512;
    x=dx>0?clamp(drag.x-(event.clientX-drag.left)*ratio/dx):.5;y=dy>0?clamp(drag.y-(event.clientY-drag.top)*ratio/dy):.5;draw();
  });
  for(const event of ['pointerup','pointercancel','lostpointercapture'])canvas.addEventListener(event,()=>{drag=null;});
  canvas.addEventListener('keydown',event=>{if(mode!=='crop'||!['ArrowLeft','ArrowRight','ArrowUp','ArrowDown'].includes(event.key))return;event.preventDefault();if(event.key==='ArrowLeft')x=clamp(x-.02);if(event.key==='ArrowRight')x=clamp(x+.02);if(event.key==='ArrowUp')y=clamp(y-.02);if(event.key==='ArrowDown')y=clamp(y+.02);draw();});
  el('game-editor-save').addEventListener('click',()=>{
    if(!source||saving||live.id!==editing.gameId||live.coverId!==editing.coverId)return;
    saving=true;el('game-editor-status').textContent=t('game.edit.saving');draw();
    window.chrome.webview.postMessage({action:'renderGameArtwork',value:{gameId:editing.gameId,coverId:editing.coverId,mode,x,y,zoom}});
    timer=setTimeout(()=>{saving=false;el('game-editor-status').textContent=t('game.artworkFailed');draw();},20000);
  });
  window.NitLinkArtworkEditor={
    update:state=>{live=state;if(dialog.open&&editing.gameId!==state.id)close();},
    notice:key=>{if(!dialog.open||!saving)return;if(key==='artworkSaved'){close();return;}if(['artworkFailed','saveFailed','invalidChoice'].includes(key)){clearTimeout(timer);saving=false;el('game-editor-status').textContent=t('game.'+key);draw();}},
    open:(cover,state)=>{
      live=state;editing={gameId:state.id,coverId:cover.id};mode=state.coverId===cover.id&&state.artworkMode&&state.artworkMode!=='original'?state.artworkMode:'fit';
      x=state.coverId===cover.id?(state.cropX??.5):.5;y=state.coverId===cover.id?(state.cropY??.5):.5;zoom=state.coverId===cover.id?(state.artworkZoom||1):1;
      source=null;saving=false;el('game-editor-status').textContent=t('game.status.loading');draw();dialog.showModal();
      const sequence=++loadSequence,img=new Image(),uri=state.coverId===cover.id&&state.sourceImage?state.sourceImage:cover.image;
      const allowed=/^https:\/\/nitlink\.invalid\/game-images\/\d{1,20}\.png$/.test(uri)||!!window.NitLinkPreview&&/^preview\/(?:fixtures\/[a-z0-9-]+\.(?:png|jpg|webp)|(?:api-images|generated-images)\/[a-f0-9]{64}\.png)$/.test(uri);
      if(!allowed){el('game-editor-status').textContent=t('game.imageFailed');return;}
      clearTimeout(sourceTimer);sourceTimer=setTimeout(()=>{if(sequence===loadSequence)el('game-editor-status').textContent=t('game.imageFailed');},12000);
      img.onload=()=>{if(sequence!==loadSequence)return;clearTimeout(sourceTimer);source=img;el('game-editor-status').textContent='';draw();};
      img.onerror=()=>{if(sequence===loadSequence){clearTimeout(sourceTimer);el('game-editor-status').textContent=t('game.imageFailed');}};img.src=uri;
    }
  };
})();
