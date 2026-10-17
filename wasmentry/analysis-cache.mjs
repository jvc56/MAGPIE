// Separate from the optional large WMP cache. Bump the database name when
// engine changes make previously accumulated simulation statistics incompatible.
function openCache() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open("magpie-analysis-v1", 1);
    request.onupgradeneeded = () => request.result.createObjectStore("positions");
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error);
    request.onblocked = () => reject(new Error("Analysis storage is blocked"));
  });
}

export async function loadAnalyses() {
  const db = await openCache();
  try {
    return await new Promise((resolve, reject) => {
      const entries = new Map();
      const transaction = db.transaction("positions");
      const cursor = transaction.objectStore("positions").openCursor();
      cursor.onsuccess = () => {
        const entry = cursor.result;
        if (!entry) return;
        if (Array.isArray(entry.value?.results)) entries.set(entry.key, entry.value);
        entry.continue();
      };
      transaction.oncomplete = () => resolve(entries);
      transaction.onabort = () => reject(transaction.error);
    });
  } finally {
    db.close();
  }
}

export async function saveAnalysis(key, value) {
  const db = await openCache();
  try {
    await new Promise((resolve, reject) => {
      const transaction = db.transaction("positions", "readwrite");
      transaction.objectStore("positions").put(value, key);
      transaction.oncomplete = resolve;
      transaction.onabort = () => reject(transaction.error);
    });
  } finally {
    db.close();
  }
}
