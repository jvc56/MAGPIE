import { openSample } from "./helpers/startup.js";
import { test, expect } from "@playwright/test";
import { mergePegRows, parsePegProgress } from "../peg-review.mjs";

test("live PEG preserves baseline values and prunes only from authoritative stage membership", { tag: "@unit" }, () => {
  const prior = [
    { move: "A1 CAT", score: 12, win: 80, depth: "greedy" },
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
    score: 12,
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
  // Exercise real PEG with a bounded candidate set, independent of how many
  // candidates a slow CI runner can evaluate in ten seconds.
  await page.addInitScript(() => {
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message, ...args) {
      if (message.type === "run") {
        message.data.commands = message.data.commands.map(command => {
          if (!command.startsWith("peg ")) return command;
          const played = command.match(/^peg leaves1 (\S+) /)[1];
          // The rational opponent keeps the greedy stage quick at 3 in the
          // bag; the pessimistic one tries every reply on every draw.
          return command.replace(/^peg leaves1 \S+ /, `peg only ${played},pass `)
            .replace(/-pegtlim \S+/, "-pegtlim 0") + " -pegtopk 2,2 -pegpess false";
        });
      }
      return post.call(this, message, ...args);
    };
  });
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {
    timeout: 30000,
  });
  await openSample(page);
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
  await expect(page.locator("#results tbody tr")).not.toHaveCount(0);
  await expect(page.locator('#results thead [data-column=score]')).toHaveText('Score');
  await expect(page.locator('#results tbody tr [data-column=score]').first()).toHaveText(/^\d+$/);
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
  if (await page.locator("#stop").isEnabled()) await page.locator("#stop").click();
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

test("completed full-depth PEG results precede unfinished shallower candidates", { tag: "@unit" }, () => {
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


test("a complete PEG's solved plays outrank what cut bounds push out of the entries", { tag: "@unit" }, () => {
  const rows = mergePegRows(
    [{ move: "A1 CAT", win: 38.9, depth: "greedy" }],
    [],
    {
      done: true,
      stage: 1,
      depth: 40,
      fieldSize: 2,
      stages: [],
      moves: ["A1 CAT", "B1 DOG"],
      evaluating: null,
      baseline: [],
      entries: [
        { move: "B1 DOG", win: 84.7, wins: null, ties: null, losses: null, cut: true, depth: 40 },
      ],
      solved: [
        { move: "A1 CAT", win: 55.6, wins: 40, ties: 0, losses: 32, cut: false, depth: 40 },
      ],
    },
  );
  const played = rows.find((row) => row.move === "A1 CAT");
  expect(played).toMatchObject({ win: 55.6, depth: "40-ply", state: "Evaluated" });
  expect(rows.find((row) => row.move === "B1 DOG")).toMatchObject({ cut: true, wins: null });
});

test("manually adding an existing candidate keeps it through PEG stages", async ({page}) => {
  test.setTimeout(60000);
  await page.addInitScript(() => {
    const post = Worker.prototype.postMessage;
    Worker.prototype.postMessage = function(message, ...args) {
      if (message.type === "run") {
        message.data.commands = message.data.commands.map(command => {
          if (!command.startsWith("peg ")) return command;
          window.pegCommand = command;
          const protectedMoves = command.match(/^peg leaves1 (\S+) /)[1];
          return command.replace(/^peg leaves1 \S+ /, `peg only ${protectedMoves},pass `)
            .replace(/-pegtlim \S+/, "-pegtlim 2") + " -pegtopk 2 -pegstride 8";
        });
      }
      return post.call(this, message, ...args);
    };
  });
  await page.goto("/wasmentry/");
  await expect(page.locator("#status")).toHaveText("Engine ready", {timeout:30000});
  await openSample(page);
  await page.locator("#gcg-file").setInputFiles("tests/fixtures/review-candidates.gcg");
  await expect(page.locator("#status")).toContainText("Loaded");
  await page.selectOption("#history-position", "21");
  await page.locator("[data-mode=kibitz]").click();
  await expect(page.locator("#stop")).toBeDisabled();
  const candidate = page.locator("#results tbody button").nth(2);
  const label = await candidate.textContent();
  await candidate.click();
  await page.locator("#analyze-move").click();
  await expect(page.locator("#status")).toHaveText("Play added");
  await page.locator("[data-mode=peg]").click();
  await expect(page.locator("#stop")).toBeDisabled({timeout:45000});
  expect(await page.evaluate(() => window.pegCommand)).toMatch(/^peg leaves1 [^ ]+,[^ ]+ /);
  const row = page.locator("#results tbody tr").filter({has:page.locator("button").filter({hasText:label})});
  await expect(row).toHaveCount(1);
  await expect(row).not.toHaveClass(/peg-pruned/);
  await expect(page.locator("#analysis-meta")).not.toContainText("protected");
});
