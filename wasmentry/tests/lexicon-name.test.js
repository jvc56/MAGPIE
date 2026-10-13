import {test, expect} from '@playwright/test';
import {LANGUAGE_LEXICONS, writeLanguageLexicon, readLanguageLexicon} from '../lexicon-name.mjs';

function bitsFor(metadata) {
  let bits = '';
  writeLanguageLexicon(metadata, (value, width) => { bits += value.toString(2).padStart(width,'0'); });
  return bits;
}
function decode(bits) {
  let offset = 0;
  const metadata = readLanguageLexicon(width => {
    if (offset + width > bits.length) throw new Error('Truncated');
    const value = parseInt(bits.slice(offset, offset+width),2);
    offset += width;
    return value;
  });
  expect(offset).toBe(bits.length);
  return metadata;
}
const literal = text => {
  const bytes = new TextEncoder().encode(text);
  return [bytes.length,...bytes].map(byte=>byte.toString(2).padStart(8,'0')).join('');
};

test('language-local registry pins every current MAGPIE-DATA lexicon ID', {tag:'@unit'}, () => {
  const expected = [
    ['en','CSW24 NWL23 CSW07 CSW12 CSW15 CSW21 NWL20 OSW1 OSW2 OSW3 OSW4 OSWI TWL06 TWL14 TWL98'],
    ['ca','DISC2'], ['nl','DSW25'], ['fr','FRA20'], ['de','RD29'], ['pl','OSPS49'],
  ];
  expect(LANGUAGE_LEXICONS).toHaveLength(6);
  expected.forEach(([language,names], languageIndex) => {
    names.split(' ').forEach((lexicon,lexiconIndex) => {
      const bits = bitsFor({language,lexicon});
      expect(bits).toBe((languageIndex+1).toString(2).padStart(4,'0')+(lexiconIndex+1).toString(2).padStart(5,'0'));
      expect(decode(bits)).toEqual({language,lexicon});
    });
  });
  // The same numeric lexicon ID means different things in different languages.
  expect(decode('000100001')).toEqual({language:'en',lexicon:'CSW24'});
  expect(decode('010000001')).toEqual({language:'fr',lexicon:'FRA20'});
});

test('literal tags/names preserve text, remain forward-compatible, and have strict bounds', {tag:'@unit'}, () => {
  for (const metadata of [
    {language:'en',lexicon:'my / lex + &= ? é漢字'},
    {language:'en',lexicon:'\uFEFFcustom'},
    {language:'en',lexicon:'é'.repeat(127)+'x'},
    {language:'und-x-custom',lexicon:'custom'},
    {language:'en',lexicon:'DISC2'}, // names are local, not globally reserved
  ]) {
    expect(decode(bitsFor(metadata))).toEqual(metadata);
  }
  expect(decode('0000'+literal('en')+'00000'+literal('CSW24'))).toEqual({language:'en',lexicon:'CSW24'});
  for (const metadata of [
    {language:'',lexicon:'a'}, {language:'en_US',lexicon:'a'},
    {language:'en',lexicon:''}, {language:'en',lexicon:'x'.repeat(256)},
    {language:'en',lexicon:'é'.repeat(128)}, {language:'en',lexicon:'\ud800'},
  ]) expect(()=>bitsFor(metadata)).toThrow();
  for (const bits of [
    '1111', // unknown language ID
    '001000010', // unknown Catalan lexicon ID
    '00010000000000000', // empty literal
    '0001000000000000111111111', // invalid UTF-8
  ]) expect(()=>decode(bits)).toThrow();
  const complete = bitsFor({language:'und-x-test',lexicon:'私の辞書'});
  for (let length=0; length<complete.length; length++) {
    expect(()=>decode(complete.slice(0,length))).toThrow();
  }
});
