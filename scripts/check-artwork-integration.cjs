// Actual production HTML/JS, real Worker and official images via loopback bridge.
// Browser persistence uses only the disposable checks profile; restore it on exit.
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const {installExactDocument}=require('./preview-browser.cjs');
const root=path.resolve(__dirname,'..'),output=path.join(root,'out/game-preview');
const profiles=['real','fixture'].map(mode=>path.join(root,'out/game-preview-api',mode+'-library-checks.json'));
const backups=profiles.map(file=>fs.existsSync(file)?fs.readFileSync(file):null);
const evidence={transport:'actual source HTML/JS; real loopback API/image bridge',clientDisplayVerified:false,checks:[]};
function check(name,value){assert(value,name);evidence.checks.push(name);}
(async()=>{
 fs.mkdirSync(output,{recursive:true});
 for(const file of profiles)fs.writeFileSync(file,JSON.stringify({records:{},currentKey:'',currentStartUnix:1700000000}));
 let browser;
 try{
  browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
  const page=await browser.newPage({viewport:{width:1440,height:1100}}),errors=[];
  await installExactDocument(page);page.on('pageerror',error=>errors.push(String(error)));
  const watch=()=>page.evaluate(()=>{window.__artworkChecks={};chrome.webview.addEventListener('message',event=>{if(event.data.gameState)window.__artworkChecks.state=event.data.gameState;if(event.data.gameEvent)window.__artworkChecks.event=event.data.gameEvent;});});
  const open=async source=>{
   await page.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks&source='+source,{waitUntil:'domcontentloaded'});
   await page.waitForFunction(()=>window.chrome?.webview&&document.querySelectorAll('.game-result').length>0,{},{timeout:40000});
   await watch();
  };
  const search=async query=>{
   await page.locator('#game-query').fill(query);await page.locator('#game-search').click();
   await page.waitForFunction(()=>document.querySelector('#game-search-status').getAttribute('aria-busy')==='false'&&document.querySelectorAll('.game-result').length>0,{},{timeout:40000});
  };
  const select=async name=>{
   await page.locator('.game-result').filter({has:page.getByText(name,{exact:true})}).first().click();
   const id={'Hyrule Warriors: Age of Calamity':5267007,'The Legend of Zelda: Tears of the Kingdom':5303117,'Astro Bot':5454475,'God of War Ragnarök':5296432}[name];
   await page.waitForFunction(id=>window.__artworkChecks?.state?.id===id,id);
   await page.locator('#game-cover-retry').click();
   await page.waitForFunction(()=>document.querySelector('#game-gallery').getAttribute('aria-busy')==='false',{},{timeout:40000});
  };
  const officialLoaded=async provider=>{
   await page.waitForFunction(()=>document.querySelector('#game-gallery').getAttribute('aria-busy')==='false',{},{timeout:40000});
   await page.waitForFunction(provider=>[...document.querySelectorAll('#game-official-gallery .game-cover')].some(card=>card.dataset.provider===provider&&!card.disabled&&card.querySelector('img')?.naturalWidth>0),provider,{timeout:30000});
   return page.locator('#game-official-gallery .game-cover[data-provider="'+provider+'"]').first();
  };
  await open('switch2');
  evidence.zeldaResults=await page.locator('.game-result-title').allTextContents();
  check('complete real Zelda results',evidence.zeldaResults.length>=27);
  check('known Switch source hides source selection guidance',!(await page.locator('#game-source-guidance').isVisible()));
  await select('Hyrule Warriors: Age of Calamity');
  check('store search stays hidden until the official source is selected',!(await page.locator('#game-store-search').isVisible()));
  await page.locator('[data-cover-provider="nintendo"]').click();
  check('official Nintendo catalogue search is available for Switch',await page.locator('#game-store-search').isVisible());
  await (await officialLoaded('nintendo-tw')).click();
  await page.waitForFunction(()=>window.__artworkChecks.state?.provider==='nintendo-tw'&&window.__artworkChecks.state?.manualCover);
  check('Taiwan excludes all Hong Kong candidates',(await page.locator('#game-official-gallery .game-cover').evaluateAll(cards=>cards.every(card=>card.dataset.provider==='nintendo-tw'))));
  const started=await page.evaluate(()=>window.__artworkChecks.state.startUnix);
  await officialLoaded('nintendo-tw');
  check('Taiwan official image remains available beside store results',await page.locator('#game-official-gallery [data-provider="nintendo-tw"]').count()>0);
  check('single-page official gallery hides pagination',!(await page.locator('#game-cover-pagination').isVisible()));
  await page.locator('#game-shape-square').click();
  await page.waitForFunction(()=>document.querySelector('#game-gallery').getAttribute('aria-busy')==='false');
  check('portrait Taiwan box is not marked square',await page.locator('#game-official-gallery .game-cover:visible').count()===0);
  check('square filter does not revive Hong Kong',await page.locator('#game-official-gallery [data-provider^="nintendo-hk"]').count()===0);
  await page.locator('#game-shape-all').click();await officialLoaded('nintendo-tw');
  await page.locator('#game-adjust-cover').click();
  await page.waitForFunction(()=>!document.querySelector('#game-editor-save').disabled);
  check('default full-image mode',await page.locator('#game-editor-canvas').getAttribute('data-mode')==='fit');
  await page.locator('[data-artwork-mode="crop"]').click();
  const canvas=await page.locator('#game-editor-canvas').boundingBox();
  await page.mouse.move(canvas.x+canvas.width/2,canvas.y+canvas.height/2);await page.mouse.down();await page.mouse.move(canvas.x+canvas.width/2,canvas.y+canvas.height*.7,{steps:8});await page.mouse.up();
  const cropY=Number(await page.locator('#game-editor-canvas').getAttribute('data-crop-y'));
  check('crop can be dragged off center',Math.abs(cropY-.5)>.01);
  check('84px and 64px previews have image pixels',await page.evaluate(()=>[84,64].every(size=>document.getElementById('game-editor-'+size).getContext('2d').getImageData(0,0,size,size).data.some((v,i)=>i%4!==3&&v>40))));
  await page.screenshot({path:path.join(output,'official-source-crop.png')});
  await page.locator('#game-editor-save').click();await page.waitForFunction(()=>!document.querySelector('#game-artwork-editor').open,{},{timeout:25000});
  await page.waitForFunction(()=>window.__artworkChecks.state.finalImageKey?.length===64);
  const edited=await page.evaluate(()=>window.__artworkChecks.state);
  check('cover edit preserves activity timer',edited.startUnix===started);
  check('actual square PNG written',fs.existsSync(path.join(root,'out/game-preview-api/generated',edited.finalImageKey+'.png')));
  const png=fs.readFileSync(path.join(root,'out/game-preview-api/generated',edited.finalImageKey+'.png'));
  check('saved artwork is a real 512x512 PNG',png.subarray(0,8).equals(Buffer.from([137,80,78,71,13,10,26,10]))&&png.readUInt32BE(16)===512&&png.readUInt32BE(20)===512);
  await page.locator('#game-current-favorite').click();
  await page.waitForFunction(()=>document.querySelector('#game-favorites [data-key="sgdb-5267007"]'));
  await page.locator('[data-cover-provider="all"]').click();
  await search('Tears of the Kingdom');await select('The Legend of Zelda: Tears of the Kingdom');await officialLoaded('nintendo-tw');
  await page.locator('#game-favorites [data-key="sgdb-5267007"] .game-my-select').click();
  await page.waitForFunction(key=>window.__artworkChecks.state?.finalImageKey===key,edited.finalImageKey);
  check('game switch restores saved crop',await page.evaluate(y=>window.__artworkChecks.state.cropY===y,edited.cropY));
  await page.reload({waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>document.querySelector('#game-current-art img')?.naturalWidth>0,{},{timeout:40000});
  await watch();
  check('restart restores adjusted official cover',(await page.locator('#game-current-art img').getAttribute('src')).includes(edited.finalImageKey));
  await page.locator('#game-cover-retry').click();await officialLoaded('nintendo-tw');
  await page.locator('[data-cover-provider="nintendo"]').click();await officialLoaded('nintendo-tw');
  await page.locator('#pane-games').evaluate(el=>el.scrollTop=0);
  await page.screenshot({path:path.join(output,'taiwan-priority-full.png')});
  for(const locale of ['zh-TW','zh-CN','en-US','de-DE']){
   await page.selectOption('#game-preview-language',locale);await page.waitForFunction(locale=>document.documentElement.lang===locale,locale);
   for(const layout of ['full','left','right']){
    await page.selectOption('#game-preview-layout',layout);
    check(locale+' '+layout+' fits available width',await page.locator('#pane-games').evaluate(el=>el.scrollWidth<=el.clientWidth));
   }
   const raw=await page.locator('#pane-games [data-i18n]').evaluateAll(elements=>elements.filter(el=>el.textContent===el.dataset.i18n).map(el=>el.dataset.i18n));
   check(locale+' has translated game labels',raw.length===0);
  }
  await page.selectOption('#game-preview-language','zh-TW');await page.selectOption('#game-preview-layout','full');
  await page.locator('#game-choose-source').click();
  check('source guidance opens real source selector',await page.locator('#hdmi-source-trigger').isVisible());
  await page.selectOption('#hdmi-source-trigger','ps5');await page.locator('#source-trigger').click();
  await page.waitForFunction(()=>window.__artworkChecks?.state?.artworkPlatform==='playstation',{},{timeout:10000});
  await search('Astro Bot');await select('Astro Bot');await (await officialLoaded('playstation-tw')).click();
  check('PS5 never receives Nintendo candidates',await page.locator('#game-official-gallery .game-cover').evaluateAll(cards=>cards.every(card=>card.dataset.provider==='playstation-tw')));
  check('PlayStation English artwork not marked Traditional Chinese',await page.locator('#game-official-gallery .game-cover-badge:visible').count()===0);
  check('PS5 source hides Nintendo store search',!(await page.locator('#game-store-search').isVisible()));
  check('PS5 local cover says PlayStation Taiwan',(await page.locator('#game-current-artwork-tag').textContent()).includes('PlayStation 台灣'));
  await page.locator('#game-current-favorite').click();await page.waitForFunction(()=>document.querySelector('#game-favorites [data-key="sgdb-5454475"]'));
  await page.setViewportSize({width:420,height:1000});await page.selectOption('#game-preview-layout','right');
  await page.locator('#pane-games').evaluate(el=>el.scrollTop=0);
  check('actual 420px PS5 view fits',await page.locator('#pane-games').evaluate(el=>el.scrollWidth<=el.clientWidth));
  await page.screenshot({path:path.join(output,'playstation-taiwan-docked.png')});
  await page.locator('#game-current-title').scrollIntoViewIfNeeded();
  await page.screenshot({path:path.join(output,'playstation-taiwan-docked-cover.png')});
  await search('God of War Ragnarök');await select('God of War Ragnarök');await officialLoaded('playstation-tw');
  check('God of War official key art is available',await page.locator('#game-official-gallery [data-provider="playstation-tw"][data-kind="keyArt"]').count()===1);
  await page.locator('#game-favorites [data-key="sgdb-5454475"] .game-my-select').click();
  await open('ps5');
  await page.waitForFunction(()=>document.querySelector('#game-current-title').textContent.includes('宇宙機器人'));
  check('PlayStation favorite survives restart',await page.locator('#game-favorites [data-key="sgdb-5454475"]').count()===1);
  await open('unknown');await search('王國之淚');await select('The Legend of Zelda: Tears of the Kingdom');
  check('unknown HDMI source never injects Nintendo art',await page.locator('#game-official-gallery .game-cover').count()===0);
  check('unknown HDMI source has prominent guidance',await page.locator('#game-source-guidance').isVisible()&&await page.locator('[data-i18n="game.chooseSourceHelp"]').isVisible());
  await page.selectOption('#game-preview-data-mode','fixture');
  await search('薩爾達');check('fixture mode remains usable',await page.locator('.game-result').count()>=8);
  check('no browser JavaScript exceptions',errors.length===0);
  evidence.errors=errors;fs.writeFileSync(path.join(output,'artwork-integration-checks.json'),JSON.stringify(evidence,null,2));
  console.log('PASS '+evidence.checks.length+' browser integration checks; '+evidence.zeldaResults.length+' live Zelda results');
 }finally{
  if(browser)await browser.close();
  profiles.forEach((file,i)=>{if(backups[i]!==null)fs.writeFileSync(file,backups[i]);else if(fs.existsSync(file))fs.unlinkSync(file);});
 }
})().catch(error=>{console.error(error);process.exitCode=1;});
