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
