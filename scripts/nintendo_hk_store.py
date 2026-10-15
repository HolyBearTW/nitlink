"""Bounded, read-only lookup of the official Hong Kong store's public catalogue.

Uses the store's normal keyword/pagination pages; never executes page scripts,
follows product links recursively, logs in, or attempts to solve a challenge.
No title list or SteamGridDB-to-Nintendo mapping is required.
"""
import copy
import html
import re
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ORIGIN='https://store.nintendo.com.hk'
LOCK=threading.Lock()
CACHE={}

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*_):return None

OPENER=urllib.request.build_opener(NoRedirect)

def image_url(url):
    if not isinstance(url,str) or len(url)>512:return False
    try:uri=urllib.parse.urlsplit(url)
    except ValueError:return False
    if uri.scheme!='https' or uri.netloc!='store.nintendo.com.hk' or uri.query or uri.fragment:return False
    if not uri.path.startswith('/media/catalog/product/') or '..' in uri.path or not uri.path.endswith(('.jpg','.jpeg','.png','.webp')):return False
    return all(re.fullmatch(r'[A-Za-z0-9_.-]{1,160}',part) and part not in ('.','..') for part in uri.path.split('/')[1:])

def cover_id(url):
    value=2166136261
    for byte in url.encode('ascii'):value=((value^byte)*16777619)&0xffffffff
    return 4200000100+value%(0xffffffff-4200000100)

def plain(value):return ' '.join(html.unescape(re.sub(r'<[^>]*>','',value)).split())

def attribute(tag,name):
    match=re.search(r'\b'+re.escape(name)+r'="([^"]*)"',tag)
    return html.unescape(match.group(1)) if match else ''

def parse(body,page):
    rows=[];ids=set()
    for tile in re.findall(r'<li\b[^>]*\bclass="[^"]*\bproduct-item\b[^"]*"[^>]*>([\s\S]*?)</li>',body):
        anchor=re.search(r'<a\b[^>]*\bclass="[^"]*\bproduct-item-link\b[^"]*"[^>]*>([\s\S]*?)</a>',tile)
        picture=re.search(r'<img\b[^>]*\bclass="[^"]*\bproduct-image-photo\b[^"]*"[^>]*>',tile)
        if not anchor or not picture:continue
        product=attribute(anchor.group(0).split('>',1)[0],'href')
        # Digital-game NSUIDs exclude merchandise, consoles, and accessories.
        if not re.fullmatch(re.escape(ORIGIN)+r'/700[0-9]{11}',product):continue
        name=plain(anchor.group(1));thumb=attribute(picture.group(0),'src')
        if not name or len(name)>256 or not image_url(thumb):continue
        # Keep the exact public image published by the store. Do not invent an
        # unverified original URL by removing Magento's cache path.
        original=thumb
        identity=cover_id(original)
        if identity in ids:continue
        ids.add(identity)
        platform=re.search(r'<p\b[^>]*class="label_platform"[^>]*>([\s\S]*?)</p>',tile)
        release=re.search(r'<div\b[^>]*class="category-product-item-released"[^>]*>([\s\S]*?)</div>',tile)
        rows.append({'id':identity,'url':original,'thumb':thumb,'author':'',
                     'provider':'nintendo-hk-store','sourceLabel':'Official Nintendo Hong Kong Store',
                     'language':None,'languageVerified':False,'languageSource':product,'region':'HK',
                     'isOfficial':True,'kind':'cover','shape':'','source':'nintendo-hk-store','dataMode':'real',
                     'productTitle':name,'productPage':product,'productId':product.rsplit('/',1)[1],
                     'productPlatform':plain(platform.group(1))[:64] if platform else '',
                     'productRelease':plain(re.sub(r'<span[^>]*>[\s\S]*?</span>','',release.group(1)))[:64] if release else ''})
    toolbar=re.search(r'id="toolbar-amount"[^>]*>([\s\S]*?)</p>',body)
    counts=re.findall(r'class="toolbar-number"[^>]*>\s*([0-9,]+)',toolbar.group(1)) if toolbar else []
    total=int(counts[-1].replace(',','')) if counts else len(rows)
    more=bool(re.search(r'<a\b[^>]*class="[^"]*\baction\s+next\b',body)) or total>(page+1)*24
    blocked=not rows and any(marker in body.lower() for marker in ('verify that you','not a robot','cf-chl-'))
    return {'status':'upstreamUnavailable' if blocked else 'ok' if rows else 'empty','covers':rows,'total':total,'page':page,'hasMore':more}

def search(query,page=0):
    if not isinstance(query,str) or len(query)>256 or type(page)!=int or not 0<=page<=1000 or any(ord(c)<32 or 0xd800<=ord(c)<=0xdfff for c in query):return {'status':'invalid','covers':[]}
    key=(query,page)
    with LOCK:
        cached=CACHE.get(key)
        if cached and cached[0]>time.monotonic():return copy.deepcopy(cached[1])
    path='/catalogsearch/result/?' if query else '/download-code?'
    params={'p':page+1,'product_list_limit':24}
    if query:params['q']=query
    try:
        request=urllib.request.Request(ORIGIN+path+urllib.parse.urlencode(params),headers={'User-Agent':'NitLink-HolyBear-GameSearch/1','Accept':'text/html'})
        with OPENER.open(request,timeout=12) as response:
            if response.headers.get_content_type()!='text/html':return {'status':'invalid','covers':[]}
            body=response.read(2*1024*1024+1)
        if len(body)>2*1024*1024:return {'status':'invalid','covers':[]}
        result=parse(body.decode('utf-8'),page)
        if result['status'] in ('ok','empty'):
            with LOCK:
                if len(CACHE)>=64:CACHE.pop(next(iter(CACHE)))
                CACHE[key]=(time.monotonic()+1800,result)
        return copy.deepcopy(result)
    except urllib.error.HTTPError as error:return {'status':'rateLimited' if error.code==429 else 'upstreamUnavailable','covers':[]}
    except (socket.timeout,TimeoutError):return {'status':'timeout','covers':[]}
    except (OSError,ValueError,UnicodeError):return {'status':'offline','covers':[]}

def matching(rows,*names):
    normalize=lambda value:re.sub(r'[\W_]+','',value.casefold())
    keys={normalize(name) for name in names if name}
    return [row for row in rows if normalize(row['productTitle']) in keys]
