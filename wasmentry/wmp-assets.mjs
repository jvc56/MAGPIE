// Versioned, same-origin WMP assets. Keep format validation before the native
// reader, which assumes trusted, complete files and aborts on short reads.
export const WMP_MAX_BYTES = 256 * 1024 * 1024;
export function wmpAsset(manifest, lexicon, kwgHash, manifestURL) {
  const entry = manifest.lexica?.[lexicon];
  if (manifest.schema !== 1 || manifest.wmp_version !== 3 || manifest.board_dim !== 15 ||
      !entry || entry.kwg_sha256 !== kwgHash || !/^[a-f0-9]{64}$/.test(entry.sha256) ||
      !Number.isSafeInteger(entry.bytes) || entry.bytes < 6 || entry.bytes > WMP_MAX_BYTES)
    throw new Error("The hosted WMP does not match this lexicon or engine. Build it on this device instead.");
  if (entry.compression !== "gzip" || !Array.isArray(entry.files) || entry.files.length < 1 || entry.files.length > 32 ||
      entry.files.reduce((total, part) => total + part.unpacked_bytes, 0) !== entry.bytes)
    throw new Error("The hosted WMP manifest is incomplete.");
  const files = entry.files.map((part, index) => {
    if (part.file !== `wmp/${lexicon}-${entry.sha256}-${String(index).padStart(2, "0")}-gzip.bin` ||
        !/^[a-f0-9]{64}$/.test(part.sha256) || !Number.isSafeInteger(part.bytes) ||
        part.bytes < 1 || part.bytes > 17 * 1024 * 1024 ||
        !Number.isSafeInteger(part.unpacked_bytes) || part.unpacked_bytes < 1 || part.unpacked_bytes > 16 * 1024 * 1024)
      throw new Error("The hosted WMP part is invalid.");
    return { ...part, url: new URL(part.file, manifestURL).href };
  });
  return { ...entry, files };
}

export function validateWMP(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  let offset = 6;
  const invalid = () => { throw new Error("The downloaded WMP is incomplete or invalid."); };
  if (bytes.length < 6 || bytes.length > WMP_MAX_BYTES || bytes[0] !== 3 || bytes[1] !== 15) invalid();
  const take = (size) => {
    if (!Number.isSafeInteger(size) || offset + size > bytes.length) invalid();
    const start = offset;
    offset += size;
    return start;
  };
  const uint = () => view.getUint32(take(4), true);
  for (let length = 2; length <= 15; length++) {
    for (let map = 0; map < 3; map++) {
      const buckets = uint();
      if (!buckets) invalid();
      const starts = take((buckets + 1) * 4);
      const entries = uint();
      const entryStart = take(entries * 32);
      let previous = 0;
      for (let bucket = 0; bucket <= buckets; bucket++) {
        const value = view.getUint32(starts + bucket * 4, true);
        if (value < previous || value > entries || (bucket === 0 && value !== 0)) invalid();
        previous = value;
      }
      if (previous !== entries) invalid();
      if (map === 0) {
        const words = uint();
        const letters = take(words * length);
        for (let entry = 0; entry < entries; entry++) {
          const start = entryStart + entry * 32;
          if (bytes[start] === 0) {
            const wordStart = view.getUint32(start + 8, true);
            const count = view.getUint32(start + 12, true);
            if (wordStart + count * length > words * length) invalid();
          }
        }
        for (let i = letters; i < offset; i++) {
          if (bytes[i] < 1 || bytes[i] > 26) invalid();
        }
      }
    }
  }
  if (offset !== bytes.length) invalid();
}

export async function sha256(bytes) {
  const digest = await crypto.subtle.digest("SHA-256", bytes);
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

export async function downloadWMP(asset, onProgress, signal) {
  if (typeof DecompressionStream !== "function") throw new Error("This browser cannot unpack hosted word maps. Build on this device instead.");
  signal?.throwIfAborted();
  const bytes = new Uint8Array(asset.bytes);
  const transferSize = asset.files.reduce((total, part) => total + part.bytes, 0);
  let received = 0, offset = 0;
  for (const part of asset.files) {
    // IndexedDB owns the persistent copy; avoid a duplicate HTTP cache copy.
    const response = await fetch(part.url, { signal, cache: "no-store" });
    if (!response.ok) throw new Error(`Could not download WMP (HTTP ${response.status}).`);
    const compressed = new Uint8Array(part.bytes);
    const reader = response.body.getReader();
    let partReceived = 0;
    try {
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        if (partReceived + value.length > part.bytes) throw new Error("The WMP download exceeds its declared size.");
        compressed.set(value, partReceived);
        partReceived += value.length;
        received += value.length;
        onProgress(received, transferSize);
      }
    } finally {
      await reader.cancel();
    }
    if (partReceived !== part.bytes || await sha256(compressed) !== part.sha256)
      throw new Error("The WMP download failed its checksum. Retry or build it on this device.");
    const expanded = new Blob([compressed]).stream().pipeThrough(new DecompressionStream("gzip")).getReader();
    const start = offset;
    try {
      while (true) {
        signal?.throwIfAborted();
        const { done, value } = await expanded.read();
        if (done) break;
        if (offset + value.length > start + part.unpacked_bytes)
          throw new Error("The expanded WMP exceeds its declared size.");
        bytes.set(value, offset);
        offset += value.length;
      }
    } finally {
      await expanded.cancel();
    }
    if (offset - start !== part.unpacked_bytes) throw new Error("The expanded WMP is incomplete.");
  }
  if (offset !== bytes.length || await sha256(bytes) !== asset.sha256)
    throw new Error("The WMP download failed its checksum. Retry or build it on this device.");
  validateWMP(bytes);
  return bytes;
}
