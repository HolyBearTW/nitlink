"""Read-only loopback preview host using the real Worker and controlled images.

This adapter exercises the production UI/bridge protocol. It is not evidence
that the unbuilt Windows host or a Discord client has passed validation.
"""
import concurrent.futures
import hashlib
import io
import json
import pathlib
import re
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from PIL import Image
from nintendo_tw_artwork import OFFICIAL, nintendo_url, legacy_taiwan_url, eshop_url, playstation_url, preferred_sources
import nintendo_hk_store as HK_STORE
import nintendo_tw_lineup as TW_LINEUP
from artwork_processing import render_artwork,generated_image

ROOT = pathlib.Path(__file__).resolve().parents[1]
API = 'https://holybear-game-api.nitlink.workers.dev'
IMAGES = ROOT / 'out/game-preview-api/images'
URL_RE = re.compile(r'https://cdn2\.steamgriddb\.com/(?:grid|thumb)/[a-f0-9]{32}\.(?:png|jpg|webp)\??\Z')
LOCK = threading.Lock()
CACHE = {}
POOL = concurrent.futures.ThreadPoolExecutor(max_workers=6)
SLOTS = threading.BoundedSemaphore(4)
PROFILE_LOCK = threading.Lock()
Image.MAX_IMAGE_PIXELS = 8 * 1024 * 1024

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_):
        return None

OPENER = urllib.request.build_opener(NoRedirect)

def text(value, limit=256):
    return isinstance(value, str) and len(value) <= limit and all(ord(c) >= 32 and ord(c) != 127 and not 0xd800 <= ord(c) <= 0xdfff for c in value)

def number(value):
    return isinstance(value, int) and not isinstance(value, bool) and 0 < value <= 0xffffffff

