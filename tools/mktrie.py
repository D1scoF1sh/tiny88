"""Pick the most common question prefixes (from the training question
generator) and write PFXNODES.BIN: the trie the DOS program walks.
Node table in BFS order so every node's children are contiguous and sorted."""
import sys, random, collections, struct
sys.path.insert(0, sys.argv[1])          # training dir
import gen2
MAXQ = 72
def clean(line):                         # same as clean() in TINY88.C
    q = ''.join(c for c in line if 32 <= ord(c) <= 126)[:MAXQ].rstrip(' ')
    if any(c.isupper() for c in q) and not any(c.islower() for c in q):
        q = q.lower()
    return q
N, budget = int(sys.argv[2]), int(sys.argv[3])
rng = random.Random(2)
cnt = collections.Counter()
extra = [l.strip() for l in open(sys.argv[5])] if len(sys.argv) > 5 else []
for i in range(N):
    q, a, k = gen2.sample(rng)
    q = clean(q)
    for n in range(1, len(q) + 1): cnt[q[:n]] += 1
for q in extra:                            # README / demo questions: always whole
    q = clean(q)
    for n in range(1, len(q) + 1): cnt[q[:n]] += 10 ** 9
# keep most frequent prefixes; a prefix's count <= its parent's, so the kept
# set is closed under taking prefixes (ties broken by length)
keep = sorted(cnt, key=lambda s: (-cnt[s], len(s)))[:budget]
keep = set(keep)
keep = {s for s in keep if all(s[:n] in keep for n in range(1, len(s)))}
children = collections.defaultdict(list)
for s in keep: children[s[:-1]].append(s)
order = ['']; i = 0
while i < len(order):
    order += sorted(children[order[i]], key=lambda s: s[-1]); i += 1
idx = {s: n for n, s in enumerate(order)}
out = open(sys.argv[4], 'wb')
out.write(b'T88N' + struct.pack('<H', len(order)))
for s in order:
    ch = children[s]
    first = idx[sorted(ch, key=lambda t: t[-1])[0]] if ch else 0
    out.write(struct.pack('<HBB', first, len(ch), ord(s[-1]) if s else 0))
print('nodes', len(order), 'max depth', max(len(s) for s in order))
tot = sum(cnt[s] for s in keep if len(s) and cnt[s] < 10**9)
print('top prefixes:', sorted(keep, key=lambda s: (-len(s) * min(cnt[s], 10**6)))[:15])
