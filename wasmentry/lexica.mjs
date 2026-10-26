// Browser profiles mirror the packaged letterdistribution CSV files.
export const LEXICA = ["CSW24", "NWL23", "FRA24"];
const ENGLISH_VALUES = {
  A: 1,
  B: 3,
  C: 3,
  D: 2,
  E: 1,
  F: 4,
  G: 2,
  H: 4,
  I: 1,
  J: 8,
  K: 5,
  L: 1,
  M: 3,
  N: 1,
  O: 1,
  P: 3,
  Q: 10,
  R: 1,
  S: 1,
  T: 1,
  U: 1,
  V: 4,
  W: 4,
  X: 8,
  Y: 4,
  Z: 10,
  "?": 0,
};
const ENGLISH_COUNTS = [
  9, 2, 2, 4, 12, 2, 3, 2, 9, 1, 1, 4, 2, 6, 8, 2, 1, 6, 4, 6, 4, 2, 2, 1, 2,
  1,
];

const FRENCH_VALUES = { ...ENGLISH_VALUES, K: 10, M: 2, Q: 8, W: 10, X: 10, Y: 10 };
const FRENCH_COUNTS = [9, 2, 2, 3, 15, 2, 2, 2, 8, 1, 1, 5, 3, 6, 6, 2, 1, 6, 6, 6, 6, 2, 1, 1, 1, 1];
export function letterDistribution(lexicon) {
  if (!LEXICA.includes(lexicon)) { throw new Error("Choose a supported lexicon."); }
  return lexicon === "FRA24" ? "french" : "english";
}
export function tileValues(lexicon) {
  return letterDistribution(lexicon) === "french" ? FRENCH_VALUES : ENGLISH_VALUES;
}
export function tileCounts(lexicon) {
  return letterDistribution(lexicon) === "french" ? FRENCH_COUNTS : ENGLISH_COUNTS;
}
export function vowelLetters(lexicon) {
  return letterDistribution(lexicon) === "french" ? "AEIOUY" : "AEIOU";
}
