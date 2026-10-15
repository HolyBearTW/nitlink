// Real Worker / controlled-image regression check. No Windows build or CTest.
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const {installExactDocument}=require('./preview-browser.cjs');
const assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
(async()=>{
  const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
  try{
    const page=await browser.newPage({viewport:{width:1440,height:1200}}),errors=[],requested=new Set();
    await installExactDocument(page);
    page.on('pageerror',error=>errors.push(String(error)));
    page.on('request',request=>{const url=new URL(request.url());if(url.searchParams.get('operation')==='covers')requested.add(Number(url.searchParams.get('gameId')));});
    await page.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks',{waitUntil:'domcontentloaded'});
    await page.waitForFunction(()=>document.querySelectorAll('.game-result').length>=28,{},{timeout:25000});
    const titles=await page.locator('.game-result-title').allTextContents();
    const totk=titles.indexOf('薩爾達傳說 王國之淚');
    assert(totk>=0&&totk<titles.indexOf('Zelda Classic'),'Provider supplement participates in title relevance');
    await page.locator('.game-result').filter({hasText:'The Legend of Zelda: Tears of the Kingdom'}).locator('.game-art[data-image-state="ok"] img').waitFor({timeout:20000});
    await page.waitForFunction(()=>[...document.querySelectorAll('#game-results .game-art')].every(art=>['ok','failed','missing'].includes(art.dataset.imageState)),{},{timeout:120000});
    const rows=await page.locator('.game-result').evaluateAll(cards=>cards.map(card=>({name:card.querySelector('.game-result-title').textContent,state:card.querySelector('.game-art').dataset.imageState,label:card.querySelector('.game-art').dataset.imageLabel,loaded:!!card.querySelector('img')?.naturalWidth})));
    assert(rows.slice(12).some(row=>row.loaded),'Thumbnails after result twelve must actually load');
    assert(rows.find(row=>row.name==='薩爾達傳說 王國之淚').loaded,'Real official TOTK thumbnail');
    assert(requested.size>12,'The bridge processes titles beyond the old cap');
    const panel=page.locator('#game-results').locator('xpath=ancestor::article[1]');
    await page.locator('.game-result').filter({hasText:'The Legend of Zelda: Tears of the Kingdom'}).scrollIntoViewIfNeeded();
    await panel.screenshot({path:path.resolve(__dirname,'../out/game-preview/search-thumbnails-full.png')});
    await page.selectOption('#game-preview-layout','right');
    assert.equal(await page.locator('#pane-games').evaluate(el=>el.scrollWidth>el.clientWidth),false);
    await panel.screenshot({path:path.resolve(__dirname,'../out/game-preview/search-thumbnails-docked.png')});
    assert.deepEqual(errors,[]);
    const evidence={totkPosition:totk+1,total:rows.length,requestedGames:requested.size,loaded:rows.filter(row=>row.loaded).length,rows,errors};
    fs.writeFileSync(path.resolve(__dirname,'../out/game-preview/search-thumbnail-checks.json'),JSON.stringify(evidence,null,2));
    console.log(JSON.stringify(evidence,null,2));
  }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
