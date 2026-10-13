import {test, expect} from '@playwright/test';
import {readCompactScore, writeCompactScore} from '../compact-score.mjs';

function encode(score) {
  let bits = '';
  writeCompactScore(score, (value,width) => { bits += value.toString(2).padStart(width,'0'); });
  return bits;
}
function decode(bits) {
  let offset = 0;
  const score = readCompactScore(width => {
    if (offset+width > bits.length) throw new Error('Truncated');
    const value = parseInt(bits.slice(offset,offset+width),2);
    offset += width;
    return value;
  });
  expect(offset).toBe(bits.length);
  return score;
}
const escaped = bytes => '1111111111'+bytes.map(byte=>byte.toString(2).padStart(8,'0')).join('');

test('compact scores pin small fields, ZigZag signs, byte boundaries and int32 extrema', {tag:'@unit'}, () => {
  for (const score of [0,1,899,900,1022]) {
    expect(encode(score)).toBe(score.toString(2).padStart(10,'0'));
    expect(decode(encode(score))).toBe(score);
  }
  for (const [score,bytes] of [
    [-1,[1]], [-64,[127]], [-65,[129,1]], [1023,[254,15]],
    [1024,[128,16]], [8191,[254,127]], [8192,[128,128,1]],
    [-2147483648,[255,255,255,255,15]], [2147483647,[254,255,255,255,15]],
  ]) {
    expect(encode(score)).toBe(escaped(bytes));
    expect(decode(escaped(bytes))).toBe(score);
  }
  // Deterministic full-range sampling, including large negative values.
  let state = 20261013;
  for (let sample=0; sample<1000; sample++) {
    state = (Math.imul(state,1664525)+1013904223)|0;
    expect(decode(encode(state))).toBe(state);
  }
});

test('compact scores reject truncation, redundant forms, overflow and unsafe inputs', {tag:'@unit'}, () => {
  for (const value of [NaN,Infinity,-Infinity,1.5,'42',null,2147483648,-2147483649]) {
    expect(()=>encode(value)).toThrow();
  }
  for (const bytes of [
    [0], [2], [252,15], // unnecessary escape: 0, 1, 1022
    [129,0], // overlong encoding of -1
    [128,128,128,128,16], // >32 bits
    [128,128,128,128,128,1], // >5 bytes
  ]) expect(()=>decode(escaped(bytes))).toThrow();
  for (const score of [0,1022,-1,1023,10000,-2147483648]) {
    const bits = encode(score);
    for (let length=0; length<bits.length; length++) expect(()=>decode(bits.slice(0,length))).toThrow();
  }
});
