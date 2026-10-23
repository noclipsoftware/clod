# Stands in for claude.exe inside clod for the landing page screenshot (clod's CLOD_CLAUDE).
# Registers itself the way Claude Code does (<CLAUDE_CONFIG_DIR>\sessions\<pid>.json) so clod
# treats the tab as a claude tab, sets a Claude-style title, and draws a still transcript in
# Claude Code's look. Redraws when the pane is resized. Never reads input.
#
#   fake_claude.py --resume <session id>     (or --session-id <id>)

import ctypes
import json
import os
import sys
import time

ORANGE = (215, 119, 87)
DIM = (125, 118, 109)
TEXT = (216, 210, 198)
GREEN = (156, 191, 126)
RED = (229, 99, 111)
ADD_BG = (34, 60, 38)
DEL_BG = (74, 34, 38)

# Each scenario: title, cwd, whether Claude is mid-turn, and the transcript as (kind, text) rows.
#   prompt / say / tool / out / add / del / ctx / spin / blank
SCENARIOS = {
    '6f1c2a90-1d4e-4b7a-9f00-000000000001': {
        'title': 'Implement motion matching locomotion',
        'cwd': 'C:\\src\\game',
        'busy': True,
        'rows': [
            ('prompt', 'replace the locomotion blend tree with motion matching'),
            ('blank', ''),
            ('say', "I'll build a feature database from the mocap clips: per frame, the root's future trajectory (3 samples) plus foot positions and velocities, normalized per feature. Then search it every 10 frames and inertialize into the best match."),
            ('blank', ''),
            ('tool', 'Write(src/anim/motion_db.cpp)'),
            ('out', 'Wrote 186 lines to src/anim/motion_db.cpp'),
            ('blank', ''),
            ('tool', 'Update(src/anim/matcher.cpp)'),
            ('out', 'Updated src/anim/matcher.cpp with 4 additions and 1 removal'),
            ('ctx', '  41   float Cost(const feature& A, const feature& B)'),
            ('ctx', '  42   {'),
            ('del', '  43 -     return 0.0f; // TODO'),
            ('add', '  43 +     float D = 0.0f;'),
            ('add', '  44 +     for(int I = 0; I < FEATURE_DIMS; ++I)'),
            ('add', '  45 +         D += Weights[I] * Square(A.V[I] - B.V[I]);'),
            ('add', '  46 +     return D;'),
            ('ctx', '  47   }'),
            ('blank', ''),
            ('tool', 'Bash(build.bat && anim_viewer --clip run_turns.bvh)'),
            ('out', 'Running…'),
            ('blank', ''),
            ('spin', 'Searching… (2m 04s · ↓ 5.8k tokens · esc to interrupt)'),
        ],
    },
    '6f1c2a90-1d4e-4b7a-9f00-000000000002': {
        'title': 'Reverse engineer the model and animation formats',
        'cwd': 'C:\\src\\re',
        'busy': False,
        'rows': [
            ('prompt', "work out the game's model and animation formats, I want them in Blender"),
            ('blank', ''),
            ('tool', 'Bash(python tools/unpak.py data/chars.pak --list)'),
            ('out', 'hero.msh        212 KB'),
            ('out2', 'hero.skl          9 KB'),
            ('out2', 'hero_run.anm     41 KB'),
            ('blank', ''),
            ('tool', 'Read(disasm/LoadMesh_1401F2C30.asm)'),
            ('out', 'Read 274 lines'),
            ('blank', ''),
            ('say', '.msh is a 32-byte header, then vertices (position, octahedral normal, half-float UV, 4 bone weights) and 16-bit indices. .anm keys each bone at 30 Hz as smallest-three quaternions, 48 bits a key.'),
            ('blank', ''),
            ('tool', 'Bash(python tools/to_gltf.py hero.msh hero.skl hero_run.anm)'),
            ('out', 'wrote hero.gltf · 18,204 tris · 64 bones · 1 clip (0.8 s)'),
            ('blank', ''),
            ('say2', 'Want me to convert the rest of chars.pak?'),
        ],
    },
    # Kept off screen by capture.py; 'later' makes them want a look once hidden: 'finish' ends the
    # turn (spinner to ✳), 'bell' rings like a permission prompt. Both show in clod's top bar.
    '6f1c2a90-1d4e-4b7a-9f00-000000000004': {
        'title': 'Add foot IK to the run cycle',
        'cwd': 'C:\\src\\game',
        'busy': True,
        'later': ('finish', 5.0),
        'rows': [
            ('prompt', 'feet slide on slopes during the run cycle, add foot IK'),
            ('blank', ''),
            ('say', 'Two-bone IK per leg, with the pelvis lowered by the larger foot offset.'),
        ],
    },
    '6f1c2a90-1d4e-4b7a-9f00-000000000006': {
        'title': 'Speed up the BVH build',
        'cwd': 'C:\\src\\meshkit',
        'busy': False,
        'later': ('bell', 5.0),
        'rows': [
            ('prompt', 'the BVH build takes 4 s on the city scene, make it faster'),
            ('blank', ''),
            ('say', 'Binned SAH with 16 bins instead of a full sweep should get most of it.'),
        ],
    },
}


