import math, random
from angel import make_angel
from PIL import Image, ImageDraw, ImageFilter, ImageOps
S=2; W,H=480*S,320*S; N=72; TAU=2*math.pi; K=3   # K = extra supersample for sprites
random.seed(7)

def dark(c,f=.55): return tuple(int(v*f) for v in c)
def light(c,f=.35): return tuple(int(v+(255-v)*f) for v in c)

def rot(pt,piv,a):
    x,y=pt[0]-piv[0],pt[1]-piv[1]
    return (piv[0]+x*math.cos(a)-y*math.sin(a),piv[1]+x*math.sin(a)+y*math.cos(a))

def make_fish(sp,tail_a,fin_a):
    L=sp['L']*S*K
    cw,ch=int(L*1.95),int(L*1.7)
    xn=L*1.5; yc=ch/2                      # nose x, centreline y
    spr=Image.new("RGBA",(cw,ch),(0,0,0,0))
    def prof(u,top=True):
        h=sp['H']*(1 if top else sp.get('belly',1))*L
        return h*(sp['pw']+(1-sp['pw'])*math.sin(math.pi*min(1,u)**sp['a'])**sp['b'])
    us=[i/60 for i in range(61)]
    topp=[(xn-u*L,yc-prof(u,True)) for u in us]
    botp=[(xn-u*L,yc+prof(u,False)) for u in us[::-1]]
    body=topp+botp
    xp=xn-L; pivot=(xp,yc)
    # --- tail
    tl=Image.new("RGBA",(cw,ch),(0,0,0,0)); td=ImageDraw.Draw(tl)
    tc=sp['tailc']
    n=len(sp['tail'])
    pts=[pivot]+[rot((xp-bx*L,yc+by*L),pivot,tail_a) for bx,by in sp['tail']]
    td.polygon(pts,fill=tc+(sp.get('tail_alpha',235),))
    for bx,by in sp['tail']:
        q=rot((xp-bx*L,yc+by*L),pivot,tail_a)
        td.line([pivot,q],fill=dark(tc,.7)+(110,),width=max(1,K))
    td.line(pts[1:],fill=dark(tc,.6)+(200,),width=K)
    spr.alpha_composite(tl)
    # --- dorsal / anal fins
    def fin(u1,u2,hgt,col,top,sweep=.0):
        f=Image.new("RGBA",(cw,ch),(0,0,0,0)); fd=ImageDraw.Draw(f)
        base=[(xn-u*L,(yc-prof(u,True)+K) if top else (yc+prof(u,False)-K)) for u in [u1+(u2-u1)*i/12 for i in range(13)]]
        tip=[]
        for i,(bx,by) in enumerate(base):
            t=i/12; hh=hgt*L*math.sin(math.pi*t**0.8)**0.9
            tip.append((bx-sweep*L*t,by+(-hh if top else hh)))
        fd.polygon(base+tip[::-1],fill=col+(225,))
        for (bx,by),(tx,ty) in list(zip(base,tip))[1::2]:
            fd.line([(bx,by),(tx,ty)],fill=dark(col,.7)+(120,),width=max(1,K//2*2))
        fd.line(tip,fill=dark(col,.6)+(190,),width=K)
        spr.alpha_composite(f)
    for fn in sp.get('fins',[]): fin(*fn)
    # --- body
    ys=[p[1] for p in body]; y0,y1=int(min(ys)),int(max(ys))+1
    mask=Image.new("L",(cw,ch),0); ImageDraw.Draw(mask).polygon(body,fill=255)
    grad=Image.new("L",(cw,ch),0)
    g=Image.linear_gradient("L").resize((cw,max(1,y1-y0)))
    grad.paste(g,(0,y0)); grad.paste(255,(0,y1,cw,ch))
    bodyimg=Image.composite(Image.new("RGB",(cw,ch),sp['belly_c']),Image.new("RGB",(cw,ch),sp['back']),grad)
    layer=bodyimg.convert("RGBA")
    dl=ImageDraw.Draw(layer)
    for st in sp.get('stripes',[]):
        u,w,col=st[:3]; edge=st[3] if len(st)>3 else None
        x=xn-u*L; ww=w*L
        if edge: dl.polygon([(x-ww*.15,0),(x+ww*1.15,0),(x+ww*.85,ch),(x-ww*.15-ww*.3,ch)],fill=edge)
        dl.polygon([(x,0),(x+ww,0),(x+ww-ww*.3,ch),(x-ww*.3,ch)],fill=col)
    if sp.get('lateral'):
        c,u1,u2,wd=sp['lateral']
        dl.line([(xn-u1*L,yc-sp.get('lat_y',0)*L),(xn-u2*L,yc-sp.get('lat_y',0)*L+K)],fill=c,width=int(wd*L))
    if sp.get('rear_red'):
        dl.polygon([(xn-.5*L,0),(xn-L,0),(xn-L,ch),(xn-.5*L,ch)],fill=sp['rear_red'])
    # soft top shine
    shine=Image.new("RGBA",(cw,ch),(0,0,0,0)); sd=ImageDraw.Draw(shine)
    sd.ellipse([xn-.85*L,yc-prof(.35)*.95,xn-.2*L,yc-prof(.35)*.35],fill=(255,255,255,70))
    shine=shine.filter(ImageFilter.GaussianBlur(L*.03))
    layer.alpha_composite(shine)
    bl=Image.new("RGBA",(cw,ch),(0,0,0,0)); bl.paste(layer,(0,0),mask)
    spr.alpha_composite(bl)
    ImageDraw.Draw(spr).line(body+[body[0]],fill=dark(sp['back'],.45)+(255,),width=int(K*1.5),joint="curve")
    # --- pectoral fin
    pf=Image.new("RGBA",(cw,ch),(0,0,0,0)); pd=ImageDraw.Draw(pf)
    px,py=xn-.3*L,yc+prof(.3,False)*.1
    fl=sp.get('pec',.2)*L
    shp=[(0,0),(-fl*.5,-fl*.35),(-fl,-fl*.1),(-fl*.9,fl*.35),(-fl*.3,fl*.3)]
    shp=[rot((px+a,py+b),(px,py),fin_a) for a,b in shp]
    pd.polygon(shp,fill=sp['pecc']+(150,),outline=dark(sp['pecc'],.6)+(220,))
    spr.alpha_composite(pf)
    # --- gill + mouth
    d=ImageDraw.Draw(spr)
    gx=xn-.27*L; gh=prof(.27)
    d.arc([gx-gh*.5,yc-gh*.95,gx+gh*.5,yc+gh*.95],110,250,fill=dark(sp['back'],.5)+(255,),width=K)
    d.line([(xn-.02*L,yc+L*.03),(xn-.06*L,yc+L*.05)],fill=dark(sp['back'],.4)+(255,),width=K)
    # --- eye
    ex,ey=xn-.14*L,yc-sp.get('eye_y',.07)*L; er=sp.get('eye',.055)*L
    d.ellipse([ex-er*1.25,ey-er*1.25,ex+er*1.25,ey+er*1.25],fill=dark(sp['back'],.4)+(255,))
    d.ellipse([ex-er,ey-er,ex+er,ey+er],fill=sp.get('iris',(250,250,240))+(255,))
    d.ellipse([ex-er*.6+K,ey-er*.6,ex+er*.6+K,ey+er*.6],fill=(10,10,15,255))
    d.ellipse([ex-er*.1,ey-er*.55,ex+er*.35,ey-er*.1],fill=(255,255,255,255))
    return spr.resize((cw//K,ch//K),Image.LANCZOS)

SP={}
SP['clown']=dict(L=44,H=.26,pw=.18,a=.8,b=.55,belly=.9,back=(255,110,15),belly_c=(255,170,60),
  stripes=[(.28,.07,(250,250,250),(10,10,10)),(.58,.09,(250,250,250),(10,10,10)),(.88,.05,(250,250,250),(10,10,10))],
  tailc=(255,120,25),tail=[(.1,-.16),(.22,-.12),(.26,0),(.22,.12),(.1,.16)],
  fins=[(.25,.85,.17,(255,115,20),True),(.55,.82,.1,(255,115,20),False)],pecc=(255,140,40),pec=.2,eye=.055)
SP['tang']=dict(L=48,H=.34,pw=.12,a=.75,b=.6,belly=.95,back=(20,70,220),belly_c=(60,130,250),
  lateral=((10,15,60),.82,.18,.07),lat_y=.02,
  tailc=(255,215,30),tail=[(.14,-.22),(.26,-.12),(.22,0),(.26,.12),(.14,.22)],
  fins=[(.15,.85,.11,(25,80,225),True,.05),(.3,.8,.1,(25,80,225),False,.05)],pecc=(255,225,50),pec=.2,eye=.05)
SP['angel']=dict(L=34,H=.42,pw=.1,a=1.0,b=.6,belly=1,back=(190,200,210),belly_c=(240,240,235),
  stripes=[(.22,.07,(30,30,40)),(.52,.07,(35,35,45)),(.78,.06,(40,40,50))],
  tailc=(200,210,220),tail=[(.18,-.2),(.3,-.09),(.28,0),(.3,.09),(.18,.2)],tail_alpha=170,
  fins=[(.2,.85,.62,(190,200,215),True,.3),(.25,.8,.55,(190,200,215),False,.3)],pecc=(215,220,230),pec=.25,eye=.05,iris=(255,150,40))
SP['gold']=dict(L=40,H=.3,pw=.2,a=.85,b=.55,belly=.95,back=(240,95,15),belly_c=(255,170,50),
  tailc=(255,120,25),tail=[(.35,-.38),(.5,-.28),(.42,-.12),(.55,0),(.42,.12),(.5,.28),(.35,.38)],tail_alpha=190,
  fins=[(.2,.8,.2,(255,120,25),True)],pecc=(255,150,50),pec=.22,eye=.055)
SP['neon']=dict(L=26,H=.17,pw=.15,a=.85,b=.55,belly=.85,back=(60,100,140),belly_c=(225,225,225),
  lateral=((40,210,255),.85,.15,.045),lat_y=.04,rear_red=(235,35,45),
  tailc=(200,210,230),tail=[(.1,-.12),(.2,-.08),(.18,0),(.2,.08),(.1,.12)],tail_alpha=170,
  fins=[],pecc=(200,210,230),pec=.14,eye=.07)

bg=Image.new("RGB",(W,H)); d=ImageDraw.Draw(bg)
for y in range(H):
    t=y/H; d.line([(0,y),(W,y)],fill=(int(20+10*(1-t)),int(110-60*t),int(190-90*t)))
for y in range(H-50*S,H):
    t=(y-(H-50*S))/(50*S); d.line([(0,y),(W,y)],fill=(int(214-40*t),int(190-40*t),int(140-40*t)))
for _ in range(500): d.point((random.randint(0,W),random.randint(H-48*S,H)),fill=(150,125,90))
for cx,cy,rx,ry,c in [(90,296,32,18,(95,95,105)),(120,302,22,12,(120,118,125)),(380,300,38,20,(90,90,100))]:
    d.ellipse([(cx-rx)*S,(cy-ry)*S,(cx+rx)*S,(cy+ry)*S],fill=c)
    d.ellipse([(cx-rx*.6)*S,(cy-ry*.8)*S,(cx+rx*.1)*S,(cy-ry*.1)*S],fill=tuple(min(255,v+30) for v in c))

# (species, cx, ax, cy, ay, phase, laps)
route=[('tang',240,175,105,18,0,1),('clown',240,150,205,14,2,1),('angel',240,185,150,30,4,1),
       ('gold',240,150,255,8,1,2),('neon',240,190,70,10,3,1),('neon',240,170,85,8,3.4,1),('neon',240,185,98,9,2.7,1)]
bub=[(random.choice([165,290,415])+random.randint(-14,14),random.random(),random.randint(2,6),random.random()*TAU,random.choice([1,2])) for _ in range(22)]
kel=[(40,150,0),(52,110,1.5),(430,170,2),(445,120,.5),(300,90,3),(170,100,4)]
def pos(r,p):
    _,cx,ax,cy,ay,ph,laps=r; a=TAU*laps*p+ph
    return cx+ax*math.sin(a), cy+ay*math.sin(2*a+ph), (1 if math.cos(a)>0 else -1)
import sys
frames=[]
for n in range(N):
    p=n/N
    img=bg.copy()
    ray=Image.new("RGBA",(W,H),(0,0,0,0)); rd=ImageDraw.Draw(ray)
    for x0,w,ph in [(80,40,0),(200,60,1.5),(330,35,3),(420,50,4.5)]:
        x0=(x0+25*math.sin(TAU*p+ph))*S; w*=S
        rd.polygon([(x0,0),(x0+w,0),(x0+w-70*S,H),(x0-40*S,H)],fill=(255,255,255,24))
    ray=ray.filter(ImageFilter.GaussianBlur(8)); img.paste(ray,(0,0),ray)
    d=ImageDraw.Draw(img)
    for x,h,ph in kel:
        pts=[((x+9*math.sin(i/22+ph-TAU*p))*S,(300-i)*S) for i in range(0,h,3)]
        d.line(pts,fill=(30,150,80),width=5*S//2)
    for i,r in enumerate(route):
        x,y,f=pos(r,p); x2,y2,_=pos(r,p+.004)
        tail=math.sin(TAU*p*6+r[5]+i)*.35
        fa=math.sin(TAU*p*6+i)*.35
        spr=make_angel(48*S,tail,fa,K=K) if r[0]=='angel' else make_fish(SP[r[0]],tail,fa)
        vx=(x2-x); vy=(y2-y)
        ang=math.atan2(vy,abs(vx)+1e-6)*(.2 if r[0]=='angel' else .6)
        if f<0: spr=ImageOps.mirror(spr)
        spr=spr.rotate(-math.degrees(ang)*(1 if f>0 else -1),resample=Image.BICUBIC,expand=True)
        # sprite nose is at x=1.5/1.95 of width; centre body on (x,y)
        img.paste(spr,(int(x*S-spr.width/2),int(y*S-spr.height/2)),spr)
    bl=Image.new("RGBA",(W,H),(0,0,0,0)); bd=ImageDraw.Draw(bl)
    for bx,off,r,ph,sp_ in bub:
        y=300-((p*sp_+off)%1)*290; x=bx+5*math.sin(TAU*p*sp_*2+ph)
        bd.ellipse([(x-r)*S,(y-r)*S,(x+r)*S,(y+r)*S],outline=(255,255,255,200),fill=(255,255,255,40),width=S)
        bd.ellipse([(x-r*.4)*S,(y-r*.5)*S,(x-r*.1)*S,(y-r*.2)*S],fill=(255,255,255,200))
    img.paste(bl,(0,0),bl)
    if n==0: img.save("fish2_still.png")
    img.save(f"f2_{n:03d}.png")
