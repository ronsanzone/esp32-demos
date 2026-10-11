"""Laptop side of devdeck: send JSON messages to the board and pull screenshots.

  deck.py bridge                 stream live dev activity to the board (Ctrl-C to stop; stop before flashing)
  deck.py send '{"t":"ping"}'    send one message, print the board's ack
  deck.py snap out.png           stream the framebuffer back and save it as PNG
  deck.py app lens               switch the board to an app (it reboots into it); works while the bridge runs
                                 ids: home devdeck painting lens field ghost
"""
import calendar, collections, glob, json, os, re, subprocess, sys, time
from urllib.parse import urlparse
import serial
from PIL import Image

PORT = '/dev/cu.usbmodem101'
W = H = 480


def open_port():
    return serial.Serial(PORT, 115200, timeout=0.5)


def send(port, msg, wait_ack=True):
    topic = msg['t']
    port.write((json.dumps(msg, separators=(',', ':')) + '\n').encode())
    if not wait_ack:
        return True
    end, buf = time.time() + 5, b''
    while time.time() < end:
        buf += port.read(1024)
        if f'@@ACK {topic}'.encode() in buf:
            return True
    return False


def snap(port, path):
    port.reset_input_buffer()
    port.write(b'{"t":"snap"}\n')
    img = Image.new('RGB', (W, H))
    buf, end = b'', time.time() + 30
    hdr = re.compile(rb'@@SNAP (\d+) (\d+) (\d+) (\d+) (\d+)\n')
    strips = 0
    while time.time() < end:
        buf += port.read(65536)
        while True:
            m = hdr.search(buf)
            if m and len(buf) >= m.end() + int(m[5]):
                x1, y1, x2, y2, n = map(int, m.groups())
                px = buf[m.end():m.end() + n]
                buf = buf[m.end() + n:]
                strip = Image.frombuffer('RGB', (x2 - x1 + 1, y2 - y1 + 1), px, 'raw', 'BGR;16', 0, 1)
                img.paste(strip, (x1, y1))
                strips += 1
                continue
            break
        if b'@@SNAPEND' in buf and not hdr.search(buf):
            break
    img.save(path)
    return strips


PROJECTS = os.path.expanduser('~/.claude/projects')
HOME = os.path.expanduser('~')
MAX_SLOTS = 6
ROSTER_IDLE_S = 20 * 60      # drop a session from the orbit after this much silence
LIVE_S = 90                  # "live" = a tool call within this window


def tool_detail(name, inp):
    """One short human string for a tool call."""
    if not isinstance(inp, dict):
        return ''
    if name == 'Bash':
        return inp.get('description') or inp.get('command', '')
    for k in ('file_path', 'notebook_path'):
        if k in inp:
            return os.path.basename(inp[k])
    if name.startswith('Web') and 'url' in inp:
        return urlparse(inp['url']).netloc
    for k in ('skill', 'description', 'pattern', 'query', 'prompt'):
        if k in inp:
            return str(inp[k])
    return ''


class Session:
    def __init__(self, sid):
        self.sid, self.cwd = sid, ''
        self.calls = collections.deque()   # timestamps of tool calls
        self.last = 0.0
        self.ctx = 0                       # tokens in the most recent request's context
        self.ctx_hist = collections.deque(maxlen=48)
        self.cache_read = self.cache_new = 0
        self.model = ''

    @property
    def label(self):
        if not self.cwd or self.cwd == HOME:
            return '~'
        return os.path.basename(self.cwd.rstrip('/'))


