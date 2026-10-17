"""Explicit bounded HTTP/image diagnostic; no Discord chat messages or uploads."""
import argparse
import concurrent.futures
import io
import json
import pathlib
import urllib.error
import urllib.parse
import urllib.request
from PIL import Image

HOSTS={'cdn2.steamgriddb.com','www.nintendo.com','www.nintendo.com.tw','store.nintendo.com.hk',
       'img-eshop.cdn.nintendo.net','image.api.playstation.com','media.discordapp.net'}
ACCEPTS=['*/*','image/*','image/png,image/jpeg,image/webp','image/avif,image/webp,image/apng,image/*,*/*;q=0.8']
AGENTS=['NitLink-HolyBear-GameSearch/1','Mozilla/5.0 (compatible; Discordbot/2.0; +https://discordapp.com)']

def allowed(url):
    try:
        uri=urllib.parse.urlsplit(url)
        return uri.scheme=='https' and uri.netloc in HOSTS and not uri.fragment
    except ValueError:return False

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self,*args):return None

def probe(item):
    original,accept,agent=item
    row={'url':original,'accept':accept,'userAgent':agent,'finalUrl':original,'redirects':[],'validImage':False}
    try:
        opener=urllib.request.build_opener(NoRedirect)
        url=original
        for redirect in range(4):
            try:
                response=opener.open(urllib.request.Request(url,headers={'Accept':accept,'User-Agent':agent}),timeout=8)
                break
            except urllib.error.HTTPError as error:
                row['status']=error.code
                if error.code not in (301,302,303,307,308):raise
                target=urllib.parse.urljoin(url,error.headers.get('Location',''))
                row['redirects'].append({'status':error.code,'url':target})
                if not allowed(target) or redirect==3:raise ValueError('Redirect outside allowed HTTPS sources or redirect limit')
                url=target
        with response:
            row.update(status=response.status,finalUrl=response.url,contentType=response.headers.get_content_type())
            body=response.read(8*1024*1024+1)
        if len(body)>8*1024*1024:raise ValueError('Image exceeds 8 MiB')
        with Image.open(io.BytesIO(body)) as picture:
            if picture.width>4096 or picture.height>4096 or picture.width*picture.height>8*1024*1024:raise ValueError('Image dimensions exceed limit')
            picture.load()
            row.update(actualFormat=picture.format,width=picture.width,height=picture.height,bytes=len(body),validImage=True)
    except Exception as error:row['error']=str(error)
    return row

def main():
    parser=argparse.ArgumentParser()
    parser.add_argument('--url',action='append',required=True)
    parser.add_argument('--output',type=pathlib.Path,default=pathlib.Path('out/artwork-http-matrix.json'))
    args=parser.parse_args()
    if len(args.url)>8 or any(not allowed(url) for url in args.url):parser.error('At most eight approved HTTPS image URLs are allowed')
    jobs=[(url,accept,agent) for url in args.url for accept in ACCEPTS for agent in AGENTS]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:rows=list(pool.map(probe,jobs))
    args.output.parent.mkdir(parents=True,exist_ok=True)
    args.output.write_text(json.dumps({'visualVerified':False,'results':rows},ensure_ascii=False,indent=2),encoding='utf-8')
    print(json.dumps({'requests':len(rows),'validImages':sum(row['validImage'] for row in rows),'output':str(args.output)},ensure_ascii=True))
    return 0

if __name__=='__main__':raise SystemExit(main())
