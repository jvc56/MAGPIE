const storageKey = "magpie-gcg-diagnostics-v1";
const limit = 150;
let entries = [];
try {
  const saved = JSON.parse(localStorage.getItem(storageKey) || "[]");
  if (Array.isArray(saved)) {
    entries = saved.filter((entry) => typeof entry === "string").slice(-limit);
  }
} catch { /* Diagnostics must work when storage is unavailable. */ }

export function gcgLogText() {
  return entries.join("\n");
}
export function logGCG(stage, details = {}) {
  const line = `${new Date().toISOString()} ${stage} ${JSON.stringify(details)}`;
  entries.push(line.slice(0, 2000));
  entries = entries.slice(-limit);
  // Write before invoking the native chooser, so a browser restart preserves it.
  try { localStorage.setItem(storageKey, JSON.stringify(entries)); } catch {}
  console.info("[Magpie GCG]", line);
  document.dispatchEvent(new Event("gcg-diagnostics"));
}
