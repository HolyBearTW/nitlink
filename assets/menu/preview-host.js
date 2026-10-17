// Opt-in preview host. The actual Windows WebView2 bridge always takes precedence.
(() => {
  if (window.chrome?.webview?.postMessage) return;
  const params=new URLSearchParams(location.search);
  if(params.get('preview')!=='games'||!['file:','http:'].includes(location.protocol)||(location.protocol==='http:'&&!['localhost','127.0.0.1'].includes(location.hostname)))return;
  const listeners=new Set(),jobs=new Map(),games=new Map(),coversById=new Map(),officialGames=new Map();
  let fixtures=[],current={},records={},generation=0,bridgeSequence=0,scenario='ok',nameCapability='unverified',acceptedImageUrls=[];
  let mode=location.protocol==='http:'&&params.get('mode')!=='fixture'?'real':'fixture';
  let showSynthetic=false,initialSearch,restoreSequence=0,initialSearchAllowed=true,controlsInitialized=false;
  let activationSequence=0,activationWrites=Promise.resolve(),activationPending=false;
  const state={locale:params.get('lang')||'zh-TW',languagePreference:params.get('lang')||'zh-TW',panelSide:params.get('layout')||'full',activeDevice:'AVerMedia Live Gamer ULTRA 2.1 · Host fixture',captureDevices:['AVerMedia Live Gamer ULTRA 2.1 · Host fixture','Elgato 4K Pro · Host fixture'],manualHdmiSource:'switch2',effectiveHdmiSource:'Nintendo Switch 2',manualHdmiSourceCustom:'',negotiatedWidth:1920,negotiatedHeight:1080,negotiatedFps:60,negotiatedFpsNumerator:60,negotiatedFpsDenominator:1,negotiatedFormat:'NV12',resolutionText:'1920×1080 / 60 FPS',linkText:'HDMI',latencyText:'8 ms',preventSleep:true,availableFormats:[{width:1920,height:1080,fps:60,fpsNumerator:60,fpsDenominator:1,format:'NV12'},{width:3840,height:2160,fps:60,fpsNumerator:60,fpsDenominator:1,format:'P010'}],captureFormatOverride:{width:0,height:0,fps:0,fpsNumerator:0,fpsDenominator:1,format:''},gameList:[{id:'spider-man-2',title:"Marvel's Spider-Man 2"},{id:'astro-bot',title:'Astro Bot'}]};
  state.discordSignal='1920×1080 · 60 FPS';
  const sourceNames={switch2:'Switch 2',switch:'Switch',ps5:'PS5',ps4:'PS4',xbox_series:'Xbox Series X|S',xbox_one:'Xbox One',pc:'PC'};
  const detectedSource=params.get('source')==='unknown'?'':'Nintendo Switch 2';
  state.manualHdmiSource=params.get('source')==='unknown'?'auto':Object.hasOwn(sourceNames,params.get('source'))?params.get('source'):'switch2';
  state.effectiveHdmiSource=state.manualHdmiSource==='auto'?detectedSource:sourceNames[state.manualHdmiSource];
  state.manualHdmiSourceOptions=[{value:'auto',label:'Auto'},...Object.entries(sourceNames).map(([value,label])=>({value,label})),{value:'other',label:'Other'}];
  const artworkPlatform=()=>{
    if(['switch','switch2'].includes(state.manualHdmiSource))return 'switch';
    if(['ps5','ps4'].includes(state.manualHdmiSource))return 'playstation';
    if(!['auto','other'].includes(state.manualHdmiSource))return 'other';
    const name=(state.effectiveHdmiSource||'').replace(/[ \t]/g,'').toLowerCase();
    return ['switch','switch2','nintendoswitch','nintendoswitch2'].includes(name)?'switch':['ps5','ps5pro','ps4','playstation5','playstation4','sonyplaystation5'].includes(name)?'playstation':name?'other':'unknown';
  };
  const preferredSources=candidates=>{
    if(artworkPlatform()==='playstation')return candidates.filter(cover=>cover.provider==='playstation-tw');
    if(artworkPlatform()!=='switch')return [];
    const taiwan=candidates.filter(cover=>cover.provider==='nintendo-tw');
    return taiwan.length?taiwan:candidates.filter(cover=>cover.provider==='nintendo-hk-eshop');
  };
  const activityArtworkPriority=cover=>{
    if(cover.provider!=='nintendo-hk-store'&&cover.shape==='square')return 0;
    if(cover.isOfficial&&cover.provider!=='nintendo-hk-store'&&cover.shape==='landscape')return 1;
    if(cover.isOfficial&&cover.provider!=='nintendo-hk-store'&&cover.shape==='portrait')return 2;
    if(!cover.isOfficial&&cover.provider==='steamgriddb'&&cover.shape==='portrait')return 3;
    return 4;
  };
  const preferredSeed=id=>preferredSources(officialGames.get(id)?.artworkSources||[])
    .filter(cover=>activityArtworkPriority(cover)<4).sort((a,b)=>activityArtworkPriority(a)-activityArtworkPriority(b))[0];
  const savedActivityPriority=record=>activityArtworkPriority({...record,shape:record.coverShape||''});
  state.holyBearUpdate={current:document.querySelector('meta[name="nitlink-version"]')?.content||'',status:'never',automatic:false};
  if(!['zh-TW','zh-CN','en-US'].includes(state.locale))state.locale=state.languagePreference='zh-TW';
  window.NitLinkPreview={mode};
  const emit=message=>listeners.forEach(listener=>listener({data:message}));
  const api=(operation,value={})=>new Promise(resolve=>{
    if(location.protocol!=='http:'){resolve({status:'offline'});return;}
    const id=++bridgeSequence,script=document.createElement('script');
    const finish=reply=>{clearTimeout(timeout);jobs.delete(id);script.remove();resolve(reply);};
    const timeout=setTimeout(()=>finish({status:'timeout'}),['covers','search'].includes(operation)?30000:20000);
    jobs.set(id,{operation,finish});script.onerror=()=>finish({status:'offline'});
    script.src='preview/bridge.js?'+new URLSearchParams({operation,locale:state.locale,platform:artworkPlatform(),bridgeId:String(id),profile:params.get('profile')||'default',...value});document.head.appendChild(script);
  });
  window.NitLinkPreviewBridge={deliver:(id,reply)=>jobs.get(id)?.finish(reply)};
  const cancel=()=>{++generation;for(const job of [...jobs.values()])if(!['library','capabilities'].includes(job.operation))job.finish({status:'cancelled'});};
  const now=()=>Math.floor(Date.now()/1000);
  const saveRecord=async key=>{
    if(location.protocol==='http:'){const reply=await api('library',{mode,action:'upsert',key,record:JSON.stringify(records[key])});if(reply.status!=='ok')emit({gameNotice:'saveFailed'});}
    else try{localStorage.setItem('nitlink.gamePreview.'+mode,JSON.stringify({records,current}));}catch(_){}
  };
  const saveCurrent=async()=>{
    if(location.protocol==='http:')await api('library',{mode,action:'current',key:current.key||'',start:String(current.startUnix||now())});
    else try{localStorage.setItem('nitlink.gamePreview.'+mode,JSON.stringify({records,current}));}catch(_){}
  };
  const push=()=>{
    if(current.key&&records[current.key]){const record=records[current.key];current={...current,...record,displayName:record.customName||record.displayNames?.[state.locale]||record.name};}
    current.locale=state.locale;current.dataMode=mode;current.discordConnected=false;
    current.artworkPlatform=artworkPlatform();current.discordRpcStatus='previewOnly';current.discordClientDisplayVerified=false;
    current.officialQuery=current.displayNames?.['zh-TW']||current.name||'';
    current.discordActualImageStatus=mode==='real'&&acceptedImageUrls.includes(current.coverUrl)?'probeAccepted':current.id?'previewOnly':'asset';
    current.discordImageStatus=current.coverUrl&&!current.image?'localImageUnavailable':['fit','crop'].includes(current.artworkMode)?'localEditOnly':current.discordActualImageStatus;current.discordNameStatus=nameCapability;
    emit({state});emit({gameState:current});emit({gameLibrary:{games:Object.values(records),favorites:Object.values(records).filter(game=>game.favorite),recent:Object.values(records).filter(game=>game.lastUsedUtc).sort((a,b)=>b.lastUsedUtc-a.lastUsedUtc).slice(0,8)}});
  };
  const artwork=cover=>({provider:cover?.provider||(cover?.synthetic?'fixture':'steamgriddb'),sourceLabel:cover?.sourceLabel||(cover?.synthetic?'Synthetic preview sample':'SteamGridDB'),isOfficial:cover?.isOfficial===true,language:cover?.languageVerified?cover.language:'',region:cover?.region||'',kind:cover?.kind||'cover',coverShape:cover?.shape||''});
  const selectArtwork=(record,cover)=>{if(record.coverId!==cover.id||record.coverUrl!==(cover.url||cover.image))Object.assign(record,{artworkMode:'original',finalImageKey:'',sourceImageKey:'',cropX:.5,cropY:.5,artworkZoom:1});Object.assign(record,artwork(cover),{coverId:cover.id,coverUrl:cover.url||cover.image,coverThumb:cover.thumb||cover.image});};
  const load=new Promise(resolve=>{const script=document.createElement('script');script.src='preview/fixtures.js';script.onload=async()=>{fixtures=window.NitLinkGameFixtures||[];const catalogue=location.protocol==='http:'?await api('artworkCatalog'):{status:'ok',games:window.NitLinkOfficialArtworkSeeds||[]};(catalogue.games||[]).forEach(game=>officialGames.set(game.id,game));resolve();};script.onerror=()=>resolve();document.head.appendChild(script);});
  const restore=async()=>{
    const restoring=++restoreSequence,restoringMode=mode;
    records={};current={key:'',id:0,coverId:0,manualCover:false,startUnix:now(),libraryRestored:true};
    if(location.protocol==='http:'){const capability=await api('capabilities');if(restoring!==restoreSequence)return false;nameCapability=capability.discordNameStatus||'unverified';acceptedImageUrls=capability.acceptedImageUrls||[];const reply=await api('library',{mode:restoringMode,action:'load'});if(restoring!==restoreSequence||mode!==restoringMode)return false;records=reply.records||{};if(reply.currentKey&&records[reply.currentKey])current={...records[reply.currentKey],startUnix:reply.currentStartUnix||now(),libraryRestored:true};}
    else try{const saved=JSON.parse(localStorage.getItem('nitlink.gamePreview.'+mode)||'{}');records=saved.records||{};current=saved.current||current;}catch(_){}
    for(const record of Object.values(records)){
      if(record.finalImageKey)record.image='preview/generated-images/'+record.finalImageKey+'.png';
      if(record.sourceImageKey)record.sourceImage='preview/generated-images/'+record.sourceImageKey+'.png';
      if(mode==='fixture'){
        const fixture=fixtures.find(game=>game.id===record.id);
        if(fixture){record.displayNames={...record.displayNames,...fixture.titles};if(record.image==='preview/fixtures/missing.png')record.image=[...fixture.artworkSources||[],...Object.values(fixture.covers).flat()].find(cover=>cover.id===record.coverId)?.image||'';}
      }
      const officialGame=officialGames.get(record.id);
      if(officialGame)record.displayNames={...record.displayNames,...officialGame.displayNames};
      const official=preferredSeed(record.id);
      if(official&&(!record.manualCover&&(activityArtworkPriority(official)<savedActivityPriority(record))||record.manualCover&&record.coverId===official.id)){
        const same=record.coverId===official.id;selectArtwork(record,official);
        if(!record.finalImageKey)record.image=mode==='fixture'?(fixtures.find(game=>game.id===record.id)?.artworkSources?.[0]?.image||''):same?record.image||'':'';
        await saveRecord(record.key);
      }
    }
    // Repair old automatically selected sample art, preserving favorites, names,
    // activity time and explicit manual choices. Synthetic art is never a default.
    if(mode==='fixture')for(const record of Object.values(records)){
      if(!record.manualCover&&/-fixture\.svg$/.test(record.coverUrl||'')){
        const cover=fixtures.find(game=>game.id===record.id)?.covers?.square?.find(cover=>!cover.synthetic);
        if(cover){Object.assign(record,{coverId:cover.id,coverUrl:cover.url||cover.image,coverThumb:cover.thumb||cover.image,image:cover.image});await saveRecord(record.key);}
      }
    }
    document.getElementById('game-preview-scenario').disabled=mode==='real';
    document.getElementById('game-preview-synthetic-control').hidden=mode==='real';push();
    if(mode==='real'&&current.key&&records[current.key]?.coverThumb&&!records[current.key].finalImageKey){
      const key=current.key,image=await api('image',{url:records[key].coverThumb});
      if(restoring===restoreSequence&&mode===restoringMode&&records[key]&&current.key===key){records[key].image=image.image||'';records[key].sourceImage=image.image||'';push();await saveRecord(key);}
    }
    return restoring===restoreSequence;
  };
  const ensure=key=>{
    if(records[key]){const record=records[key],known=games.get(record.id),displayName=known?.titles?.[state.locale]||known?.displayName;if(displayName)record.displayNames[state.locale]=displayName;return record;}
    const game=games.get(Number(key.replace('sgdb-','')))||(mode==='fixture'&&fixtures.find(game=>'sgdb-'+game.id===key))||state.gameList.find(game=>game.id===key);if(!game)return null;
    const name=game.name||game.title;
    const cover=preferredSeed(game.id)||(game.defaultCover&&activityArtworkPriority(game.defaultCover)<4?game.defaultCover:null)||game.covers?.square?.find(cover=>!cover.synthetic);
    return records[key]={key,id:typeof game.id==='number'?game.id:0,name,displayNames:mode==='fixture'&&game.titles?{...game.titles}:{[state.locale]:game.displayName||name},customName:'',coverId:cover?.id||0,coverUrl:cover?.url||cover?.image||'',coverThumb:cover?.thumb||cover?.image||'',image:cover?.image||'',...artwork(cover),manualCover:false,favorite:false,lastUsedUtc:0,dataMode:mode};
  };
  const later=(callback,token,ms=350)=>setTimeout(()=>{if(token===generation)callback();},ms);
  const art=cover=>scenario==='imageFailure'?'preview/fixtures/missing.png':cover.image;
  const imageBatch=async(items,token,callback)=>{let index=0;await Promise.all(Array.from({length:3},async()=>{while(token===generation&&index<items.length){const item=items[index++],reply=await api('image',{url:item.url});if(token===generation)callback(item,reply);}}));};
  const searchThumbnail=async(game,token)=>{
    let chosen,image='',failed=false;
    const tryCandidates=async candidates=>{
      for(const cover of candidates.slice(0,3)){
        if(token!==generation)return;
        const reply=await api('image',{url:cover.thumb||cover.url});
        if(token!==generation)return;
        if(reply.status==='ok'&&reply.image){chosen=cover;image=reply.image;cover.image=image;return;}
        failed=true;
      }
    };
    const officialFor=shape=>(game.artworkSources||[]).filter(cover=>cover.isOfficial===true&&cover.provider!=='nintendo-hk-store'&&cover.shape===shape);
    const titleKey=value=>String(value||'').normalize('NFKC').replace(/[\s\p{P}\p{S}_]+/gu,'').toLocaleLowerCase();
    const searchOfficialLandscape=async()=>{
      if(artworkPlatform()!=='switch'||!game.displayName||officialFor('landscape').some(cover=>cover.provider==='nintendo-tw'))return [];
      const reply=await api('covers',{gameId:game.id,shape:'landscape',page:0,provider:'nintendo',officialQuery:game.displayName});
      if(token!==generation)return [];
      if(!['ok','empty','partial'].includes(reply.status))failed=true;
      const names=new Set([game.displayName,game.name].map(titleKey).filter(Boolean));
      return [...(reply.covers||[]),...(reply.artworkSources||[])].filter(cover=>cover.provider==='nintendo-tw'&&cover.isOfficial===true&&cover.shape==='landscape'&&names.has(titleKey(cover.productTitle)));
    };
    const communityFor=async shape=>{
      const reply=await api('covers',{gameId:game.id,shape,page:0,provider:'community'});
      if(token!==generation)return [];
      if(!['ok','empty','partial'].includes(reply.status))failed=true;
      return (reply.covers||[]).filter(cover=>cover.provider==='steamgriddb'&&cover.isOfficial!==true&&cover.shape===shape);
    };
    // Search tiles are horizontal: official landscape, community landscape,
    // official portrait, then community portrait. Activity art has its own rank.
    await tryCandidates(officialFor('landscape'));
    if(!image&&token===generation)await tryCandidates(await searchOfficialLandscape());
    if(!image&&token===generation)await tryCandidates(await communityFor('landscape'));
    if(!image&&token===generation)await tryCandidates(officialFor('portrait'));
    if(!image&&token===generation)await tryCandidates(await communityFor('portrait'));
    if(token!==generation)return;
    if(chosen){
      game.defaultCover=chosen;
      const key='sgdb-'+game.id;
      const record=records[key],priority=activityArtworkPriority(chosen);
      if(record&&!record.manualCover&&!record.finalImageKey&&priority<4&&(priority<savedActivityPriority(record)||record.coverId===chosen.id)){
        selectArtwork(record,chosen);record.image=image;record.sourceImage=image;saveRecord(key);push();
      }
    }
    emit({gameEvent:{kind:'image',request:game.thumbnailRequest,gameId:game.id,coverId:0,locale:game.thumbnailLocale,
      coverIdSelected:chosen?.id||0,coverProvider:chosen?.provider||'',coverShape:chosen?.shape||'',coverIsOfficial:chosen?.isOfficial===true,coverUrlSelected:chosen?.url||'',image,imageStatus:image?'ok':failed?'failed':'missing'}});
  };
  const request=async({action,value})=>{
    await load;
    if(action==='ready'){initControls();document.getElementById('game-fixture-banner').hidden=false;document.querySelector('[data-tab="pane-games"]').click();document.getElementById('game-preview-language').value=state.locale;document.getElementById('game-preview-data-mode').value=mode;layout(state.panelSide);if(await restore()&&initialSearchAllowed)initialSearch=setTimeout(()=>document.getElementById('game-search').click(),200);return;}
    if(action==='cancelGameSearch'){initialSearchAllowed=false;clearTimeout(initialSearch);cancel();return;}
    if(action==='searchGames'){
      initialSearchAllowed=false;
      clearTimeout(initialSearch);
      cancel();const token=generation,locale=state.locale;
      if(mode==='real'){
        const reply=await api('search',{query:value.query});if(token!==generation)return;
        (reply.games||[]).forEach(game=>{game.imageStatus='loading';game.thumbnailRequest=value.request;game.thumbnailLocale=locale;games.set(game.id,game);});emit({gameEvent:{...reply,kind:'search',request:value.request,locale}});
        // Enrich every result with bounded concurrency. Known official images do
        // not need a covers lookup and must not wait behind unrelated titles.
        const queue=[...(reply.games||[])].sort((a,b)=>Number(!!b.artworkSources?.length)-Number(!!a.artworkSources?.length));
        let index=0;await Promise.all(Array.from({length:2},async()=>{while(token===generation&&index<queue.length)await searchThumbnail(queue[index++],token);}));return;
      }
      later(()=>{const q=String(value.query).trim().toLowerCase();const numeric=/^\d+$/.test(q)||/^https:\/\/store\.steampowered\.com\/app\/\d+/.test(q);let rows=fixtures.filter(game=>numeric||[...Object.values(game.titles),game.name,game.slug].some(name=>name.toLowerCase().includes(q)));if(['empty','upstreamUnavailable','timeout'].includes(scenario))rows=[];rows.forEach(game=>games.set(game.id,game));const status=scenario==='imageFailure'?(rows.length?'ok':'empty'):scenario!=='ok'?scenario:rows.length?'ok':'empty';emit({gameEvent:{kind:'search',request:value.request,locale,status,dataMode:'fixture',queriedTerms:[value.query],suggestions:[],games:rows.map(game=>({...game,image:game.covers.square[0]?art(game.covers.square[0]):'',displayName:game.titles[state.locale],source:'fixture',dataMode:'fixture'}))}});},token);return;
    }
    if(action==='setGame'){
      initialSearchAllowed=false;
      clearTimeout(initialSearch);
      if(current.key===value&&!activationPending){push();return;}
      cancel();const existed=!!records[value],record=ensure(value);if(!record)return;
      const sequence=++activationSequence,selectedMode=mode;activationPending=true;record.lastUsedUtc=now();
      const selected={...record,startUnix:current.key===value?current.startUnix:now(),libraryRestored:existed},snapshot=JSON.stringify(record);
      if(location.protocol==='http:'){
        activationWrites=activationWrites.catch(()=>{}).then(()=>api('library',{mode:selectedMode,action:'activate',key:value,record:snapshot,start:String(selected.startUnix)}));
        const reply=await activationWrites;if(reply.status!=='ok'){if(sequence===activationSequence)activationPending=false;emit({gameNotice:'saveFailed'});return;}
      }
      if(sequence!==activationSequence||selectedMode!==mode)return;
      activationPending=false;current=selected;push();if(location.protocol!=='http:')await saveCurrent();
      if(mode==='real'&&record.coverThumb&&!record.finalImageKey){const token=generation,image=await api('image',{url:record.coverThumb});if(token===generation&&current.key===value&&image.status==='ok'){record.image=image.image||'';record.sourceImage=record.image;push();}}return;
    }
    if(action==='addGameFavorite'||action==='removeGameFavorite'){const record=ensure(value);if(!record)return;record.favorite=action==='addGameFavorite';await saveRecord(value);push();emit({gameNotice:record.favorite?'favoriteAdded':'favoriteRemoved'});return;}
    if(action==='setGameCustomName'){const record=ensure(value.key);if(!record)return;record.customName=value.name.trim();await saveRecord(value.key);push();emit({gameNotice:'nameSaved'});return;}
    if(action==='clearGame'){cancel();const sequence=++activationSequence,selectedMode=mode;activationPending=false;const cleared={key:'',id:0,coverId:0,startUnix:now(),libraryRestored:true};if(location.protocol==='http:'){activationWrites=activationWrites.catch(()=>{}).then(()=>api('library',{mode:selectedMode,action:'current',key:'',start:String(cleared.startUnix)}));await activationWrites;}if(sequence===activationSequence&&mode===selectedMode){current=cleared;if(location.protocol!=='http:')await saveCurrent();push();}return;}
    if(action==='getGameCovers'){
      cancel();const token=generation,locale=state.locale;let reply;
      const liveCovers=mode==='real'||value.provider==='nintendo';
      if(liveCovers)reply=await api('covers',{gameId:value.gameId,shape:value.shape,page:value.page||0,provider:value.provider||'all',officialQuery:value.officialQuery||''});
      else{const game=fixtures.find(game=>game.id===value.gameId);let rows=value.shape==='all'?Object.values(game?.covers||{}).flat():game?.covers[value.shape]||[];rows=rows.filter(cover=>showSynthetic||!cover.synthetic).sort((a,b)=>Number(!!a.synthetic)-Number(!!b.synthetic));if(['empty','timeout','upstreamUnavailable'].includes(scenario))rows=[];const size=8,page=value.page||0;reply={status:scenario==='imageFailure'?'ok':scenario!=='ok'?scenario:rows.length?'ok':'empty',covers:rows.slice(page*size,(page+1)*size),total:rows.length,page,hasMore:(page+1)*size<rows.length,dataMode:'fixture'};await new Promise(resolve=>setTimeout(resolve,400));}
      if(!liveCovers){const preferred=value.provider==='community'?[]:preferredSources(fixtures.find(game=>game.id===value.gameId)?.artworkSources||[]);reply.artworkSources=preferred.filter(cover=>value.shape==='all'||cover.shape===value.shape);reply.officialProvider=preferred[0]?.provider||'';}
      const candidates=[...(reply.artworkSources||[]),...(reply.covers||[])];
      if(token!==generation)return;coversById.clear();candidates.forEach(cover=>coversById.set(cover.id,cover));
      const record=records[current.key];
      emit({gameEvent:{...reply,kind:'covers',request:value.request,gameId:value.gameId,locale,shape:value.shape}});
      const applyAutomatic=cover=>{
        const priority=activityArtworkPriority(cover);
        if(record&&current.id===value.gameId&&!record.manualCover&&!record.finalImageKey&&value.shape==='all'&&value.provider==='all'&&!value.page&&cover.image&&priority<4&&
          (priority<savedActivityPriority(record)||record.coverId===cover.id)){
          selectArtwork(record,cover);record.image=cover.image;record.sourceImage=cover.image;push();
        }
      };
      if(!liveCovers){candidates.forEach((cover,index)=>later(()=>{applyAutomatic(cover);if(record?.coverId===cover.id){record.image=cover.image;push();saveRecord(record.key);}emit({gameEvent:{kind:'image',request:value.request,gameId:value.gameId,coverId:cover.id,locale,image:art(cover),imageStatus:'ok'}});},token,80+index*60));return;}
      await imageBatch(candidates.map(cover=>({cover,url:cover.thumb||cover.url})),token,(item,image)=>{item.cover.image=image.image||'';item.cover.imageStatus=image.status==='ok'?'ok':'failed';applyAutomatic(item.cover);emit({gameEvent:{kind:'image',request:value.request,gameId:value.gameId,coverId:item.cover.id,locale,image:image.image||'',imageStatus:item.cover.imageStatus}});if(current.key&&records[current.key]?.coverId===item.cover.id){const selected=records[current.key];if(!selected.finalImageKey){selected.image=image.image||'';selected.sourceImage=selected.image;}push();}});if(current.key)await saveRecord(current.key);return;
    }
    if(action==='selectGameCover'){const cover=coversById.get(value.coverId);if(!cover?.image||cover.imageStatus==='failed'){emit({gameNotice:'imageFailed'});return;}if(current.id===value.gameId){const record=records[current.key];selectArtwork(record,cover);Object.assign(record,{image:cover.image,sourceImage:cover.image,manualCover:true,artworkMode:'original',finalImageKey:'',sourceImageKey:''});push();await saveRecord(current.key);emit({gameNotice:'coverSaved'});}return;}
    if(action==='renderGameArtwork'){
      const key=current.key,record=records[key];if(!record||record.id!==value.gameId||record.coverId!==value.coverId){emit({gameNotice:'invalidChoice'});return;}
      cancel();const token=generation;await saveRecord(key);
      const result=await api('renderArtwork',{mode,gameId:value.gameId,coverId:value.coverId,renderMode:value.mode,x:value.x,y:value.y,zoom:value.zoom});
      if(token!==generation||current.key!==key)return;
      if(result.status!=='ok'){emit({gameNotice:'artworkFailed'});return;}
      const {status,...rendered}=result;Object.assign(record,rendered,{manualCover:true});await saveRecord(key);push();emit({gameNotice:'artworkSaved'});return;
    }
    if(action==='restoreAutoCover'){if(current.key){const record=records[current.key],official=preferredSeed(record.id);Object.assign(record,artwork(official),{manualCover:false,coverId:official?.id||0,coverUrl:official?.url||'',coverThumb:official?.thumb||'',coverShape:official?.shape||'',image:'',sourceImage:'',artworkMode:'original',finalImageKey:'',sourceImageKey:''});await saveRecord(current.key);push();emit({gameNotice:'autoRestored'});}return;}
    if(action==='saveGameSettings'){emit({gameNotice:'qualitySaved'});return;}
    if(action==='setLanguage'){state.locale=value==='system'?'zh-TW':value;state.languagePreference=value;push();return;}
    if(action==='cyclePanelSide'){layout({full:'right',right:'left',left:'full'}[state.panelSide]||'full');push();return;}
    if(action==='setManualHdmiSource'){const previous=artworkPlatform();state.manualHdmiSource=value;state.effectiveHdmiSource=value==='auto'?detectedSource:value==='other'?state.manualHdmiSourceCustom||detectedSource:sourceNames[value]||value;if(previous!==artworkPlatform()){cancel();games.clear();coversById.clear();}push();return;}
    if(action==='setManualHdmiSourceCustom'){const previous=artworkPlatform();state.manualHdmiSourceCustom=value;state.effectiveHdmiSource=value||detectedSource;if(previous!==artworkPlatform()){cancel();games.clear();coversById.clear();}push();return;}
    if(action==='setPreferredDevice'){state.activeDevice=value;push();return;}
    if(action==='setCaptureFormatOverride'){state.captureFormatOverride=value;push();return;}
    if(action==='javascriptException'){console.error('Preview bridge:',value);return;}
    if(action.startsWith('toggle')){const keys={toggleHDR:'hdrEnabled',toggleColorExpansion:'colorExpansion',toggleNIS:'nisEnabled',toggleVSync:'vsync',toggleLowLatency:'lowLatency',togglePreventSleep:'preventSleep',toggleMute:'audioMuted'};if(keys[action])state[keys[action]]=!state[keys[action]];}push();
  };
  const layout=value=>{const side=String(value).toLowerCase();state.panelSide=side;document.documentElement.dataset.side=side;document.documentElement.style.background='#101319';document.body.style.width=side==='full'?'100%':'min(420px, 100vw)';document.body.style.marginLeft=side==='right'?'auto':'0';const app=document.querySelector('.app');app.style.width=side==='full'?'100%':'min(420px, 100vw)';app.style.left=side==='right'?'auto':'0';app.style.right=side==='left'?'auto':'0';document.getElementById('game-preview-layout').value=side;};
  window.chrome=window.chrome||{};window.chrome.webview={postMessage:request,addEventListener:(type,listener)=>{if(type==='message')listeners.add(listener);}};
  function initControls(){
    if(controlsInitialized)return;controlsInitialized=true;
    document.getElementById('game-preview-layout').addEventListener('change',event=>{layout(event.target.value);push();});
    document.getElementById('game-preview-language').addEventListener('change',event=>request({action:'setLanguage',value:event.target.value}));
    document.getElementById('game-preview-synthetic').addEventListener('change',event=>{showSynthetic=event.target.checked;if(current.id)document.getElementById('game-cover-retry').click();});
    document.getElementById('game-preview-data-mode').addEventListener('change',async event=>{if(event.target.value==='real'&&location.protocol!=='http:'){event.target.value='fixture';emit({gameNotice:'realRequiresServer'});return;}cancel();mode=event.target.value;window.NitLinkPreview.mode=mode;games.clear();coversById.clear();if(await restore())document.getElementById('game-search').click();});
    document.getElementById('game-preview-scenario').addEventListener('change',event=>{scenario=event.target.value;document.getElementById('game-search').click();queueMicrotask(()=>{const token=generation;if(current.id)setTimeout(()=>{if(token===generation)document.getElementById('game-cover-retry').click();},1200);});});
    if(!document.getElementById('game-query').value)document.getElementById('game-query').value='薩爾達';
  }
})();
