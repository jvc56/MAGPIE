import { test, expect } from "@playwright/test";
import { mergePegRows, parsePegProgress } from "../peg-review.mjs";

test("live PEG preserves baseline values and prunes only from authoritative stage membership", () => {
  const prior = [
    { move: "A1 CAT", win: 80, depth: "greedy" },
    { move: "B1 DOG", win: 70, depth: "greedy" },
    { move: "C1 HEN", win: 60, depth: "greedy" },
  ];
  const progress = {
    stage: 1,
    depth: 2,
    done: false,
    fieldSize: 2,
    stages: [],
    moves: ["A1 CAT", "B1 DOG"],
    evaluating: "B1 DOG",
    baseline: [],
    entries: [{ move: "A1 CAT", win: 65, depth: 2 }],
  };
  const parsed = parsePegProgress(
    `peginfo ${JSON.stringify(progress)}\nother output`,
  );
  const rows = mergePegRows(prior, [], parsed);
  expect(rows.find((r) => r.move === "B1 DOG")).toMatchObject({
    win: 70,
    depth: "greedy",
    state: "Evaluating",
  });
  expect(rows.find((r) => r.move === "A1 CAT")).toMatchObject({
    win: 65,
    depth: "2-ply",
    state: "Evaluated",
  });
  expect(rows.find((r) => r.move === "C1 HEN")).toMatchObject({
    state: "Pruned",
  });
  // A truncated move list or transient empty leaderboard cannot prove pruning.
  const truncated = mergePegRows(prior, [], { ...progress, fieldSize: 100 });
  expect(truncated.every((row) => row.state !== "Pruned")).toBe(true);
});

test("PEG streams stages and candidates before completion and preserves them in history", async ({
  page,
}) => {
  test.setTimeout(60000);
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  await page
    .locator("#gcg-file")
    .setInputFiles("tests/fixtures/review-candidates.gcg");
  await expect(page.locator("#status")).toContainText("Loaded");
  await page.selectOption("#history-position", "21");
  await page.locator("#open-analysis-settings").click();
  await page.locator("#seconds").fill("10");
  await page.locator("#back-to-game").click();
  await page.locator("[data-mode=peg]").click();
  await expect(page.locator("#peg-progress")).toBeVisible();
  await expect(page.locator("#peg-progress")).toContainText("retained", {
    timeout: 15000,
  });
  await expect(page.locator("body")).toHaveClass(/busy/);
  await expect(page.locator("#results tbody")).toContainText("Evaluating");
  await expect(page.locator("#results tbody")).toContainText("Queued");
  await page.screenshot({
    path: test.info().outputPath("peg-live-desktop.png"),
    fullPage: true,
  });
  await page.setViewportSize({ width: 390, height: 844 });
  await page.locator("#peg-progress").scrollIntoViewIfNeeded();
  await page.evaluate(
    () =>
      new Promise((resolve) =>
        requestAnimationFrame(() => requestAnimationFrame(resolve)),
      ),
  );
  await page.screenshot({
    path: test.info().outputPath("peg-live-mobile.png"),
  });
  await page.locator("#stop").click();
  await expect(page.locator("body")).not.toHaveClass(/busy/);
  await expect(page.locator("#analysis-meta")).toContainText("Played #");
  const stages = await page.locator("#peg-progress").textContent();
  const rows = await page.locator("#results").textContent();
  await page.selectOption("#history-position", "22");
  await expect(page.locator("#peg-progress")).toBeHidden();
  await page.selectOption("#history-position", "21");
  await expect(page.locator("#peg-progress")).toHaveText(stages);
  await expect(page.locator("#results")).toHaveText(rows);
  expect(
    await page.evaluate(
      () => document.documentElement.scrollWidth <= innerWidth,
    ),
  ).toBe(true);
});

test("completed full-depth PEG results precede unfinished shallower candidates", () => {
  const progress = {
    stage: 5,
    depth: 40,
    done: true,
    fieldSize: 2,
    stages: [],
    moves: ["A1 CAT", "B1 DOG"],
    evaluating: null,
    baseline: [{ move: "B1 DOG", win: 90, depth: 8 }],
    entries: [{ move: "A1 CAT", win: 70, depth: 40 }],
  };
  const rows = mergePegRows(
    [],
    [{ move: "A1 CAT", win: 70, depth: "full" }],
    progress,
  );
  expect(rows[0]).toMatchObject({ move: "A1 CAT", depth: "full" });
  expect(rows[1]).toMatchObject({
    move: "B1 DOG",
    depth: "8-ply",
    state: "Not deepened",
  });
});
