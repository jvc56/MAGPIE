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

const multiset = (tiles) => [...tiles].sort().join("");
const factorial = (n) => n < 2 ? 1 : n * factorial(n - 1);
const permutations = (text) => factorial(text.length) / [...new Set(text)]
  .reduce((count, tile) => count * factorial([...text].filter(value => value === tile).length), 1);

// Factor only complete Cartesian products or complete permutation sets. A slash
// retains an order-sensitive boundary; letters within each block are a multiset.
function factor(sequences) {
  const unique = [...new Set(sequences)];
  const width = unique[0].length;
  if (!width) return [{blocks:[], sequences:[""]}];
  for (let split = 1; split < width; split++) {
    const left = [...new Set(unique.map(value => value.slice(0, split)))];
    const right = [...new Set(unique.map(value => value.slice(split)))];
    const members = new Set(unique);
    if (unique.length !== left.length * right.length ||
        !left.every(prefix => right.every(suffix => members.has(prefix + suffix)))) continue;
    return factor(left).flatMap(prefix => factor(right).map(suffix => ({
      blocks:[...prefix.blocks, ...suffix.blocks],
      sequences:prefix.sequences.flatMap(a => suffix.sequences.map(b => a + b)),
    })));
  }
  const sorted = multiset(unique[0]);
  if (unique.every(value => multiset(value) === sorted) && unique.length === permutations(sorted))
    return [{blocks:[sorted], sequences:unique}];
  return unique.map(value => ({blocks:[...value], sequences:[value]}));
}

export function groupPegOutcomes(rows) {
  const groups = new Map();
  for (const row of rows) {
    const drawn = multiset(row.drawn);
    const key = JSON.stringify([drawn, multiset(row.remaining), row.margin, row.pv || ""]);
    if (!groups.has(key)) groups.set(key, {drawn, margin:row.margin, pv:row.pv || "", weights:new Map()});
    const group = groups.get(key);
    group.weights.set(row.remaining, (group.weights.get(row.remaining) || 0) + row.weight);
  }
  return [...groups.values()].flatMap(group => factor([...group.weights.keys()]).map(form => ({
    sequence:[group.drawn, ...form.blocks].filter(Boolean).join("/"),
    margin:group.margin,
    weight:form.sequences.reduce((sum, sequence) => sum + group.weights.get(sequence), 0),
    pv:group.pv,
  })));
}

export function sortPegOutcomes(rows, sort) {
  return [...rows].sort((a, b) => {
    const result = sort.startsWith("weight") ? b.weight - a.weight || b.margin - a.margin
      : b.margin - a.margin || b.weight - a.weight;
    return (sort.endsWith("-asc") ? -result : result) || a.sequence.localeCompare(b.sequence);
  });
}

export function renderPegOutcomes(container, data, state) {
  const grouped = groupPegOutcomes(data.rows);
  const totalWeight = data.rows.reduce((sum, row) => sum + row.weight, 0);
  const draw = () => {
    container.replaceChildren();
    const wrapper = document.createElement("div");
    wrapper.className = "peg-outcome-view";
    const toolbar = document.createElement("div");
    toolbar.className = "peg-outcome-toolbar";
    const title = document.createElement("strong");
    title.textContent = `Draw outcomes · ${data.depth ? `${data.depth}-ply` : "greedy"}`;
    const label = document.createElement("label");
    label.textContent = "Sort ";
    const sort = document.createElement("select");
    sort.setAttribute("aria-label", "Sort draw outcomes");
    for (const [value, text] of [["result", "Result: best first"], ["result-asc", "Result: worst first"],
      ["weight", "Weight: most frequent"], ["weight-asc", "Weight: least frequent"]]) {
      const option = document.createElement("option");
      option.value = value;
      option.textContent = text;
      sort.append(option);
    }
    sort.value = state.sort;
    sort.addEventListener("change", () => { state.sort = sort.value; state.page = 0; draw(); });
    label.append(sort);
    toolbar.append(title, label);
    const rows = sortPegOutcomes(grouped, state.sort);
    const pages = Math.max(1, Math.ceil(rows.length / 8));
    state.page = Math.min(state.page, pages - 1);
    const table = document.createElement("table");
    table.className = "peg-outcome-table";
    const header = table.createTHead().insertRow();
    const hasPV = rows.some(row => row.pv);
    for (const text of ["Draw sequence", "Result", "Weight", ...(hasPV ? ["Principal variation"] : [])]) {
      const th = document.createElement("th");
      th.scope = "col";
      th.textContent = text;
      header.append(th);
    }
    const body = table.createTBody();
    for (const row of rows.slice(state.page * 8, state.page * 8 + 8)) {
      const tr = body.insertRow();
      tr.insertCell().textContent = row.sequence || "—";
      const result = tr.insertCell();
      result.textContent = row.margin === 0 ? "Tie" : `${row.margin > 0 ? "Win" : "Loss"} ${Math.abs(row.margin)}`;
      result.className = row.margin > 0 ? "winning" : row.margin < 0 ? "losing" : "tie";
      tr.insertCell().textContent = `${row.weight.toLocaleString()} (${(100 * row.weight / totalWeight).toFixed(1)}%)`;
      if (hasPV) {
        const cell = tr.insertCell();
        if (row.pv) {
          const details = document.createElement("details");
          const summary = document.createElement("summary");
          summary.textContent = "Show line";
          const line = document.createElement("div");
          line.className = "peg-outcome-pv";
          line.textContent = row.pv;
          details.append(summary, line);
          cell.append(details);
        } else cell.textContent = "—";
      }
    }
    const footer = document.createElement("div");
    footer.className = "peg-outcome-toolbar";
    const note = document.createElement("span");
    note.textContent = "Letters within each block are unordered; / separates draw groups. Weight counts possible draws.";
    const pager = document.createElement("span");
    pager.className = "peg-outcome-pager";
    for (const [label, delta] of [["Previous outcomes", -1], ["Next outcomes", 1]]) {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = delta < 0 ? "←" : "→";
      button.setAttribute("aria-label", label);
      button.disabled = state.page + delta < 0 || state.page + delta >= pages;
      button.addEventListener("click", () => { state.page += delta; draw(); });
      if (delta > 0) pager.append(` ${state.page + 1} / ${pages} `);
      pager.append(button);
    }
    footer.append(note, pager);
    wrapper.append(toolbar, table, footer);
    container.append(wrapper);
  };
  draw();
}