class Transcripts:
    """Tails ~/.claude/projects/*/<session>.jsonl (plus each session's subagents/) incrementally."""

    def __init__(self):
        self.offsets, self.sessions, self.slots = {}, {}, []

    def _files(self, horizon):
        now = time.time()
        for f in glob.glob(f'{PROJECTS}/*/*.jsonl') + glob.glob(f'{PROJECTS}/*/*/subagents/*.jsonl'):
            try:
                if now - os.path.getmtime(f) < horizon:
                    yield f
            except OSError:
                pass

    @staticmethod
    def _session_id(path):
        parts = path.split(os.sep)
        return parts[-3] if parts[-2] == 'subagents' else parts[-1][:-6]

    def poll(self):
        """Read new lines; return [(session, tool, detail)] for new tool calls."""
        events = []
        for f in self._files(ROSTER_IDLE_S):
            size = os.path.getsize(f)
            first = f not in self.offsets
            start = max(0, size - 400_000) if first else self.offsets[f]
            if size <= start:
                self.offsets[f] = size
                continue
            with open(f, 'rb') as fh:
                fh.seek(start)
                data = fh.read(size - start)
            cut = data.rfind(b'\n') + 1          # only consume complete lines
            self.offsets[f] = start + cut
            sub = os.sep + 'subagents' + os.sep in f
            s = self.sessions.setdefault(self._session_id(f), Session(self._session_id(f)))
            for line in data[:cut].splitlines():
                ev = self._ingest(s, line, sub)
                if ev and not first:
                    events.extend(ev)
        return events

    def _ingest(self, s, line, sub):
        try:
            d = json.loads(line)
        except ValueError:
            return None
        if not sub and d.get('cwd'):
            s.cwd = d['cwd']
        m = d.get('message') or {}
        if d.get('type') != 'assistant' or not isinstance(m.get('content'), list):
            return None
        ts = time.time()
        try:
            ts = calendar.timegm(time.strptime(d['timestamp'][:19], '%Y-%m-%dT%H:%M:%S'))
        except (KeyError, ValueError):
            pass
        u = m.get('usage') or {}
        if not sub and u:
            ctx = u.get('input_tokens', 0) + u.get('cache_read_input_tokens', 0) + u.get('cache_creation_input_tokens', 0)
            if ctx:
                s.ctx, s.model = ctx, m.get('model', '')
                s.cache_read, s.cache_new = u.get('cache_read_input_tokens', 0), u.get('cache_creation_input_tokens', 0)
                if not s.ctx_hist or s.ctx_hist[-1] != ctx:
                    s.ctx_hist.append(ctx)
        out = []
        for c in m['content']:
            if isinstance(c, dict) and c.get('type') == 'tool_use':
                s.calls.append(ts)
                s.last = max(s.last, ts)
                out.append((s, c.get('name', '?'), tool_detail(c.get('name', ''), c.get('input'))[:80]))
        return out

    def roster(self):
        """Stable slot assignment: a session keeps its planet until it goes idle."""
        now = time.time()
        for s in self.sessions.values():
            while s.calls and now - s.calls[0] > 600:
                s.calls.popleft()
        self.slots = [sid for sid in self.slots if now - self.sessions[sid].last < ROSTER_IDLE_S]
        for s in sorted(self.sessions.values(), key=lambda s: -s.last):
            if len(self.slots) >= MAX_SLOTS:
                break
            if s.sid not in self.slots and now - s.last < ROSTER_IDLE_S:
                self.slots.append(s.sid)
        return [self.sessions[sid] for sid in self.slots]


STAGES = [  # deep-work stage -> skill-name patterns, checked in order (first match wins)
    ('Ticket', r'refine-ticket'),
    ('Questions', r'research-questions'),
    ('Research', r'(^|[-:])research'),
    ('Design', r'design|brainstorm|grill|prd|tdd'),
    ('Outline', r'outline(?!-implementer)|^dw-04'),
    ('Plan', r'plan|brief'),
    ('Build', r'implement|subagent-driven|executing|spawn'),
    ('Ship', r'submit-pr|yeet|babysit|finishing|describe-pr|review'),
]
SLASH = re.compile(r'<command-name>/([\w:.-]+)</command-name>')


def ago(ts):
    s = time.time() - ts
    for unit, n in (('d', 86400), ('h', 3600), ('m', 60)):
        if s >= n:
            return f'{int(s // n)}{unit} ago'
    return 'just now'


def pipeline_msg():
    """Count deep-work skill runs (Skill tool calls + slash commands) across every transcript."""
    counts, last = [0] * len(STAGES), (0, -1, '')
    for f in glob.glob(f'{PROJECTS}/*/*.jsonl') + glob.glob(f'{PROJECTS}/*/*/subagents/*.jsonl'):
        for line in open(f, errors='ignore'):
            if '"Skill"' not in line and '<command-name>' not in line:
                continue
            try:
                d = json.loads(line)
            except ValueError:
                continue
            m, names = d.get('message') or {}, []
            if d.get('type') == 'assistant' and isinstance(m.get('content'), list):
                names = [c['input'].get('skill', '') for c in m['content']
                         if isinstance(c, dict) and c.get('type') == 'tool_use' and c.get('name') == 'Skill']
            elif d.get('type') == 'user' and isinstance(m.get('content'), str):
                names = SLASH.findall(m['content'])
            for name in names:
                for i, (_, pat) in enumerate(STAGES):
                    if re.search(pat, name.split(':')[-1]):
                        counts[i] += 1
                        ts = calendar.timegm(time.strptime(d.get('timestamp', '1970-01-01T00:00:00')[:19], '%Y-%m-%dT%H:%M:%S'))
                        if ts > last[0]:
                            last = (ts, i, name)
                        break
    return {'t': 'pipe', 'c': counts, 'last': last[1], 'skill': last[2].split(':')[-1], 'ago': ago(last[0]) if last[0] else '', 'total': sum(counts)}


