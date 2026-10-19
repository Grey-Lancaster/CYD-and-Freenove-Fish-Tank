import math
from PIL import Image, ImageDraw, ImageFilter

def rot(pt,piv,a):
    x,y=pt[0]-piv[0],pt[1]-piv[1]
    return (piv[0]+x*math.cos(a)-y*math.sin(a),piv[1]+x*math.sin(a)+y*math.cos(a))

def cat(pts,n=8):
    out=[]
    for i in range(len(pts)-1):
        p0=pts[max(0,i-1)];p1=pts[i];p2=pts[i+1];p3=pts[min(len(pts)-1,i+2)]
        for j in range(n):
            t=j/n;t2=t*t;t3=t2*t
            out.append(tuple(.5*((2*p1[k])+(-p0[k]+p2[k])*t+(2*p0[k]-5*p1[k]+4*p2[k]-p3[k])*t2+(-p0[k]+3*p1[k]-3*p2[k]+p3[k])*t3) for k in (0,1)))
    out.append(pts[-1]); return out

GRAY=(118,124,136); GRAYD=(70,75,88)

def make_angel(Lpx,tail_a,fin_a,K=3):
    L=Lpx*K
    cw,ch=int(L*2.2),int(L*2.6)
    xn=L*1.55; yc=ch*.5
    P=lambda u,v:(xn-u*L,yc-v*L)
    pivot=P(1.0,0)
    spr=Image.new("RGBA",(cw,ch),(0,0,0,0))
    # ---- tail: broad rounded fan
    tl=Image.new("RGBA",(cw,ch),(0,0,0,0)); td=ImageDraw.Draw(tl)
    ang=[-52+104*i/14 for i in range(15)]
    tpts=[]
    for i,a in enumerate(ang):
        r=.52+(.03 if i%2 else 0)           # slight scalloped edge
        tpts.append(rot(P(1.0+r*math.cos(math.radians(a)),r*math.sin(math.radians(a))),pivot,tail_a))
    td.polygon([rot(P(.97,.05),pivot,tail_a)]+tpts+[rot(P(.97,-.05),pivot,tail_a)],fill=GRAY+(235,))
    for q in tpts[1:-1]: td.line([pivot,q],fill=GRAYD+(120,),width=max(1,K))
    td.line(tpts,fill=GRAYD+(230,),width=int(K*1.3))
    spr.alpha_composite(tl)
    D=ImageDraw.Draw(spr)
    # ---- dorsal & anal fins (gray sickles)
    dl=cat([(.24,.30),(.36,.52),(.54,.78),(.66,1.0)])+cat([(.66,1.0),(.72,.78),(.80,.50),(.90,.24),(1.0,.08)])
    dors=[P(*p) for p in dl]+[P(.8,.2),P(.5,.4)]
    al=cat([(.30,-.34),(.42,-.58),(.54,-.78),(.62,-1.0)])+cat([(.62,-1.0),(.70,-.76),(.78,-.52),(.88,-.26),(1.0,-.08)])
    anal=[P(*p) for p in al]+[P(.8,-.18),P(.5,-.4)]
    # pelvic filament: long tapered ribbon
    sw=math.sin(fin_a)*.05
    pel=cat([(.34,-.40),(.40,-.62),(.46+sw,-.85),(.50+sw*2,-1.12)])
    pelL=[P(u,v) for u,v in pel]; pelR=[P(u+.05,v+.0) for u,v in pel[:-1]][::-1]
    for poly in (dors,anal): D.polygon(poly,fill=GRAY+(255,))
    D.polygon(pelL+pelR,fill=GRAY+(255,))
    for poly in (dors,anal,pelL+pelR): D.line(poly+[poly[0]],fill=GRAYD+(255,),width=int(K*1.1))
    # ---- body (cream top, white belly)
    top=cat([(0,0),(.08,.16),(.22,.36),(.42,.52),(.60,.52),(.80,.28),(1.0,.08)])
    bot=cat([(1.0,-.08),(.80,-.24),(.60,-.46),(.40,-.50),(.20,-.34),(.08,-.14),(0,0)])
    body=[P(*p) for p in top+bot]
    mask=Image.new("L",(cw,ch),0); ImageDraw.Draw(mask).polygon(body,fill=255)
    layer=Image.new("RGBA",(cw,ch),(0,0,0,255)); ld=ImageDraw.Draw(layer)
    for y in range(ch):
        t=min(1,max(0,(y-(yc-.32*L))/(.34*L)))
        c=(int(236+(248-236)*t),int(224+(248-224)*t),int(176+(246-176)*t))
        ld.line([(0,y),(cw,y)],fill=c)
    bl=Image.new("RGBA",(cw,ch),(0,0,0,0)); bl.paste(layer,(0,0),mask); spr.alpha_composite(bl)
    D.line(body+[body[0]],fill=(95,98,105,255),width=int(K*1.1))
    # redraw body over fin bases so fins tuck behind it
    spr2=Image.new("RGBA",(cw,ch),(0,0,0,0)); spr2.paste(layer,(0,0),mask)
    # ---- thin black stripes through body and fin bases
    st=Image.new("RGBA",(cw,ch),(0,0,0,0)); sd=ImageDraw.Draw(st)
    union=Image.new("L",(cw,ch),0); ud=ImageDraw.Draw(union)
    for poly in (dors,anal,body): ud.polygon(poly,fill=255)
    for u,w,v1,v2,lean in [(.19,.022,.26,-.2,.0),(.36,.024,.50,-.44,.04),(.50,.024,.56,-.52,.04),(.64,.022,.52,-.48,.04)]:
        x1,y1=P(u,v1);x2,y2=P(u+lean,v2);ww=w*L
        sd.polygon([(x1,y1),(x1-ww,y1),(x2-ww,y2),(x2,y2)],fill=(20,20,26,255))
    sm=Image.new("RGBA",(cw,ch),(0,0,0,0)); sm.paste(st,(0,0),union); spr.alpha_composite(sm)
    D.line(body+[body[0]],fill=(95,98,105,255),width=int(K*1.1))
    # ---- pectoral fin, gill, mouth, eye
    px,py=P(.30,-.02); fl=.2*L
    shp=[(0,0),(-fl*.6,-fl*.35),(-fl*1.1,-fl*.05),(-fl*.9,fl*.35),(-fl*.3,fl*.25)]
    shp=[rot((px+a,py+b),(px,py),fin_a*.9) for a,b in shp]
    D.polygon(shp,fill=(235,238,242,150),outline=(110,114,125,210))
    gx,gy=P(.30,0)
    D.arc([gx-L*.07,gy-L*.22,gx+L*.07,gy+L*.22],110,250,fill=(110,114,125,255),width=K)
    D.line([P(.012,-.004),P(.05,-.025)],fill=(70,55,55,255),width=K)
    ex,ey=P(.115,.055); er=.034*L
    D.ellipse([ex-er*1.35,ey-er*1.35,ex+er*1.35,ey+er*1.35],fill=(25,22,28,255))
    D.ellipse([ex-er,ey-er,ex+er,ey+er],fill=(225,40,45,255))
    D.ellipse([ex-er*.5+K*.5,ey-er*.5,ex+er*.5+K*.5,ey+er*.5],fill=(10,5,8,255))
    D.ellipse([ex-er*.2,ey-er*.65,ex+er*.3,ey-er*.15],fill=(255,255,255,255))
    return spr.resize((cw//K,ch//K),Image.LANCZOS)

if __name__=="__main__":
    bg=Image.new("RGB",(600,420),(24,90,160))
    for i,ta in enumerate((-.25,0,.25)):
        s=make_angel(110,ta,ta,K=3); bg.paste(s,(0+i*195,10),s)
    bg.save("angel_test.png")
