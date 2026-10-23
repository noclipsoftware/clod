# Writes the landing page's text and icon to content\ for the site generator (kevin.gd's build.py),
# from CLAUDE.md's "## Website content" section and build\clod.ico. Run by `build content`, after
# the screenshots.
#
#   content\content.json  {"tagline": ..., "install": ..., "features": [...]}
#   content\clod.png      the icon, 256 px
#   content\clod.ico      the icon at favicon sizes

import json
import os
import sys

from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT = os.path.join(ROOT, 'content')

# --- text: the section's "Tagline:" and "Install:" lines and its "- " list ---
content = {'tagline': '', 'install': '', 'features': []}
insection = False
with open(os.path.join(ROOT, 'CLAUDE.md'), encoding='utf-8') as f:
    for line in f.read().splitlines():
        if line.startswith('## '):
            insection = line.startswith('## Website content')
        elif insection and line.startswith('Tagline:'):
            content['tagline'] = line[len('Tagline:'):].strip()
        elif insection and line.startswith('Install:'):
            content['install'] = line[len('Install:'):].strip()
        elif insection and line.startswith('- '):
            content['features'].append(line[2:].strip())
if not content['tagline'] or not content['features']:
    sys.exit("CLAUDE.md's \"## Website content\" section has no Tagline: line or no features")

os.makedirs(OUT, exist_ok=True)
with open(os.path.join(OUT, 'content.json'), 'w', encoding='utf-8', newline='\n') as f:
    json.dump(content, f, indent=2, ensure_ascii=False)
    f.write('\n')

# --- icon: build.bat draws build\clod.ico (tools\make_icon.cpp); the page wants a PNG and a small favicon ---
ico = Image.open(os.path.join(ROOT, 'build', 'clod.ico'))
ico.size = max(ico.info['sizes'])
ico.load()
ico.save(os.path.join(OUT, 'clod.png'), optimize=True)
ico.save(os.path.join(OUT, 'clod.ico'), sizes=[(16, 16), (32, 32), (48, 48)])

print('wrote content.json, clod.png, clod.ico to', OUT)