def gh(*args):
    import subprocess
    return json.loads(subprocess.run(['gh', *args], capture_output=True, text=True, check=True).stdout)


def pulse_msg():
    q = '{viewer{contributionsCollection{contributionCalendar{totalContributions weeks{contributionDays{contributionLevel contributionCount}}}}}}'
    cal = gh('api', 'graphql', '-f', f'query={q}')['data']['viewer']['contributionsCollection']['contributionCalendar']
    lvl = {'NONE': '0', 'FIRST_QUARTILE': '1', 'SECOND_QUARTILE': '2', 'THIRD_QUARTILE': '3', 'FOURTH_QUARTILE': '4'}
    days = [d for w in cal['weeks'][-26:] for d in w['contributionDays']]
    counts = [d['contributionCount'] for d in days]
    streak, i = 0, len(counts) - 1
    if counts and counts[-1] == 0:
        i -= 1                                   # today still empty doesn't break the streak
    while i >= 0 and counts[i] > 0:
        streak, i = streak + 1, i - 1
    prs = []
    for p in gh('search', 'prs', '--author=@me', '--state=open', '--json', 'number,title,repository,url', '--limit', '4'):
        roll = gh('pr', 'view', p['url'], '--json', 'statusCheckRollup')['statusCheckRollup']
        states = {(c.get('conclusion') or c.get('state') or c.get('status') or '').upper() for c in roll}
        st = ('fail' if states & {'FAILURE', 'ERROR', 'TIMED_OUT', 'CANCELLED', 'ACTION_REQUIRED'}
              else 'pending' if states & {'PENDING', 'IN_PROGRESS', 'QUEUED', 'EXPECTED', 'WAITING', ''} and roll
              else 'pass' if roll else 'no checks')
        prs.append({'r': f"{p['repository']['name']}#{p['number']}", 'ti': p['title'], 'st': st})
    return {'t': 'pulse', 'total': cal['totalContributions'], 'streak': streak,
            'lv': ''.join(lvl.get(d['contributionLevel'], '0') for d in days), 'd14': counts[-14:], 'prs': prs}


def slow_feeds(latest):
    """Background thread: refresh the expensive feeds every 10 minutes."""
    while True:
        for name, fn in (('pipe', pipeline_msg), ('pulse', pulse_msg)):
            try:
                latest[name] = fn()
            except Exception as e:  # a gh hiccup shouldn't kill the bridge
                print(name, 'feed failed:', e, flush=True)
        time.sleep(600)


def ctx_msg(roster):
    items, focus = [], 0
    for i, s in enumerate(roster):
        if s.last > roster[focus].last:
            focus = i
        hist = list(s.ctx_hist)[-32:]
        items.append({'p': s.label, 'u': s.ctx, 'cr': s.cache_read,
                      'max': 1_000_000 if '1m' in s.model or s.ctx > 200_000 else 200_000,
                      'm': s.model.replace('claude-', ''), 'h': hist,
                      'cmp': sum(1 for a, b in zip(hist, hist[1:]) if b < a * 0.6)})
    return {'t': 'ctx', 'focus': focus, 's': items}


PIDFILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.bridge.pid')
OUTBOX = os.path.join(os.path.dirname(os.path.abspath(__file__)), '.bridge.outbox')
APPS = ('home', 'devdeck', 'painting', 'lens', 'field', 'ghost')


def bridge_running():
    try:
        os.kill(int(open(PIDFILE).read()), 0)
        return True
    except (OSError, ValueError):
        return False


class Commits:
    """Watches the repos that live sessions work in; new HEAD commits seed the Living Painting."""

    def __init__(self):
        self.heads, self.last = {}, 0

    def poll(self, roster):
        if time.time() - self.last < 20:
            return []
        self.last = time.time()
        out = []
        for cwd in {s.cwd for s in roster if s.cwd and s.cwd != HOME}:
            try:
                top = subprocess.run(['git', '-C', cwd, 'rev-parse', '--show-toplevel'],
                                     capture_output=True, text=True, timeout=5).stdout.strip()
                if not top:
                    continue
                head = subprocess.run(['git', '-C', top, 'rev-parse', 'HEAD'],
                                      capture_output=True, text=True, timeout=5).stdout.strip()
            except (OSError, subprocess.SubprocessError):
                continue
            if head and self.heads.get(top) not in (None, head):
                out.append({'t': 'commit', 'h': head[:7], 'r': os.path.basename(top)})
            if head:
                self.heads[top] = head
        return out
SHOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'shot.png')


def bridge():
    import signal
    want_shot = []
    signal.signal(signal.SIGUSR1, lambda *_: want_shot.append(1))   # `deck.py shot` asks for a snapshot
    with open(PIDFILE, 'w') as f:
        f.write(str(os.getpid()))
    import threading
    latest, sent = {}, {}
    threading.Thread(target=slow_feeds, args=(latest,), daemon=True).start()
    tr = Transcripts()
    commits = Commits()
    port, last_roster, board_app = None, 0, '?'

    while True:
        try:
            if want_shot and port is not None:
                want_shot.clear()
                tile = open(SHOT + '.tile').read().strip() if os.path.exists(SHOT + '.tile') else ''
                if tile:
                    send(port, {'t': 'tile', 'i': int(tile)}, wait_ack=False)
                    time.sleep(1.6)                     # let entrance animations settle
                print('shot:', snap(port, SHOT), 'strips', flush=True)
            if port is None:
                port = open_port()
                print('connected', PORT, flush=True)
                last_roster = 0
                sent.clear()                            # board may have rebooted: resend everything
            events = tr.poll()
            roster = tr.roster()
            if time.time() - last_roster > 2:
                labels = collections.Counter()
                items = []
                for s in roster:
                    labels[s.label] += 1
                    name = s.label if labels[s.label] == 1 else f'{s.label}#{labels[s.label]}'
                    items.append({'p': name, 'n': len(s.calls), 'live': int(time.time() - s.last < LIVE_S)})
                rate = sum(1 for s in roster for t in s.calls if time.time() - t < 60)
                send(port, {'t': 'agents', 's': items, 'r': rate}, wait_ack=False)
                send(port, ctx_msg(roster), wait_ack=False)
                for name, msg in list(latest.items()):
                    if sent.get(name) != msg:
                        send(port, msg, wait_ack=False)
                        sent[name] = msg
                last_roster = time.time()
            for s, tool, detail in events[-8:]:         # a burst collapses to its newest 8 sparks
                if s.sid in tr.slots:
                    send(port, {'t': 'tool', 's': tr.slots.index(s.sid), 'n': tool, 'd': detail}, wait_ack=False)
                    time.sleep(0.12)
            for msg in commits.poll(roster):
                send(port, msg, wait_ack=False)
                print('commit', msg['r'], msg['h'], flush=True)
            if os.path.exists(OUTBOX):                  # `deck.py app ...` while the bridge owns the port
                with open(OUTBOX) as f:
                    lines = f.read().splitlines()
                os.remove(OUTBOX)
                for line in lines:
                    send(port, json.loads(line), wait_ack=False)
            rx = port.read(port.in_waiting or 0)        # drain acks/logs so the board never blocks on TX
            m = re.search(rb'@@APP (\w+)', rx)
            if m and m.group(1).decode() != board_app:
                board_app = m.group(1).decode()
                print('board app:', board_app, flush=True)
            time.sleep(0.5)
        except (serial.SerialException, OSError) as e:
            print('port lost:', e, flush=True)
            port = None
            time.sleep(1)


if __name__ == '__main__':
    cmd = sys.argv[1]
    if cmd == 'bridge':
        bridge()
    if cmd == 'shot':          # snapshot via the running bridge (it owns the port)
        os.remove(SHOT) if os.path.exists(SHOT) else None
        with open(SHOT + '.tile', 'w') as f:
            f.write(sys.argv[2] if len(sys.argv) > 2 else '')
        os.kill(int(open(PIDFILE).read()), __import__('signal').SIGUSR1)
        for _ in range(100):
            if os.path.exists(SHOT):
                time.sleep(0.3)
                print(SHOT)
                break
            time.sleep(0.1)
        sys.exit(0)
    if cmd == 'app':
        app = sys.argv[2] if len(sys.argv) > 2 else ''
        if app not in APPS:
            sys.exit(f'unknown app {app!r}; pick one of: {" ".join(APPS)}')
        msg = json.dumps({'t': 'app', 'id': app})
        if bridge_running():
            with open(OUTBOX, 'a') as f:
                f.write(msg + '\n')
            print(f'queued for the bridge: {app}')
            sys.exit(0)
    with open_port() as p:
        if cmd == 'app':
            print('switching' if send(p, json.loads(msg)) else 'NO ACK')
        elif cmd == 'send':
            print('ack' if send(p, json.loads(sys.argv[2])) else 'NO ACK')
        elif cmd == 'snap':
            print(f'{snap(p, sys.argv[2])} strips -> {sys.argv[2]}')
