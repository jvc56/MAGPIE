import { test, expect } from "@playwright/test";
import { parseCGP, SAMPLES } from "../analysis-model.mjs";
import {
  placeTile,
  remainingRack,
  moveNotation,
  normalizeMove,
} from "../move-entry.mjs";
test("entry consumes real tiles before blanks and never overwrites the board", () => {
  const position = parseCGP(SAMPLES.opening);
  position.racks[0] = "AAT?";
  let tiles = placeTile(position, [], 7, 7, "a");
  tiles = placeTile(position, tiles, 7, 8, "a");
  tiles = placeTile(position, tiles, 7, 9, "a");
  expect(tiles.map((tile) => tile.letter).join("")).toBe("AAa");
  expect(remainingRack(position.racks[0], tiles)).toEqual(["T"]);
  expect(() => placeTile(position, tiles, 7, 10, "A")).toThrow();
  expect(() => placeTile(position, tiles, 7, 7, "T")).toThrow();
  position.board[8][8] = "O";
  expect(() => placeTile(position, [], 8, 8, "A")).toThrow();
  expect(placeTile(position, [], 7, 7, "a", true)[0].letter).toBe("a");
});
test("notation absorbs existing prefixes and suffixes and rejects gaps and bends", () => {
  const position = parseCGP(SAMPLES.opening);
  position.board[7][7] = "C";
  position.board[7][9] = "T";
  expect(moveNotation(position, [{ row: 7, col: 8, letter: "A" }])).toBe(
    "8H .A.",
  );
  expect(() =>
    moveNotation(position, [
      { row: 0, col: 0, letter: "A" },
      { row: 0, col: 2, letter: "T" },
    ]),
  ).toThrow("gaps");
  expect(() =>
    moveNotation(position, [
      { row: 0, col: 0, letter: "A" },
      { row: 1, col: 1, letter: "T" },
    ]),
  ).toThrow("row or column");
  expect(
    moveNotation(position, [
      { row: 13, col: 14, letter: "A" },
      { row: 14, col: 14, letter: "T" },
    ]),
  ).toBe("O14 AT");
  expect(normalizeMove("8h C(a)T")).toBe("8H CaT");
  expect(normalizeMove("exchange ae?")).toBe("ex AE?");
  expect(() => normalizeMove("8H CAT -lex NWL23")).toThrow();
});

test("duplicate rack tiles keep their slots through placement, movement and rack insertion", async () => {
  const {rackSlots, reorderRack} = await import('../move-entry.mjs');
  const position = parseCGP(SAMPLES.opening);
  position.racks[0] = 'ABAC?';
  const tiles = placeTile(position, [], 7, 7, 'A', false, 2);
  expect(remainingRack(position.racks[0], tiles).join('')).toBe('ABC?');
  const reordered = reorderRack(position.racks[0], tiles, 4, 1);
  expect(remainingRack(reordered.rack, reordered.tiles).join('')).toBe('A?BC');
  expect(reordered.tiles[0].rackIndex).toBe(3);
  const moved = placeTile({...position, racks:[reordered.rack,'']}, [], 8, 8, 'A', false, reordered.tiles[0].rackIndex);
  expect(remainingRack(reordered.rack, moved).join('')).toBe('A?BC');
  const returned = reorderRack(reordered.rack, moved, moved[0].rackIndex, 4);
  expect(returned.rack).toBe('A?BCA');
  expect(rackSlots('A?A', [{row:0,col:0,letter:'A'}, {row:0,col:1,letter:'A',rackIndex:0}]).indexes).toEqual([2,0]);
});
