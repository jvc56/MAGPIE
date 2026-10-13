#!/usr/bin/env python3
"""Offline, experimental XSG encoder/decoder and corpus benchmark. Not a URL API."""
import argparse
import base64
from collections import Counter
import ctypes
from functools import lru_cache
import gzip
import hashlib
import itertools
import json
import math
from pathlib import Path
import re
import statistics
import struct
import time
import zlib

ALPHABET = '?ABCDEFGHIJKLMNOPQRSTUVWXYZ'
DISTRIBUTION = Counter('??AAAAAAAAABBCCDDDDEEEEEEEEEEEEFFGGGHHIIIIIIIIIJKLLLLMMNNNNNNOOOOOOOOPPQRRRRRRSSSSTTTTTTUUUUVVWWXYYZ')
LEXICONS = 'CSW24 NWL23 CSW07 CSW12 CSW15 CSW21 NWL20 OSW1 OSW2 OSW3 OSW4 OSWI TWL06 TWL14 TWL98'.split()
COORD = re.compile(r'^(?:[A-O](?:[1-9]|1[0-5])|(?:[1-9]|1[0-5])[A-O])$')
TYPES = ['play', 'pass', 'exchange', 'count', 'withdraw', 'other']


class Bits:
    def __init__(self, bits=''):
        self.bits = bits
        self.offset = 0

    def put(self, value, width):
        if not 0 <= value < 2 ** width:
            raise ValueError('Field overflow')
        self.bits += format(value, f'0{width}b') if width else ''

    def get(self, width):
        if self.offset + width > len(self.bits):
            raise ValueError('Truncated XSG')
        value = int(self.bits[self.offset:self.offset+width], 2) if width else 0
        self.offset += width
        return value

    def var(self, value=None):
        if value is not None:
            if not 0 <= value <= 0xffffffff:
                raise ValueError('Varint overflow')
            while value >= 128:
                self.put((value & 127) | 128, 8)
                value >>= 7
            self.put(value, 8)
            return
        value = 0
        for index in range(5):
            byte = self.get(8)
            if index == 4 and byte > 15:
                raise ValueError('Varint overflow')
            value |= (byte & 127) << (7 * index)
            if byte < 128:
                if index and not byte:
                    raise ValueError('Nonminimal varint')
                return value
        raise ValueError('Varint overflow')

    def signed(self, value=None):
        if value is not None:
            self.var(2*value if value >= 0 else -2*value-1)
        else:
            encoded = self.var()
            return encoded//2 if encoded % 2 == 0 else -(encoded//2)-1

    def text(self, value=None):
        if value is not None:
            raw = value.encode('utf-8')
            if len(raw) > 4096:
                raise ValueError('Text too long')
            self.var(len(raw))
            for byte in raw:
                self.put(byte, 8)
        else:
            length = self.var()
            if length > 4096:
                raise ValueError('Text too long')
            return bytes(self.get(8) for _ in range(length)).decode('utf-8')

    def rank(self, order, value=None):
        # Order-k Exp-Golomb: a geometric head and logarithmic long tail.
        if value is not None:
            prefix = (value >> order)+1
            width = prefix.bit_length()
            self.put(0, width-1)
            self.put(prefix, width)
            self.put(value & ((1 << order)-1), order)
        else:
            zeros = 0
            while not self.get(1):
                zeros += 1
                if zeros > 24:
                    raise ValueError('Rank overflow')
            prefix = (1 << zeros) | self.get(zeros)
            return ((prefix-1) << order) | self.get(order)

    def packed(self):
        padded = self.bits + '0' * (-len(self.bits) % 8)
        return bytes(int(padded[offset:offset+8], 2) for offset in range(0, len(padded), 8))


def rank_size(rank, order):
    return 2 * (((rank >> order)+1).bit_length()-1)+1+order


def sorted_tiles(tiles):
    return ''.join(sorted(tiles))


def letters(word):
    return Counter('?' if char.islower() else char for char in word if char != '.')


def tiles(counter):
    return ''.join(sorted(counter.elements()))


def coord(location):
    if not COORD.fullmatch(location):
        raise ValueError('Invalid board coordinate')
    down = location[0].isalpha()
    row = int(location[1:] if down else location[:-1])-1
    col = ord(location[0] if down else location[-1])-65
    return row*15+col, down


