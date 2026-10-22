# Lists the escape sequences in a raw terminal capture, skipping plain cursor/color/erase ones.
import re
import sys
from collections import Counter

data = open(sys.argv[1], 'rb').read()
pattern = re.compile(rb'\x1b(?:\[[0-?]*[ -/]*[@-~]|\][^\x07\x1b]*(?:\x07|\x1b\\)|[P_^][^\x1b]*\x1b\\|.)', re.S)
boring = re.compile(rb'\x1b\[[0-9;]*[mHGKJCDABXd]$')
counts = Counter(s for s in pattern.findall(data) if not boring.match(s))
print(len(data), 'bytes')
for seq, n in counts.most_common(60):
    print(n, seq)
