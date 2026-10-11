import { test, expect } from "@playwright/test";
import { sortTiles, formatTileGroups } from "../tile-order.mjs";
import { sortRack, remainingRack } from "../move-entry.mjs";

const orders = {
  alphabetical: "ABCEY??",
  "blanks-alphabetical": "??ABCEY",
  vowels: "AEBCY??",
  "blanks-vowels": "??AEBCY",
};

test("alphagrams preserve inventory and only reorder unordered move groups", () => {
  expect(sortTiles("B?EA?YC")).toBe("ABCEY??");
  expect(sortTiles("B?EA?YC", "invalid")).toBe("ABCEY??");
  expect(sortTiles("", "vowels")).toBe("");
  for (const [order, expected] of Object.entries(orders)) {
    expect(sortTiles("B?EA?YC", order)).toBe(expected);
    expect(formatTileGroups("ex B?EA?YC", order)).toBe(`ex ${expected}`);
    expect(formatTileGroups("(exch B?EA?YC)", order)).toBe(`(exch ${expected})`);
    expect(formatTileGroups("-B?EA?YC +0 42", order)).toBe(`-${expected} +0 42`);
    expect(formatTileGroups("(B?EA?YC) +20 42", order)).toBe(`(${expected}) +20 42`);
    expect(formatTileGroups("8H CAT (B?EA?YC +20)", order)).toBe(`8H CAT (${expected} +20)`);
    for (const word of ["E7 N(A)RWAL", "8H (CAT)S", "8H TRAIn", "pass"]) {
      expect(formatTileGroups(word, order)).toBe(word);
    }
  }
});

test("sorting with staged duplicate tiles and a designated blank preserves rack ownership", () => {
  const rack = "BAB?AE?";
  const tiles = [
    { row: 7, col: 7, letter: "B", rackIndex: 2 },
    { row: 7, col: 8, letter: "A", rackIndex: 4 },
    { row: 7, col: 9, letter: "z", rackIndex: 6 },
  ];
  for (const order of Object.keys(orders)) {
    const sorted = sortRack(rack, tiles, order);
    expect(sorted.rack).toBe(sortTiles(rack, order));
    expect(remainingRack(sorted.rack, sorted.tiles).join("")).toBe(sortTiles("BA?E", order));
    expect(sorted.tiles.map(tile => [tile.row, tile.col, tile.letter])).toEqual(tiles.map(tile => [tile.row, tile.col, tile.letter]));
    // Both selected duplicates and the second blank keep their identity.
    for (const [index, letter] of [[0,"B"], [1,"A"], [2,"?"]]) {
      expect(sorted.tiles[index].rackIndex).toBe(sorted.rack.lastIndexOf(letter));
    }
    expect(sortRack(sorted.rack, sorted.tiles, "alphabetical")).toEqual(sortRack(rack, tiles, "alphabetical"));
  }
});

async function rackLetters(page) {
  return page.locator("#rack-tiles .rack-tile").evaluateAll(tiles => tiles.map(tile => tile.dataset.letter).join(""));
}
async function poolLetters(page) {
  return page.locator("#unseen-tiles span").evaluateAll(tiles => tiles.map(tile => tile.dataset.letter).join(""));
}
const leaves = page => page.locator('#results tbody [data-column="leave"]').allTextContents();

test("independent alphagram and pool preferences update existing results and persist on mobile", async ({page}) => {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", { timeout: 30000 });
  expect(await poolLetters(page)).toBe("AEIOUBCDFGHJKLMNPQRSTVWXYZ?");
  await page.locator("#position-editor > summary").click();
  await page.locator("#rack").fill("B?EA?YC");
  await page.locator("#rack").blur();
  await page.locator("#sort-rack").click();
  expect(await rackLetters(page)).toBe("ABCEY??");
  await page.locator('[data-mode="kibitz"]').click();
  await expect(page.locator("#status")).toHaveText("Complete", { timeout: 30000 });
  const originalLeaves = await leaves(page);
  expect(originalLeaves.length).toBeGreaterThan(1);
  expect(originalLeaves.every(leave => leave === sortTiles(leave))).toBe(true);
  const originalMoves = await page.locator('#results tbody [data-column="move"]').allTextContents();
  const originalPool = await poolLetters(page);
  await page.locator("#open-settings").click();
  await expect(page.locator("#tile-order")).toHaveValue("alphabetical");
  await expect(page.locator("#unseen-order")).toHaveValue("vowels");
  for (const [order, expected] of Object.entries(orders)) {
    await page.selectOption("#tile-order", order);
    expect(await rackLetters(page)).toBe(expected);
    expect(await leaves(page)).toEqual(originalLeaves.map(leave => sortTiles(leave, order)));
    expect(await poolLetters(page)).toBe(originalPool);
  }
  expect(await leaves(page)).not.toEqual(originalLeaves);
  expect(await page.locator('#results tbody [data-column="move"]').allTextContents()).toEqual(originalMoves);
  await expect(page.locator("#status")).toHaveText("Complete");
  await page.selectOption("#unseen-order", "alphabetical");
  expect(await poolLetters(page)).toBe(sortTiles(originalPool));
  expect(await rackLetters(page)).toBe("??AEBCY");
  await page.setViewportSize({ width: 390, height: 844 });
  await expect(page.locator("#unseen-order")).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth)).toBe(390);
  await page.screenshot({path:test.info().outputPath("tile-order-settings-mobile.png"),fullPage:true});
  await page.reload();
  await expect(page.locator("#status")).toHaveText("Engine ready", { timeout: 30000 });
  await page.locator("#open-settings").click();
  await expect(page.locator("#tile-order")).toHaveValue("blanks-vowels");
  await expect(page.locator("#unseen-order")).toHaveValue("alphabetical");
  expect(await poolLetters(page)).toBe("ABCDEFGHIJKLMNOPQRSTUVWXYZ?");
});

test("changing alphagram order keeps pending tiles and preserves subsequent manual rack ordering", async ({page}) => {
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", { timeout: 30000 });
  await page.locator("#new-game").click();
  await page.getByRole("button", {name:"Record a game",exact:true}).click();
  await page.locator("#new-game-form button.primary").click();
  await expect(page.locator("#status")).toHaveText("Game ready · enter the first rack");
  await page.locator("#rack").fill("BAB?AE?");
  await page.locator("#rack").dispatchEvent("change");
  await page.locator('#board [data-row="7"][data-col="7"]').click();
  await page.keyboard.press("b");
  await page.keyboard.press("a");
  await page.keyboard.press("Shift+Z");
  await expect(page.locator("#board .pending")).toHaveCount(3);
  await page.locator("#open-settings").click();
  await page.selectOption("#tile-order", "blanks-vowels");
  await page.locator("#back-to-game").click();
  expect(await rackLetters(page)).toBe("?AEB");
  await expect(page.locator("#board .pending .tile-letter")).toHaveText(["B", "A", "Z"]);
  await page.locator("#recall-tiles").click();
  expect(await rackLetters(page)).toBe("??AAEBB");
  const lastTile = page.locator("#rack-tiles .rack-tile").last();
  await lastTile.focus();
  await page.keyboard.press("Alt+ArrowLeft");
  await page.keyboard.press("Alt+ArrowLeft");
  expect(await rackLetters(page)).toBe("??AABEB");
  await page.locator("#open-settings").click();
  await page.selectOption("#unseen-order", "alphabetical");
  await page.locator("#back-to-game").click();
  expect(await rackLetters(page)).toBe("??AABEB");
  await page.locator("#sort-rack").click();
  expect(await rackLetters(page)).toBe("??AAEBB");
});
