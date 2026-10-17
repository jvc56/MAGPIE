import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import { resolve } from "node:path";
import { parseCGP, toCGP, inventory } from "../analysis-model.mjs";

const root = fileURLToPath(new URL("../../", import.meta.url));
export function findPlay(cgp) {
  const position = parseCGP(cgp);
  // Never use the computer's hidden rack to choose the human player's move.
  if (inventory(position).bagCount > 0) position.racks[1] = "";
  const commands = [
    `set -lex ${position.lexicon} -wmp false -savesettings false -numplays 5 -s1 equity -s2 equity -r1 all -r2 all`,
    `cgp ${toCGP(position)}`,
    "generate",
    "quit",
    "",
  ].join("\n");
  const started = performance.now();
  const result = spawnSync(resolve(root, "bin/magpie"), [], {
    cwd: root,
    input: commands,
    encoding: "utf8",
    timeout: 10000,
  });
  if (result.error || result.status)
    throw result.error || new Error(result.stderr);
  if (/\(error \d+\)/.test(result.stdout + result.stderr))
    throw new Error(result.stdout + result.stderr);
  const match = result.stdout.match(
    /\b1:\s+((?:[A-O]\d{1,2}|\d{1,2}[A-O])\s+[A-Za-z().?]+|\(exch [A-Z?]+\)|pass)/,
  );
  if (!match)
    throw new Error(
      `No play in native output: ${result.stdout}\n${result.stderr}`,
    );
  const move = match[1]
    .replace(/^\(exch ([A-Z?]+)\)$/, "ex $1")
    .replace(/[()]/g, "");
  return {
    move,
    milliseconds: performance.now() - started,
    cgp: toCGP(position),
    output: result.stdout,
  };
}
if (process.argv[1] === fileURLToPath(import.meta.url)) {
  console.log(
    JSON.stringify(findPlay(process.argv.slice(2).join(" ")), null, 2),
  );
}
