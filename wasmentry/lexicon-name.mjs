// Shared XSP/XSG metadata. IDs are array index + 1, scoped by language.
// Append only: never reorder, rename, or reuse a wire ID. Further names can
// always use literal encoding if a table exhausts its fixed-width ID space.
// Production data/lexica/*.kwg in jvc56/MAGPIE-DATA at
// ee46dfdb7d878584ab48e7158edc516aba3bdfc1 (2026-10-13).
// *_super21.klv2 are leave tables, not separate lexicons.
export const LANGUAGE_LEXICONS = Object.freeze([
  Object.freeze({language:'en', lexicons:Object.freeze([
    'CSW24', 'NWL23', 'CSW07', 'CSW12', 'CSW15', 'CSW21', 'NWL20',
    'OSW1', 'OSW2', 'OSW3', 'OSW4', 'OSWI', 'TWL06', 'TWL14', 'TWL98',
  ])}),
  Object.freeze({language:'ca', lexicons:Object.freeze(['DISC2'])}),
  Object.freeze({language:'nl', lexicons:Object.freeze(['DSW25'])}),
  Object.freeze({language:'fr', lexicons:Object.freeze(['FRA20'])}),
  Object.freeze({language:'de', lexicons:Object.freeze(['RD29'])}),
  Object.freeze({language:'pl', lexicons:Object.freeze(['OSPS49'])}),
]);
const utf8 = new TextEncoder();
const decoder = new TextDecoder('utf-8', {fatal:true, ignoreBOM:true});
const invalid = () => new Error('Invalid encoded language or lexicon name.');

function literalBytes(value, limit) {
  if (typeof value !== 'string' || !value.length || value.length > limit) throw invalid();
  const bytes = utf8.encode(value);
  if (bytes.length > limit || decoder.decode(bytes) !== value) throw invalid();
  return bytes;
}
function validateLanguage(language) {
  // Bounded language-tag syntax; deliberately no network/IANA lookup.
  if (typeof language !== 'string' || language.length > 63 ||
      !/^[A-Za-z]{1,8}(?:-[A-Za-z0-9]{1,8})*$/.test(language)) throw invalid();
}
function writeLiteral(bytes, write) {
  write(bytes.length, 8);
  for (const byte of bytes) write(byte, 8);
}
function readLiteral(read, limit) {
  const length = read(8);
  if (!length || length > limit) throw invalid();
  try {
    return decoder.decode(Uint8Array.from({length}, () => read(8)));
  } catch { throw invalid(); }
}

export function languageForLexicon(lexicon) {
  return LANGUAGE_LEXICONS.find(entry => entry.lexicons.includes(lexicon))?.language;
}

// Callbacks read/write unsigned MSB-first fields; readers must enforce bounds.
export function writeLanguageLexicon({language, lexicon}, write) {
  validateLanguage(language);
  const name = literalBytes(lexicon, 255);
  const languageId = LANGUAGE_LEXICONS.findIndex(entry => entry.language === language) + 1;
  write(languageId, 4);
  if (!languageId) writeLiteral(literalBytes(language, 63), write);
  const lexiconId = languageId ? LANGUAGE_LEXICONS[languageId - 1].lexicons.indexOf(lexicon) + 1 : 0;
  write(lexiconId, 5);
  if (!lexiconId) writeLiteral(name, write);
}

export function readLanguageLexicon(read) {
  const languageId = read(4);
  if (languageId > LANGUAGE_LEXICONS.length) throw invalid();
  const language = languageId ? LANGUAGE_LEXICONS[languageId - 1].language : readLiteral(read, 63);
  validateLanguage(language);
  // Accept literal known tags/names too, so adding registry entries later
  // cannot change or invalidate older links.
  const entry = LANGUAGE_LEXICONS.find(item => item.language === language);
  const lexiconId = read(5);
  const lexicon = lexiconId ? entry?.lexicons[lexiconId - 1] : readLiteral(read, 255);
  if (!lexicon) throw invalid();
  return {language, lexicon};
}
