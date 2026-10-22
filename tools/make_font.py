# Builds clod's bundled font, "Iosevka Clod": Iosevka Term (SIL OFL 1.1, no Reserved Font Name) with
# only its family name changed, so it installs next to any Iosevka the user already has.
# Needs fontTools (pip install fonttools).
# usage: python tools/make_font.py <IosevkaTerm-Regular.ttf> <IosevkaTerm-Bold.ttf> <out dir>
import os, sys
from fontTools.ttLib import TTFont

def rename(src, dst, style):
    f = TTFont(src)
    for r in f['name'].names:
        if r.nameID in (1, 3, 4, 16, 21):
            r.string = r.toUnicode().replace('Iosevka Term', 'Iosevka Clod')
        elif r.nameID == 6:
            r.string = 'IosevkaClod-' + style
    f.save(dst)

regular, bold, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
rename(regular, os.path.join(out, 'IosevkaClod-Regular.ttf'), 'Regular')
rename(bold, os.path.join(out, 'IosevkaClod-Bold.ttf'), 'Bold')
