// One display order for unordered tile groups. Words and engine identities
// retain their original order; cached results can be reformatted on demand.
export const TILE_ORDERS = {
  alphabetical: "ABCDEFGHIJKLMNOPQRSTUVWXYZ?",
  "blanks-alphabetical": "?ABCDEFGHIJKLMNOPQRSTUVWXYZ",
  vowels: "AEIOUBCDFGHJKLMNPQRSTVWXYZ?",
  "blanks-vowels": "?AEIOUBCDFGHJKLMNPQRSTVWXYZ",
};
export const DEFAULT_TILE_ORDER = "alphabetical";
export function tileAlphabet(order = DEFAULT_TILE_ORDER) {
  return TILE_ORDERS[Object.hasOwn(TILE_ORDERS, order) ? order : DEFAULT_TILE_ORDER];
}
export function compareTiles(a, b, order = DEFAULT_TILE_ORDER) {
  const alphabet = tileAlphabet(order);
  return alphabet.indexOf(a) - alphabet.indexOf(b);
}
export function sortTiles(tiles, order = DEFAULT_TILE_ORDER) {
  return [...tiles].sort((a, b) => compareTiles(a, b, order)).join("");
}
export function formatTileGroups(text, order = DEFAULT_TILE_ORDER) {
  return text
    .replace(/^(\(exch |ex(?:ch)? |-)([A-Z?]+)(?=\)|\s|$)/,
      (_, prefix, tiles) => prefix + sortTiles(tiles, order))
    .replace(/^\(([A-Z?]+)\)/, (_, tiles) => `(${sortTiles(tiles, order)})`)
    .replace(/\(([A-Z?]+)( [+-]\d+\))/g,
      (_, tiles, score) => `(${sortTiles(tiles, order)}${score}`);
}
