"""Refresh bounded preview fixtures from the public Worker; no credentials.

Run explicitly when changing fixtures. The preview itself is fully offline.
Chinese titles below are fixture labels, not claims about Worker translation.
"""
import concurrent.futures
import json
import pathlib
import urllib.parse
import urllib.request
import html
import sys
from PIL import Image
from nintendo_tw_artwork import OFFICIAL

ROOT = pathlib.Path(__file__).resolve().parents[1]
API = 'https://holybear-game-api.nitlink.workers.dev'
DEST = ROOT / 'preview/fixtures'
TITLES = [
    ('zelda-botw', 'The Legend of Zelda: Breath of the Wild', '薩爾達傳說 曠野之息', '塞尔达传说 旷野之息'),
    ('zelda-totk', 'The Legend of Zelda: Tears of the Kingdom', '薩爾達傳說 王國之淚', '塞尔达传说 王国之泪'),
    ('hyrule-warriors', 'Hyrule Warriors', '薩爾達無雙', '塞尔达无双'),
    ('hyrule-calamity', 'Hyrule Warriors: Age of Calamity', '薩爾達無雙 災厄啟示錄', '塞尔达无双 灾厄启示录'),
    ('hyrule-imprisonment', 'Hyrule Warriors: Age of Imprisonment', 'ZELDA無雙 封印戰記', 'ZELDA无双 封印战记'),
    ('zelda-awakening', "The Legend of Zelda: Link's Awakening", '薩爾達傳說 織夢島', '塞尔达传说 织梦岛'),
    ('zelda-ocarina', 'The Legend of Zelda: Ocarina of Time', '薩爾達傳說 時之笛', '塞尔达传说 时之笛'),
    ('zelda-majora', "The Legend of Zelda: Majora's Mask", '薩爾達傳說 穆修拉的假面', '塞尔达传说 穆修拉的假面'),
    ('sonic-crossworlds', 'Sonic Racing: CrossWorlds', '索尼克賽車 交叉世界', '索尼克赛车 交叉世界'),
    ('megaman-exe', 'Mega Man Battle Network Legacy Collection', '洛克人 EXE 合集', '洛克人 EXE 合集'),
]

def official_covers(slug):
    name=next((entry[1] for entry in TITLES if entry[0]==slug),'')
    game=next((game for game in OFFICIAL.catalogue() if game['name']==name),None)
    if not game:return []
    output=[]
    for cover in game['artworkSources']:
        url=cover['url'];filename=slug+'-official-'+str(cover['id'])+'.png';destination=DEST/filename
        if not destination.is_file():
            request=urllib.request.Request(url,headers={'User-Agent':'NitLink-HolyBear-GameSearch/1'})
            with urllib.request.urlopen(request,timeout=12) as response:
                if response.url!=url or response.headers.get_content_type() not in ('image/jpeg','image/png'):raise ValueError('Unexpected artwork response')
                data=response.read(8*1024*1024+1)
            if len(data)>8*1024*1024:raise ValueError('Oversized artwork')
            import io
            with Image.open(io.BytesIO(data)) as art:
                if art.width>4096 or art.height>4096 or art.width*art.height>8*1024*1024:raise ValueError('Oversized decoded artwork')
                if cover.get('crop'):
                    area=cover['crop'];art=art.crop((area['x'],area['y'],area['x']+area['width'],area['y']+area['height']))
                art.thumbnail((512,512));art.convert('RGBA').save(destination)
        output.append({**cover,'image':'preview/fixtures/'+filename})
    return output

def synthetic_covers(slug, titles, shape, game_index):
    """Explicitly authored SVG fixtures, not claimed official localized art."""
    output = []
    labels = [('zh-TW', titles['zh-TW'], '#317c68'), ('zh-CN', titles['zh-CN'], '#3e668f'),
              ('en-US', titles['en-US'], '#675497'), ('ja-JP', '日本語カバー例', '#8f574a'),
              ('zxx', '', '#526b77')]
    width, height = (600, 600) if shape == 'square' else (600, 900)
    for index, (language, title, color) in enumerate(labels):
        filename = f'{slug}-{shape}-{language.lower()}-fixture.svg'
        words = title.split(' ') if language == 'en-US' else [title[i:i+8] for i in range(0, len(title), 8)]
        lines, line = [], ''
        for word in words:
            if len(line) + len(word) > 22:
                lines.append(line); line = word
            else:
                line += (' ' if line else '') + word
        if line:
            lines.append(line)
        text = ''.join(f'<tspan x="300" dy="48">{html.escape(line)}</tspan>' for line in lines)
        svg = f'''<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">
<defs><linearGradient id="b" x2="0" y2="1"><stop stop-color="{color}"/><stop offset="1" stop-color="#101b26"/></linearGradient></defs>
<rect width="600" height="{height}" fill="url(#b)"/><circle cx="480" cy="120" r="150" fill="#fff" opacity=".035"/>
<path d="M300 110l75 130H225Z M225 240l-75 130h150Z M375 240l-75 130h150Z" fill="#efce78" opacity=".72"/>
<path d="M0 {height-110}L130 {height-240}L220 {height-120}L365 {height-300}L600 {height-100}V{height}H0Z" fill="#061722" opacity=".55"/>
<text x="300" y="{height//2+60}" fill="#fff" font-family="Microsoft JhengHei, Microsoft YaHei, Segoe UI, sans-serif" font-size="36" font-weight="600" text-anchor="middle">{text}</text>
</svg>'''
        (DEST / filename).write_text(svg, encoding='utf-8')
        output.append({'id': 4000000000 + game_index * 100 + (0 if shape == 'square' else 10) + index,
                       'image':'preview/fixtures/' + filename, 'author':'NitLink preview fixture',
                       'language':language, 'languageVerified':True, 'languageSource':'fixture.authored-svg',
                       'shape':shape, 'source':'fixture', 'synthetic':True,
                       'kind':'logoOnly' if language == 'zxx' else 'cover'})
    return output

