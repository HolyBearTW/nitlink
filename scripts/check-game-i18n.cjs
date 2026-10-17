// Resource and ordinary-browser checks. No Windows build or CTest.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const {installExactDocument}=require('./preview-browser.cjs');
const root=path.resolve(__dirname,'..'),locales=['zh-TW','zh-CN','en-US','de-DE'],comparisonLocales=['zh-TW','en-US','de-DE'],context={window:{}},evidencePath=process.env.NITLINK_I18N_EVIDENCE||path.join(root,'out/game-preview/i18n-checks.json');
for(const locale of locales)vm.runInNewContext(fs.readFileSync(path.join(root,'locales',locale+'.js'),'utf8'),context);
const resources=context.window.NitLinkLocales;
const keys=[...new Set(locales.flatMap(locale=>Object.keys(resources[locale]).filter(key=>key.startsWith('game.'))))];
for(const locale of locales)for(const key of keys)assert.equal(typeof resources[locale][key],'string',locale+': '+key);
const source=['assets/menu/menu.js','assets/menu/artwork-editor.js','nitlink-menu.html'].map(file=>fs.readFileSync(path.join(root,file),'utf8')).join('\n');
for(const match of source.matchAll(/["'](game\.[\w.]+)["']/g))if(!match[1].endsWith('.'))for(const locale of locales)assert(resources[locale][match[1]],locale+': '+match[1]);
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try{
   const page=await browser.newPage({viewport:{width:1440,height:1100}}),evidence=[],pageErrors=[];
   page.on('pageerror',error=>pageErrors.push(error.message));
  // The ordinary URL can be interrupted by local web protection. Record that
  // separately, then use the same exact-source transport as the review launcher.
  let normalUrlWorks=true;
   const url='http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks&lang=zh-TW&source=switch2';
  try{await page.goto(url,{waitUntil:'domcontentloaded',timeout:10000});}
  catch(error){normalUrlWorks=false;await installExactDocument(page);await page.goto(url,{waitUntil:'domcontentloaded',timeout:10000});}
   await page.locator('.game-result').first().waitFor({timeout:25000});
   await page.evaluate(()=>{
    window.__gameEvents=[];window.__gameStates=[];
    const gameEvent=window._gameEvent,gameState=window._updateGameState;
    window._gameEvent=value=>{window.__gameEvents.push(value);return gameEvent(value);};
    window._updateGameState=value=>{window.__gameStates.push(value);return gameState(value);};
   });
   const directSearches=[['曠野之息',35744],['災厄啟示錄',5267007]],directSearchEvidence=[];
   for(const [query,gameId] of directSearches){
    await page.evaluate(()=>{window.__gameEvents.length=0;});
    await page.locator('#game-query').fill(query);await page.locator('#game-search').click();
    await page.waitForFunction(({gameId})=>window.__gameEvents.some(event=>event.kind==='search'&&(event.games||[]).some(game=>game.id===gameId)),{gameId},{timeout:30000});
    const event=await page.evaluate(({gameId})=>window.__gameEvents.findLast(value=>value.kind==='search'&&(value.games||[]).some(game=>game.id===gameId)),{gameId});
    assert(event.workerCount>0,query+': Worker must find the game without a title mapping');
    directSearchEvidence.push({query,gameId,workerCount:event.workerCount});
   }
   evidence.push({directSearches:directSearchEvidence});
   const selectedGame=5303117,gameName='The Legend of Zelda: Tears of the Kingdom',officialQuery='薩爾達傳說 王國之淚';
   for(const locale of comparisonLocales){
   await page.selectOption('#game-preview-language',locale);
   await page.waitForFunction(locale=>document.documentElement.lang===locale,locale);
    await page.evaluate(()=>{window.__gameEvents.length=0;});
    // Keep the search term in the Taiwan catalogue's product language while
    // changing only the UI locale. English names need not match that catalogue.
    await page.locator('#game-query').fill(officialQuery);
    await page.locator('#game-search').click();
    await page.waitForFunction(({locale,selectedGame})=>window.__gameEvents.some(event=>event.kind==='search'&&event.locale===locale&&(event.games||[]).some(game=>game.id===selectedGame)),{locale,selectedGame},{timeout:30000});
    const search=await page.evaluate(({locale,selectedGame})=>window.__gameEvents.findLast(event=>event.kind==='search'&&event.locale===locale&&(event.games||[]).some(game=>game.id===selectedGame)),{locale,selectedGame});
    assert(search.workerCount>0,locale+': live Worker returned games');
    assert(['ok','partial'].includes(search.status),locale+': live Worker search status');
    await page.waitForFunction(({selectedGame})=>window.__gameEvents.some(event=>event.kind==='image'&&event.gameId===selectedGame&&event.coverId===0&&event.coverProvider==='nintendo-tw'&&event.coverShape==='landscape'),{selectedGame},{timeout:30000});
    const thumbnail=await page.evaluate(selectedGame=>window.__gameEvents.findLast(event=>event.kind==='image'&&event.gameId===selectedGame&&event.coverId===0&&event.coverProvider==='nintendo-tw'),selectedGame);
    await page.evaluate(()=>{window.__gameStates.length=0;});
    await page.locator('.game-result').filter({hasText:gameName}).first().click();
    await page.waitForFunction(({locale,selectedGame})=>window.__gameStates.some(state=>state.id===selectedGame&&state.locale===locale&&state.artworkPlatform==='switch'),{locale,selectedGame},{timeout:15000});
    await page.locator('[data-cover-provider="nintendo"]').click();
    await page.locator('#game-official-query').fill(officialQuery);
    await page.locator('#game-official-search').click();
    // The Nintendo Taiwan directory is searched independently of UI locale.
    await page.locator('#game-shape-all').click();
    await page.waitForFunction(({locale,selectedGame})=>window.__gameEvents.some(event=>event.kind==='covers'&&event.locale===locale&&event.gameId===selectedGame&&event.shape==='all'),{locale,selectedGame},{timeout:30000});
    await page.waitForFunction(()=>document.querySelectorAll('#game-official-gallery .game-cover[data-provider="nintendo-tw"]').length>0,{},{timeout:15000}).catch(async error=>{
     console.error('Official gallery state:',await page.evaluate(()=>({coverEvents:window.__gameEvents.filter(event=>event.kind==='covers').map(({locale,gameId,shape,status,officialStatus,officialProvider,artworkSources,covers})=>({locale,gameId,shape,status,officialStatus,officialProvider,artworkSources,covers:covers?.length})),count:document.querySelectorAll('#game-official-gallery .game-cover').length,images:[...document.querySelectorAll('#game-official-gallery img')].slice(0,3).map(image=>({src:image.currentSrc,state:image.closest('.game-art')?.dataset.imageState,naturalWidth:image.naturalWidth}))})));
     throw error;
    });
    const payload=await page.evaluate(({locale,selectedGame})=>window.__gameEvents.findLast(event=>event.kind==='covers'&&event.locale===locale&&event.gameId===selectedGame&&event.shape==='all'),{locale,selectedGame});
    const official=(payload.artworkSources||[]).filter(cover=>cover.provider==='nintendo-tw'&&cover.isOfficial===true);
    assert(official.length>0,locale+': Taiwan official artwork is in the frontend event');
    assert(official.every(cover=>cover.shape==='landscape'&&cover.url.startsWith('https://images.ctfassets.net/o5v89n4kg6h4/')),locale+': all live Taiwan results are horizontal Contentful images');
    assert.equal(payload.officialProvider,'nintendo-tw',locale+': same official provider');
   const labels=await page.evaluate(()=>({group:document.querySelector('[data-i18n="game.officialGroup"]').textContent,badge:[...document.querySelectorAll('#game-official-gallery .game-cover-badge')].find(el=>!el.hidden&&el.getClientRects().length)?.textContent,community:document.querySelector('#game-community-heading').textContent,untranslated:[...document.querySelectorAll('#pane-games [data-i18n]')].filter(el=>el.textContent===el.dataset.i18n).map(el=>el.dataset.i18n)}));
   assert.equal(labels.group,resources[locale]['game.officialGroup']);assert.equal(labels.badge,resources[locale]['game.officialBadge']);assert.equal(labels.community,resources[locale]['game.communityGroup']);assert.deepEqual(labels.untranslated,[]);
   await page.selectOption('#game-preview-layout','right');
   assert.equal(await page.locator('#pane-games').evaluate(el=>el.scrollWidth>el.clientWidth),false);
    const card=await page.locator('#game-official-gallery .game-cover[data-provider="nintendo-tw"]').first().evaluate(el=>({provider:el.dataset.provider,kind:el.dataset.kind,visible:!!el.getClientRects().length,imageState:el.querySelector('.game-art')?.dataset.imageState,imageWidth:el.querySelector('img')?.naturalWidth}));
    assert.equal(card.visible,true,locale+': official cover is present in the rendered DOM');
    await page.waitForFunction(()=>{const image=document.querySelector('#game-official-gallery .game-cover[data-provider="nintendo-tw"] img');return image?.complete&&image.naturalWidth>0;},{},{timeout:20000});
    card.imageState='ok';card.imageWidth=await page.locator('#game-official-gallery .game-cover[data-provider="nintendo-tw"] img').first().evaluate(image=>image.naturalWidth);
    const gameState=await page.evaluate(({locale,selectedGame})=>window.__gameStates.findLast(state=>state.id===selectedGame&&state.locale===locale),{locale,selectedGame});
    evidence.push({locale,workerStatus:search.status,workerCount:search.workerCount,gameId:selectedGame,platform:gameState.artworkPlatform,searchThumbnail:{provider:thumbnail.coverProvider,shape:thumbnail.coverShape,id:thumbnail.coverIdSelected},officialCount:official.length,sourceId:official[0].id,provider:official[0].provider,url:official[0].url,shape:official[0].shape,...card,...labels});
    await page.selectOption('#game-preview-layout','full');
  }
   assert.deepEqual(pageErrors,[],'no JavaScript page errors');
   const localeEvidence=evidence.filter(item=>item.locale);
   assert.equal(localeEvidence.length,comparisonLocales.length,'all requested locales produced artwork evidence');
   assert.equal(new Set(localeEvidence.map(item=>JSON.stringify({id:item.gameId,platform:item.platform,provider:item.provider,url:item.url,shape:item.shape}))).size,1,'same game, platform, source, URL, and artwork shape in all locales: '+JSON.stringify(localeEvidence));
   fs.mkdirSync(path.dirname(evidencePath),{recursive:true});fs.writeFileSync(evidencePath,JSON.stringify({gameKeys:keys.length,normalUrlWorks,exactSourceFallback:!normalUrlWorks,pageErrors,evidence},null,2));
   console.log('All '+keys.length+' game keys and '+comparisonLocales.length+' live Worker / dynamic Nintendo gallery locale paths passed. No JavaScript page errors. Ordinary URL: '+normalUrlWorks+'; exact-source launcher available.');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
