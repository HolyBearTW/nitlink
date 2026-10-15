// Focused browser/HTTP checks only; never compiles Windows code or invokes CTest.
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const seeds=require('../official-artwork.json').games;
const out=path.resolve(__dirname,'../out/game-preview');
const url='http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks';
const profile=path.resolve(__dirname,'../out/game-preview-api/real-library-checks.json');
const {installExactDocument}=require('./preview-browser.cjs');
(async()=>{
  const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
  try{
    const page=await browser.newPage({viewport:{width:1440,height:1600}}),errors=[],evidence=[];
    await installExactDocument(page);
    page.on('pageerror',error=>errors.push(String(error)));
    await page.goto(url,{waitUntil:'commit'});
    await page.waitForFunction(()=>document.querySelectorAll('.game-result').length===28,{},{timeout:25000});
    await page.evaluate(()=>{
      window._artworkActions=[];
      const bridge=window.chrome.webview,post=bridge.postMessage;
      bridge.postMessage=message=>{window._artworkActions.push(message);return post(message);};
      bridge.addEventListener('message',event=>{if(event.data.gameLibrary)window._lastArtworkLibrary=structuredClone(event.data.gameLibrary);});
    });
    for(const seed of seeds){
      await page.locator('.game-result').filter({hasText:seed.name}).click();
      await page.waitForFunction(id=>document.querySelector(`.game-result[aria-pressed="true"]`)?.textContent.includes(id),seed.name);
      await page.locator('#game-cover-retry').click();
      await page.waitForSelector('#game-official-gallery .game-cover');
      await page.waitForFunction(()=>document.querySelector('#game-official-gallery img')?.complete&&document.querySelector('#game-official-gallery img')?.naturalWidth>0,{},{timeout:20000});
      await page.locator('#game-official-gallery .game-cover').first().click();
      await page.waitForFunction(()=>!document.getElementById('game-current-artwork-tag').hidden);
      assert.equal(await page.locator('#game-official-gallery .game-cover').first().textContent(),'','Image-only cover button');
      assert.equal(await page.locator('#game-official-gallery .game-cover').first().getAttribute('aria-pressed'),'true');
      await page.waitForFunction(()=>document.querySelector('#game-discord-art img')?.getAttribute('src')===document.querySelector('#game-current-art img')?.getAttribute('src'));
      assert((await page.locator('#game-discord-image-status').textContent()).includes('尚未驗證'));
      assert(!(await page.locator('#game-discord-title').textContent()).includes('我的收藏'));
      await page.locator('#game-official-gallery .game-cover-info').first().click();
      const details=await page.locator('#game-cover-detail-fields').textContent();
      assert(details.includes('Official Nintendo Taiwan')&&details.includes('繁中')&&details.includes('TW'));
      assert(details.includes(seed.artworkSources[0].kind==='logo'?'非完整封面':'盒裝封面'));
      await page.locator('#game-cover-details-close').click();
      const imageDimensions=await page.locator('#game-official-gallery img').first().evaluate(img=>({width:img.naturalWidth,height:img.naturalHeight}));
      assert.equal(imageDimensions.height>imageDimensions.width,seed.artworkSources[0].shape==='portrait');
      assert((await page.locator('#game-official-section').boundingBox()).height<160,'A single official image stays compact');
      const activityStart=JSON.parse(fs.readFileSync(profile,'utf8')).currentStartUnix;
      await page.locator('#game-shape-square').click();
      await page.waitForFunction(()=>document.getElementById('game-cover-status').textContent.indexOf('載入中')<0);
      assert.equal(await page.locator('#game-official-section').isVisible(),false,'Portrait and landscape official art are excluded from squares');
      await page.locator('#game-shape-portrait').click();
      await page.waitForFunction(()=>document.getElementById('game-cover-status').textContent.indexOf('載入中')<0);
      assert.equal(await page.locator('#game-official-section').isVisible(),seed.artworkSources[0].shape==='portrait');
      await page.locator('#game-shape-all').click();
      await page.waitForFunction(()=>document.querySelector('#game-official-gallery img')?.complete&&document.querySelector('#game-official-gallery img')?.naturalWidth>0);
      assert.equal(JSON.parse(fs.readFileSync(profile,'utf8')).currentStartUnix,activityStart,'Shape changes preserve timing');
      if((await page.locator('#game-current-favorite').textContent()).includes('加入'))await page.locator('#game-current-favorite').click();
      await page.waitForSelector(`.game-my-card[data-key="sgdb-${seed.id}"]`);
      await page.evaluate(()=>{
        window._imageRefs=[...document.querySelectorAll('#game-favorites img,#game-results img,#game-official-gallery img')].filter(image=>image.complete&&image.naturalWidth>0);
        window._imageCount=window._artworkActions.filter(message=>message.action==='getGameCovers').length;
        const library=structuredClone(window._lastArtworkLibrary);library.games[0].lastUsedUtc+=1;window._updateGameLibrary(library);
      });
      await page.locator(`.game-my-card[data-key="sgdb-${seed.id}"] .game-my-select`).click();
      assert.equal(await page.evaluate(()=>window._imageRefs.every(image=>image.isConnected)),true,'Library sync and reselect keep loaded image nodes');
      assert.equal(await page.evaluate(()=>window._artworkActions.filter(message=>message.action==='getGameCovers').length===window._imageCount),true,'Reselect does not request the gallery again');
      await page.waitForFunction(()=>![...document.querySelectorAll('#game-gallery .game-art')].some(art=>art.dataset.imageState==='loading'),{},{timeout:25000});
      const geometry=await page.locator('#game-gallery').evaluate(gallery=>({columns:getComputedStyle(gallery).gridTemplateColumns.split(' ').length,overflow:gallery.scrollWidth>gallery.clientWidth}));
      assert(geometry.columns>=5&&geometry.columns<=6);assert.equal(geometry.overflow,false);
      await page.evaluate(()=>{document.querySelector('main').scrollTop=0;document.querySelector('#game-results').scrollTop=0;});
      await page.locator('.game-column').last().screenshot({path:path.join(out,`nintendo-tw-${seed.id}-full.png`)});
      await page.selectOption('#game-preview-layout','right');
      const dock=await page.locator('#game-gallery').evaluate(gallery=>({columns:getComputedStyle(gallery).gridTemplateColumns.split(' ').length,overflow:document.getElementById('pane-games').scrollWidth>document.getElementById('pane-games').clientWidth}));
      assert(dock.columns>=3&&dock.columns<=5);assert.equal(dock.overflow,false);
      if(seed.id===5267007){await page.locator('.game-column').last().screenshot({path:path.join(out,'nintendo-tw-docked.png')});}
      await page.selectOption('#game-preview-layout','full');
      evidence.push({id:seed.id,url:seed.artworkSources[0].url,details,geometry,dock});
    }
    await page.reload({waitUntil:'commit'});
    await page.waitForFunction(()=>document.getElementById('game-current-artwork-tag')?.hidden===false,{},{timeout:20000});
    const saved=JSON.parse(fs.readFileSync(profile,'utf8'));
    for(const seed of seeds){const game=saved.records['sgdb-'+seed.id];assert.equal(game.provider,'nintendo-tw');assert.equal(game.isOfficial,true);assert.equal(game.language,'zh-TW');assert.equal(game.region,'TW');assert.equal(game.coverUrl,seed.artworkSources[0].url);assert.equal(game.manualCover,true);}
    await page.locator('.game-result').filter({hasText:'薩爾達傳說 曠野之息'}).click();
    await page.waitForFunction(()=>document.getElementById('game-current-title').textContent.includes('曠野之息'));
    await page.locator('#game-shape-square').click();
    await page.locator('#game-cover-retry').click();
    await page.waitForFunction(()=>document.querySelectorAll('#game-gallery .game-cover').length===29,{},{timeout:20000});
    assert.equal(await page.locator('#game-cover-pagination').isVisible(),false,'Single page hides pagination');
    const delayed=await browser.newPage({viewport:{width:1440,height:1100}});let hold=false;const release=[];
    await installExactDocument(delayed);
    await delayed.route('**/preview/bridge.js?*',async route=>{
      const operation=new URL(route.request().url()).searchParams.get('operation');
      if(operation==='search')hold=true;
      if(operation==='image'&&hold)await new Promise(resolve=>release.push(resolve));
      await route.continue().catch(()=>{});
    });
    await delayed.goto(url,{waitUntil:'commit'});
    await delayed.waitForFunction(()=>document.querySelectorAll('.game-result').length===28,{},{timeout:25000});
    await delayed.locator('.game-result').filter({hasText:'薩爾達傳說 曠野之息'}).click();
    await delayed.waitForFunction(()=>document.getElementById('game-current-title').textContent.includes('曠野之息'));
    await delayed.locator('#game-shape-square').click();
    assert.equal(await delayed.locator('#game-results .game-art[data-image-state="loading"]').count(),0,'Cancelling search enrichment settles its placeholders');
    await delayed.locator('#game-cover-retry').click();
    await delayed.waitForFunction(()=>document.querySelectorAll('#game-gallery .game-cover').length===29,{},{timeout:20000});
    assert((await delayed.locator('#game-gallery .game-art[data-image-state="loading"]').count())>0);
    await delayed.locator('#game-query').fill('薩爾達無雙');
    assert.equal(await delayed.locator('#game-gallery .game-art[data-image-state="loading"]').count(),0,'Typing a new search settles cancelled gallery images');
    hold=false;release.splice(0).forEach(resolve=>resolve());await delayed.unroute('**/preview/bridge.js?*');
    await delayed.close();
    assert.equal(errors.length,0,errors.join('\n'));
    fs.writeFileSync(path.join(out,'official-artwork-checks.json'),JSON.stringify({evidence,errors,checks:['three genuine publisher images','distinct official catalogue supplement','box art vs logo','image-only gallery','details attribution','manual source persistence','no reselect reload','stable image DOM during library sync','cancelled slow images settle','5-6 wide columns','compact responsive gallery','single-page controls hidden','current and Discord art synchronized','client rendering explicitly unverified','shape filters also cover official art','no favorite title suffix','shape changes preserve timestamp']},null,2));
    console.log('Nintendo Taiwan artwork and loading checks passed. Evidence:',out);
  }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
