// Live official-store / actual-UI checks only. No Windows build or CTest.
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const {installExactDocument}=require('./preview-browser.cjs');
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..'),profile=path.join(root,'out/game-preview-api/real-library-checks.json');
(async()=>{
 const backup=fs.existsSync(profile)?fs.readFileSync(profile):null;
 const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1500,height:1250}}),errors=[],evidence={};
  await installExactDocument(page);page.on('pageerror',error=>errors.push(String(error)));
  await page.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks',{waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>document.querySelectorAll('.game-result').length>=28,{},{timeout:25000});
  await page.locator('.game-result').filter({hasText:'The Legend of Zelda: Breath of the Wild'}).click();
  await page.waitForFunction(()=>document.getElementById('game-current-title').textContent.includes('曠野之息'));
  await page.locator('#game-cover-retry').click();
  const hk=page.locator('#game-official-gallery .game-cover[data-provider="nintendo-hk-store"]');
  await hk.first().waitFor({timeout:30000});await hk.first().locator('.game-art[data-image-state="ok"]').waitFor({timeout:20000});
  assert.equal(await page.locator('#game-official-gallery .game-cover').first().getAttribute('data-provider'),'nintendo-hk-store');
  const stamp=JSON.parse(fs.readFileSync(profile)).currentStartUnix,title=await page.locator('#game-current-title').textContent();
  await hk.first().click();
  await page.waitForFunction(()=>document.getElementById('game-current-artwork-tag').textContent.includes('香港任天堂商店'));
  await page.waitForFunction(()=>document.getElementById('game-notice').textContent.includes('封面'));
  let saved=JSON.parse(fs.readFileSync(profile));assert.equal(saved.currentStartUnix,stamp);assert.equal(saved.records[saved.currentKey].provider,'nintendo-hk-store');
  assert.equal(new URL(saved.records[saved.currentKey].coverUrl).hostname,'store.nintendo.com.hk');
  assert.equal(await page.locator('#game-current-title').textContent(),title);
  await page.locator('#game-official-gallery .game-cover-info').first().click();
  const details=await page.locator('#game-cover-detail-fields').textContent();assert(details.includes('Official Nintendo Hong Kong Store')&&details.includes('未知')&&details.includes('曠野之息'));
  await page.locator('#game-cover-details-close').click();evidence.details=details;
  await page.locator('[data-cover-provider="nintendo"]').click();
  await page.locator('#game-official-query').fill('瑪利歐賽車');await page.locator('#game-official-search').click();
  await page.waitForFunction(()=>[...document.querySelectorAll('.game-cover-caption')].some(el=>el.textContent.includes('瑪利歐賽車')),{},{timeout:30000});
  evidence.mario=await page.locator('.game-cover-caption').allTextContents();
  await page.locator('#game-official-query').fill('Minecraft');await page.locator('#game-official-search').click();
  await page.waitForFunction(()=>[...document.querySelectorAll('.game-cover-caption')].some(el=>el.textContent.includes('Minecraft')),{},{timeout:30000});
  evidence.thirdParty=await page.locator('.game-cover-caption').allTextContents();
  await page.locator('#game-official-query').fill('');await page.locator('#game-official-search').click();
  await page.waitForFunction(()=>document.querySelectorAll('#game-official-gallery .game-cover').length===24,{},{timeout:30000});
  assert.equal(await page.locator('#game-cover-next').isEnabled(),true);
  evidence.totalLabel=await page.locator('#game-cover-page').textContent();
  const ids=await page.locator('.game-cover').evaluateAll(cards=>cards.map(card=>card.dataset.coverId));
  await page.locator('#game-cover-next').click();
  await page.waitForFunction(first=>document.getElementById('game-cover-page').textContent.includes('第 2 頁')&&document.querySelectorAll('#game-official-gallery .game-cover').length===24&&document.querySelector('#game-official-gallery .game-cover')?.dataset.coverId!==first,ids[0],{timeout:30000});
  const next=await page.locator('.game-cover').evaluateAll(cards=>cards.map(card=>card.dataset.coverId));assert.notDeepEqual(ids,next);evidence.pages=[ids.length,next.length];
  for(const shape of ['square','portrait','all']){
   await page.locator('#game-shape-'+shape).click();
   await page.waitForFunction(()=>document.getElementById('game-gallery').getAttribute('aria-busy')==='false'&&document.querySelectorAll('#game-official-gallery .game-cover').length===24,{},{timeout:30000});
   await page.waitForFunction(()=>document.querySelectorAll('#game-official-gallery .game-art[data-image-state="loading"]').length===0,{},{timeout:30000});
   if(shape!=='all')for(const art of await page.locator('#game-official-gallery .game-cover-tile:visible img').all()){
    const size=await art.evaluate(el=>[el.naturalWidth,el.naturalHeight]);assert(shape==='square'?size[0]===size[1]:size[1]>size[0]);
   }
  }
  for(const locale of ['zh-TW','zh-CN','en-US']){
   await page.selectOption('#game-preview-language',locale);await page.waitForFunction(locale=>document.documentElement.lang===locale,locale);
   assert.equal(await page.locator('[data-cover-provider="nintendo"]').textContent(),locale==='en-US'?'Nintendo Hong Kong Store':'香港任天堂商店');
   await page.selectOption('#game-preview-layout','right');assert.equal(await page.locator('#pane-games').evaluate(el=>el.scrollWidth>el.clientWidth),false);
  }
  await page.selectOption('#game-preview-language','zh-TW');await page.selectOption('#game-preview-layout','full');
  await page.locator('#game-official-query').fill('曠野之息');await page.locator('#game-official-search').click();
  await page.waitForFunction(()=>[...document.querySelectorAll('.game-cover-caption')].some(el=>el.textContent.includes('曠野之息')),{},{timeout:30000});
  await page.waitForFunction(()=>document.querySelector('#game-official-gallery img')?.naturalWidth>0,{},{timeout:20000});
  await page.waitForFunction(()=>[...document.querySelectorAll('#game-official-gallery .game-art')].every(art=>art.dataset.imageState!=='loading'),{},{timeout:20000});
  const panel=page.locator('#game-official-gallery').locator('xpath=ancestor::article[1]');await panel.screenshot({path:path.join(root,'out/game-preview/hk-store-full.png')});
  await page.selectOption('#game-preview-layout','right');await panel.screenshot({path:path.join(root,'out/game-preview/hk-store-docked.png')});
  await page.locator('#game-adjust-cover').click();
  await page.waitForFunction(()=>document.getElementById('game-artwork-editor').open&&document.getElementById('game-editor-status').textContent==='');
  await page.locator('#game-editor-save').click();await page.waitForFunction(()=>!document.getElementById('game-artwork-editor').open,{},{timeout:20000});
  const generated=JSON.parse(fs.readFileSync(profile)).records['sgdb-35744'].finalImageKey;
  assert.match(generated,/^[a-f0-9]{64}$/);const png=fs.readFileSync(path.join(root,'out/game-preview-api/generated',generated+'.png'));
  assert.equal(png.readUInt32BE(16),512);assert.equal(png.readUInt32BE(20),512);evidence.generatedFit=generated;
  await page.reload({waitUntil:'domcontentloaded'});await page.waitForFunction(key=>document.querySelector('#game-current-art img')?.getAttribute('src')?.includes(key)&&document.querySelector('#game-current-art img')?.naturalWidth>0,generated,{timeout:20000});
  saved=JSON.parse(fs.readFileSync(profile));assert.equal(saved.currentStartUnix,stamp);assert.equal(saved.records[saved.currentKey].provider,'nintendo-hk-store');
  assert.deepEqual(errors,[]);evidence.errors=errors;evidence.checks=['general live catalogue, no seed list','HK priority','third-party games','real pagination','actual-image shape filters','source and unknown-language details','manual selection and restart persistence','unchanged game title and timer','three languages and Docked'];
  fs.writeFileSync(path.join(root,'out/game-preview/hk-store-checks.json'),JSON.stringify(evidence,null,2));console.log(JSON.stringify(evidence,null,2));
 }finally{await browser.close();if(backup)fs.writeFileSync(profile,backup);}
})().catch(error=>{console.error(error);process.exitCode=1;});
