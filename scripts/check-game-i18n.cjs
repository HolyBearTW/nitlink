// Resource and ordinary-browser checks. No Windows build or CTest.
const fs=require('node:fs'),vm=require('node:vm'),path=require('node:path'),assert=require('node:assert/strict');
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const {installExactDocument}=require('./preview-browser.cjs');
const root=path.resolve(__dirname,'..'),locales=['zh-TW','zh-CN','en-US'],context={window:{}};
for(const locale of locales)vm.runInNewContext(fs.readFileSync(path.join(root,'locales',locale+'.js'),'utf8'),context);
const resources=context.window.NitLinkLocales;
const keys=[...new Set(locales.flatMap(locale=>Object.keys(resources[locale]).filter(key=>key.startsWith('game.'))))];
for(const locale of locales)for(const key of keys)assert.equal(typeof resources[locale][key],'string',locale+': '+key);
const source=['assets/menu/menu.js','assets/menu/artwork-editor.js','nitlink-menu.html'].map(file=>fs.readFileSync(path.join(root,file),'utf8')).join('\n');
for(const match of source.matchAll(/["'](game\.[\w.]+)["']/g))if(!match[1].endsWith('.'))for(const locale of locales)assert(resources[locale][match[1]],locale+': '+match[1]);
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try{
  const page=await browser.newPage({viewport:{width:1440,height:1100}}),evidence=[];
  // The ordinary URL can be interrupted by local web protection. Record that
  // separately, then use the same exact-source transport as the review launcher.
  let normalUrlWorks=true;
  const url='http://127.0.0.1:8765/nitlink-menu.html?preview=games&profile=checks';
  try{await page.goto(url,{waitUntil:'domcontentloaded',timeout:10000});}
  catch(error){normalUrlWorks=false;await installExactDocument(page);await page.goto(url,{waitUntil:'domcontentloaded',timeout:10000});}
  await page.waitForFunction(()=>document.querySelectorAll('.game-result').length>=28,{},{timeout:25000});
  await page.locator('.game-result').filter({hasText:'The Legend of Zelda: Tears of the Kingdom'}).click();
  await page.waitForFunction(()=>document.getElementById('game-current-title').textContent.includes('王國之淚'));
  await page.locator('#game-cover-retry').click();
  await page.waitForFunction(()=>document.querySelector('#game-official-gallery img')?.naturalWidth>0);
  for(const locale of locales){
   await page.selectOption('#game-preview-language',locale);
   await page.waitForFunction(locale=>document.documentElement.lang===locale,locale);
   const labels=await page.evaluate(()=>({group:document.querySelector('[data-i18n="game.officialGroup"]').textContent,badge:[...document.querySelectorAll('#game-official-gallery .game-cover-badge')].find(el=>!el.hidden&&el.getClientRects().length)?.textContent,community:document.querySelector('#game-community-heading').textContent,untranslated:[...document.querySelectorAll('#pane-games [data-i18n]')].filter(el=>el.textContent===el.dataset.i18n).map(el=>el.dataset.i18n)}));
   assert.equal(labels.group,resources[locale]['game.officialGroup']);assert.equal(labels.badge,resources[locale]['game.officialBadge']);assert.equal(labels.community,resources[locale]['game.communityGroup']);assert.deepEqual(labels.untranslated,[]);
   await page.selectOption('#game-preview-layout','right');
   assert.equal(await page.locator('#pane-games').evaluate(el=>el.scrollWidth>el.clientWidth),false);
   evidence.push({locale,...labels});await page.selectOption('#game-preview-layout','full');
  }
  fs.writeFileSync(path.join(root,'out/game-preview/i18n-checks.json'),JSON.stringify({gameKeys:keys.length,normalUrlWorks,exactSourceFallback:!normalUrlWorks,evidence},null,2));
  console.log('All '+keys.length+' game keys and live labels passed. Ordinary URL: '+normalUrlWorks+'; exact-source launcher available.');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
