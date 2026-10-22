# Prints rows of width-tricky sequences, then overwrites parts of each row by absolute position,
# the way Claude's renderer redraws. Run it directly and inside clod: the screens must match.
import sys
import time

samples = [
    'warn ⚠️', 'check ✅', 'tick ✔️', 'accent é',
    'family \U0001f468‍\U0001f469‍\U0001f467', 'thumb \U0001f44d\U0001f3fd', 'flag \U0001f1fa\U0001f1f8',
    'keycap 1️⃣', 'heart ❤️', 'cjk 中文', 'claude ⏺ ⎿ ✻ ✳',
    'trigram ☰', 'new emoji \U0001fac8',
]
out = sys.stdout
out.write('\x1b[2J\x1b[H')
for i, s in enumerate(samples):
    out.write('\x1b[%d;1H%s|tail-%d' % (i + 1, s, i))
out.flush()
time.sleep(0.3)
# Partial redraws at fixed columns, after the tricky glyphs.
for i in range(len(samples)):
    out.write('\x1b[%d;22H#%02d' % (i + 1, i))
    out.write('\x1b[%d;30H@' % (i + 1))
out.write('\x1b[%d;1H' % (len(samples) + 1))
out.flush()
time.sleep(3)