def get_json(path):
    request = urllib.request.Request(API + path, headers={'User-Agent': 'NitLink-HolyBear-GameSearch/1', 'Accept':'application/json'})
    with urllib.request.urlopen(request, timeout=15) as response:
        if response.headers.get_content_type() != 'application/json':
            raise ValueError('Unexpected MIME')
        body = response.read(256 * 1024 + 1)
        if len(body) > 256 * 1024:
            raise ValueError('Oversized response')
        return json.loads(body)

def prepare(index_entry):
    game_index, entry = index_entry
    slug, name, tw, cn = entry
    search = get_json('/games/search?' + urllib.parse.urlencode({'q': name, 'lang': 'en-US'}))
    games = search.get('data', [])
    exact = next((g for g in games if g['name'].casefold() == name.casefold()), None)
    if not exact:
        exact = next((g for g in games if name.casefold() in g['name'].casefold()), None)
    if not exact:
        raise ValueError(f'No verified fixture ID for {name}')
    result = {**exact, 'fixture': True, 'slug': slug,
              'titles': {'en-US': name, 'zh-TW': tw, 'zh-CN': cn}, 'covers': {}}
    for shape in ('square', 'portrait'):
        rows = get_json(f"/games/{exact['id']}/covers?shape={shape}").get('data', [])[:3]
        selected = []
        for index, cover in enumerate(rows):
            uri = urllib.parse.urlparse(cover['thumb'])
            if uri.scheme != 'https' or uri.netloc != 'cdn2.steamgriddb.com' or not uri.path.startswith('/thumb/'):
                continue
            ext = pathlib.PurePosixPath(uri.path).suffix.lower()
            if ext not in ('.jpg', '.png', '.webp'):
                continue
            filename = f'{slug}-{shape}-{index + 1}{ext}'
            request = urllib.request.Request(cover['thumb'], headers={'User-Agent': 'NitLink-Preview-Fixtures/1'})
            with urllib.request.urlopen(request, timeout=12) as response:
                if response.url.split('/')[2] != 'cdn2.steamgriddb.com' or response.headers.get_content_type() not in ('image/png', 'image/jpeg', 'image/webp'):
                    continue
                image = response.read(2 * 1024 * 1024 + 1)
                if len(image) > 2 * 1024 * 1024:
                    continue
            (DEST / filename).write_bytes(image)
            selected.append({**cover, 'image': 'preview/fixtures/' + filename,
                             'languageVerified': False, 'language': '', 'shape':shape,
                             'source':'fixture', 'synthetic':False})
        result['covers'][shape] = selected + synthetic_covers(slug, result['titles'], shape, game_index)
    result['artworkSources']=official_covers(slug)
    return result

if __name__ == '__main__':
    DEST.mkdir(parents=True, exist_ok=True)
    output = ROOT / 'preview/fixtures.js'
    if sys.argv[1:] == ['--official-only']:
        fixtures = json.JSONDecoder().raw_decode(output.read_text(encoding='utf-8').split('window.NitLinkGameFixtures = ',1)[1])[0]
        for game in fixtures:
            official = official_covers(game['slug'])
            if official:
                game['artworkSources']=official
                for shape in game['covers']:game['covers'][shape]=[cover for cover in game['covers'][shape] if cover.get('source') not in ('nintendo','nintendo-tw')]
    else:
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            fixtures = list(pool.map(prepare, enumerate(TITLES)))
    catalogue=OFFICIAL.catalogue()
    for game in catalogue:
        cached=next((fixture for fixture in fixtures if fixture['id']==game['id']),None)
        if cached:game['artworkSources']=cached.get('artworkSources',game['artworkSources'])
    output.write_text('// Public-API IDs and art; translated display labels are fixtures.\n'
                      'window.NitLinkGameFixtures = ' + json.dumps(fixtures, ensure_ascii=False, indent=2) + ';\n'
                      'window.NitLinkOfficialArtworkSeeds = '+json.dumps(catalogue,ensure_ascii=False,indent=2)+';\n', encoding='utf-8')
    print('Prepared', len(fixtures), 'games and', sum(len(c) for g in fixtures for c in g['covers'].values()), 'cached cover thumbnails.')
