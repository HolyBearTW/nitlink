"""Local official artwork provider; shared seed file with the native host.

Never crawls pages or accepts caller-supplied artwork URLs. Catalogue additions
are labelled provider mappings, separate from the Worker's search response.
"""
import copy
import json
import pathlib
import re
import urllib.parse

ROOT = pathlib.Path(__file__).resolve().parents[1]
IMAGE_PATH = re.compile(r'/[A-Za-z0-9_./-]+\.(?:png|jpg|webp)\Z')

def nintendo_url(url):
    if not isinstance(url,str) or len(url)>512:return False
    uri=urllib.parse.urlsplit(url)
    if uri.scheme=='https' and uri.netloc=='img-eshop.cdn.nintendo.net':return bool(re.fullmatch(r'/i/[a-f0-9]{64}\.jpg',uri.path)) and not uri.query and not uri.fragment
    return (uri.scheme=='https' and uri.netloc in ('www.nintendo.com','www.nintendo.com.tw')
            and not uri.query and not uri.fragment and '..' not in uri.path and '//' not in uri.path
            and bool(IMAGE_PATH.fullmatch(uri.path)) and (uri.netloc!='www.nintendo.com' or uri.path.startswith('/tw/')))

def legacy_taiwan_url(url):
    if not nintendo_url(url):return False
    try:return urllib.parse.urlsplit(url).netloc in ('www.nintendo.com','www.nintendo.com.tw')
    except ValueError:return False

def eshop_url(url):
    if not nintendo_url(url):return False
    try:return urllib.parse.urlsplit(url).netloc=='img-eshop.cdn.nintendo.net'
    except ValueError:return False

def playstation_url(url):
    if not isinstance(url,str) or len(url)>512:return False
    return bool(re.fullmatch(r'https://image\.api\.playstation\.com/vulcan/[A-Za-z0-9_/-]+\.(?:png|jpg|webp)',url)) and '..' not in url and '//' not in url[8:]

def preferred_sources(covers, platform):
    if platform=='playstation':return [cover for cover in covers if cover.get('provider')=='playstation-tw']
    if platform!='switch':return []
    taiwan=[cover for cover in covers if cover.get('provider')=='nintendo-tw']
    return taiwan or [cover for cover in covers if cover.get('provider')=='nintendo-hk-eshop']

def valid_text(value, limit=256):
    return isinstance(value,str) and len(value)<=limit and all(ord(c)>=32 and ord(c)!=127 and not 0xd800<=ord(c)<=0xdfff for c in value)

class NintendoTaiwanArtworkProvider:
    provider='nintendo-tw'
    source_label='Official Nintendo Taiwan'
    def __init__(self, path=None):self.path=path or ROOT/'official-artwork.json'
    def catalogue(self, locale='zh-TW'):
        try:
            if self.path.stat().st_size>256*1024:return []
            raw=json.loads(self.path.read_text(encoding='utf-8'))
            if raw.get('version')!=1 or not isinstance(raw.get('games'),list) or len(raw['games'])>256:return []
            games=[];game_ids=set();cover_ids=set()
            for entry in raw['games']:
                game_id=entry.get('id');name=entry.get('name');names=entry.get('displayNames',{})
                if type(game_id)!=int or not 0<game_id<=0xffffffff or game_id in game_ids or not valid_text(name) or not name:continue
                if not isinstance(names,dict) or len(names)>4 or any(key not in ('zh-TW','zh-CN','en-US','de-DE') or not valid_text(value) for key,value in names.items()):continue
                sources=entry.get('artworkSources',[])
                if not isinstance(sources,list) or len(sources)>8:continue
                covers=[]
                for source in sources:
                    cover_id=source.get('id');url=source.get('url');page=source.get('sourcePage')
                    if (type(cover_id)!=int or not 4200000000<=cover_id<=0xffffffff or cover_id in cover_ids
                        or not (playstation_url(url) if source.get('provider')=='playstation-tw' else nintendo_url(url)) or (source.get('provider'),source.get('sourceLabel'),source.get('region')) not in
                           ((self.provider,self.source_label,'TW'),('nintendo-hk-eshop','Official Nintendo eShop CDN','HK'),('playstation-tw','Official PlayStation Taiwan','TW'))
                        or source.get('language') not in ('zh-TW','zh-CN','en-US','de-DE','ja-JP','zxx',None) or source.get('isOfficial') is not True
                        or source.get('kind') not in ('boxArt','logo','keyArt','squareIcon') or source.get('shape') not in ('square','portrait','landscape')
                        or not valid_text(page) or not page.startswith(('https://www.playstation.com/zh-hant-tw/games/',) if source.get('provider')=='playstation-tw' else ('https://www.nintendo.com/tw/','https://www.nintendo.com.tw/','https://img-eshop.cdn.nintendo.net/i/'))):continue
                    cover_ids.add(cover_id)
                    crop=source.get('crop')
                    if crop is not None and (not isinstance(crop,dict) or set(crop)!={'x','y','width','height'} or
                        any(type(crop[k])!=int or not (0 if k in ('x','y') else 1)<=crop[k]<=4096 for k in crop)):continue
                    covers.append({**source,'thumb':url,'author':source.get('author') if valid_text(source.get('author'),128) else 'Sony Interactive Entertainment' if source.get('provider')=='playstation-tw' else 'Nintendo','languageSource':page,'languageVerified':source.get('language') is not None,
                                   'source':source['provider'],'dataMode':'provider','synthetic':False})
                terms=entry.get('searchTerms',[])
                if not covers or not isinstance(terms,list) or len(terms)>16 or any(not valid_text(term) for term in terms):continue
                game_ids.add(game_id)
                games.append({'id':game_id,'name':name,'displayNames':names,'displayName':names.get(locale) or name,
                              'displayNameSource':covers[0]['provider'],'searchTerms':terms,'source':covers[0]['provider'],
                              'dataMode':'provider','artworkSources':covers})
            return games
        except (OSError,ValueError,TypeError,AttributeError):return []
    def lookup(self, game_id, locale='zh-TW'):
        return next((game for game in self.catalogue(locale) if game['id']==game_id),None)
    def search(self, query, locale):
        needle=query.casefold()
        return [game for game in self.catalogue(locale) if needle and
                (query==str(game['id']) or any(needle in term.casefold() for term in [game['name'],*game['displayNames'].values(),*game['searchTerms']]))]
    def candidates(self, game_id, platform=None):
        game=self.lookup(game_id);covers=game['artworkSources'] if game else []
        return preferred_sources(covers,platform) if platform is not None else covers
    def allows(self, url):
        return any(cover['url']==url for game in self.catalogue() for cover in game['artworkSources'])
    def crop(self, url):
        return next((cover.get('crop') for game in self.catalogue() for cover in game['artworkSources'] if cover['url']==url),None)
    def merge(self, games, query, locale, platform='unknown'):
        output=copy.deepcopy(games);ids={game['id'] for game in games}
        for game in output:
            official=self.lookup(game['id'],locale)
            if official and (covers:=preferred_sources(official['artworkSources'],platform)):
                game['artworkSources']=covers
                if locale=='zh-TW':game['displayName']=official['displayName'];game['displayNameSource']=official['source']
        for game in self.search(query,locale):
            game['artworkSources']=preferred_sources(game['artworkSources'],platform)
            if game['artworkSources'] and game['id'] not in ids:output.append(game)
        return output

OFFICIAL=NintendoTaiwanArtworkProvider()