def canonical_move(board, location, word):
    start, down = coord(location)
    word = word.replace('(', '').replace(')', '')
    if not 1 <= len(word) <= 15:
        raise ValueError('Invalid move span')
    normalized = ''
    for index, char in enumerate(word):
        square = start+index*(15 if down else 1)
        if square >= 225 or (not down and square//15 != start//15):
            raise ValueError('Move outside board')
        old = board[square]
        if old:
            if char != '.' and char.upper() != old.upper():
                raise ValueError('Conflicting play-through')
            normalized += '.'
        elif char in 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz':
            normalized += char
        else:
            raise ValueError('Invalid played letter')
    if not letters(normalized):
        raise ValueError('No new tiles')
    return [location, normalized]


class State:
    def __init__(self):
        self.board = [''] * 225
        self.carry = [Counter(), Counter()]
        self.scores = [0, 0]
        self.last = None

    def apply(self, event):
        player = event['player']
        rack = Counter(event['rack'])
        kind = event['kind']
        if kind == 'play':
            self.last = (player, self.board[:])
            start, down = coord(event['action'][0])
            for index, char in enumerate(event['action'][1]):
                if char != '.':
                    self.board[start+index*(15 if down else 1)] = char
            rack -= letters(event['action'][1])
        elif kind == 'exchange':
            rack -= Counter(event['action'])
        elif kind == 'count':
            # Unknown exchanged identities destroy certainty about the leave.
            rack.clear()
        elif kind == 'withdraw':
            if self.last is None or self.last[0] != player:
                raise ValueError('Withdrawal without preceding player move')
            self.board = self.last[1]
            self.last = None
        self.carry[player] = rack
        self.scores[player] = event['total']


def parse_gcg(raw, default_lexicon='NWL23'):
    try:
        text = raw.decode('utf-8-sig')
    except UnicodeDecodeError:
        text = raw.decode('latin-1')
    game = {'language':'en', 'lexicon':default_lexicon, 'players':[], 'events':[]}
    nicknames = {}
    state = State()
    for line in text.splitlines():
        line = line.strip()
        if line.startswith('#lexicon '):
            game['lexicon'] = line.split()[1]
        elif re.match(r'#player[12]\s', line):
            parts = line.split(None, 2)
            player = int(parts[0][-1])-1
            if player != len(game['players']):
                raise ValueError('Unordered player headers')
            game['players'].append([parts[1], parts[2] if len(parts) > 2 else parts[1]])
            nicknames[parts[1]] = player
        elif line.startswith('#game-type ') and line != '#game-type classic':
            raise ValueError('Nonstandard game type')
        elif line.startswith('#rack'):
            # Retain final/partial-game rack headers too.
            game.setdefault('final_racks', []).append(line)
        elif line.startswith('>'):
            match = re.fullmatch(r'>([^:]+):\s*(.*?)\s+([+-]?\d+)\s+(-?\d+)', line)
            if not match or match[1] not in nicknames:
                raise ValueError('Unsupported event line')
            player = nicknames[match[1]]
            parts = match[2].split()
            rack = ''
            if len(parts) >= 2 and re.fullmatch(r'[A-Z?]{1,7}', parts[0]):
                rack = sorted_tiles(parts.pop(0))
            if not parts:
                raise ValueError('Missing action')
            if len(parts) == 2 and COORD.fullmatch(parts[0]):
                kind = 'play'
                action = canonical_move(state.board, *parts)
                # A partial rack cannot erase the tiles proved by the move.
                rack = tiles(Counter(rack) | letters(action[1]))
            elif parts == ['-']:
                kind, action = 'pass', ''
            elif parts == ['--']:
                kind, action = 'withdraw', ''
            elif len(parts) == 1 and re.fullmatch(r'-[A-Z?]+', parts[0]):
                kind, action = 'exchange', sorted_tiles(parts[0][1:])
                rack = tiles(Counter(rack) | Counter(action))
            elif len(parts) == 1 and re.fullmatch(r'-[1-7]', parts[0]):
                kind, action = 'count', int(parts[0][1:])
            else:
                kind, action = 'other', ' '.join(parts)
            if len(rack) > 7:
                raise ValueError('Rack over seven tiles')
            event = dict(player=player, rack=rack, kind=kind, action=action,
                         delta=int(match[3]), total=int(match[4]))
            state.apply(event)
            game['events'].append(event)
    if len(game['players']) != 2 or not game['events'] or len(game['events']) > 1000:
        raise ValueError('Incomplete or oversized game')
    if game['lexicon'] not in LEXICONS:
        raise ValueError('Non-English or unknown lexicon')
    return game


def gcg(game):
    lines = ['#character-encoding UTF-8', '#lexicon '+game['lexicon']]
    lines += [f'#player{index+1} {nick} {name}' for index, (nick, name) in enumerate(game['players'])]
    for event in game['events']:
        action = event['action']
        if event['kind'] == 'play':
            action = ' '.join(action)
        elif event['kind'] == 'pass':
            action = '-'
        elif event['kind'] == 'withdraw':
            action = '--'
        elif event['kind'] in ('exchange', 'count'):
            action = '-'+str(action)
        lines.append(f">{game['players'][event['player']][0]}: {event['rack']} {action} {event['delta']:+d} {event['total']}")
    lines += game.get('final_racks', [])
    return '\n'.join(lines)+'\n'


@lru_cache(maxsize=128)
def draw_table(pool, count):
    # Exact probability-ranked multisets, not ordered tile sequences. Counts
    # are products of binomial coefficients; the common denominator cancels.
    if not 0 <= count <= 3:
        raise ValueError('Pilot probability tables cover at most three tiles')
    live = [index for index, amount in enumerate(pool) if amount > 0]
    entries = []
    for combination in itertools.combinations_with_replacement(live, count):
        counts = Counter(combination)
        if any(amount > pool[index] for index, amount in counts.items()):
            continue
        weight = math.prod(math.comb(pool[index], amount) for index, amount in counts.items())
        entries.append((weight, ''.join(ALPHABET[index] for index in combination)))
    entries.sort(key=lambda entry: (-entry[0], entry[1]))
    ordered = tuple(entry[1] for entry in entries)
    return ordered, {rack:rank for rank,rack in enumerate(ordered)}


def rack_base(state, player, implied, reset):
    return Counter(implied) if reset else state.carry[player] | Counter(implied)


def draw_pool(state, base):
    occupied = letters(''.join(state.board))
    return tuple(max(0, DISTRIBUTION[char]-occupied[char]-base[char]) for char in ALPHABET)


def write_rack(out, state, event, implied, order, stats, use_carry=True, use_probability=True):
    rack = Counter(event['rack'])
    base = rack_base(state, event['player'], implied, not use_carry)
    reset = bool(base-rack) or not use_carry
    base = rack_base(state, event['player'], implied, reset)
    if base-rack:
        raise ValueError('Implied tiles absent from rack')
    extra = tiles(rack-base)
    out.put(int(reset), 1)
    out.put(len(extra), 3)
    if not extra:
        return
    table = draw_table(draw_pool(state,base),len(extra)) if len(extra) <= 3 and use_probability else None
    rank = table[1].get(extra) if table else None
    if rank is not None:
        stats['draw_ranks'].append(rank)
    ranked = rank is not None and rank_size(rank,order) < len(extra)*5
    out.put(int(ranked), 1)
    if ranked:
        out.rank(order, rank)
    else:
        for char in extra:
            out.put(ALPHABET.index(char),5)


def read_rack(inp, state, player, implied, order):
    base = rack_base(state,player,implied,inp.get(1))
    count = inp.get(3)
    extra = ''
    if count:
        if inp.get(1):
            entries = draw_table(draw_pool(state,base),count)[0]
            rank = inp.rank(order)
            if rank >= len(entries):
                raise ValueError('Invalid draw rank')
            extra = entries[rank]
        else:
            for _ in range(count):
                index = inp.get(5)
                if index >= len(ALPHABET):
                    raise ValueError('Invalid rack tile')
                extra += ALPHABET[index]
            if extra != sorted_tiles(extra):
                raise ValueError('Unsorted rack multiset')
    result = tiles(base+Counter(extra))
    if len(result) > 7:
        raise ValueError('Oversized decoded rack')
    return result


def write_move(out, board, action):
    start, down = coord(action[0])
    out.put(start,8)
    out.put(int(down),1)
    out.put(len(action[1])-1,4)
    for index,char in enumerate(action[1]):
        if not board[start+index*(15 if down else 1)]:
            out.put(ord(char.upper())-65+(26 if char.islower() else 0),6)


def read_move(inp, board):
    start, down, length = inp.get(8), inp.get(1), inp.get(4)+1
    if start >= 225:
        raise ValueError('Bad start square')
    row,col = divmod(start,15)
    location = chr(65+col)+str(row+1) if down else str(row+1)+chr(65+col)
    word = ''
    for index in range(length):
        square = start+index*(15 if down else 1)
        if square >= 225 or (not down and square//15 != row):
            raise ValueError('Bad move span')
        if board[square]:
            word += '.'
        else:
            value = inp.get(6)
            if value >= 52:
                raise ValueError('Bad tile')
            word += chr((97 if value >= 26 else 65)+value%26)
    return canonical_move(board,location,word)


class Native:
    def __init__(self, library, data):
        self.lib = ctypes.CDLL(str(Path(library).resolve()))
        self.lib.magpie_create_with_options.argtypes = [ctypes.c_char_p,ctypes.c_char_p,ctypes.c_bool]
        self.lib.magpie_create_with_options.restype = ctypes.c_void_p
        self.lib.magpie_run_sync.argtypes = [ctypes.c_void_p,ctypes.c_char_p]
        for name in ['magpie_get_last_command_output','magpie_get_and_clear_error']:
            function = getattr(self.lib,name)
            function.argtypes = [ctypes.c_void_p]
            function.restype = ctypes.c_void_p
        self.lib.magpie_destroy.argtypes = [ctypes.c_void_p]
        self.free = ctypes.CDLL(None).free
        self.free.argtypes = [ctypes.c_void_p]
        self.handle = self.lib.magpie_create_with_options(str(Path(data).resolve()).encode(), b'/dev/null', False)
        self.lexicon = None
        self.data = Path(data)
        self.binary_hash = hashlib.sha256(Path(library).read_bytes()).digest()

    def run(self, command):
        failed = self.lib.magpie_run_sync(self.handle,command.encode())
        ptr = (self.lib.magpie_get_and_clear_error if failed else self.lib.magpie_get_last_command_output)(self.handle)
        text = ctypes.string_at(ptr).decode() if ptr else ''
        if ptr:
            self.free(ptr)
        if failed:
            raise ValueError(text.strip())
        return text

    @lru_cache(maxsize=16)
    def fingerprint(self, lexicon):
        digest = hashlib.sha256(self.binary_hash)
        digest.update(b'XSG-pilot-equity-top1024-v1')
        for extension in ['kwg','klv2']:
            digest.update((self.data/'lexica'/f'{lexicon}.{extension}').read_bytes())
        return digest.digest()[:16]

    @lru_cache(maxsize=2048)
    def moves(self, board, rack, lexicon):
        if lexicon not in LEXICONS or not re.fullmatch(r'[A-Z?]{1,7}',rack):
            return ()
        if self.lexicon != lexicon:
            self.run(f'set -lex {lexicon} -wmp false -wit false -rit false -numplays 1024 -maxnumdplays 1024 -s1 equity -s2 equity -r1 all -r2 all -shwithmoves false')
            self.lexicon = lexicon
        rows = []
        for row in range(15):
            cells = board[row*15:row*15+15]
            rows.append(''.join(str(len(list(group))) if not char else ''.join(group) for char,group in itertools.groupby(cells, key=lambda char:bool(char))))
        # groupby above uses boolean keys; occupied groups retain their letters.
        self.run('cgp '+ '/'.join(rows)+f' {rack}/ 0/0 0')
        output = self.run('generate -hr false')
        result = []
        for line in output.splitlines():
            match = re.match(r'\s*(\d+):\s+([A-O]\d+|\d+[A-O])\s+([A-Za-z().]+)\s+(?:[?A-Z]+\s+)?(-?\d+)\s',line)
            if match:
                try:
                    action = canonical_move(board,match[2],match[3])
                    result.append((int(match[1])-1,action))
                except ValueError:
                    pass
        return tuple((rank, tuple(action)) for rank, action in result)

    def close(self):
        self.lib.magpie_destroy(self.handle)


def encode(game, native=None, draw_order=4, move_order=2, carry=True, probability=True, rank_lexicon="CSW24"):
    out = Bits()
    out.put(1,4)  # experimental XSG body version, separate from XSP
    out.put(1,4)  # English language ID
    out.put(LEXICONS.index(game['lexicon'])+1,5)
    out.put(draw_order,3)
    out.put(move_order,3)
    out.put(bool(native),1)
    if native:
        out.put(LEXICONS.index(rank_lexicon)+1,5)
        for byte in native.fingerprint(rank_lexicon):
            out.put(byte,8)
    for nick,name in game['players']:
        out.text(nick)
        out.put(name == nick,1)
        if name != nick:
            out.text(name)
    out.var(len(game['events']))
    stats = {'draw_ranks':[], 'move_ranks':[], 'ranked':0, 'explicit':0, 'native_errors':0}
    state = State()
    for event in game['events']:
        out.put(event['player'],1)
        out.put(TYPES.index(event['kind']),3)
        kind = event['kind']
        implied = ''
        if kind == 'play':
            implied = tiles(letters(event['action'][1]))
            explicit = Bits()
            write_move(explicit,state.board,event['action'])
            write_rack(explicit,state,event,implied,draw_order,stats,carry,probability)
            ranked = None
            if native:
                try:
                    choices = native.moves(tuple(state.board),event['rack'],rank_lexicon)
                    rank = next((rank for rank,action in choices if list(action) == event['action']),None)
                    if rank is not None:
                        stats['move_ranks'].append(rank)
                        ranked = Bits()
                        write_rack(ranked,state,event,'',draw_order,stats,carry,probability)
                        ranked.rank(move_order,rank)
                except ValueError:
                    stats['native_errors'] += 1
            choose_ranked = ranked is not None and len(ranked.bits) < len(explicit.bits)
            out.put(choose_ranked,1)
            out.bits += ranked.bits if choose_ranked else explicit.bits
            stats['ranked' if choose_ranked else 'explicit'] += 1
        else:
            if kind == 'exchange':
                implied = event['action']
                out.put(len(implied),3)
                for char in implied:
                    out.put(ALPHABET.index(char),5)
            elif kind == 'count':
                out.put(event['action'],3)
            elif kind == 'other':
                out.text(event['action'])
            write_rack(out,state,event,implied,draw_order,stats,carry,probability)
        out.signed(event['delta'])
        expected = state.scores[event['player']]+event['delta']
        out.put(expected != event['total'],1)
        if expected != event['total']:
            out.signed(event['total'])
        state.apply(event)
    final = game.get('final_racks',[])
    out.var(len(final))
    for line in final:
        out.text(line)
    payload = b'XSG'+out.packed()
    packet = payload+struct.pack('>I',zlib.crc32(payload))
    return base64.urlsafe_b64encode(packet).decode().rstrip('='),stats


def decode(token, native=None):
    if len(token) > 1000000 or not re.fullmatch(r'[A-Za-z0-9_-]+',token):
        raise ValueError('Invalid XSG token')
    packet = base64.urlsafe_b64decode(token+'='*(-len(token)%4))
    if len(packet) < 8 or packet[:3] != b'XSG' or struct.pack('>I',zlib.crc32(packet[:-4])) != packet[-4:]:
        raise ValueError('Damaged XSG')
    if base64.urlsafe_b64encode(packet).decode().rstrip('=') != token:
        raise ValueError('Noncanonical base64')
    inp = Bits(''.join(format(byte,'08b') for byte in packet[3:-4]))
    if inp.get(4) != 1 or inp.get(4) != 1:
        raise ValueError('Unsupported XSG format/language')
    lexicon_id = inp.get(5)
    if not 1 <= lexicon_id <= len(LEXICONS):
        raise ValueError('Unknown lexicon')
    game = dict(language='en',lexicon=LEXICONS[lexicon_id-1],players=[],events=[])
    draw_order,move_order = inp.get(3),inp.get(3)
    rank_lexicon = None
    if inp.get(1):
        rank_lexicon_id = inp.get(5)
        if not 1 <= rank_lexicon_id <= len(LEXICONS):
            raise ValueError('Unknown rank lexicon')
        rank_lexicon = LEXICONS[rank_lexicon_id-1]
        fingerprint = bytes(inp.get(8) for _ in range(16))
        if native is None or fingerprint != native.fingerprint(rank_lexicon):
            raise ValueError('Matching native rank profile is required')
    for _ in range(2):
        nick = inp.text()
        game['players'].append([nick,nick if inp.get(1) else inp.text()])
    count = inp.var()
    if count > 1000:
        raise ValueError('Too many events')
    state = State()
    for _ in range(count):
        player,kind_id = inp.get(1),inp.get(3)
        if kind_id >= len(TYPES):
            raise ValueError('Invalid event type')
        kind = TYPES[kind_id]
        implied,action = '', ''
        if kind == 'play':
            if inp.get(1):
                rack = read_rack(inp,state,player,'',draw_order)
                rank = inp.rank(move_order)
                if native is None:
                    raise ValueError('Native profile required')
                choices = dict(native.moves(tuple(state.board),rack,rank_lexicon))
                if rank not in choices:
                    raise ValueError('Invalid move rank')
                action = list(choices[rank])
            else:
                action = read_move(inp,state.board)
                rack = read_rack(inp,state,player,tiles(letters(action[1])),draw_order)
        else:
            if kind == 'exchange':
                for _ in range(inp.get(3)):
                    index = inp.get(5)
                    if index >= len(ALPHABET):
                        raise ValueError('Invalid exchanged tile')
                    action += ALPHABET[index]
                implied = action
            elif kind == 'count':
                action = inp.get(3)
                if not action:
                    raise ValueError('Zero exchange count')
            elif kind == 'other':
                action = inp.text()
            rack = read_rack(inp,state,player,implied,draw_order)
        delta = inp.signed()
        total = inp.signed() if inp.get(1) else state.scores[player]+delta
        event = dict(player=player,kind=kind,action=action,rack=rack,delta=delta,total=total)
        state.apply(event)
        game['events'].append(event)
    final_count = inp.var()
    if final_count > 1000:
        raise ValueError('Too many final rack records')
    if final_count:
        game['final_racks'] = [inp.text() for _ in range(final_count)]
    remaining = len(inp.bits)-inp.offset
    if remaining >= 8 or inp.get(remaining):
        raise ValueError('Trailing data')
    return game


def raw_file(path):
    raw = path.read_bytes()
    return gzip.decompress(raw) if path.suffix == '.gz' else raw


def choose_games(root, limit, skips):
    # Deterministic file ordering, then content deduplication before splitting.
    paths = [path for path in Path(root).rglob('*') if path.name.endswith(('.gcg','.gcg.gz'))]
    paths.sort(key=lambda path:hashlib.sha256(path.name.encode()).digest())
    seen = set()
    selected = []
    for path in paths:
        if len(selected) >= limit:
            break
        try:
            raw = raw_file(path)
            game = parse_gcg(raw)
            digest = hashlib.sha256(json.dumps(game,sort_keys=True).encode()).hexdigest()
            if digest in seen:
                skips['duplicate'] += 1
                continue
            seen.add(digest)
            category = 'bot-present' if any('bot' in nick.lower() for nick,_ in game['players']) else 'human-only'
            selected.append((path,raw,game,digest,category))
        except (ValueError,KeyError,IndexError,UnicodeError,OSError) as error:
            skips[str(error).split('\n')[0][:160]] += 1
    return len(paths),selected


def summarize(rows):
    result = {'games':len(rows)}
    for key in ['original_chars','normalized_chars','zlib_chars','explicit_chars','carry_chars','draw_chars','hybrid_chars','ranked','plays']:
        values = [row[key] for row in rows]
        result[key] = {'mean':round(statistics.mean(values),2),'median':statistics.median(values),
                       'p95':sorted(values)[min(len(values)-1,int(len(values)*.95))]}
    return result


def benchmark(args):
    native = Native(args.library,args.data)
    corpora,manifest,draw_ranks,move_ranks = {},[],[],[]
    started = time.monotonic()
    for spec in args.corpus:
        label,root = spec.split('=',1)
        skips = Counter()
        available,selected = choose_games(root,args.games,skips)
        # Whole-game split. No positions from the same game cross the boundary.
        train = [item for item in selected if int(item[3][-1],16) < 8]
        heldout = [item for item in selected if int(item[3][-1],16) >= 8]
        corpora[label] = dict(available=available,selected=len(selected),skips=dict(skips),heldout=heldout,
                             training_games=len(train),records=[])
        for path,raw,game,digest,category in train:
            _,stats = encode(game,native)
            draw_ranks.extend(stats['draw_ranks'])
            move_ranks.extend(stats['move_ranks'])
            manifest.append(dict(source=label,file=path.name,sha256=hashlib.sha256(raw).hexdigest(),split='train',category=category))
        print(f'{label}: trained on {len(train)} games, {len(heldout)} held out',flush=True)
    draw_costs = [sum(rank_size(rank,order) for rank in draw_ranks) for order in range(7)]
    move_costs = [sum(rank_size(rank,order) for rank in move_ranks) for order in range(7)]
    draw_order = min(range(7),key=lambda order:draw_costs[order])
    move_order = min(range(7),key=lambda order:move_costs[order])
    print(f'Tuned draw order={draw_order}, move order={move_order}',flush=True)
    results = {'format':'experimental-XSG-v1','engine_fingerprint':native.fingerprint('CSW24').hex(),
               'rank_lexicon':'CSW24','draw_order':draw_order,'move_order':move_order,
               'training_rank_costs':{'draw':draw_costs,'move':move_costs},'corpora':{}}
    for label,corpus in corpora.items():
        for index,(path,raw,game,digest,category) in enumerate(corpus.pop('heldout')):
            normalized = gcg(game).encode()
            row = dict(file=path.name,category=category,events=len(game['events']),
                       original_chars=len(raw),normalized_chars=len(normalized),
                       zlib_chars=len(base64.urlsafe_b64encode(zlib.compress(normalized,9)).rstrip(b'=')))
            for mode,options in [
                ('explicit',dict(carry=False,probability=False)),
                ('carry',dict(probability=False)), ('draw',{}), ('hybrid',dict(native=native)),
            ]:
                token,stats = encode(game,draw_order=draw_order,move_order=move_order,**options)
                decoded = decode(token,options.get('native'))
                if decoded != game or parse_gcg(gcg(decoded).encode()) != game:
                    raise AssertionError(f'Round-trip mismatch: {path} ({mode})')
                row[mode+'_chars'] = len(token)
                if mode == 'hybrid':
                    row['ranked'] = stats['ranked']
                    row['plays'] = stats['ranked']+stats['explicit']
                    row['native_errors'] = stats['native_errors']
                    row['top1'] = sum(rank == 0 for rank in stats['move_ranks'])
                    row['top10'] = sum(rank < 10 for rank in stats['move_ranks'])
                    row['matched_ranks'] = len(stats['move_ranks'])
                    # Standalone fallback can beat the profile header on short games.
                    if row['draw_chars'] <= row['hybrid_chars']:
                        row['hybrid_chars'] = row['draw_chars']
                        row['ranked'] = 0
            corpus['records'].append(row)
            manifest.append(dict(source=label,file=path.name,sha256=hashlib.sha256(raw).hexdigest(),split='heldout',category=category))
            if index % 50 == 0:
                print(f'{label}: verified {index+1} held-out games',flush=True)
        rows = corpus['records']
        corpus['summary'] = summarize(rows) if rows else {}
        corpus['by_category'] = {category:summarize([row for row in rows if row['category']==category])
                                 for category in sorted({row['category'] for row in rows})}
        results['corpora'][label] = corpus
    results['elapsed_seconds'] = round(time.monotonic()-started,2)
    results['manifest'] = manifest
    Path(args.output).write_text(json.dumps(results,indent=2)+'\n')
    print(json.dumps({label:corpus['summary'] for label,corpus in results['corpora'].items()},indent=2))
    native.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command',required=True)
    bench = sub.add_parser('benchmark')
    bench.add_argument('--corpus',action='append',required=True,help='LABEL=DIRECTORY')
    bench.add_argument('--games',type=int,default=400,help='Sample count per corpus before train/test split')
    bench.add_argument('--library',default='bin/libmagpie.dylib')
    bench.add_argument('--data',default='data')
    bench.add_argument('--output',required=True)
    enc = sub.add_parser('encode')
    enc.add_argument('gcg')
    enc.add_argument('--library', help='Enable static-rank encoding with this native library')
    enc.add_argument('--data',default='data')
    enc.add_argument('--draw-order',type=int,choices=range(7),default=4)
    enc.add_argument('--move-order',type=int,choices=range(7),default=2)
    dec = sub.add_parser('decode')
    dec.add_argument('token')
    dec.add_argument('--library',help='Library matching the encoded static-rank profile')
    dec.add_argument('--data',default='data')
    args = parser.parse_args()
    if args.command == 'benchmark':
        benchmark(args)
    else:
        native = Native(args.library,args.data) if args.library else None
        try:
            if args.command == 'encode':
                game = parse_gcg(raw_file(Path(args.gcg)))
                token = encode(game,draw_order=args.draw_order,move_order=args.move_order)[0]
                if native:
                    ranked = encode(game,native,draw_order=args.draw_order,move_order=args.move_order)[0]
                    if len(ranked) < len(token):
                        token = ranked
                print(token)
            else:
                print(gcg(decode(args.token,native)),end='')
        finally:
            if native:
                native.close()


if __name__ == '__main__':
    main()
