"""Bounded, read-only access to Nintendo Taiwan's public software directory."""
import copy
import json
import re
import socket
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

ORIGIN = 'https://www.nintendo.com'
DIRECTORY = 'https://www.nintendo.com/tw/software/switch'
IMAGE_PREFIX = 'https://images.ctfassets.net/o5v89n4kg6h4/'
LOCK = threading.Lock()
CACHE = {}


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *_):
        return None


OPENER = urllib.request.build_opener(NoRedirect)


def image_url(url):
    if not isinstance(url, str) or len(url) > 512 or not url.startswith(IMAGE_PREFIX):
        return False
    try:
        uri = urllib.parse.urlsplit(url)
    except ValueError:
        return False
    if uri.scheme != 'https' or uri.netloc != 'images.ctfassets.net' or uri.fragment:
        return False
    if uri.query not in ('w=400&fm=webp', 'fm=webp&w=400'):
        return False
    parts = uri.path.removeprefix('/o5v89n4kg6h4/').split('/')
    if len(parts) != 3 or any(not re.fullmatch(r'[A-Za-z0-9_-]{1,96}', part) for part in parts[:2]):
        return False
    return bool(re.fullmatch(r'[A-Za-z0-9_-]{1,96}\.(?:jpg|jpeg|png|webp)', parts[2]))


def cover_id(url):
    value = 2166136261
    for byte in url.encode('ascii'):
        value = ((value ^ byte) * 16777619) & 0xffffffff
    return 4200000100 + value % (0xffffffff - 4200000100)


def valid_text(value, limit=256):
    return isinstance(value, str) and 0 < len(value) <= limit and all(ord(c) >= 32 and ord(c) != 127 and not 0xd800 <= ord(c) <= 0xdfff for c in value)


def parse(body, page):
    payload = json.loads(body.decode('utf-8'))
    if not isinstance(payload, dict) or not isinstance(payload.get('items'), list) or len(payload['items']) > 24:
        return {'status': 'invalid', 'covers': [], 'page': page, 'hasMore': False, 'total': 0}
    covers, seen_ids, seen_urls = [], set(), set()
    for item in payload['items']:
        if not isinstance(item, dict) or item.get('region') != 'taiwan':
            continue
        title = item.get('title')
        platform = item.get('hardwareCategory')
        hero = item.get('imageHero')
        if not valid_text(title) or not valid_text(platform, 64) or not platform.startswith('Nintendo Switch') or not isinstance(hero, dict):
            continue
        url = hero.get('url')
        if not image_url(url) or url in seen_urls:
            continue
        identity = cover_id(url)
        while identity in seen_ids and identity < 0xffffffff:
            identity += 1
        seen_ids.add(identity)
        seen_urls.add(url)
        product_page = DIRECTORY
        detail = item.get('pageLinkCustom')
        if isinstance(detail, str) and detail.startswith('/tw/') and '..' not in detail and '\\' not in detail:
            product_page = ORIGIN + detail
        covers.append({'id': identity, 'url': url, 'thumb': url, 'author': 'Nintendo Taiwan',
                       'provider': 'nintendo-tw', 'sourceLabel': 'Official Nintendo Taiwan',
                       'language': 'zh-TW', 'languageVerified': True, 'languageSource': product_page,
                       'region': 'TW', 'isOfficial': True, 'kind': 'keyArt', 'shape': 'landscape',
                       'source': 'nintendo-tw', 'dataMode': 'real', 'synthetic': False,
                       'productTitle': title, 'productPage': product_page,
                       'productPlatform': platform, 'productRelease': ''})
    total = payload.get('total', len(covers))
    if not isinstance(total, int) or isinstance(total, bool) or total < 0:
        total = len(covers)
    more = payload.get('more') is True or total > (page + 1) * 24
    return {'status': 'ok' if covers else 'empty', 'covers': covers, 'total': total,
            'page': page, 'hasMore': more, 'officialProvider': 'nintendo-tw'}


def search(query='', page=0):
    if not isinstance(query, str) or len(query) > 256 or type(page) is not int or not 0 <= page <= 1000 or any(ord(c) < 32 or 0xd800 <= ord(c) <= 0xdfff for c in query):
        return {'status': 'invalid', 'covers': [], 'page': page, 'hasMore': False, 'total': 0}
    key = (query, page)
    with LOCK:
        cached = CACHE.get(key)
        if cached and cached[0] > time.monotonic():
            return copy.deepcopy(cached[1])
    params = {'size': 24}
    if query:
        params['k'] = query
        params['directory'] = 'software'
        if page:params['p'] = page + 1
        path = '/tw/api/search?' + urllib.parse.urlencode(params)
    else:
        params['sftab'] = 'all'
        # Nintendo's software directory uses spage for browse pagination;
        # p is ignored by that endpoint and repeats the first 24 products.
        if page:params['spage'] = page + 1
        path = '/tw/api/software?' + urllib.parse.urlencode(params)
    request = urllib.request.Request(ORIGIN + path, headers={
        'User-Agent': 'NitLink-HolyBear-GameSearch/1', 'Accept': 'application/json'})
    try:
        with OPENER.open(request, timeout=12) as response:
            if response.headers.get_content_type() != 'application/json':
                return {'status': 'invalid', 'covers': [], 'page': page, 'hasMore': False, 'total': 0}
            body = response.read(2 * 1024 * 1024 + 1)
        if len(body) > 2 * 1024 * 1024:
            return {'status': 'invalid', 'covers': [], 'page': page, 'hasMore': False, 'total': 0}
        result = parse(body, page)
        if result['status'] in ('ok', 'empty'):
            with LOCK:
                if len(CACHE) >= 64:
                    CACHE.pop(next(iter(CACHE)))
                CACHE[key] = (time.monotonic() + 1800, result)
        return copy.deepcopy(result)
    except urllib.error.HTTPError as error:
        return {'status': 'rateLimited' if error.code == 429 else 'upstreamUnavailable', 'covers': [], 'page': page, 'hasMore': False, 'total': 0}
    except (socket.timeout, TimeoutError):
        return {'status': 'timeout', 'covers': [], 'page': page, 'hasMore': False, 'total': 0}
    except (OSError, ValueError, UnicodeError, TypeError):
        return {'status': 'offline', 'covers': [], 'page': page, 'hasMore': False, 'total': 0}


def matching(rows, *names):
    normalize = lambda value: re.sub(r'[\W_]+', '', value.casefold())
    keys = {normalize(name) for name in names if isinstance(name, str) and name}
    return [row for row in rows if normalize(row.get('productTitle', '')) in keys]
