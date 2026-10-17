const DATABASE = "magpie-wmp";
const DATABASE_VERSION = 1;
const FORMAT = 2;
// Bump whenever builder or pruning semantics change, even with identical KWG.
// The name predates dropping the WIT; changing it would discard saved WMPs.
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

// Earlier releases also saved a WIT beside each WMP. Nothing reads it now, so
// delete it to free its storage; the WMP beside it stays valid.
const witKey = (lexicon) => `${lexicon}:wit`;
function dropSavedWIT(tx, metadata) {
  if (!("wit_bytes" in metadata) && !("wit_sha256" in metadata)) return metadata;
  const stripped = { ...metadata };
  delete stripped.wit_bytes;
  delete stripped.wit_sha256;
  tx.objectStore("files").delete(witKey(metadata.lexicon));
  tx.objectStore("metadata").put(stripped);
  return stripped;
}

export async function listWMPs() {
  let files = [];
  await transaction("readwrite", (tx) => {
    const request = tx.objectStore("metadata").getAll();
    request.onsuccess = () => { files = request.result.map((metadata) => dropSavedWIT(tx, metadata)); };
  });
  return files;
}
export async function readWMP(lexicon, kwgHash) {
  let metadata;
  const data = await transaction("readonly", (tx) => {
    const request = tx.objectStore("metadata").get(lexicon);
    request.onsuccess = () => { metadata = request.result; };
    return tx.objectStore("files").get(lexicon);
  });
  if (!metadata || metadata.format !== FORMAT || metadata.builder !== BUILDER || metadata.wmp_version !== 3 || metadata.board_dim !== 15 || metadata.kwg_sha256 !== kwgHash) return null;
  return data ? { metadata,
    bytes: new Uint8Array(data instanceof Blob ? await data.arrayBuffer() : data),
  } : null;
}
export async function saveWMP(lexicon, kwgHash, sha256, source, bytes) {
  const write = (useBlobs) => transaction("readwrite", (tx) => {
    const files = tx.objectStore("files");
    tx.objectStore("metadata").put({ lexicon, format: FORMAT, builder: BUILDER, wmp_version: 3,
      board_dim: 15, kwg_sha256: kwgHash, sha256, source, bytes: bytes.byteLength });
    files.put(useBlobs ? new Blob([bytes], { type: "application/octet-stream" }) : bytes, lexicon);
    files.delete(witKey(lexicon));
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
    tx.objectStore("files").delete(witKey(lexicon));
    tx.objectStore("metadata").delete(lexicon);
  });
}
