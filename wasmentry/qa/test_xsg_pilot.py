import os
from pathlib import Path
import unittest

import xsg_pilot as codec

GAME = '''#character-encoding UTF-8
#lexicon NWL23
#player1 Alice Alice Example
#player2 Bob Bób Example
>Alice: AAERT 8H TEA +6 6
>Bob: BC 9H BC +10 10
>Bob: BC -- -10 0
>Alice: AAR 7H AR +8 14
>Bob: ?EI -2 +0 0
>Alice: AXYZ -XYZ +0 14
>Bob: I - +0 0
>Alice: A (time) -10 4
>Alice: (I) +2 6
#note This annotation is intentionally discarded.
#rack2 I
'''


class CodecTests(unittest.TestCase):
    def test_roundtrip_partial_racks_names_phony_withdrawal_and_count_exchange(self):
        game = codec.parse_gcg(GAME.encode())
        for carry in [False, True]:
            for probability in [False, True]:
                token, _ = codec.encode(game, carry=carry, probability=probability)
                decoded = codec.decode(token)
                self.assertEqual(game, decoded)
                self.assertEqual(game, codec.parse_gcg(codec.gcg(decoded).encode()))
        self.assertEqual(game['players'][1][1], 'Bób Example')
        self.assertNotIn('#note', codec.gcg(game))
        self.assertEqual(game['events'][4]['kind'], 'count')
        self.assertEqual(game['events'][4]['action'], 2)
        self.assertEqual(game['events'][2]['kind'], 'withdraw')

    def test_probability_weights_are_multiset_frequencies(self):
        pool = tuple({'A':3,'B':2,'C':1}.get(char,0) for char in codec.ALPHABET)
        entries, ranks = codec.draw_table(pool,2)
        self.assertEqual(entries, ('AB','AA','AC','BC','BB'))
        self.assertEqual(ranks['AB'],0)
        # AB has 3*2 ways; AA has C(3,2), not 3*2.

    def test_carryover_overlaps_played_tiles_and_resets_for_missing_tiles(self):
        state = codec.State()
        state.carry[0] = codec.Counter('AAE')
        event = {'player':0,'rack':'AAERT'}
        out = codec.Bits()
        stats = {'draw_ranks':[]}
        codec.write_rack(out,state,event,'ART',3,stats)
        # Union uses max multiplicity, not addition: AAE | ART = AAERT.
        self.assertEqual(out.bits,'0000')
        self.assertEqual(codec.read_rack(codec.Bits(out.bits),state,0,'ART',3),'AAERT')
        event['rack'] = 'AR'
        out = codec.Bits()
        codec.write_rack(out,state,event,'AR',3,stats)
        self.assertEqual(out.bits,'1000')
        self.assertEqual(codec.read_rack(codec.Bits(out.bits),state,0,'AR',3),'AR')

    def test_unknown_exchange_clears_carry_and_blank_is_physical_blank(self):
        state = codec.State()
        state.apply(dict(player=0,rack='AERT',kind='count',action=2,total=0))
        self.assertFalse(state.carry[0])
        game = codec.parse_gcg(GAME.replace('AAERT 8H TEA','AERT? 8H TEa').encode())
        self.assertEqual(game['events'][0]['rack'],'?AERT')
        self.assertEqual(codec.decode(codec.encode(game)[0]),game)

    def test_damage_truncation_bad_geometry_and_varints(self):
        token = codec.encode(codec.parse_gcg(GAME.encode()))[0]
        for bad in ['',token+'=',token[:-1],token[1:],'!'*5]:
            with self.assertRaises((ValueError,EOFError)):
                codec.decode(bad)
        packet = bytearray(codec.base64.urlsafe_b64decode(token+'='*(-len(token)%4)))
        for index in range(len(packet)):
            damaged = packet[:]
            damaged[index] ^= 1
            with self.assertRaises(ValueError):
                codec.decode(codec.base64.urlsafe_b64encode(damaged).decode().rstrip('='))
        for word,location in [('AA','15O'),('.A','8H'),('AAA','O14')]:
            with self.assertRaises(ValueError):
                codec.canonical_move(['']*225,location,word)
        for bits in ['1000000000000000','10000000'*5+'00000001']:
            with self.assertRaises(ValueError):
                codec.Bits(bits).var()

    def test_incomplete_racks_cannot_invent_unrecorded_tiles(self):
        partial = GAME.replace('AAERT 8H TEA','T 8H TEA').replace('AAR 7H AR',' 7H AR')
        game = codec.parse_gcg(partial.encode())
        self.assertEqual(game['events'][0]['rack'],'AET')
        self.assertEqual(game['events'][3]['rack'],'AR')
        self.assertEqual(codec.decode(codec.encode(game)[0]),game)

    def test_rank_codes_roundtrip_with_bounds(self):
        for order in range(7):
            for rank in [0,1,2,63,1023,4096]:
                out = codec.Bits()
                out.rank(order,rank)
                self.assertEqual(len(out.bits),codec.rank_size(rank,order))
                self.assertEqual(codec.Bits(out.bits).rank(order),rank)

    @unittest.skipUnless(os.environ.get('XSG_NATIVE_LIBRARY'), 'Set XSG_NATIVE_LIBRARY for native move-rank replay')
    def test_native_ranks_and_fingerprint_guard(self):
        native = codec.Native(os.environ['XSG_NATIVE_LIBRARY'],os.environ.get('XSG_DATA','data'))
        try:
            game = codec.parse_gcg(b'''#lexicon NWL23
#player1 A Alice
#player2 B Bob
>A: AEGINRT 8B INGRATE +70 70
>B: BC 9H BC +10 10
''')
            token,stats = codec.encode(game,native)
            self.assertGreater(stats['ranked'],0)
            self.assertGreater(stats['explicit'],0)
            native.moves.cache_clear()
            self.assertEqual(codec.decode(token,native),game)
            with self.assertRaisesRegex(ValueError,'profile'):
                codec.decode(token)
            native.binary_hash = b'wrong engine'
            native.fingerprint.cache_clear()
            with self.assertRaisesRegex(ValueError,'profile'):
                codec.decode(token,native)
        finally:
            native.close()


if __name__ == '__main__':
    unittest.main()
