// Lightweight German selection, reload, and standalone system-detection check.
const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
const assert=require('node:assert/strict'),path=require('node:path');
(async()=>{
 const browser=await chromium.launch({executablePath:'C:/Program Files/Google/Chrome/Application/chrome.exe',headless:true});
 try{
  const page=await browser.newPage();
  await page.addInitScript(()=>{
   Object.defineProperty(navigator,'languages',{configurable:true,get:()=>['de-CH','de']});
   Object.defineProperty(navigator,'language',{configurable:true,get:()=>'de-CH'});
  });
  const html=path.resolve(__dirname,'../nitlink-menu.html').replaceAll('\\','/');
  await page.goto('file:///'+html,{waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>document.documentElement.lang==='de-DE'&&document.querySelector('[data-i18n="nav.video"]')?.textContent==='Video');
  assert.equal(await page.locator('#language-select').inputValue(),'system','system preference uses detected German');
  await page.locator('[data-tab="pane-shortcuts"]').click();
  assert.equal(await page.locator('#pane-shortcuts [data-i18n="shortcut.ctrlArrows"]').textContent(),'Strg+Pfeiltasten');
  await page.locator('[data-tab="pane-video"]').click();
  await page.locator('#language-select').selectOption('de-DE');
  assert.equal(await page.locator('#language-select').inputValue(),'de-DE');
  await page.reload({waitUntil:'domcontentloaded'});
  await page.waitForFunction(()=>document.documentElement.lang==='de-DE');
  assert.equal(await page.locator('#language-select').inputValue(),'de-DE','German preference persists after reload');
  assert.equal(await page.locator('[data-i18n="language.german"]').textContent(),'Deutsch','German language name is localized');
  await page.locator('[data-tab="pane-video"]').click();
  await page.locator('#language-select').selectOption('system');
  await page.locator('[data-tab="pane-shortcuts"]').click();
  assert.equal(await page.locator('#pane-shortcuts [data-i18n="shortcut.ctrlArrows"]').textContent(),'Strg+Pfeiltasten');
  console.log('German locale switching, reload persistence, system detection (de-CH), and Strg shortcut label passed.');
 }finally{await browser.close();}
})().catch(error=>{console.error(error);process.exitCode=1;});
