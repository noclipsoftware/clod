# Captures build\clodw.exe for the landing page (kevin.gd/clod) as a PNG with real alpha, into
# content\ (`build content` runs it for both scenes). The window is shot over pure
# black and over pure white, and the difference gives each pixel's opacity (difference matting).
# See-through cells come out at clod's opacity setting, and the title bar, rounded corners and
# shadow keep their own alpha. What's on screen comes from a scene in SCENES:
#   grid   2x2: two fake Claude sessions at work, the home screen, neovim in a shell tab
#   shell  one pane: cmd + clink in a git repo: ll, grep, and clink listing git's completions (Alt+=)
#
#   python tools\screenshot\capture.py [--scene grid|shell] [--out <png>] [--size WxH]
#                                      [--opacity 95] [--keep-scratch]
#
# Everything runs isolated: a throwaway `clodw --test` server, USERPROFILE pointed at a scratch home
# (fresh ~\.clod with these settings), CLAUDE_CONFIG_DIR at fake sessions, CLOD_CLAUDE at fake_claude.py.

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import tkinter

from PIL import Image, ImageChops, ImageGrab

HERE = os.path.dirname(os.path.abspath(__file__))
CLOD = os.path.abspath(os.path.join(HERE, '..', '..'))  # the clod repo: tools\screenshot\..\..

# Sessions on the home screen, newest first. Ids and titles of the ones a scene opens match
# fake_claude.py's SCENARIOS.
SESSIONS = [
    ('6f1c2a90-1d4e-4b7a-9f00-000000000001', 'C:\\src\\game', 'Implement motion matching locomotion'),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000002', 'C:\\src\\re', 'Reverse engineer the model and animation formats'),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000003', 'C:\\src\\meshkit', 'Fix non-manifold edges after collapse'),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000004', 'C:\\src\\game', 'Add foot IK to the run cycle'),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000005', 'C:\\src\\re', 'Unpack the audio banks'),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000006', 'C:\\src\\meshkit', 'Speed up the BVH build'),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000007', 'C:\\src\\game', 'Inertialization for blend transitions'),
    ('6f1c2a90-1d4e-4b7a-9f00-000000000008', 'C:\\src\\dotfiles', 'Clean up my PowerShell profile'),
]

# A scene: the layout key (as VT bytes), what each pane shows in clod's pane order (top left,
# top right, bottom left, bottom right), sessions opened first and sent back off screen (Ctrl+\) so
# the top bar and home screen show tabs that want a look (fake_claude.py's 'later' makes one finish
# and one ring once hidden), the outer window size in physical pixels, and the output file.
# A pane is a session opened through the home screen's filter (/), the home screen itself, or a shell
# tab (t) with text typed into it as is ('\r' is Enter; ('alt', key) for Alt+key).
SCENES = {
    'grid': {
        'layout': '\x1b[1;8B',  # Ctrl+Shift+Alt+Down: 2x2
        'panes': [
            ('session', 'motion matching'),
            ('home', None),
            ('shell', ['cd /d C:\\src\\meshkit\r', 'nvim src\\simplify.cpp\r']),
            ('session', 'animation formats'),
        ],
        'hidden': ['foot IK', 'BVH build'],
        'size': '1700x1060',
        'out': 'screenshot.png',
    },
    'shell': {
        'layout': '\x1b[1;8A',  # Ctrl+Shift+Alt+Up: single
        'panes': [
            ('shell', ['cd /d C:\\src\\meshkit\r', 'cls\r', 'll\r', 'grep -rn --color TODO src\r',
                       'git ch', ('alt', '=')]),  # unfinished, then Alt+=: clink lists the completions
        ],
        'hidden': [],
        'size': '1400x900',
        'out': 'screenshot-shell.png',
    },
}

# The fake codebase (neovim's file, the shell's git repo). C:\src is only removed afterwards if this
# script made it.
MESHKIT = 'C:\\src\\meshkit'
# Its git history, oldest first: date, message, files that commit adds. Fixed dates and author keep
# the hashes the same every run.
MESHKIT_HISTORY = [
    ('2026-10-12T10:14:00', 'Indexed mesh and OBJ loading', ['README.md', 'build.bat', 'src/mesh.h']),
    ('2026-10-13T21:40:00', 'Half-edge connectivity with boundary edges', ['src/halfedge.cpp']),
    ('2026-10-15T19:05:00', 'Quadric error metric simplification', ['src/quadric.h', 'src/heap.h', 'src/simplify.cpp']),
    ('2026-10-16T22:31:00', 'Test that simplified meshes stay manifold', ['tests/test_simplify.cpp']),
    ('2026-10-19T20:12:00', 'Binned SAH BVH build', ['src/bvh.cpp']),
]

