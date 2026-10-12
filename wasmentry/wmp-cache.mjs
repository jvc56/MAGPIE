const DATABASE = "magpie-wmp";
const DATABASE_VERSION = 1;
const FORMAT = 2;
// Bump whenever builder or pruning semantics change, even with identical KWG.
const BUILDER = "wmp-wit-2026-10-review2";

async function open() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(DATABASE, DATABASE_VERSION);
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains("files")) request.result.createObjectStore("files");
      if (!request.result.objectStoreNames.contains("metadata")) request.result.createObjectStore("metadata", { keyPath: "lexicon" });
    };
    request.onerror = () => reject(request.error);
    let blocked = false;
    request.onblocked = () => {
      blocked = true;
      reject(new Error("Close other Magpie tabs to update the word-map cache."));
    };
    request.onsuccess = () => {
      const db = request.result;
      db.onversionchange = () => db.close();
      if (blocked) db.close();
      else resolve(db);
    };
  });
}

async function transaction(mode, perform) {
  const db = await open();
  try {
    return await new Promise((resolve, reject) => {
      const tx = db.transaction(["files", "metadata"], mode);
      const result = perform(tx);
      tx.oncomplete = () => resolve(result?.result);
      tx.onabort = () => reject(tx.error || new Error("WMP cache transaction failed."));
      tx.onerror = () => {}; // onabort reports the final error
    });
  } finally {
    db.close();
  }
}

export function listWMPs() {
  return transaction("readonly", (tx) => tx.objectStore("metadata").getAll());
}
export async function readWMP(lexicon, kwgHash) {
  let metadata, wit;
  const data = await transaction("readonly", (tx) => {
    const request = tx.objectStore("metadata").get(lexicon);
    request.onsuccess = () => { metadata = request.result; };
    const witRequest = tx.objectStore("files").get(`${lexicon}:wit`);
    witRequest.onsuccess = () => { wit = witRequest.result; };
    return tx.objectStore("files").get(lexicon);
  });
  if (!metadata || metadata.format !== FORMAT || metadata.builder !== BUILDER || metadata.wmp_version !== 3 || metadata.board_dim !== 15 || metadata.kwg_sha256 !== kwgHash) return null;
  return data ? { metadata,
    bytes: new Uint8Array(data instanceof Blob ? await data.arrayBuffer() : data),
    wit: wit ? new Uint8Array(wit instanceof Blob ? await wit.arrayBuffer() : wit) : null,
  } : null;
}
export async function saveWMP(lexicon, kwgHash, sha256, source, bytes, wit, witHash) {
  const write = (useBlobs) => transaction("readwrite", (tx) => {
    const files = tx.objectStore("files");
    const metadata = tx.objectStore("metadata");
    metadata.put({ lexicon, format: FORMAT, builder: BUILDER, wmp_version: 3,
      board_dim: 15, kwg_sha256: kwgHash, sha256, source, bytes: bytes.byteLength,
      wit_bytes: wit?.byteLength || 0, wit_sha256: witHash || null });
    files.put(useBlobs ? new Blob([bytes], { type: "application/octet-stream" }) : bytes, lexicon);
    if (wit) files.put(useBlobs ? new Blob([wit], { type: "application/octet-stream" }) : wit, `${lexicon}:wit`);
    else files.delete(`${lexicon}:wit`);
  });
  try {
    await write(true);
  } catch (error) {
    // Some WebKit environments cannot persist Blob data from a worker. The
    // aborted transaction is atomic; retry the same record as typed arrays.
    // Do not retry quota failures or other unrelated storage errors.
    if (error.name !== "UnknownError" || !/Blob\/File data/.test(error.message)) throw error;
    await write(false);
  }
}
export function removeWMP(lexicon) {
  return transaction("readwrite", (tx) => {
    tx.objectStore("files").delete(lexicon);
    tx.objectStore("files").delete(`${lexicon}:wit`);
    tx.objectStore("metadata").delete(lexicon);
  });
}
