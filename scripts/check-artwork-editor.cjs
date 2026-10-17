// Actual UI + real Worker + durable preview-host PNG output. No native build.
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const {installExactDocument}=require('./preview-browser.cjs');
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const root=path.resolve(__dirname,'..'),profile=path.join(root,'out/game-preview-api/real-library-checks.json');
const saved=()=>JSON.parse(fs.readFileSync(profile,'utf8'));
const record=()=>saved().records['sgdb-5267007'];
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1440,height:1400}}),errors=[],evidence=[];
  await installExactDocument(page);
  page.on('pageerror',error=>errors.push(String(error)));
  await page.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks',{waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>document.querySelectorAll('.game-result').length===28,{},{timeout:25000});
  await page.locator('.game-result').filter({hasText:'Hyrule Warriors: Age of Calamity'}).click();
  await page.waitForFunction(()=>document.getElementById('game-current-title').textContent.includes('災厄啟示錄'));
  await page.locator('#game-cover-retry').click();
  await page.waitForFunction(()=>document.querySelectorAll('#game-official-gallery .game-cover').length===2);
  await page.locator('#game-official-gallery .game-cover').first().click();
  const start=saved().currentStartUnix;
  await page.locator('#game-adjust-cover').click();
  await page.waitForFunction(()=>!document.getElementById('game-editor-save').disabled);
  assert.equal(await page.locator('#game-editor-canvas').getAttribute('data-mode'),'fit');
  await page.locator('#game-artwork-editor').screenshot({path:path.join(root,'out/game-preview/editor-fit.png')});
  await page.locator('#game-editor-save').click();
  await page.waitForFunction(()=>!document.getElementById('game-artwork-editor').open);
  assert.equal(record().artworkMode,'fit');assert.match(record().finalImageKey,/^[a-f0-9]{64}$/);
  const fit=record().finalImageKey;
  assert(fs.statSync(path.join(root,'out/game-preview-api/generated',fit+'.png')).size>1000);
  assert((await page.locator('#game-current-artwork-tag').textContent()).includes('由官方素材製作'));
  assert((await page.locator('#game-discord-image-status').textContent()).includes('尚不能傳給 Discord'));
  const currentSrc=await page.locator('#game-current-art img').getAttribute('src');assert.equal(await page.locator('#game-discord-art img').getAttribute('src'),currentSrc);
  assert.notEqual(await page.locator('#game-discord-original-art img').getAttribute('src'),currentSrc,'Original RPC candidate is separate');
  await page.locator('#game-adjust-cover').click();await page.waitForFunction(()=>!document.getElementById('game-editor-save').disabled);
  await page.locator('[data-artwork-mode="crop"]').click();
  await page.locator('#game-editor-reset').click();
  await page.locator('#game-editor-zoom').fill('1.35');
  const box=await page.locator('#game-editor-canvas').boundingBox();
  await page.mouse.move(box.x+box.width/2,box.y+box.height/2);await page.mouse.down();await page.mouse.move(box.x+box.width/2+35,box.y+box.height/2-45,{steps:6});await page.mouse.up();
  const crop=await page.locator('#game-editor-canvas').evaluate(canvas=>({x:Number(canvas.dataset.cropX),y:Number(canvas.dataset.cropY),zoom:Number(canvas.dataset.zoom)}));
  assert(crop.x!==.5||crop.y!==.5);assert.equal(crop.zoom,1.35);
  assert.equal(await page.locator('#game-editor-84').getAttribute('width'),'84');assert.equal(await page.locator('#game-editor-64').getAttribute('width'),'64');
  await page.locator('#game-artwork-editor').screenshot({path:path.join(root,'out/game-preview/editor-crop.png')});
  await page.locator('#game-editor-save').click();await page.waitForFunction(()=>!document.getElementById('game-artwork-editor').open);
  assert.equal(record().artworkMode,'crop');assert.notEqual(record().finalImageKey,fit);assert.equal(record().cropX,crop.x);assert.equal(record().cropY,crop.y);
  const cropped=record().finalImageKey;
  await page.locator('.game-result').filter({hasText:'The Legend of Zelda: Breath of the Wild'}).click();
  await page.locator('.game-result').filter({hasText:'Hyrule Warriors: Age of Calamity'}).click();
  await page.waitForFunction(()=>document.querySelector('#game-current-art img')?.naturalWidth===512);
  assert.equal(await page.locator('#game-current-art img').getAttribute('src'),'preview/generated-images/'+cropped+'.png');
  await page.reload({waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>document.querySelector('#game-current-art img')?.naturalWidth===512);
  assert.equal(record().finalImageKey,cropped);assert.equal(record().artworkMode,'crop');
  const afterSwitch=saved().currentStartUnix;
  for(const locale of ['en-US','zh-CN','zh-TW','de-DE']){
   await page.selectOption('#game-preview-language',locale);await page.locator('#game-adjust-cover').click();await page.waitForFunction(()=>!document.getElementById('game-editor-save').disabled);
   assert.equal(await page.locator('#game-artwork-editor').evaluate(dialog=>dialog.scrollWidth>dialog.clientWidth),false);
   assert(!(await page.locator('#game-artwork-editor').textContent()).includes('game.edit.'));
   await page.locator('#game-editor-close').click();
  }
  await page.selectOption('#game-preview-layout','right');await page.locator('#game-adjust-cover').click();await page.waitForFunction(()=>!document.getElementById('game-editor-save').disabled);
  const bounds=await page.locator('#game-artwork-editor').boundingBox();assert(bounds.width<=420);
  await page.locator('#game-artwork-editor').screenshot({path:path.join(root,'out/game-preview/editor-docked.png')});
  await page.locator('[data-artwork-mode="original"]').click();await page.locator('#game-editor-save').click();await page.waitForFunction(()=>!document.getElementById('game-artwork-editor').open);
  assert.equal(record().artworkMode,'original');assert(record().sourceImageKey);assert.equal(saved().currentStartUnix,afterSwitch);
  await page.locator('#game-adjust-cover').click();await page.waitForFunction(()=>!document.getElementById('game-editor-save').disabled);await page.locator('[data-artwork-mode="fit"]').click();await page.locator('#game-editor-save').click();await page.waitForFunction(()=>!document.getElementById('game-artwork-editor').open);
  await page.selectOption('#game-preview-layout','full');await page.setViewportSize({width:1440,height:1700});await page.locator('.game-column').last().screenshot({path:path.join(root,'out/game-preview/adjusted-cover-full.png')});
  assert.equal(errors.length,0,errors.join('\n'));
  const offline=await browser.newPage({viewport:{width:900,height:1000}});
  await installExactDocument(offline);
  let imageRequests=0;
  await offline.route('**/preview/bridge.js?*',async route=>{
    const params=new URL(route.request().url()).searchParams;
    if(['search','covers','image'].includes(params.get('operation'))){if(params.get('operation')==='image')++imageRequests;await route.abort();return;}
    await route.continue();
  });
  await offline.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks',{waitUntil:'domcontentloaded'});
  await offline.waitForFunction(()=>document.querySelector('#game-current-art img')?.naturalWidth===512);
  await offline.locator('#game-adjust-cover').click();await offline.waitForFunction(()=>!document.getElementById('game-editor-save').disabled);
  await offline.locator('[data-artwork-mode="fit"]').click();await offline.locator('#game-editor-save').click();await offline.waitForFunction(()=>!document.getElementById('game-artwork-editor').open);
  assert.equal(imageRequests,0,'Restore and editing saved art require no source download');await offline.close();
  evidence.push({fit,cropped,crop,originalFile:record().sourceImageKey,initialStart:start,afterGameSwitch:afterSwitch,checks:['two verified official aspect ratios','real 512px PNG files','fit and crop','dragging and zoom','84px and 64px preview','explicit local-only Discord status','separate original RPC candidate','per-game restore','restart restore','offline restore and editing without original URL','original mode','three languages','420px editor','no timer reset while editing']});
  fs.writeFileSync(path.join(root,'out/game-preview/artwork-editor-checks.json'),JSON.stringify({evidence,errors},null,2));
  console.log('Artwork editor, actual PNG output, restore, Discord boundary and responsive checks passed.');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
