import {test, expect} from '@playwright/test';
import {decodePosition, encodePosition, positionLink, positionFromURL} from '../position-link.mjs';
import {emptyPosition, parseCGP, SAMPLES, toCGP} from '../analysis-model.mjs';
import {openGameActions} from './helpers/startup.js';

function rewrite(token, mutate) {
  const bytes = Buffer.from(token, 'base64url');
  mutate(bytes);
  let crc = 0xffff;
  for (const byte of bytes.subarray(0,-2)) {
    crc ^= byte << 8;
    for (let bit=0;bit<8;bit++) crc = ((crc << 1) ^ (crc & 0x8000 ? 0x1021 : 0)) & 0xffff;
  }
  bytes.writeUInt16BE(crc,bytes.length-2);
  return bytes.toString('base64url');
}

test('compact position encoding preserves CGP fields, blanks, phonies and all 100 tiles', {tag:'@unit'}, () => {
  const full = emptyPosition();
  const distribution = 'AAAAAAAAABBCCDDDDEEEEEEEEEEEEFFGGGHHIIIIIIIIIJKLLLLMMNNNNNNOOOOOOOOPPQRRRRRRSSSSTTTTTTUUUUVVWWXYYZab';
  expect(distribution).toHaveLength(100);
  [...distribution].forEach((tile,index) => full.board[Math.floor(index/15)][index%15] = tile);
  full.scores = [-10000,10000];
  full.zeros = 6;
  const cases = [...Object.values(SAMPLES).map(text=>parseCGP(text)),full,emptyPosition('NWL23')];
  const phony = emptyPosition('NWL23');
  phony.board[7].splice(5,5,...'QzXJA');
  phony.racks = ['?AI','EU'];
  cases.push(phony);
  for (const position of cases) {
    const encoded = encodePosition(position);
    expect(encoded).toMatch(/^[A-Za-z0-9_-]+$/);
    expect(encoded.length).toBeLessThanOrEqual(135);
    expect(toCGP(decodePosition(encoded))).toBe(toCGP(position));
  }
  expect(encodePosition(full)).toHaveLength(135);
  // Fixed wire-format vector, not just encoder/decoder agreement.
  expect(encodePosition(emptyPosition())).toBe("EnEE4gAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAANW3");
});

test('position links reject damaged, unsupported and noncanonical data', {tag:'@unit'}, () => {
  const encoded = encodePosition(parseCGP(SAMPLES.peg));
  for (const bad of ['', '!', 'A', encoded+'=', 'x'.repeat(10000), encoded.slice(1), encoded.slice(0,-1)]) {
    expect(()=>decodePosition(bad)).toThrow(/damaged or incomplete/);
  }
  const bytes = Buffer.from(encoded,'base64url');
  for (let bit=0;bit<bytes.length*8;bit++) {
    const changed = Buffer.from(bytes);
    changed[bit>>3] ^= 1 << (bit%8);
    expect(()=>decodePosition(changed.toString('base64url'))).toThrow(/damaged or incomplete/);
  }
  expect(()=>decodePosition(rewrite(encoded,bytes=>{bytes[0] = (bytes[0]&15)|32;}))).toThrow(/unsupported format/);
  const empty = encodePosition(emptyPosition());
  // Scoreless-turns=7, invalid board letter=31, and nonzero trailing padding.
  expect(()=>decodePosition(rewrite(empty,bytes=>{bytes[4] |= 0x1c;}))).toThrow();
  expect(()=>decodePosition(rewrite(empty,bytes=>{bytes[bytes.length-3] |= 1;}))).toThrow();
  const one = emptyPosition(); one.board[0][0] = 'A';
  expect(()=>decodePosition(rewrite(encodePosition(one),bytes=>{bytes[33] |= 7;bytes[34] |= 0xc0;}))).toThrow();
  expect(()=>positionFromURL('https://example.com/?p='+encoded+'&p='+encoded)).toThrow();
  expect(positionFromURL('https://example.com/?other=1')).toBeNull();
  expect(()=>positionFromURL('https://example.com/?p=')).toThrow();
});

test('position link uses current release path and carries only position data', {tag:'@unit'}, () => {
  const position = parseCGP(SAMPLES.midgame);
  const url = positionLink(position,'https://magpie.birdtest.org/releases/v-next/wasmentry/?other=secret#old');
  expect(url).toMatch(/^https:\/\/magpie.birdtest.org\/releases\/v-next\/wasmentry\/\?p=[\w-]+$/);
  expect(toCGP(positionFromURL(url))).toBe(toCGP(position));
});

test('shared position opens directly, survives reload, and offers a copy fallback', async ({page}) => {
  await page.addInitScript(() => {
    Object.defineProperty(navigator,'clipboard',{value:{writeText:async()=>{throw new Error('Clipboard denied');}}});
  });
  const token = encodePosition(parseCGP(SAMPLES.peg));
  await page.goto('/wasmentry/?p='+token);
  await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
  await expect(page.locator('#new-game-dialog')).toBeHidden();
  await expect(page.locator('#rack')).toHaveValue('ENOSTXY');
  await expect(page.locator('#opponent-rack')).toHaveValue('ACEISUY');
  await expect(page.locator('#current-lexicon')).toHaveText('NWL23');
  await openGameActions(page);
  await page.locator('#copy-position-link').click();
  await expect(page.locator('#position-link-dialog')).toBeVisible();
  expect(await page.locator('#position-link-text').inputValue()).toBe(new URL('/wasmentry/?p='+token,page.url()).href);
  await page.locator('#close-position-link').click();
  await page.reload();
  await expect(page.locator('#rack')).toHaveValue('ENOSTXY');
  await expect(page.locator('#new-game-dialog')).toBeHidden();
});

test('bad position link shows a recoverable message and CGP import still works', async ({page}) => {
  const errors=[]; page.on('pageerror', error=>errors.push(error.message));
  await page.goto('/wasmentry/?p=not-a-position');
  await expect(page.locator('#import-dialog')).toBeVisible();
  await expect(page.locator('#import-error')).toContainText('damaged or incomplete');
  await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
  await page.locator('#cgp').fill(SAMPLES.opening);
  await page.locator('#import-form button[type="submit"]').click();
  await expect(page.locator('#import-dialog')).toBeHidden();
  await expect(page.locator('#rack')).toHaveValue('AEINRST');
  expect(errors).toEqual([]);
});

test('copy succeeds for analysis and cannot export hidden live racks', async ({page}) => {
  await page.addInitScript(() => {
    window.copiedLinks = [];
    Object.defineProperty(navigator,'clipboard',{value:{writeText:async text=>{window.copiedLinks.push(text);}}});
  });
  await page.goto('/wasmentry/?p='+encodePosition(parseCGP(SAMPLES.opening)));
  await expect(page.locator('#status')).toHaveText('Engine ready',{timeout:30000});
  await openGameActions(page);
  await page.locator('#copy-position-link').click();
  await expect(page.locator('#status')).toHaveText('Position link copied');
  expect(await page.evaluate(()=>window.copiedLinks.length)).toBe(1);
  await page.locator('#new-game').click();
  await page.locator('#new-game-form button.primary').click();
  await expect(page.locator('#new-game-dialog')).toBeHidden();
  await expect(page.locator('#name-one')).toHaveText('You · Your turn');
  await expect(page.locator('#copy-position-link')).toBeDisabled();
  // Guard the handler too, rather than relying solely on button state.
  await page.locator('#copy-position-link').dispatchEvent('click');
  expect(await page.evaluate(()=>window.copiedLinks.length)).toBe(1);
});