def sgr(fg=None, bg=None, bold=False):
    s = '\x1b[0'
    if bold:
        s += ';1'
    if fg:
        s += ';38;2;%d;%d;%d' % fg
    if bg:
        s += ';48;2;%d;%d;%d' % bg
    return s + 'm'


def wrap(text, width):
    words = text.split(' ')
    lines = ['']
    for w in words:
        if lines[-1] and len(lines[-1]) + 1 + len(w) > width:
            lines.append(w)
        else:
            lines[-1] = (lines[-1] + ' ' + w) if lines[-1] else w
    return lines


def render(scenario, cols, rows):
    # --- transcript rows to screen lines: (left marker, text, style) per line ---
    lines = []
    body = max(10, cols - 4)
    for kind, text in scenario['rows']:
        if kind == 'blank':
            lines.append(sgr())
        elif kind == 'prompt':
            for i, part in enumerate(wrap(text, body)):
                lines.append(sgr(DIM) + ('> ' if i == 0 else '  ') + part)
        elif kind in ('say', 'say2'):
            for i, part in enumerate(wrap(text, body)):
                mark = sgr(TEXT) + '⏺ ' if (i == 0 and kind == 'say') else '  '
                lines.append(mark + sgr(TEXT) + part)
        elif kind == 'tool':
            name, _, rest = text.partition('(')
            lines.append(sgr(GREEN) + '⏺ ' + sgr(TEXT, bold=True) + name + sgr(TEXT) + '(' + rest)
        elif kind == 'out':
            lines.append(sgr(DIM) + '  ⎿  ' + text)
        elif kind == 'out2':
            lines.append(sgr(DIM) + '     ' + text)
        elif kind == 'ctx':
            lines.append(sgr(DIM) + '    ' + text)
        elif kind == 'add':
            lines.append(sgr() + '    ' + sgr(TEXT, ADD_BG) + text.ljust(body - 2))
        elif kind == 'del':
            lines.append(sgr() + '    ' + sgr(TEXT, DEL_BG) + text.ljust(body - 2))
        elif kind == 'spin':
            word, _, rest = text.partition(' (')
            lines.append(sgr(ORANGE) + '✻ ' + word + ' ' + sgr(DIM) + '(' + rest)

    # --- screen: the transcript's tail above Claude's input box ---
    box = 4  # rule, prompt, rule, hint
    room = max(1, rows - box)
    shown = lines[-room:]
    out = '\x1b[?25l\x1b[H\x1b[2J'
    top = room - len(shown)
    for i, line in enumerate(shown):
        out += '\x1b[%d;1H' % (top + i + 1) + line + sgr() + '\x1b[K'
    rule = sgr(DIM) + '─' * cols
    out += '\x1b[%d;1H' % (rows - 3) + rule
    out += '\x1b[%d;1H' % (rows - 2) + sgr(TEXT) + '> '
    out += '\x1b[%d;1H' % (rows - 1) + rule
    out += '\x1b[%d;1H' % rows + sgr(DIM) + '  ? for shortcuts' + sgr()
    out += '\x1b[%d;3H' % (rows - 2)
    if not scenario['busy']:
        out += '\x1b[?25h'
    return out


def main():
    args = sys.argv[1:]
    session = args[args.index('--resume') + 1] if '--resume' in args else args[args.index('--session-id') + 1]
    scenario = SCENARIOS[session]

    # VT output on this console (ConPTY passes it on to clod)
    kernel32 = ctypes.windll.kernel32
    handle = kernel32.GetStdHandle(-11)
    mode = ctypes.c_uint32()
    kernel32.GetConsoleMode(handle, ctypes.byref(mode))
    kernel32.SetConsoleMode(handle, mode.value | 0x0004)
    sys.stdout.reconfigure(encoding='utf-8')

    # --- register like Claude Code, so clod sees a claude tab with this session ---
    sessions = os.path.join(os.environ['CLAUDE_CONFIG_DIR'], 'sessions')
    os.makedirs(sessions, exist_ok=True)
    with open(os.path.join(sessions, '%d.json' % os.getpid()), 'w', encoding='utf-8') as f:
        # compact separators: clod looks for "key":"value" with no spaces
        json.dump({'pid': os.getpid(), 'sessionId': session, 'cwd': scenario['cwd']}, f, separators=(',', ':'))

    title = ('◐ ' if scenario['busy'] else '✳ ') + scenario['title']
    sys.stdout.write('\x1b]0;' + title + '\x07\x1b[?1049h')
    size = None
    start = time.time()
    later = scenario.get('later')
    while True:
        now = os.get_terminal_size()  # not shutil's: it trusts COLUMNS/LINES, which hold the whole window's size
        if now != size:
            size = now
            sys.stdout.write(render(scenario, now.columns, now.lines))
            sys.stdout.flush()
        if later and time.time() - start > later[1]:
            sys.stdout.write('\x1b]0;✳ ' + scenario['title'] + '\x07' if later[0] == 'finish' else '\x07')
            sys.stdout.flush()
            later = None
        time.sleep(0.2)


main()
