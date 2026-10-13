#!/usr/bin/env python3
"""Offline word-cover experiment; never changes the production link format.

Inputs: training word-count TSVs and independent held-out CGPs from
word-frequency-pilot.c. Uses both axes, literal fallbacks, and real bitstream
round trips. Reports complete payload sizes including metadata and CRC.
"""
import argparse
import base64
import collections
import gzip
import heapq
import json
import math
from pathlib import Path
import statistics
import zlib


class Bits:
    def __init__(self, data=''):
        self.data = data
        self.offset = 0

    def put(self, value, size):
        self.data += format(value, f'0{size}b') if size else ''

    def get(self, size):
        result = int(self.data[self.offset:self.offset+size] or '0', 2)
        self.offset += size
        return result


def codebook(counts):
    heap = [(count, word, word) for word, count in counts.items()]
    heapq.heapify(heap)
    while len(heap) > 1:
        first, second = heapq.heappop(heap), heapq.heappop(heap)
        heapq.heappush(heap, (first[0]+second[0], min(first[1], second[1]), (first[2], second[2])))
    lengths = {}
    def visit(node, depth):
        if isinstance(node, str):
            lengths[node] = max(1, depth)
        else:
            visit(node[0], depth+1)
            visit(node[1], depth+1)
    visit(heap[0][2], 0)
    codes, tree = {}, {}
    value, previous = 0, 0
    for word in sorted(lengths, key=lambda word: (lengths[word], word)):
        length = lengths[word]
        value <<= length-previous
        codes[word] = (value, length)
        branch = tree
        for bit in format(value, f'0{length}b'):
            branch = branch.setdefault(int(bit), {})
        branch['word'] = word
        value += 1
        previous = length
    return codes, tree


def read_word(bits, tree):
    branch = tree
    while 'word' not in branch:
        branch = branch[bits.get(1)]
    return branch['word']


def parse(cgp):
    board, racks, scores, zeros, *_ = cgp.split()
    cells = []
    for row in board.split('/'):
        index = 0
        while index < len(row):
            if row[index].isdigit():
                end = index+1
                while end < len(row) and row[end].isdigit(): end += 1
                cells.extend(['']*int(row[index:end])); index = end
            else:
                cells.append(row[index]); index += 1
    assert len(cells) == 225
    return cells, racks, scores, zeros


