export function parsePegProgress(text) {
  const line = text.split("\n").find((value) => value.startsWith("peginfo "));
  return line ? JSON.parse(line.slice(8)) : null;
}
const depthOrder = (depth) =>
  depth === "full" ? Number.MAX_SAFE_INTEGER : parseInt(depth) || 0;
const depthLabel = (depth) => (depth === 0 ? "greedy" : `${depth}-ply`);

// Retain the last measured value while a survivor awaits its deeper search.
// Membership comes from the engine, never from a temporary leaderboard gap.
export function mergePegRows(previous, parsed, progress) {
  if (!progress) return parsed;
  const rows = new Map(previous.map((row) => [row.move, { ...row }]));
  for (const row of progress.baseline)
    rows.set(row.move, {
      ...rows.get(row.move),
      ...row,
      depth: depthLabel(row.depth),
    });
  for (const row of progress.entries)
    rows.set(row.move, {
      ...rows.get(row.move),
      ...row,
      depth: depthLabel(row.depth),
    });
  for (const move of progress.moves)
    if (!rows.has(move)) rows.set(move, { move, depth: "—" });
  if (progress.done)
    for (const row of parsed)
      rows.set(row.move, {
        ...rows.get(row.move),
        ...row,
        depth:
          row.depth === "current" ? depthLabel(progress.depth) : row.depth,
      });
  const field = new Set(progress.moves);
  const evaluated = new Set(progress.entries.map((row) => row.move));
  const completeField =
    progress.stage > 0 && field.size >= progress.fieldSize;
  for (const row of rows.values()) {
    row.state =
      completeField && !field.has(row.move)
        ? "Pruned"
        : progress.evaluating === row.move
          ? "Evaluating"
          : evaluated.has(row.move)
            ? "Evaluated"
            : progress.done
              ? "Not deepened"
              : progress.stage > 0
                ? "Queued"
                : "Evaluated";
  }
  const sorted = [...rows.values()].sort(
    (a, b) =>
      Number(a.state === "Pruned") - Number(b.state === "Pruned") ||
      depthOrder(b.depth) - depthOrder(a.depth) ||
      (b.win ?? -Infinity) - (a.win ?? -Infinity) ||
      (b.spread ?? -Infinity) - (a.spread ?? -Infinity),
  );
  return sorted.map((row, index) => ({ ...row, rank: index + 1 }));
}
