import { test, expect } from "@playwright/test";
import {
  SAMPLES,
  parseCGP,
  toCGP,
  inventory,
  previewMove,
  parseResults,
  analysisCommands,
} from "../analysis-model.mjs";

test("CGP validates tile inventory and preserves blank tiles", () => {
  for (const [name, cgp] of Object.entries(SAMPLES)) {
    const position = parseCGP(cgp);
    expect(parseCGP(toCGP(position))).toEqual(position);
    expect(inventory(position).bagCount).toBe(
      { opening: 86, midgame: 9, peg: 1, endgame: 0 }[name],
    );
  }
  expect(() => parseCGP(SAMPLES.opening.replace("AEINRST/", "ZZ/"))).toThrow(
    "Too many Z",
  );
  expect(() =>
    parseCGP(SAMPLES.opening.replace("AEINRST/", "AEINRST/ZZ")),
  ).toThrow("unavailable Z");
  expect(() => parseCGP(SAMPLES.opening + " -threads 999")).toThrow(
    "only the -lex option",
  );
});

test("candidate preview follows board coordinates and preserves played-through tiles", () => {
  const position = parseCGP(SAMPLES.midgame);
  expect(previewMove(position, "14F ZI(N)E")).toEqual([
    { row: 13, col: 5, letter: "Z" },
    { row: 13, col: 6, letter: "I" },
    { row: 13, col: 8, letter: "E" },
  ]);
  expect(previewMove(parseCGP(SAMPLES.opening), "H8 aT")).toEqual([
    { row: 7, col: 7, letter: "a" },
    { row: 8, col: 7, letter: "T" },
  ]);
  expect(previewMove(position, "(exch E)")).toEqual([]);
});

test("result adapters preserve pass, empty leaves, uncertainty and endgame continuations", () => {
  const sim = parseResults(
    "1: 8H RETAINS 66 0 54.2 0.3 42.7 0.2 66.0 512",
    "sim",
  )[0];
  expect(sim).toMatchObject({
    leave: "",
    score: 66,
    win: 54.2,
    winSE: 0.3,
    equity: 42.7,
    iterations: 512,
  });
  const lines = parseResults(
    "pass -70 -16 6B (E)F 10\n6B (E)F 10 -74 -20 pass\n7E (O)F 10 (V +8) -80 -26",
    "endgame",
  );
  expect(lines).toHaveLength(3);
  expect(lines[0]).toMatchObject({
    move: "pass",
    score: 0,
    value: -70,
    spread: -16,
    continuation: "6B (E)F 10",
  });
  expect(lines[1]).toMatchObject({
    score: 10,
    value: -74,
    spread: -20,
    continuation: "pass",
  });
  expect(
    parseResults("full 1 8H RETAINS 4 1 0 90.00 +12.5", "peg")[0],
  ).toMatchObject({ depth: "full", win: 90, spread: 12.5 });
});

test("mode guards reject wrong bag sizes and infer the empty-bag opponent rack", () => {
  const settings = { seconds: 10, threads: 4, candidates: 50, plies: 4 };
  expect(() =>
    analysisCommands(parseCGP(SAMPLES.opening), "peg", settings),
  ).toThrow("1–4");
  expect(() =>
    analysisCommands(parseCGP(SAMPLES.peg), "endgame", settings),
  ).toThrow("empty bag");
  const position = parseCGP(SAMPLES.endgame.replace("FV/AADIZ", "FV/"));
  expect(analysisCommands(position, "endgame", settings)[1]).toContain(
    "FV/AADIZ",
  );
  expect(position.racks[1]).toBe("");
});