def runs(cells):
    result = []
    for step in (1, 15):
        for start in range(225):
            if not cells[start]: continue
            if (start % 15 if step == 1 else start // 15) and cells[start-step]: continue
            indices = []
            current = start
            while current < 225 and cells[current]:
                indices.append(current)
                if step == 1 and current % 15 == 14: break
                current += step
            if len(indices) >= 2:
                result.append((indices, ''.join(cells[index].upper() for index in indices)))
    return result


def cover(cells, codes, bitmap):
    candidates = runs(cells)
    index_bits = math.ceil(math.log2(max(1, len(candidates))))
    # Bitmap mode: stop bit + deterministic occupied-run index. Geometry mode:
    # word/literal bit + absolute start square + direction. Literals need their
    # square only in geometry mode; overlaps can make either axis worthwhile.
    overhead = 1+index_bits if bitmap else 10
    remaining = {index for index, letter in enumerate(cells) if letter}
    chosen = []
    while True:
        options = []
        for index, (indices, word) in enumerate(candidates):
            if word not in codes: continue
            gain = (5 if bitmap else 14)*len(remaining.intersection(indices)) - overhead - codes[word][1]
            if gain > 0: options.append((gain, -index, index))
        if not options: break
        index = max(options)[2]
        chosen.append(index)
        remaining.difference_update(candidates[index][0])
    bits = Bits()
    if bitmap:
        for letter in cells: bits.put(bool(letter), 1)
        for index in chosen:
            bits.put(1,1); bits.put(index,index_bits); bits.put(*codes[candidates[index][1]])
        bits.put(0,1)
        for index in sorted(remaining): bits.put(ord(cells[index].upper())-65,5)
    else:
        bits.put(len(chosen)+len(remaining),8)
        for index in chosen:
            indices, word = candidates[index]
            bits.put(1,1); bits.put(indices[0],8); bits.put(indices[1]-indices[0] == 15,1)
            bits.put(*codes[word])
        for index in sorted(remaining):
            bits.put(0,1); bits.put(index,8); bits.put(ord(cells[index].upper())-65,5)
    return bits.data, len(chosen), len(remaining)


def uncover(data, tree, bitmap):
    bits = Bits(data)
    cells = ['']*225
    if bitmap:
        occupied = [bool(bits.get(1)) for _ in cells]
        candidates = runs(['A' if present else '' for present in occupied])
        width = math.ceil(math.log2(max(1, len(candidates))))
        while bits.get(1):
            indices, _ = candidates[bits.get(width)]
            word = read_word(bits,tree)
            assert len(word) == len(indices)
            for index, letter in zip(indices,word):
                assert not cells[index] or cells[index] == letter
                cells[index] = letter
        for index, present in enumerate(occupied):
            if present and not cells[index]: cells[index] = chr(65+bits.get(5))
    else:
        for _ in range(bits.get(8)):
            kind, start = bits.get(1), bits.get(8)
            if kind:
                step = 15 if bits.get(1) else 1
                word = read_word(bits,tree)
                for offset, letter in enumerate(word):
                    index = start+offset*step
                    assert not cells[index] or cells[index] == letter
                    cells[index] = letter
            else: cells[start] = chr(65+bits.get(5))
    assert bits.offset == len(data)
    return cells


def url_size(bits):
    # Byte padding, two-byte CRC, and unpadded base64url, as in production.
    return math.ceil((math.ceil(bits/8)+2)*8/6)


def summarize(values):
    return {'mean':round(statistics.mean(values),2), 'median':statistics.median(values),
            'p95':sorted(values)[math.ceil(.95*len(values))-1], 'max':max(values)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--counts', nargs='+', required=True)
    parser.add_argument('--positions', required=True)
    parser.add_argument('--output', required=True)
    parser.add_argument('--limit', type=int, help='Retain only this many most frequent words')
    args = parser.parse_args()
    counts = collections.Counter()
    for filename in args.counts:
        for line in Path(filename).read_text().splitlines():
            word, count = line.split('\t'); counts[word] += int(count)
    total_occurrences = sum(counts.values())
    if args.limit: counts = collections.Counter(dict(sorted(counts.items(),key=lambda item:(-item[1],item[0]))[:args.limit]))
    codes, tree = codebook(counts)
    paths = {'packed':[], 'deflate':[], 'bitmap_words':[], 'geometry_words':[], 'best_with_fallback':[]}
    leftovers = []; chosen_words = []
    for cgp in Path(args.positions).read_text().splitlines():
        cells, racks, _, _ = parse(cgp)
        tiles = [letter for letter in cells if letter]
        # Common v1 metadata (44 bits), blanks and racks; occupancy/letters
        # are measured separately for each board codec.
        common = 44 + 2 + 7*sum(letter.islower() for letter in tiles) + 5*len(racks.replace('/',''))
        packed_bits = common + 225 + 5*len(tiles)
        paths['packed'].append(url_size(packed_bits))
        compressor = zlib.compressobj(9,zlib.DEFLATED,-15)
        paths['deflate'].append(len(base64.urlsafe_b64encode(compressor.compress(cgp.encode())+compressor.flush()).rstrip(b'=')))
        sizes = [packed_bits]
        for bitmap, name in [(True,'bitmap_words'),(False,'geometry_words')]:
            data, chosen, leftover = cover(cells,codes,bitmap)
            assert uncover(data,tree,bitmap) == [letter.upper() for letter in cells]
            sizes.append(common+len(data))
            paths[name].append(url_size(sizes[-1]))
            if bitmap: leftovers.append(leftover); chosen_words.append(chosen)
        paths['best_with_fallback'].append(url_size(min(sizes)+2))
    # Explicit arbitrary-letter/isolated-square fallback, no dictionary match.
    arbitrary = ['']*225
    for index, letter in zip([0,1,2,17,32,224], 'QZXJAQ'): arbitrary[index] = letter
    for bitmap in [True,False]:
        data, _, _ = cover(arbitrary,codes,bitmap)
        assert uncover(data,tree,bitmap) == arbitrary
    book = json.dumps(sorted((word,length) for word,(_,length) in codes.items()),separators=(',',':')).encode()
    result = {'training_word_occurrences':total_occurrences, 'codebook_words':len(codes),
              'codebook_json_bytes':len(book), 'codebook_gzip_bytes':len(gzip.compress(book,mtime=0)),
              'heldout_positions':len(paths['packed']), 'sizes':{name:summarize(values) for name,values in paths.items()},
              'bitmap_mean_word_tokens':round(statistics.mean(chosen_words),2),
              'bitmap_mean_literal_letters':round(statistics.mean(leftovers),2),
              'top_words':counts.most_common(20)}
    Path(args.output).write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps(result,indent=2))

if __name__ == '__main__': main()
