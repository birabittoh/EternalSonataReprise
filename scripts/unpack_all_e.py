"""Bulk-decode every .e asset listed in index.vmtoc into extracted/e/."""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unpack_e import ROOT, load_toc, unpack

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(REPO, 'extracted', 'e')


def main():
    toc = load_toc()
    names = sorted(n for n in toc if n.endswith('.e'))
    ok = bad = 0
    for rel in names:
        size, flag = toc[rel]
        src = os.path.join(ROOT, rel.replace('/', os.sep))
        if not os.path.exists(src):
            print('MISSING  %s' % rel)
            bad += 1
            continue
        out = unpack(open(src, 'rb').read(), size, flag)
        dst = os.path.join(DEST, rel.replace('/', os.sep))
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, 'wb') as f:
            f.write(out)
        if len(out) == size:
            ok += 1
        else:
            bad += 1
            print('SHORT    %s flag=%d expected=%d got=%d' % (rel, flag, size, len(out)))
    print('\n%d/%d decoded to full size (%d bad) -> %s' % (ok, len(names), bad, DEST))


if __name__ == '__main__':
    main()
