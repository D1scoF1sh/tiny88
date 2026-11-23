import numpy as np, struct, sys
VOCAB, CTX = 97, 128
def load(path):
    b = open(path,'rb').read(); assert b[:4]==b'TNY2'
    D,L,H,HID = struct.unpack('<4H', b[4:12]); o=12
    def table(rows, cols):
        nonlocal o
        sc=np.zeros(rows,np.float32); w=np.zeros((rows,cols),np.int8)
        for r in range(rows):
            sc[r]=struct.unpack('<f',b[o:o+4])[0]; o+=4
            w[r]=np.frombuffer(b[o:o+cols],np.int8); o+=cols
        return sc,w
    def gains():
        nonlocal o
        g=np.frombuffer(b[o:o+4*D],np.float32).copy(); o+=4*D; return g
    m=dict(D=D,L=L,H=H,HID=HID)
    m['emb']=table(VOCAB,D); m['pos']=table(CTX,D); m['layers']=[]
    for l in range(L):
        ly={}
        ly['g1']=gains()
        for k in 'q','k','v','o': ly[k]=table(D,D)
        ly['g2']=gains(); ly['f1']=table(HID,D); ly['f2']=table(D,HID)
        m['layers'].append(ly)
    m['gf']=gains(); assert o==len(b)
    return m
def deq(t): return t[1].astype(np.float32)*t[0][:,None]
class Ref:
    def __init__(s,m):
        s.m=m; s.D=m['D']; s.H=m['H']; s.HD=s.D//s.H
        s.E=deq(m['emb']); s.P=deq(m['pos'])
        s.W=[{k:deq(ly[k]) for k in 'qkvo'}|{'f1':deq(ly['f1']),'f2':deq(ly['f2']),'g1':ly['g1'],'g2':ly['g2']} for ly in m['layers']]
        s.stats={}
    def reset(s): s.K=[[] for _ in s.W]; s.V=[[] for _ in s.W]
    def st(s,k,v):
        s.stats[k]=max(s.stats.get(k,0),float(np.abs(v).max()))
    def store(s,v):  # mimic int8 kv cache
        big=max(np.abs(v).max(),1e-20); q=np.round(v*127/big).astype(np.float32)
        q=np.where(v*127/big<0, np.ceil(v*127/big-0.5), np.floor(v*127/big+0.5))
        return q*big/127
    def norm(s,x,g): return x/np.sqrt((x*x).mean()+1e-5)*g
    def step(s,tok,p):
        x=s.E[tok]+s.P[p]; s.st('x0',x)
        for l,W in enumerate(s.W):
            h=s.norm(x,W['g1']); s.st('h',h)
            k=W['k']@h; v=W['v']@h; q=W['q']@h
            s.st('q',q); s.st('k',k); s.st('v',v)
            s.K[l].append(s.store(k)); s.V[l].append(s.store(v))
            K=np.array(s.K[l]); V=np.array(s.V[l]); a=np.zeros(s.D,np.float32)
            for hh in range(s.H):
                sl=slice(hh*s.HD,(hh+1)*s.HD)
                sc=K[:,sl]@q[sl]/np.sqrt(s.HD); s.st('score',sc)
                e=np.exp(sc-sc.max()); e/=e.sum(); a[sl]=e@V[:,sl]
            o=W['o']@a; x=x+o; s.st('x',x)
            h=s.norm(x,W['g2']); f=np.maximum(W['f1']@h,0); s.st('f',f)
            x=x+W['f2']@f; s.st('x',x)
        return s.E@s.norm(x,s.m['gf'])
    def answer(s,q):
        s.reset(); p=0
        for c in q: s.step(ord(c)-30,p); p+=1
        sc=s.step(1,p); p+=1; out=''
        while p<CTX:
            t=int(np.argmax(sc))
            if t<=1: break
            out+=chr(t+30); sc=s.step(t,p); p+=1
        return out
if __name__=='__main__':
    m=load(sys.argv[1]); r=Ref(m)
    for q in sys.argv[2:]: print(q,'=>',r.answer(q))
    print(r.stats)
