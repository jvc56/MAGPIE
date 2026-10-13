// Shared XSP/XSG score field: ten bits for 0..1022, otherwise escape 1023
// followed by canonical unsigned LEB128 of a ZigZag-encoded signed int32.
const MIN_SCORE = -2147483648;
const MAX_SCORE = 2147483647;
const invalid = () => new Error('Invalid compact score.');

export function writeCompactScore(score, write) {
  if (!Number.isInteger(score) || score < MIN_SCORE || score > MAX_SCORE) throw invalid();
  if (score >= 0 && score < 1023) {
    write(score, 10);
    return;
  }
  write(1023, 10);
  // Arithmetic avoids JS bitwise sign extension at the int32 boundaries.
  let value = score < 0 ? -score * 2 - 1 : score * 2;
  do {
    const byte = value % 128;
    value = Math.floor(value / 128);
    write(byte + (value ? 128 : 0), 8);
  } while (value);
}

export function readCompactScore(read) {
  const small = read(10);
  if (small !== 1023) return small;
  let value = 0;
  for (let index = 0; index < 5; index++) {
    const byte = read(8);
    if (index === 4 && byte > 15) throw invalid();
    value += (byte & 127) * 2 ** (7 * index);
    if (!(byte & 128)) {
      if (index && byte === 0) throw invalid(); // nonminimal LEB128
      const score = value % 2 ? -(value + 1) / 2 : value / 2;
      if (score >= 0 && score < 1023) throw invalid(); // unnecessary escape
      return score;
    }
  }
  throw invalid();
}
