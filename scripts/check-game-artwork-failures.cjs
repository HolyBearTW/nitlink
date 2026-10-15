// Real catalogue with one explicitly injected image-request failure.
// This validates failure/retry behavior, not native Windows or Discord rendering.
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const seed=require('../official-artwork.json').games[0],official=seed.artworkSources[0];
const profile=path.resolve(__dirname,'../out/game-preview-api/real-library-checks.json');
const saved=()=>JSON.parse(fs.readFileSync(profile,'utf8'));
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1440,height:1400}}),errors=[];
  page.on('pageerror',error=>errors.push(String(error)));
  await page.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks',{waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>document.querySelectorAll('.game-result').length===28,{},{timeout:20000});
  await page.locator('.game-result').filter({hasText:seed.name}).click();
  const before=saved().currentStartUnix;
  let fail=true;
  await page.route('**/preview/bridge.js?*',async route=>{
   const query=new URL(route.request().url()).searchParams;
   if(fail&&query.get('operation')==='image'&&query.get('url')===official.url){
    await route.fulfill({status:200,contentType:'text/javascript',body:`window.NitLinkPreviewBridge.deliver(${Number(query.get('bridgeId'))},{"status":"failed","image":""});`});return;
   }
   await route.continue();
  });
  await page.locator('#game-cover-retry').click();
  await page.waitForFunction(()=>document.querySelector('#game-official-gallery .game-art')?.dataset.imageState==='failed');
  assert.equal(await page.locator('#game-official-gallery .game-cover').isDisabled(),true);
  assert.equal(await page.locator('#game-official-section').isVisible(),false);
  assert((await page.locator('#game-cover-status').textContent()).includes('圖片不可用'));
  await page.evaluate(value=>window.chrome.webview.postMessage({action:'selectGameCover',value}),{gameId:seed.id,coverId:official.id});
  await page.waitForFunction(()=>document.querySelector('#game-notice').textContent.includes('圖片載入失敗'));
  assert.equal(saved().records['sgdb-'+seed.id].coverUrl,official.url,'A failed image never deletes the saved selection');
  assert((await page.locator('#game-discord-image-status').textContent()).includes('尚未通過載入驗證'));
  assert((await page.locator('#game-discord-art img').getAttribute('src')).endsWith('assets/nitlink.svg'));
  fail=false;await page.locator('#game-cover-retry').click();
  await page.locator('#game-official-gallery .game-cover').click();
  await page.waitForFunction(()=>document.querySelector('#game-current-art img')?.getAttribute('src')===document.querySelector('#game-discord-art img')?.getAttribute('src'));
  await page.locator('#game-gallery .game-cover:enabled:visible').first().click();
  await page.waitForFunction(()=>document.getElementById('game-current-artwork-tag').hidden);
  await page.locator('#game-auto').click();
  await page.waitForFunction(()=>!document.getElementById('game-current-artwork-tag').hidden&&document.querySelector('#game-current-art img')?.naturalWidth>0);
  assert.equal(saved().currentStartUnix,before);
  await page.waitForFunction(()=>document.getElementById('game-cover-mode').textContent==='自動選圖');
  assert.equal(saved().records['sgdb-'+seed.id].provider,'nintendo-tw');
  assert.equal(saved().records['sgdb-'+seed.id].manualCover,false);
  assert.equal(errors.length,0,errors.join('\n'));
  fs.writeFileSync(path.resolve(__dirname,'../out/game-preview/artwork-failure-checks.json'),JSON.stringify({source:'Real Worker + official publisher mapping; one deliberate image-response failure',checks:['failed image hidden','selection disabled','bridge rejects failed cover','saved URL retained','explicit NitLink fallback reason','retry restores chosen image','current and Discord preview synchronized','automatic choice restores official priority','activity timestamp preserved'],errors},null,2));
  console.log('Real artwork failure, fallback, retry and automatic-selection checks passed.');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
