// URL transport only. The native codecs validate versions, checksums and fields.
export const MAX_LINK_TOKEN = 65536;
export const XS_OPERATIONS = { encodePosition: 0, decodePosition: 1, encodeGame: 2, decodeGame: 3 };

export function validateLinkToken(token) {
  if (typeof token !== "string" || token.length < 10 || token.length > MAX_LINK_TOKEN || !/^[A-Za-z0-9_-]+$/.test(token)) {
    throw new Error("This shared link is incomplete, damaged, or too long.");
  }
  return token;
}

export function readSharedLink(href) {
  const params = new URL(href).searchParams;
  const count = params.getAll("xsp").length + params.getAll("xsg").length;
  if (!count) return null;
  if (count !== 1) throw new Error("Use a link containing just one position or game.");
  const kind = params.has("xsp") ? "xsp" : "xsg";
  return { kind, token: validateLinkToken(params.get(kind)) };
}

export function sharedLink(href, kind, token) {
  if (!["xsp", "xsg"].includes(kind)) throw new Error("Unknown link type.");
  const url = new URL(href);
  url.search = "";
  url.hash = "";
  url.searchParams.set(kind, validateLinkToken(token));
  return url.href;
}

export function xsAssets(operation, profile) {
  const lexica = operation === "decodePosition" || operation === "decodeGame"
    ? ["CSW24", "NWL23"] : [profile === 2 ? "NWL23" : "CSW24"];
  const extensions = operation.endsWith("Position") ? ["xsw.json.gz"] : ["kwg", "klv2"];
  return lexica.flatMap(lexicon => extensions.map(extension => `xs/english-v1/${lexicon}.${extension}`));
}
