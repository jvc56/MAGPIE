import { french } from "./fr.mjs";

export const LANGUAGES = ["en", "fr"];
export function preferredLanguage(storage, languages = globalThis.navigator?.languages || []) {
  try {
    const saved = (storage === undefined ? globalThis.localStorage : storage)?.getItem("magpie-language");
    if (LANGUAGES.includes(saved)) { return saved; }
  } catch { /* Storage is optional. */ }
  return languages[0]?.toLowerCase().startsWith("fr") ? "fr" : "en";
}
// Accessing localStorage itself may throw in restricted browser contexts.
let language = preferredLanguage();
export function currentLanguage() { return language; }
export function translate(key, values = [], locale = language) {
  const template = locale === "fr" ? french[key] ?? key : key;
  return template.replace(/\{(\d+)\}/g, (match, index) => values[index] ?? match);
}
// Both plain messages and tagged templates use the English text as the key.
// Interpolated player names, moves, and game text are never translated.
export function t(message, ...values) {
  const key = typeof message === "string" ? message
    : message.map((part, index) => part + (index < values.length ? `{${index}}` : "")).join("");
  return translate(key, values);
}

const bindings = [];
export function localizeDocument(root = document.body) {
  // Capture only the authored HTML, once, before dynamic game content is rendered.
  // Keeping text nodes preserves nested controls, keyboard hints, and listeners.
  const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT);
  while (walker.nextNode()) {
    const node = walker.currentNode;
    if (node.parentElement.closest("script, style, code, textarea, svg")) { continue; }
    const key = node.textContent.trim().replace(/\s+/g, " ");
    if (!Object.hasOwn(french, key)) { continue; }
    const leading = node.textContent.match(/^\s*/)[0];
    const trailing = node.textContent.match(/\s*$/)[0];
    bindings.push(() => { if (node.isConnected) { node.textContent = leading + t(key) + trailing; } });
  }
  for (const element of root.querySelectorAll("[title], [aria-label], [placeholder]")) {
    for (const attribute of ["title", "aria-label", "placeholder"]) {
      const key = element.getAttribute(attribute);
      if (key && Object.hasOwn(french, key)) {
        bindings.push(() => element.setAttribute(attribute, t(key)));
      }
    }
  }
  applyLanguage();
}
function applyLanguage() {
  document.documentElement.lang = language;
  for (const update of bindings) { update(); }
}
export function setLanguage(next) {
  if (!LANGUAGES.includes(next)) { throw new Error("Unsupported interface language."); }
  language = next;
  try { localStorage.setItem("magpie-language", next); } catch {}
  applyLanguage();
}
