// Serve the exact workspace document to the review browser's local origin.
// This avoids machine HTTP filters rewriting the HTML/CSP. All bridge and image
// requests still use the controlled loopback server; no remote API data is mocked.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto');
const root=path.resolve(__dirname,'..');
async function installExactDocument(target){
  await target.route(/^http:\/\/(?:127\.0\.0\.1|localhost):8765\/nitlink-menu\.html(?:\?.*)?$/,async route=>{
    const body=fs.readFileSync(path.join(root,'nitlink-menu.html'));
    const policy=body.toString('utf8').match(/<meta http-equiv="Content-Security-Policy" content="([^"]+)"/)[1];
    await route.fulfill({status:200,contentType:'text/html; charset=utf-8',headers:{'Content-Security-Policy':policy,'Cache-Control':'no-store','X-Content-Type-Options':'nosniff'},body});
  });
  const scripts=['locales/en-US.js','locales/zh-TW.js','locales/zh-CN.js','assets/menu/menu.js','assets/menu/preview-host.js','assets/menu/artwork-editor.js'];
  await target.route(/^http:\/\/(?:127\.0\.0\.1|localhost):8765\/(?:locales\/(?:en-US|zh-TW|zh-CN)\.js|assets\/menu\/(?:menu|preview-host|artwork-editor)\.js)(?:\?v=20261015-artwork3)?$/,async route=>{
    const resource=new URL(route.request().url()).pathname.slice(1);if(!scripts.includes(resource)){await route.abort();return;}
    await route.fulfill({status:200,contentType:'text/javascript; charset=utf-8',headers:{'Cache-Control':'no-store','X-Content-Type-Options':'nosniff'},body:fs.readFileSync(path.join(root,resource))});
  });
}
module.exports={installExactDocument};
if(require.main===module)(async()=>{
  const {chromium}=require('../out/ui-preview-runtime/node_modules/playwright-core');
  const executablePath=['C:/Program Files/Google/Chrome/Application/chrome.exe','C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe'].find(file=>fs.existsSync(file));
  if(!executablePath)throw Error('Chrome or Edge is required for the interactive preview.');
  const browser=await chromium.launch({executablePath,headless:false});
  const context=await browser.newContext({viewport:null});await installExactDocument(context);
  const page=await context.newPage();
  await page.goto('http://127.0.0.1:8765/nitlink-menu.html?preview=games',{waitUntil:'domcontentloaded'});
  const output=path.join(root,'out/game-preview');fs.mkdirSync(output,{recursive:true});
  fs.writeFileSync(path.join(output,'interactive-browser-source.json'),JSON.stringify({file:path.join(root,'nitlink-menu.html'),sha256:crypto.createHash('sha256').update(fs.readFileSync(path.join(root,'nitlink-menu.html'))).digest('hex'),mode:'actual source document; controlled live API bridge'},null,2));
  await new Promise(resolve=>browser.on('disconnected',resolve));
})().catch(error=>{console.error(error);process.exitCode=1;});