user32 = ctypes.windll.user32
WM_CHAR = 0x0102
WM_SYSCHAR = 0x0106
WM_KEYDOWN = 0x0100
WM_KEYUP = 0x0101
WM_CLOSE = 0x0010
VK_ESCAPE = 0x1B
HWND_TOPMOST = wt.HWND(-1)
SWP_NOMOVE, SWP_NOSIZE, SWP_SHOWWINDOW = 0x2, 0x1, 0x40


def post_text(hwnd, text):
    for ch in text:
        user32.PostMessageW(hwnd, WM_CHAR, ord(ch), 0)
        time.sleep(0.03)


def post_key(hwnd, vk):
    user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 0)
    user32.PostMessageW(hwnd, WM_KEYUP, vk, 0xC0000000)
    time.sleep(0.05)


def open_session(hwnd, text):
    # Filter the home screen's sessions and open the first match in this pane. Backspaces clear the
    # last filter: Esc would too, but with an empty filter it means "back to the tab you came from".
    post_text(hwnd, '/' + '\b' * 40 + text + '\r')
    time.sleep(2.5)  # cmd + clink + fake claude start, then the claude scan picks the tab up


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--scene', default='grid', choices=sorted(SCENES))
    ap.add_argument('--out', default='')
    ap.add_argument('--size', default='')
    ap.add_argument('--opacity', default='95')
    ap.add_argument('--keep-scratch', action='store_true')
    a = ap.parse_args()
    scene = SCENES[a.scene]
    out_path = a.out or os.path.join(CLOD, 'content', scene['out'])
    width, height = (int(v) for v in (a.size or scene['size']).split('x'))
    clodw = os.path.join(CLOD, 'build', 'clodw.exe')
    fake = os.path.join(HERE, 'fake_claude.py')
    if not os.path.exists(clodw):
        sys.exit('no clodw.exe at ' + clodw + ' (run build.bat in the clod repo)')
    if ' ' in sys.executable or ' ' in fake:
        sys.exit('python and fake_claude.py need paths without spaces (CLOD_CLAUDE goes inside cmd /k "...")')
    git = shutil.which('git')
    nvim = shutil.which('nvim')
    if not git or not nvim:
        sys.exit('git and nvim need to be on PATH')

    # Physical pixels everywhere: window size, backdrop and screen grab must agree.
    ctypes.windll.shcore.SetProcessDpiAwareness(2)

    # --- scratch home: settings, and fake Claude sessions for the home screen ---
    scratch = tempfile.mkdtemp(prefix='clodshot-')
    home = os.path.join(scratch, 'home')
    claude_dir = os.path.join(scratch, 'claude')
    os.makedirs(os.path.join(home, '.clod'))
    with open(os.path.join(home, '.clod', 'settings.txt'), 'w', encoding='utf-8') as f:
        f.write('font = Iosevka Clod\nopacity = %s\ntheme = Clod\nkeep_active = off\n' % a.opacity)
    # clod's default clink settings, minus the "F2=List Suggestions" hint: it comes and goes with
    # timing, so captures would differ. (clod seeds every other default file; existing ones stay.)
    os.makedirs(os.path.join(home, '.clod', 'clink'))
    with open(os.path.join(CLOD, 'shell', 'clink', 'clink_settings'), encoding='utf-8') as f:
        clink_settings = f.read()
    with open(os.path.join(home, '.clod', 'clink', 'clink_settings'), 'w', encoding='utf-8') as f:
        f.write(clink_settings + '\nautosuggest.hint = False\n')
    now = time.time()
    for i, (sid, cwd, title) in enumerate(SESSIONS):
        project = os.path.join(claude_dir, 'projects', ''.join(c if c.isalnum() else '-' for c in cwd))
        os.makedirs(project, exist_ok=True)
        path = os.path.join(project, sid + '.jsonl')
        with open(path, 'w', encoding='utf-8') as f:
            # compact separators: clod looks for "key":"value" with no spaces
            f.write(json.dumps({'type': 'user', 'cwd': cwd, 'sessionId': sid,
                                'message': {'role': 'user', 'content': title}}, separators=(',', ':')) + '\n')
            f.write(json.dumps({'type': 'ai-title', 'aiTitle': title, 'sessionId': sid}, separators=(',', ':')) + '\n')
        stamp = now - 60 * (i * i * 7 + 2)  # minutes to days ago, newest first
        os.utime(path, (stamp, stamp))

    # --- nvim for the shell tab: a wrapper first on PATH, so the tab is named "nvim src\..." ---
    bin_dir = os.path.join(scratch, 'bin')
    os.makedirs(bin_dir)
    with open(os.path.join(bin_dir, 'nvim.bat'), 'w', encoding='utf-8') as f:
        f.write('@"%s" --clean -n -i NONE -c "luafile %s" %%*\n' % (nvim, os.path.join(HERE, 'nvim_init.lua')))

    env = {k: v for k, v in os.environ.items()
           if not k.startswith('CLAUDE_CODE') and k not in ('CLAUDECODE', 'LS_COLORS')}  # clod's own ls colors
    env['USERPROFILE'] = home
    env['CLAUDE_CONFIG_DIR'] = claude_dir
    env['CLOD_CLAUDE'] = sys.executable + ' ' + fake
    env['PATH'] = bin_dir + os.pathsep + env['PATH']

    # --- the fake codebase at a path that reads well on screen, as a git repo with history, plus
    # the sessions' folders (clod says "Directory missing" for a session whose folder isn't there) ---
    fake_dirs = sorted(set([MESHKIT] + [cwd for _, cwd, _ in SESSIONS]))
    if any(os.path.exists(d) for d in fake_dirs):
        sys.exit('one of %s already exists; not touching it' % ', '.join(fake_dirs))
    made_src = not os.path.exists('C:\\src')
    shutil.copytree(os.path.join(HERE, 'meshkit'), MESHKIT)
    for d in fake_dirs:
        os.makedirs(d, exist_ok=True)

    def remove_fake_dirs():
        # git's object files are read-only, which rmtree can't delete until the flag is off
        def writable(func, path, _):
            os.chmod(path, stat.S_IWRITE)
            func(path)
        for d in (['C:\\src'] if made_src else fake_dirs):
            try:
                shutil.rmtree(d, onerror=writable)
            except OSError as e:
                print('could not remove', d, '-', e)

    git_env = dict(os.environ, GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull,
                   GIT_AUTHOR_NAME='meshkit', GIT_AUTHOR_EMAIL='meshkit@example.com',
                   GIT_COMMITTER_NAME='meshkit', GIT_COMMITTER_EMAIL='meshkit@example.com')
    subprocess.run([git, 'init', '-q', '-b', 'main'], cwd=MESHKIT, env=git_env, check=True)
    dir_stamps = {}
    for date, message, files in MESHKIT_HISTORY:
        stamp = time.mktime(time.strptime(date, '%Y-%m-%dT%H:%M:%S'))
        for name in files:
            os.utime(os.path.join(MESHKIT, name), (stamp, stamp))  # ll shows the commit's date
            dir_stamps[os.path.dirname(name)] = stamp
        subprocess.run([git, 'add'] + files, cwd=MESHKIT, env=git_env, check=True)
        subprocess.run([git, 'commit', '-q', '-m', message], cwd=MESHKIT, check=True,
                       env=dict(git_env, GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date))
    with open(os.path.join(MESHKIT, 'src', 'bvh.cpp'), 'a', encoding='utf-8') as f:
        f.write('\n// TODO: refit instead of rebuilding when only the vertices moved\n')  # the prompt's *
    dir_stamps['.git'] = stamp
    for name, stamp in dir_stamps.items():
        if name:
            os.utime(os.path.join(MESHKIT, name), (stamp, stamp))  # folders too, last commit into them

    # --- launch a throwaway clodw and size its window ---
    proc = subprocess.Popen([clodw, '--test'], env=env, cwd=MESHKIT)
    hwnd = 0
    for _ in range(200):
        time.sleep(0.1)
        found = []

        @ctypes.WINFUNCTYPE(wt.BOOL, wt.HWND, wt.LPARAM)
        def each(h, _):
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(h, ctypes.byref(pid))
            name = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(h, name, 64)
            if pid.value == proc.pid and name.value == 'clodw' and user32.IsWindowVisible(h):
                found.append(h)
            return True
        user32.EnumWindows(each, 0)
        if found:
            hwnd = found[0]
            break
    if not hwnd:
        proc.kill()
        remove_fake_dirs()
        sys.exit('clodw window never showed up')
    user32.ShowWindow(hwnd, 9)  # SW_RESTORE
    user32.SetWindowPos(hwnd, 0, 160, 160, width, height, 0)  # clear of the screen edge, shadow included
    time.sleep(2.0)  # server up, home screen drawn

    # --- drive it: the layout, then fill each pane in turn (Alt+Q moves the focus to the next) ---
    # Posted messages can't hold modifiers down, so the layout key goes in as the VT sequence clodw
    # would send for it, all at once so the server reads one key, not a lone Esc.
    for ch in scene['layout']:
        user32.PostMessageW(hwnd, WM_CHAR, ord(ch), 0)
    time.sleep(0.5)
    for text in scene['hidden']:
        open_session(hwnd, text)
        post_text(hwnd, '\x1c')  # Ctrl+\: this pane back to the home screen, the tab runs on unseen
        time.sleep(0.5)
    panes = scene['panes']
    for i, (kind, what) in enumerate(panes):
        if i > 0:
            user32.PostMessageW(hwnd, WM_SYSCHAR, ord('q'), 0x20000000)  # Alt+Q
            time.sleep(0.5)
        if kind == 'session':
            open_session(hwnd, what)
        elif kind == 'shell':
            post_text(hwnd, 't')
            time.sleep(2.5)  # cmd + clink up to the prompt
            for text in what:
                if isinstance(text, tuple):  # ('alt', key)
                    user32.PostMessageW(hwnd, WM_SYSCHAR, ord(text[1]), 0x20000000)
                else:
                    post_text(hwnd, text)
                time.sleep(1.0)
    # end with the focus on the home screen (if one is showing), its filter cleared
    kinds = [kind for kind, _ in panes]
    if 'home' in kinds:
        for _ in range((kinds.index('home') - (len(panes) - 1)) % len(panes)):
            user32.PostMessageW(hwnd, WM_SYSCHAR, ord('q'), 0x20000000)
            time.sleep(0.5)
        post_text(hwnd, '/' + '\b' * 40)
        post_key(hwnd, VK_ESCAPE)  # while editing the filter, Esc only clears it
    time.sleep(5.0)  # tabs settle: scans, notices, home screen sections

    # --- capture over black, then over white ---
    user32.SetWindowTextW(hwnd, 'clod')  # not "clod TEST", the --test server's title
    rect = wt.RECT()
    ctypes.windll.dwmapi.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(rect), ctypes.sizeof(rect))  # visible frame
    margin = 64  # room for the window's shadow
    box = (rect.left - margin, rect.top - margin, rect.right + margin, rect.bottom + margin)
    backdrop = tkinter.Tk()
    backdrop.overrideredirect(True)
    backdrop.attributes('-topmost', True)
    backdrop.geometry('%dx%d+%d+%d' % (box[2] - box[0], box[3] - box[1], box[0], box[1]))
    # The matte needs both shots of the same frame: black, white, then black again, and retry while
    # the two black ones differ (clod still updating something).
    for attempt in range(5):
        shots = []
        for color in ('black', 'white', 'black'):
            backdrop.configure(bg=color)
            backdrop.update()
            user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW)
            user32.SetForegroundWindow(hwnd)
            time.sleep(0.6)
            shots.append(ImageGrab.grab(bbox=box, all_screens=True).convert('RGB'))
        if ImageChops.difference(shots[0], shots[2]).getbbox() is None:
            break
        print('window changed during the capture, again')
        time.sleep(1.0)
    else:
        print('warning: the window never held still; the matte may show ghosting')
    backdrop.destroy()

    # --- close the window (a --test server ends with it) and clean up ---
    user32.PostMessageW(hwnd, WM_CLOSE, 0, 0)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    time.sleep(1.0)  # the tabs' consoles close after the window
    remove_fake_dirs()
    if a.keep_scratch:
        print('scratch kept at', scratch)
    else:
        time.sleep(1.0)
        shutil.rmtree(scratch, ignore_errors=True)

    # --- matte: alpha = 1 - (white - black), color = black / alpha ---
    black, white = shots[0], shots[1]
    diff = ImageChops.subtract(white, black).convert('L')
    alpha = ImageChops.invert(diff)
    r, g, b = black.split()
    def unpremultiply(ch):
        return Image.frombytes('L', ch.size, bytes(
            min(255, (c * 255 + al // 2) // al) if al else 0 for c, al in zip(ch.tobytes(), alpha.tobytes())))
    out = Image.merge('RGBA', [unpremultiply(r), unpremultiply(g), unpremultiply(b), alpha])
    out = out.crop(alpha.point(lambda v: 255 if v > 2 else 0).getbbox())
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    out.save(out_path, optimize=True)
    print('wrote', out_path, out.size)


main()
