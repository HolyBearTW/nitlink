"""Preview equivalent of the native WIC renderer; emits durable PNG files.

No upload, public hosting, caller-supplied local path, or browser data URL input.
"""
import hashlib
import io
import math
import pathlib
import re
import threading
from PIL import Image,ImageFilter,ImageEnhance

ROOT=pathlib.Path(__file__).resolve().parents[1]
GENERATED=ROOT/'out/game-preview-api/generated'
KEY=re.compile(r'[a-f0-9]{64}\Z')
WRITE_LOCK=threading.Lock()
def generated_image(key):
    if not isinstance(key,str) or not KEY.fullmatch(key):return None
    path=GENERATED/(key+'.png')
    if not path.is_file() or path.is_symlink() or path.stat().st_size>2*1024*1024:return None
    try:
        with Image.open(path) as image:
            if image.format!='PNG' or image.width>512 or image.height>512:return None
            return image.convert('RGBA')
    except (OSError,ValueError):return None
def save_image(image):
    output=io.BytesIO();image.save(output,format='PNG');data=output.getvalue()
    if len(data)>2*1024*1024:raise ValueError('Oversized output')
    key=hashlib.sha256(data).hexdigest();GENERATED.mkdir(parents=True,exist_ok=True)
    path=GENERATED/(key+'.png')
    with WRITE_LOCK:
        if not path.is_file():
            files=list(GENERATED.glob('*.png'))
            if len(files)>=2048 or sum(p.stat().st_size for p in files)+len(data)>512*1024*1024:raise ValueError('Artwork storage is full')
            temporary=path.with_suffix('.tmp');temporary.write_bytes(data);temporary.replace(path)
    return key
def render_artwork(record,mode,x,y,zoom,load_image,image_folder):
    if mode not in ('fit','crop','original') or any(not isinstance(v,(int,float)) or isinstance(v,bool) or not math.isfinite(v) for v in (x,y,zoom)) or not 0<=x<=1 or not 0<=y<=1 or not 1<=zoom<=3:return {'status':'invalid'}
    source=generated_image(record.get('sourceImageKey'))
    fixture=record.get('sourceImage') or record.get('image','')
    if source is None and re.fullmatch(r'preview/fixtures/[a-z0-9-]+\.(?:png|jpg|webp)',fixture):
        with Image.open(ROOT/fixture) as local:
            if local.width>4096 or local.height>4096 or getattr(local,'n_frames',1)!=1:return {'status':'invalid'}
            source=local.convert('RGBA');source.thumbnail((512,512))
    if source is None:
        reply=load_image(record.get('coverThumb') or record.get('coverUrl'))
        if reply.get('status')!='ok':return {'status':'failed'}
        with Image.open(image_folder/pathlib.Path(reply['image']).name) as image:source=image.convert('RGBA')
    source_key=save_image(source);width,height=source.size
    if mode=='original':result=source
    else:
        scale=max(512/width,512/height)*(zoom if mode=='crop' else 1)
        size=(max(512,round(width*scale)),max(512,round(height*scale)))
        enlarged=source.resize(size,Image.Resampling.LANCZOS)
        left=round((size[0]-512)*(x if mode=='crop' else .5));top=round((size[1]-512)*(y if mode=='crop' else .5))
        crop=enlarged.crop((left,top,left+512,top+512))
        result=Image.new('RGBA',(512,512),(22,22,22,255));result.alpha_composite(crop)
        if mode=='fit':
            result=ImageEnhance.Brightness(result.filter(ImageFilter.GaussianBlur(12.5))).enhance(.65)
            fit=min(512/width,512/height);foreground=source.resize((max(1,round(width*fit)),max(1,round(height*fit))),Image.Resampling.LANCZOS)
            result.alpha_composite(foreground,((512-foreground.width)//2,(512-foreground.height)//2))
    final_key=save_image(result)
    return {'status':'ok','image':'preview/generated-images/'+final_key+'.png','sourceImage':'preview/generated-images/'+source_key+'.png',
            'finalImageKey':final_key,'sourceImageKey':source_key,'artworkMode':mode,'cropX':x,'cropY':y,'artworkZoom':zoom,'discordArtworkState':'unverified' if mode=='original' else 'localOnly'}
