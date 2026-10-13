import { parseCGP, toCGP } from "./analysis-model.mjs";
import { writeLanguageLexicon, readLanguageLexicon, languageForLexicon } from "./lexicon-name.mjs";

// v1/v3: MSB-first fields, described in docs/position-links.md. CRC-16/CCITT-FALSE
// detects accidental damage; it is not authentication. No dictionary required.
// Version 2 was used for the offline word-codec example; do not reinterpret it.
const VERSION = 3;
// Wire IDs are fixed independently of the settings menu ordering.
const LEGACY_LEXICA = ["CSW24", "NWL23"];
// 136 for a registered pair; bounded literal tags/names allow future codecs.
const MAX_LENGTH = 563;
const invalid = () => new Error("This position link is damaged or incomplete. Ask for a new link or paste its CGP.");

function checksum(bytes) {
  let crc = 0xffff;
  for (const byte of bytes) {
    crc ^= byte << 8;
    for (let bit = 0; bit < 8; bit++) {
      crc = ((crc << 1) ^ (crc & 0x8000 ? 0x1021 : 0)) & 0xffff;
    }
  }
  return crc;
}
function base64(bytes) {
  return btoa(String.fromCharCode(...bytes)).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
export function encodePosition(position) {
  // Existing CGP callers have no language field; this board codec is English.
  const language = position.language ?? languageForLexicon(position.lexicon) ?? 'en';
  if (language !== 'en') throw new Error('This position codec supports English tiles only.');
  // Validate the English board/racks independently of the browser’s installed
  // lexicons. Unknown names are data and must never reach an engine command.
  const checked = parseCGP(toCGP({...position, lexicon:"CSW24"}));
  const bytes = [];
  let offset = 0;
  const write = (value, bits) => {
    for (let bit = bits - 1; bit >= 0; bit--, offset++) {
      const index = offset >> 3;
      bytes[index] = (bytes[index] || 0) | (((value >> bit) & 1) << (7 - (offset & 7)));
    }
  };
  write(VERSION, 4);
  writeLanguageLexicon({language, lexicon:position.lexicon}, write);
  for (const score of checked.scores) write(score + 10000, 15);
  write(checked.zeros, 3);
  for (const rack of checked.racks) write(rack.length, 3);
  const cells = checked.board.flat();
  for (const tile of cells) write(tile ? 1 : 0, 1);
  const tiles = cells.filter(Boolean);
  for (const tile of tiles) write(tile.toUpperCase().charCodeAt(0) - 65, 5);
  const blanks = tiles.flatMap((tile, index) => tile === tile.toLowerCase() ? [index] : []);
  write(blanks.length, 2);
  for (const index of blanks) write(index, 7);
  for (const rack of checked.racks) {
    for (const tile of rack) write(tile === "?" ? 26 : tile.charCodeAt(0) - 65, 5);
  }
  const crc = checksum(bytes);
  bytes.push(crc >> 8, crc & 255);
  return base64(bytes);
}

export function decodePosition(text) {
  if (typeof text !== "string" || !text.length || text.length > MAX_LENGTH ||
      !/^[A-Za-z0-9_-]+$/.test(text) || text.length % 4 === 1) throw invalid();
  let bytes;
  try {
    bytes = Uint8Array.from(atob(text.replace(/-/g, "+").replace(/_/g, "/")), letter => letter.charCodeAt(0));
  } catch { throw invalid(); }
  if (bytes.length < 3 || base64(bytes) !== text) throw invalid();
  const payload = bytes.subarray(0, -2);
  if (checksum(payload) !== (bytes.at(-2) << 8 | bytes.at(-1))) throw invalid();
  let offset = 0;
  const read = bits => {
    if (offset + bits > payload.length * 8) throw invalid();
    let value = 0;
    for (let bit = 0; bit < bits; bit++, offset++) {
      value = (value << 1) | ((payload[offset >> 3] >> (7 - (offset & 7))) & 1);
    }
    return value;
  };
  const version = read(4);
  if (version !== 1 && version !== VERSION) throw new Error("This position link uses an unsupported format. Try opening it in a newer Magpie release.");
  let metadata;
  try {
    metadata = version === 1 ? {language:'en', lexicon:LEGACY_LEXICA[read(1)]} : readLanguageLexicon(read);
  } catch { throw invalid(); }
  if (metadata.language !== 'en') throw new Error('This position link uses a language this browser preview cannot load. It supports English tiles only.');
  const scores = [read(15) - 10000, read(15) - 10000];
  const zeros = read(3);
  const lengths = [read(3), read(3)];
  const occupied = Array.from({length:225}, () => read(1));
  const tiles = occupied.filter(Boolean).map(() => {
    const letter = read(5);
    if (letter > 25) throw invalid();
    return String.fromCharCode(65 + letter);
  });
  const blanks = read(2);
  if (blanks > 2) throw invalid();
  let previous = -1;
  for (let blank = 0; blank < blanks; blank++) {
    const index = read(7);
    if (index <= previous || index >= tiles.length) throw invalid();
    tiles[index] = tiles[index].toLowerCase();
    previous = index;
  }
  const racks = lengths.map(length => Array.from({length}, () => {
    const letter = read(5);
    if (letter > 26) throw invalid();
    return letter === 26 ? "?" : String.fromCharCode(65 + letter);
  }).join(""));
  let tileIndex = 0;
  const cells = occupied.map(present => present ? tiles[tileIndex++] : "");
  const board = Array.from({length:15}, (_, row) => cells.slice(row * 15, row * 15 + 15));
  try {
    const position = parseCGP(toCGP({board, racks, scores, zeros, lexicon:"CSW24"}));
    // Exact field lengths and zero padding reject trailing data, without
    // re-encoding an escaped name that may acquire a registry ID in the future.
    const padding = payload.length * 8 - offset;
    if (padding >= 8 || read(padding) !== 0) throw invalid();
    return {...position, ...metadata};
  } catch { throw invalid(); }
}

export function positionLink(position, pageURL) {
  const url = new URL(pageURL);
  url.search = "";
  url.hash = "";
  url.searchParams.set("p", encodePosition(position));
  return url.href;
}
export function positionFromURL(pageURL) {
  const values = new URL(pageURL).searchParams.getAll("p");
  if (!values.length) return null;
  if (values.length !== 1) throw invalid();
  return decodePosition(values[0]);
}
