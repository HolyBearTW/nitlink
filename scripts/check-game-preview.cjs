// Lightweight browser checks only. Does not build NitLink or invoke CTest.
const {chromium} = require('../out/ui-preview-runtime/node_modules/playwright-core');
const fs = require('node:fs');
const path = require('node:path');
const assert = require('node:assert/strict');
const {installExactDocument}=require('./preview-browser.cjs');
(async () => {
  const browser = await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
  try {
  const page = await browser.newPage({viewport:{width:1440,height:1000}});
  await installExactDocument(page);
  const errors = [];
  page.on('pageerror', error=>errors.push(String(error)));
  page.on('console', message=>{if(message.type()==='error' && !message.text().includes('404')) errors.push(message.text());});
  const output = path.resolve(__dirname,'../out/game-preview'); fs.mkdirSync(output,{recursive:true});
  await page.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games&mode=fixture&profile=checks',{waitUntil:'commit'});
  await page.waitForSelector('.game-result',{timeout:10000}).catch(async error=>{console.log('Fixture startup:',await page.evaluate(()=>({url:location.href,ready:document.readyState,mode:window.NitLinkPreview,query:document.querySelector('#game-query')?.value,status:document.querySelector('#game-search-status')?.textContent,pane:document.querySelector('#pane-games')?.dataset.active,count:document.querySelectorAll('.game-result').length,banner:document.querySelector('#game-fixture-banner')?.hidden})));throw error;});
  await page.locator('.game-result').first().click();
  await page.waitForFunction(()=>document.getElementById('game-current-title').textContent.includes('曠野之息'));
  assert.equal(await page.locator('#game-shape-all').getAttribute('aria-pressed'),'true','all cover shapes are selected by default');
  assert.equal(await page.locator('#game-gallery').getAttribute('data-shape'),'all','default cover request displays every orientation');
  await page.locator('#game-cover-retry').click();
  await page.waitForSelector('.game-cover');
  await page.waitForFunction(()=>document.querySelector('#game-gallery img')?.complete);
  await page.locator('.game-cover').nth(1).click();
  const profile=path.resolve(__dirname,'../out/game-preview-api/fixture-library-checks.json');
  const timerBefore = JSON.parse(fs.readFileSync(profile,'utf8')).currentStartUnix;
  const start = await page.evaluate(()=>document.getElementById('game-discord-time').textContent);
  assert.match(await page.locator('#game-cover-mode').textContent(),/手動/);
  assert.equal(Math.round((await page.locator('#game-discord-art').boundingBox()).width),84,'Discord activity artwork remains 84px wide');
  await page.screenshot({path:path.join(output,'full-zh-TW.png')});
  const geometry = [], locales = ['zh-TW','zh-CN','en-US','de-DE'];
  for (const locale of locales) {
    await page.selectOption('#game-preview-language',locale);
    await page.waitForFunction(locale=>document.documentElement.lang===locale,locale);
    assert.equal(await page.locator('#game-query').getAttribute('placeholder'),await page.evaluate(()=>window.NitLinkLocales[document.documentElement.lang]['game.searchPlaceholder']),`${locale} game search placeholder`);
    for(const layout of ['full','left','right']) {
      await page.selectOption('#game-preview-layout',layout);
      await page.waitForTimeout(100);
      const result = await page.evaluate(()=>{
        const app=document.querySelector('.app').getBoundingClientRect();
        const pane=document.querySelector('#pane-games');
        const buttons=[...document.querySelectorAll('.rail-item')].map(el=>({text:el.textContent.trim(),rect:el.getBoundingClientRect().toJSON()}));
        return {width:app.width,overflow:pane.scrollWidth>pane.clientWidth,columns:getComputedStyle(document.querySelector('.game-layout')).gridTemplateColumns,
          clipped:buttons.filter(b=>b.rect.left<app.left-1||b.rect.right>app.right+1).map(b=>b.text)};
      });
      assert.equal(result.overflow,false,`${locale}/${layout} horizontal overflow`);
      assert.equal(result.clipped.length,0,`${locale}/${layout} navigation clipped`);
      geometry.push({locale,layout,...result});
      if(locale==='zh-TW' && layout==='right') await page.locator('.app').screenshot({path:path.join(output,'docked-zh-TW.png')});
    }
  }
  const aboutGeometry = [];
  for (const locale of locales) {
    await page.selectOption('#game-preview-language',locale);
    for (const layout of ['full','left','right']) {
      await page.selectOption('#game-preview-layout',layout);
      await page.locator('[data-tab="pane-about"]').click();
      const result = await page.evaluate(()=>{
        const strings=window.NitLinkLocales[document.documentElement.lang],pane=document.querySelector('#pane-about'),main=document.querySelector('main');
        return {side:document.documentElement.dataset.side,technical:document.querySelector('#about-details-summary').textContent,
          contributors:document.querySelector('#about-contributors-summary').textContent,
          noka:document.querySelector('[data-i18n="about.noka123Contribution"]').textContent,
          expected:{technical:strings['about.technicalInformation'],contributors:strings['about.contributors'],noka:strings['about.noka123Contribution']},
          overflow:pane.scrollWidth>pane.clientWidth||main.scrollWidth>main.clientWidth||document.documentElement.scrollWidth>innerWidth};
      });
      assert.equal(result.side,layout,`${locale}/${layout} About layout applied`);
      assert.equal(result.technical,result.expected.technical,`${locale}/${layout} technical label localized`);
      assert.equal(result.contributors,result.expected.contributors,`${locale}/${layout} contributors label localized`);
      assert.equal(result.noka,result.expected.noka,`${locale}/${layout} German contributor credit localized`);
      assert.equal(result.overflow,false,`${locale}/${layout} About has no horizontal overflow`);
      aboutGeometry.push({locale,layout,...result});
      await page.locator('[data-tab="pane-shortcuts"]').click();
      const shortcut=await page.locator('#pane-shortcuts [data-i18n="shortcut.ctrlArrows"]').textContent();
      assert.equal(shortcut,await page.evaluate(()=>window.NitLinkLocales[document.documentElement.lang]['shortcut.ctrlArrows']),`${locale} Ctrl/Strg shortcut label`);
      await page.locator('[data-tab="pane-games"]').click();
    }
  }
  await page.selectOption('#game-preview-layout','full');
  await page.selectOption('#game-preview-language','zh-TW');
  await page.locator('#game-shape-portrait').click();
  await page.waitForSelector('.game-cover');
  assert.equal(await page.locator('#game-cover-mode').textContent(),'手動指定封面');
  assert.equal(JSON.parse(fs.readFileSync(profile,'utf8')).currentStartUnix,timerBefore,
    'Cover, locale and layout changes preserve the fixture play timer');
  for (const width of [900,420]) {
    await page.setViewportSize({width,height:1000});
    await page.selectOption('#game-preview-layout','full');
    const layout = await page.evaluate(()=>({columns:getComputedStyle(document.querySelector('.game-layout')).gridTemplateColumns,
      overflow:document.querySelector('#pane-games').scrollWidth>document.querySelector('#pane-games').clientWidth}));
    assert.equal(layout.columns.split(' ').length,1,'Small Full windows use one column');
    assert.equal(layout.overflow,false,'Small Full window stays inside its content width');
  }
  await page.setViewportSize({width:1440,height:1000});
  await page.locator('#game-auto').click();
  await page.waitForFunction(()=>document.getElementById('game-cover-mode').textContent==='自動選圖');
  for(const scenario of ['empty','upstreamUnavailable','timeout','imageFailure']) {
    await page.selectOption('#game-preview-scenario',scenario);
    await page.waitForTimeout(2100);
    if(scenario==='imageFailure') {
      await page.waitForSelector('.game-art[data-image-state="failed"]');
    } else {
      assert.equal(await page.locator('.game-result').count(),0);
      assert.equal(await page.locator('#game-search-fallback').isVisible(),true);
    }
  }
  await page.selectOption('#game-preview-scenario','ok');
  await page.waitForTimeout(1000);
  await page.locator('#game-query').fill('索尼克');
  await page.waitForTimeout(150);
  await page.locator('#game-query').fill('洛克人');
  await page.waitForFunction(()=>document.querySelector('.game-result-title')?.textContent.includes('洛克人'));
  assert.equal(await page.locator('.game-result').count(),1);
  await page.locator('.game-result').click();
  await page.waitForFunction(()=>document.getElementById('game-current-title').textContent.includes('洛克人'));
  await page.locator('#game-cover-retry').click();
  await page.waitForSelector('.game-cover');
  await page.locator('.game-cover').nth(1).click();
  await page.reload();
  await page.waitForFunction(()=>document.getElementById('game-current-title').textContent.includes('洛克人'));
  assert.equal(await page.locator('#game-cover-mode').textContent(),'手動指定封面');
  for (const tab of ['pane-video','pane-audio','pane-shortcuts','pane-about','pane-games']) {
    await page.locator(`[data-tab="${tab}"]`).click();
    assert.equal(await page.locator(`#${tab}`).isVisible(),true);
  }
  await page.locator('[data-tab="pane-video"]').click();
  await page.locator('#source-trigger').click();
  await page.waitForSelector('.source-format-details');
  assert.equal(await page.locator('#hdmi-source-trigger').isVisible(),true);
  await page.locator('.source-format-details summary').click();
  assert.equal(await page.locator('.source-format-details select').count(),3);
  const native = await browser.newPage();
  await installExactDocument(native);
  await native.addInitScript(()=>{window.chrome=window.chrome||{}; window.chrome.webview={postMessage:()=>{},addEventListener:()=>{}};});
  await native.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games',{waitUntil:'domcontentloaded'});
  assert.equal(await native.evaluate(()=>!!window.NitLinkPreview),false,'Native host must take precedence');
  const filePage = await browser.newPage();
  await filePage.goto('file:///'+path.resolve(__dirname,'../nitlink-menu.html').replaceAll('\\','/')+'?preview=games');
  await filePage.waitForSelector('.game-result');
  assert.equal(await filePage.locator('#game-fixture-banner').isVisible(),true);
  assert.equal(errors.filter(e=>!e.includes('ERR_FILE_NOT_FOUND')&&!e.includes('Failed to load resource')).length,0,errors.join('\n'));
  fs.writeFileSync(path.join(output,'browser-checks.json'),JSON.stringify({geometry,aboutGeometry,checks:['game and cover selection','84px Discord artwork','four-language About and shortcut labels','automatic/manual covers','four languages','Full/left/right layouts','failure and empty scenarios','latest query wins','persistent manual fixture choice','existing panes/source format controls','native bridge precedence','direct file entry'],errors,start},null,2));
  console.log('Browser preview checks passed:',geometry.length,'language/layout combinations. Evidence:',output);
  } finally { await browser.close(); }
})().catch(error=>{console.error(error);process.exitCode=1;});
