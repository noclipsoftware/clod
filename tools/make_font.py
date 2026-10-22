# Builds clod's bundled font, "Iosevka Clod": Iosevka Term (SIL OFL 1.1, no Reserved Font Name) with
# only its family name changed, so it installs next to any Iosevka the user already has. The files are
# named IosevkaClod-<version>-<style>.ttf: InstallFonts installs a new version under its own name.
# Needs fontTools (pip install fonttools).
# usage: python tools/make_font.py <IosevkaTerm-Regular.ttf> <IosevkaTerm-Bold.ttf> <out dir>
import os, sys
from fontTools.ttLib import TTFont

def rename(src, out, style):
    f = TTFont(src)
    version = f['name'].getDebugName(5).replace('Version', '').strip() # "Version 34.9.0"
    for r in f['name'].names:
        if r.nameID in (1, 3, 4, 16, 21):
            r.string = r.toUnicode().replace('Iosevka Term', 'Iosevka Clod')
        elif r.nameID == 6:
            r.string = 'IosevkaClod-' + style
    f.save(os.path.join(out, 'IosevkaClod-%s-%s.ttf' % (version, style)))

regular, bold, out = sys.argv[1:4]
os.makedirs(out, exist_ok=True)
rename(regular, out, 'Regular')
rename(bold, out, 'Bold')
