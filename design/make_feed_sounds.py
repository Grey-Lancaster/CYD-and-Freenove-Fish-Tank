# Candidate fish-feeding sounds (16 kHz mono wav), for auditioning.
import math, random, struct, wave
SR=16000; random.seed(5)
def save(name, buf, peak=9000):
    m=max(abs(v) for v in buf) or 1
    pcm=[int(v/m*peak) for v in buf]
    with wave.open(f"design/{name}.wav","wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(SR)
        w.writeframes(struct.pack("<%dh"%len(pcm),*pcm))
def ding(buf,t0,f,dur=0.6,amp=1.0):
    for k in range(int(dur*SR)):
        t=k/SR; env=math.exp(-t/0.12)*min(1,k/(0.002*SR))
        s=math.sin(2*math.pi*f*t)+0.35*math.sin(2*math.pi*f*2.76*t)*math.exp(-t/0.05)+0.2*math.sin(2*math.pi*f*5.4*t)*math.exp(-t/0.02)
        i=t0+k
        if i<len(buf): buf[i]+=amp*env*s
def plop(buf,t0,f0,dur=0.12,amp=1.0):
    ph=0
    for k in range(int(dur*SR)):
        t=k/SR; f=f0*(1+5*t); ph+=2*math.pi*f/SR
        env=min(1,k/(0.002*SR))*math.exp(-t/(dur/3))
        i=t0+k
        if i<len(buf): buf[i]+=amp*env*math.sin(ph)
def tick(buf,t0,amp=1.0):
    lp=0
    for k in range(int(0.05*SR)):
        t=k/SR; lp+=(random.uniform(-1,1)-lp)*0.5
        i=t0+k
        if i<len(buf): buf[i]+=amp*lp*math.exp(-t/0.008)
# 1. ding ding
b=[0.0]*int(1.3*SR); ding(b,0,1320); ding(b,int(0.28*SR),1760,amp=0.9); save("feed_ding_ding",b)
# 2. water plop plop
b=[0.0]*int(0.9*SR); plop(b,0,700); plop(b,int(0.17*SR),950,amp=0.8); plop(b,int(0.33*SR),600,amp=0.6); save("feed_plop",b)
# 3. flakes: tick-tick-tick then tiny splash
b=[0.0]*int(0.9*SR)
for i,d in enumerate([0,0.05,0.09,0.16,0.2,0.27]): tick(b,int(d*SR),amp=1.0-0.1*i)
plop(b,int(0.3*SR),1100,dur=0.08,amp=0.8); save("feed_sprinkle",b)