def fetch_json(path):
    with LOCK:
        cached = CACHE.get(path)
        if cached and cached[0] > time.monotonic():
            return cached[1]
    request = urllib.request.Request(API + path, headers={'User-Agent':'NitLink-HolyBear-GameSearch/1','Accept':'application/json'})
    try:
        with OPENER.open(request, timeout=12) as response:
            if response.headers.get_content_type() != 'application/json':
                return {'status':'invalid','data':[]}
            body = response.read(256 * 1024 + 1)
            if len(body) > 256 * 1024:
                return {'status':'invalid','data':[]}
            reply = json.loads(body.decode('utf-8'), parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
        if reply.get('ok') is not True or not isinstance(reply.get('data'), list):
            return {'status':'workerError','data':[]}
        result = {'status':'ok','data':reply['data'], 'lookups':reply.get('lookups',{}),
                  'resolvedQueries':reply.get('resolvedQueries',[]), 'page':reply.get('page',0),
                  'hasMore':reply.get('hasMore',False), 'total':reply.get('total',len(reply['data']))}
        with LOCK:
            if len(CACHE) >= 64:
                CACHE.pop(next(iter(CACHE)))
            CACHE[path] = (time.monotonic() + (1800 if '/covers?' in path else 600), result)
        return result
    except urllib.error.HTTPError as error:
        return {'status':'rateLimited' if error.code == 429 else 'workerError','data':[]}
    except (socket.timeout, TimeoutError):
        return {'status':'timeout','data':[]}
    except (ValueError, UnicodeError, TypeError):
        return {'status':'invalid','data':[]}
    except urllib.error.URLError:
        return {'status':'offline','data':[]}

def expansion(query):
    terms, suggestions = [query], []
    try:
        raw = (ROOT / 'game-search-expansions.json').read_bytes()
        if len(raw) > 32768:
            return terms, suggestions
        data = json.loads(raw)
        for group in data.get('groups',[])[:16]:
            aliases = group.get('aliases',[])[:24]
            if any(text(alias) and alias.casefold() == query.casefold() for alias in aliases):
                terms += [term for term in group.get('queries',[])[:5] if text(term) and term]
                suggestions += [term for term in group.get('suggestions',[])[:8] if text(term) and term]
    except (OSError, ValueError, TypeError, AttributeError):
        pass
    return list(dict.fromkeys(terms))[:6], list(dict.fromkeys(suggestions))[:8]

def search(query, locale, platform='unknown'):
    terms, suggestions = expansion(query)
    primary = fetch_json('/games/search?' + urllib.parse.urlencode({'q':query,'lang':locale}))
    # Worker v5 already expands series queries. Preserve its complete response;
    # extra client queries are only a fallback for insufficient coverage.
    if len(primary.get('data',[])) >= 8:
        terms, replies = [query], [primary]
    else:
        replies = [primary] + list(POOL.map(lambda term: fetch_json('/games/search?' + urllib.parse.urlencode({'q':term,'lang':locale})),terms[1:]))
    games, lookups, resolved, seen = [], {}, [], set()
    unavailable = False
    partial = False
    for term, reply in zip(terms,replies):
        raw_lookups = reply.get('lookups',{})
        valid_lookups = {key:value for key,value in raw_lookups.items() if text(key,64) and text(value,64)} if isinstance(raw_lookups,dict) else {}
        lookups[term] = valid_lookups or {'worker':reply['status']}
        unavailable |= reply['status'] != 'ok'
        partial |= any(value in ('unavailable','error','timeout') for value in valid_lookups.values())
        resolved += [name for name in reply.get('resolvedQueries',[])[:12] if text(name)]
        for game in reply['data']:
            if not isinstance(game,dict) or not number(game.get('id')) or game['id'] in seen or not text(game.get('name')) or not game['name']:
                continue
            seen.add(game['id'])
            display = game.get('displayName')
            games.append({'id':game['id'],'name':game['name'],'displayName':display if text(display) and display else game['name'],
                          'source':game.get('source') if text(game.get('source'),64) else 'steamgriddb','dataMode':'real'})
    status = 'partial' if partial or (games and unavailable) else 'ok' if games else 'upstreamUnavailable' if unavailable else 'empty'
    if not games and all(reply['status'] != 'ok' for reply in replies):
        status = replies[0]['status']
    worker_count=len(games);games=OFFICIAL.merge(games,query,locale,platform)
    if games and status not in ('ok','partial'):status='partial'
    if platform=='switch' and games:
        sources=TW_LINEUP.search(query,0)['covers']
        normalize=lambda value: re.sub(r'[\W_]+','',value.casefold()) if isinstance(value,str) else ''
        unique_exact_query=len(games)==1
        localized_names={}
        if locale!='zh-TW' and sources:
            # Taiwan's catalogue uses Traditional Chinese product names. Resolve
            # the same Worker results in zh-TW so UI locale cannot hide a match.
            localized=fetch_json('/games/search?'+urllib.parse.urlencode({'q':query,'lang':'zh-TW'}))
            result_ids={game['id'] for game in games}
            for item in localized.get('data',[]):
                if isinstance(item,dict) and number(item.get('id')) and item['id'] in result_ids and text(item.get('displayName')):
                    localized_names[item['id']]=item['displayName']
        changed=False
        for game in games:
            names=[game.get('name',''),game.get('displayName','')]
            if localized_names.get(game['id']):names.append(localized_names[game['id']])
            if unique_exact_query:names.append(query)
            for cover in TW_LINEUP.matching(sources,*names):
                artwork=game.setdefault('artworkSources',[])
                if not any(existing.get('url')==cover['url'] for existing in artwork):
                    artwork.insert(0,cover);changed=True
                break
        if changed:status=status if status in ('ok','partial') else 'partial'
    return {'status':status,'games':games,'workerCount':worker_count,'lookups':lookups,'queriedTerms':terms,
            'resolvedQueries':list(dict.fromkeys(resolved))[:24], 'suggestions':suggestions,'total':len(games),'dataMode':'real'}

def covers(game_id, shape, locale, page, provider='all', official_query='', platform='unknown'):
    primary=OFFICIAL.candidates(game_id,platform) if provider!='community' else []
    catalog=TW_LINEUP.search(official_query,page) if platform=='switch' and provider!='community' and official_query else None
    if provider=='nintendo':
        if catalog is None:catalog={'status':'idle','covers':[],'total':0,'page':page,'hasMore':False,'officialProvider':''}
        if platform=='switch' and (catalog['status']!='ok' or not catalog['covers']):
            fallback=HK_STORE.search(official_query,page)
            if fallback['covers'] or catalog['status']!='ok':catalog=fallback
            catalog.setdefault('officialProvider','nintendo-hk-store')
        candidates=[cover for cover in catalog.get('covers',[]) if shape=='all' or cover.get('shape')==shape]
        seen={cover.get('url') for cover in candidates}
        if page==0 and not candidates:
            for cover in preferred_sources(primary,platform):
                if (shape=='all' or cover.get('shape')==shape) and cover.get('url') not in seen:
                    candidates.append(cover);seen.add(cover.get('url'))
        provider_name=(candidates[0].get('provider','') if candidates else '') or catalog.get('officialProvider') or (primary[0]['provider'] if primary else '')
        status=catalog.get('status','idle')
        if candidates and status in ('empty','idle'):status='ok'
        return {'status':status,'page':page,'hasMore':catalog.get('hasMore',False),'total':catalog.get('total',len(candidates))+max(0,len(candidates)-len(catalog.get('covers',[]))),
                'artworkSources':candidates,'covers':[],'dataMode':'real','officialStatus':catalog.get('status','idle'),'officialProvider':provider_name}
    reply = fetch_json(f'/games/{game_id}/covers?shape={shape}&page={page}')
    rows, seen = [], set()
    for cover in reply['data']:
        if not isinstance(cover,dict) or not number(cover.get('id')) or cover['id'] in seen or not text(cover.get('url'),512) or not URL_RE.fullmatch(cover['url']):
            continue
        seen.add(cover['id'])
        thumb = cover.get('thumb')
        language = cover.get('language')
        source = cover.get('languageSource')
        verified = cover.get('languageVerified') is True and language in ('zh-TW','zh-CN','en-US','de-DE','ja-JP','zxx') and text(source,128) and bool(source)
        rows.append({'id':cover['id'],'url':cover['url'],'thumb':thumb if text(thumb,512) and URL_RE.fullmatch(thumb) else cover['url'],
                     'author':cover.get('author') if text(cover.get('author'),128) else '', 'shape':shape,
                     'language':language if verified else '', 'languageVerified':verified,
                     'languageSource':source if verified else '', 'source':'steamgriddb','dataMode':'real',
                     'provider':'steamgriddb','sourceLabel':'SteamGridDB','isOfficial':False,'kind':'cover','region':''})
    rows.sort(key=lambda cover: 0 if cover['languageVerified'] and cover['language']==locale else 2 if cover['languageVerified'] else 1)
    status = reply['status'] if reply['status']!='ok' else 'ok' if rows else 'invalid' if reply['data'] else 'empty'
    official=preferred_sources(primary,platform)
    if provider=='all' and catalog:
        official.extend(cover for cover in catalog.get('covers',[]) if (shape=='all' or cover['shape']==shape) and not any(existing['url']==cover['url'] for existing in official))
        official=preferred_sources(official,platform)
    official=[cover for cover in official if shape=='all' or cover['shape']==shape]
    if official and status=='empty':status='ok'
    elif official and status!='ok':status='partial'
    official_status=catalog.get('status','idle') if catalog else 'ok' if primary else 'idle'
    official_provider=(official[0].get('provider','') if official else '') or (catalog or {}).get('officialProvider','') or (primary[0]['provider'] if primary else '')
    return {'status':status,'covers':rows,'artworkSources':official,'total':reply.get('total',len(rows)),
            'page':reply.get('page',page),'hasMore':reply.get('hasMore',False),'dataMode':'real','officialStatus':official_status,'officialProvider':official_provider}

def image(url):
    if not text(url,512) or not (URL_RE.fullmatch(url) or OFFICIAL.allows(url) or nintendo_url(url) or HK_STORE.image_url(url) or TW_LINEUP.image_url(url)):
        return {'status':'invalid','image':''}
    key = hashlib.sha256(('v3:'+url).encode('ascii')).hexdigest()
    IMAGES.mkdir(parents=True, exist_ok=True)
    path = IMAGES / (key + '.png')
    if not path.is_file():
        try:
            request = urllib.request.Request(url, headers={'User-Agent':'NitLink-HolyBear-GameSearch/1','Accept':'image/png,image/jpeg,image/webp'})
            with OPENER.open(request, timeout=8) as response:
                mime = response.headers.get_content_type()
                if mime not in ('image/png','image/jpeg','image/webp'):
                    return {'status':'invalid','image':''}
                data = response.read(8 * 1024 * 1024 + 1)
            if len(data) > 8 * 1024 * 1024:
                return {'status':'invalid','image':''}
            blocked_path=ROOT/'unavailable-artwork.json'
            if blocked_path.is_file() and blocked_path.stat().st_size<=16384:
                blocked=json.loads(blocked_path.read_text(encoding='utf-8'))
                if blocked.get('version')==1 and hashlib.sha256(data).hexdigest() in blocked.get('sha256',[])[:64]:
                    return {'status':'removed','image':''}
            with Image.open(io.BytesIO(data)) as art:
                expected = {'image/png':'PNG','image/jpeg':'JPEG','image/webp':'WEBP'}[mime]
                if art.format != expected or getattr(art,'n_frames',1) != 1 or art.width > 4096 or art.height > 4096 or art.width*art.height > Image.MAX_IMAGE_PIXELS:
                    return {'status':'invalid','image':''}
                crop=OFFICIAL.crop(url)
                if crop:
                    if crop['x']+crop['width']>art.width or crop['y']+crop['height']>art.height:return {'status':'invalid','image':''}
                    art=art.crop((crop['x'],crop['y'],crop['x']+crop['width'],crop['y']+crop['height']))
                art.thumbnail((512,512))
                output = io.BytesIO(); art.convert('RGBA').save(output,format='PNG')
            data = output.getvalue()
            if len(data) > 2 * 1024 * 1024:
                return {'status':'invalid','image':''}
            with LOCK:
                files = sorted(IMAGES.glob('*.png'),key=lambda item:item.stat().st_mtime)
                total = sum(item.stat().st_size for item in files)
                while files and (len(files)>=128 or total+len(data)>64*1024*1024):
                    old=files.pop(0); total-=old.stat().st_size; old.unlink()
                path.write_bytes(data)
        except (OSError, ValueError, Image.DecompressionBombError):
            return {'status':'failed','image':''}
    return {'status':'ok','image':'preview/api-images/'+key+'.png'}

def dispatch(params):
    operation=params.get('operation',[''])[0]
    locale=params.get('locale',[''])[0]
    if locale not in ('zh-TW','zh-CN','en-US','de-DE'):
        return {'status':'invalid'}
    if not SLOTS.acquire(timeout=1):
        return {'status':'rateLimited'}
    try:
        if operation=='artworkCatalog':return {'status':'ok','games':OFFICIAL.catalogue(locale)}
        if operation=='capabilities':
            try:
                path=ROOT/'out/discord-name-probe.json'
                probe=json.loads(path.read_text(encoding='utf-8-sig')) if path.is_file() and path.stat().st_size<16384 else {}
                art_path=ROOT/'out/discord-artwork-probe.json'
                art=json.loads(art_path.read_text(encoding='utf-8-sig')) if art_path.is_file() and art_path.stat().st_size<16384 else {}
                return {'status':'ok','discordNameStatus':'accepted' if probe.get('accepted') is True else 'unverified',
                        'acceptedImageUrls':[p['url'] for p in art.get('probes',[]) if p.get('imageAccepted') is True and OFFICIAL.allows(p.get('url'))],
                        'visualVerified':False}
            except (OSError,ValueError):return {'status':'ok','discordNameStatus':'unverified','visualVerified':False}
        if operation=='library':
            return library(params)
        if operation=='renderArtwork':
            profile=library({**params,'action':['load']});record=profile.get('records',{}).get(profile.get('currentKey'))
            try:
                if not record or record.get('id')!=int(params.get('gameId',['0'])[0]) or record.get('coverId')!=int(params.get('coverId',['0'])[0]):return {'status':'invalid'}
                return render_artwork(record,params.get('renderMode',[''])[0],float(params.get('x',['.5'])[0]),float(params.get('y',['.5'])[0]),float(params.get('zoom',['1'])[0]),image,IMAGES)
            except (ValueError,OSError):return {'status':'failed'}
        if operation=='search':
            query=params.get('query',[''])[0].strip()
            platform=params.get('platform',['unknown'])[0]
            return search(query,locale,platform) if text(query) and query and platform in ('switch','playstation','unknown','other') else {'status':'invalid','games':[]}
        if operation=='covers':
            id_text=params.get('gameId',[''])[0]; shape=params.get('shape',[''])[0]; page=params.get('page',['0'])[0]
            if not re.fullmatch(r'[1-9]\d{0,9}',id_text) or not number(int(id_text)) or shape not in ('square','portrait','landscape','all') or not re.fullmatch(r'\d{1,4}',page) or int(page)>1000:
                return {'status':'invalid','covers':[]}
            provider=params.get('provider',['all'])[0];official_query=params.get('officialQuery',[''])[0]
            if provider not in ('all','nintendo','community') or (provider=='nintendo' and not text(official_query)):return {'status':'invalid','covers':[]}
            platform=params.get('platform',['unknown'])[0]
            if platform not in ('switch','playstation','unknown','other'):return {'status':'invalid'}
            return covers(int(id_text),shape,locale,int(page),provider,official_query,platform)
        if operation=='image':
            return image(params.get('url',[''])[0])
        return {'status':'invalid'}
    finally:
        SLOTS.release()

def library(params):
    mode=params.get('mode',[''])[0]; action=params.get('action',[''])[0]
    if mode not in ('real','fixture') or action not in ('load','current','upsert','activate'):
        return {'status':'invalid'}
    folder=ROOT/'out/game-preview-api'; folder.mkdir(parents=True,exist_ok=True)
    profile_name=params.get('profile',['default'])[0]
    if profile_name not in ('default','checks'):return {'status':'invalid'}
    path=folder/(mode+'-library'+('' if profile_name=='default' else '-checks')+'.json')
    with PROFILE_LOCK:
        profile={'records':{},'currentKey':'','currentStartUnix':int(time.time())}
        try:
            if path.is_file() and path.stat().st_size<=1024*1024:
                loaded=json.loads(path.read_text(encoding='utf-8'))
                if isinstance(loaded.get('records'),dict) and len(loaded['records'])<=256: profile=loaded
        except (OSError,ValueError,TypeError): pass
        if action=='load': return {'status':'ok',**profile}
        key=params.get('key',[''])[0]
        if key and not re.fullmatch(r'[a-z0-9_-]{1,64}',key): return {'status':'invalid'}
        if action=='current':
            stamp=params.get('start',['0'])[0]
            if not re.fullmatch(r'\d{1,12}',stamp):return {'status':'invalid'}
            profile['currentKey']=key;profile['currentStartUnix']=int(stamp)
        else:
            try: record=json.loads(params.get('record',[''])[0])
            except ValueError:return {'status':'invalid'}
            if not isinstance(record,dict) or record.get('key')!=key or not text(record.get('name')) or not record['name'] or not text(record.get('customName','')):
                return {'status':'invalid'}
            if key.startswith('sgdb-') and (not number(record.get('id')) or key!='sgdb-'+str(record['id'])):return {'status':'invalid'}
            if not isinstance(record.get('favorite'),bool) or not isinstance(record.get('manualCover'),bool):return {'status':'invalid'}
            names=record.get('displayNames',{})
            if not isinstance(names,dict) or len(names)>4 or any(locale not in ('zh-TW','zh-CN','en-US','de-DE') or not text(name) for locale,name in names.items()):return {'status':'invalid'}
            for field in ('coverUrl','coverThumb'):
                url=record.get(field,'')
                allowed=not url or (text(url,512) and (URL_RE.fullmatch(url) or OFFICIAL.allows(url) or nintendo_url(url) or HK_STORE.image_url(url) or TW_LINEUP.image_url(url))) or (mode=='fixture' and re.fullmatch(r'preview/fixtures/[a-z0-9-]+\.(?:jpg|png|webp|svg)',url))
                if not allowed:return {'status':'invalid'}
            provider=record.get('provider','steamgriddb');is_official=record.get('isOfficial',False)
            if provider not in (('steamgriddb','nintendo-tw','nintendo-hk-eshop','nintendo-hk-store','playstation-tw','fixture') if mode=='fixture' else ('steamgriddb','nintendo-tw','nintendo-hk-eshop','nintendo-hk-store','playstation-tw')) or type(is_official)!=bool:return {'status':'invalid'}
            if provider in ('nintendo-tw','nintendo-hk-eshop'):
                expected=('TW','Official Nintendo Taiwan') if provider=='nintendo-tw' else ('HK','Official Nintendo eShop CDN')
                if not is_official or record.get('language') not in ('zh-TW','zh-CN','en-US','de-DE','ja-JP','zxx','') or (record.get('region'),record.get('sourceLabel'))!=expected or record.get('kind') not in ('boxArt','logo','keyArt','squareIcon'):return {'status':'invalid'}
                if provider=='nintendo-tw' and TW_LINEUP.image_url(record.get('coverUrl')):
                    if record.get('kind') not in ('logo','keyArt') or record.get('coverShape')!='landscape' or (record.get('coverThumb') and not TW_LINEUP.image_url(record['coverThumb'])):return {'status':'invalid'}
                elif provider=='nintendo-tw' and (not legacy_taiwan_url(record.get('coverUrl')) or (record.get('coverThumb') and not legacy_taiwan_url(record['coverThumb']))):return {'status':'invalid'}
                elif provider=='nintendo-hk-eshop' and (not eshop_url(record.get('coverUrl')) or (record.get('coverThumb') and not eshop_url(record['coverThumb']))):return {'status':'invalid'}
            elif provider=='playstation-tw':
                if not is_official or (record.get('region'),record.get('sourceLabel'))!=('TW','Official PlayStation Taiwan') or not playstation_url(record.get('coverUrl')) or not any(c['url']==record.get('coverUrl') for c in OFFICIAL.candidates(record.get('id'))):return {'status':'invalid'}
                if record.get('coverThumb') and not playstation_url(record['coverThumb']):return {'status':'invalid'}
                if record.get('language') not in ('zh-TW','zh-CN','en-US','de-DE','ja-JP','zxx','') or record.get('kind') not in ('squareIcon','boxArt','keyArt','logo'):return {'status':'invalid'}
            elif provider=='nintendo-hk-store':
                if not is_official or record.get('language') or (record.get('region'),record.get('sourceLabel'))!=('HK','Official Nintendo Hong Kong Store') or record.get('kind')!='cover' or not HK_STORE.image_url(record.get('coverUrl')):return {'status':'invalid'}
            elif is_official or nintendo_url(record.get('coverUrl','')):return {'status':'invalid'}
            for field in ('image','sourceImage'):
                image_uri=record.get(field,'')
                if image_uri and not re.fullmatch(r'preview/(?:fixtures/[a-z0-9-]+\.(?:jpg|png|webp|svg)|(?:api-images|generated-images)/[a-f0-9]{64}\.png)',image_uri):return {'status':'invalid'}
            if record.get('artworkMode','original') not in ('original','fit','crop'):return {'status':'invalid'}
            for field,lo,hi in (('cropX',0,1),('cropY',0,1),('artworkZoom',1,3)):
                val=record.get(field,1 if field=='artworkZoom' else .5)
                if type(val) not in (int,float) or not lo<=val<=hi:return {'status':'invalid'}
            for field in ('finalImageKey','sourceImageKey'):
                key_value=record.get(field,'')
                if key_value and not re.fullmatch(r'[a-f0-9]{64}',key_value):return {'status':'invalid'}
            if len(profile['records'])>=256 and key not in profile['records']:return {'status':'libraryFull'}
            if action=='activate' and key in profile['records']:
                # Activation must not overwrite a newer favorite/name/artwork
                # save with an earlier selection snapshot.
                old_record=profile['records'][key]
                record={**record,**old_record,'lastUsedUtc':max(record.get('lastUsedUtc',0),old_record.get('lastUsedUtc',0))}
            profile['records'][key]=record
            if action=='activate':
                stamp=params.get('start',['0'])[0]
                if not re.fullmatch(r'\d{1,12}',stamp):return {'status':'invalid'}
                profile['currentKey']=key;profile['currentStartUnix']=int(stamp)
        contents=json.dumps(profile,ensure_ascii=False,allow_nan=False,indent=2)
        if len(contents.encode('utf-8'))>1024*1024:return {'status':'libraryFull'}
        temporary=path.with_suffix('.tmp');temporary.write_text(contents,encoding='utf-8');temporary.replace(path)
        return {'status':'ok'}
