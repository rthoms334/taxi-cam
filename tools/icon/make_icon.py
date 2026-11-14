# Usage: python tools/icon/make_icon.py src/app/taxi-cam.ico [preview.png]
# Draws the Taxi Cam icon at each shell size: a dark tile with the hold-short
# marking (two dashed lines over two solid ones) and TAXI in plain strokes.
import sys
from PIL import Image, ImageDraw, ImageChops
TILE=(18,20,23,255); YELLOW=(242,194,48,255)
SIZES=[16,20,24,32,40,48,64,128,256]
def draw(size):
    k=8; n=size*k
    def aa(fn):
        m=Image.new('L',(n,n),0); fn(ImageDraw.Draw(m)); return m.resize((size,size),Image.BOX)
    radius=size*56/256
    inner=aa(lambda d:d.rounded_rectangle((0,0,n-1,n-1),radius*k,fill=255))
    img=Image.new('RGBA',(size,size),(0,0,0,0))
    img.paste(Image.new('RGBA',(size,size),TILE),(0,0),inner)
    # The marking, on whole pixels so the lines stay crisp.
    small=size<=24
    marks=Image.new('L',(size,size),0); d=ImageDraw.Draw(marks)
    t=max(1,round(size*(22 if small else 13)/256)); gap=max(1,round(size*(16 if small else 9)/256))
    y=round(size*(46 if small else 40)/256)
    dash=max(2,round(size*30/256)); space=max(1,round(size*16/256))
    rows=[True,False] if small else [True,True,False,False]
    for dashed in rows:
        if dashed:
            x=(size-((size+space)//(dash+space))*(dash+space)+space)//2
            while x<size:
                d.rectangle((x,y,x+dash-1,y+t-1),fill=255); x+=dash+space
        else:
            d.rectangle((0,y,size-1,y+t-1),fill=255)
        y+=t+gap
    marks=ImageChops.multiply(marks,inner)
    img.paste(Image.new('RGBA',(size,size),YELLOW),(0,0),marks)
    # TAXI: one stroke weight, drawn large and reduced.
    top=(y+max(1,round(size*10/256)))*k; bottom=round(n*(216 if not small else 222)/256); h=bottom-top
    w=max(1.25*k,h*0.2)  # stroke
    lw=h*0.66; iw=w; sp=h*0.17
    total=lw*3+iw+sp*3
    if total>n*0.8:  # The smallest sizes: narrower letters rather than clipped ones.
        f=(n*0.8-iw)/(total-iw); lw*=f; sp*=f; total=lw*3+iw+sp*3
    x0=(n-total)/2
    def letters(d):
        x=x0
        # T
        d.rectangle((x,top,x+lw,top+w),fill=255); d.rectangle((x+lw/2-w/2,top,x+lw/2+w/2,bottom),fill=255); x+=lw+sp
        # A
        ax=x+lw/2
        d.polygon([(ax-w*0.55,top),(ax+w*0.55,top),(x+lw,bottom),(x+lw-w*1.05,bottom),(ax,top+w*1.1),(x+w*1.05,bottom),(x,bottom)],fill=255)
        bar=bottom-h*0.34; d.rectangle((x+lw*0.2,bar,x+lw*0.8,bar+w*0.9),fill=255); x+=lw+sp
        # X
        d.polygon([(x,top),(x+w*1.1,top),(x+lw,bottom),(x+lw-w*1.1,bottom)],fill=255)
        d.polygon([(x+lw,top),(x+lw-w*1.1,top),(x,bottom),(x+w*1.1,bottom)],fill=255); x+=lw+sp
        # I
        d.rectangle((x,top,x+iw,bottom),fill=255)
    text=ImageChops.multiply(aa(letters),inner)
    img.paste(Image.new('RGBA',(size,size),YELLOW),(0,0),text)
    return img
if __name__=='__main__':
    out=sys.argv[1]; images=[draw(s) for s in SIZES]
    images[-1].save(out,format='ICO',sizes=[(s,s) for s in SIZES],append_images=images[:-1],bitmap_format='bmp')
    if len(sys.argv)>2:
        sheet=Image.new('RGB',(256*4,256),(68,68,68))
        for i,s in enumerate([256,48,32,16]):
            im=draw(s).resize((256,256),Image.NEAREST); sheet.paste(im,(i*256,0),im)
        sheet.save(sys.argv[2])
