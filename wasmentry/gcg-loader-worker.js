// File I/O and text preparation are isolated from the page and engine workers.
self.onmessage = async ({ data: { file, pastedText, defaultLexicon, reportProgress } }) => {
  // Older, already-open tabs expect exactly one result message.
  const progress = (stage) => {
    if (reportProgress === true) {
      self.postMessage({ type: "progress", stage });
    }
  };
  try {
    progress("loader.received");
    if ((file?.size ?? new Blob([pastedText]).size) > 1024 * 1024) {
      throw new Error("GCG files must be smaller than 1 MB.");
    }
    progress(file ? "file.read" : "text.prepare");
    const bytes = file ? await file.arrayBuffer() : null;
    progress("text.decode");
    let text = (file ? new TextDecoder().decode(bytes) : pastedText).replace(/^\uFEFF/, "");
    if (!text.trim()) {
      throw new Error("Paste a GCG game record first.");
    }
    const encoding = text
      .match(/^#character-encoding\s+([^\r\n]+)/i)?.[1]
      .trim();
    if (encoding && !/^(utf-?8|iso[- ]8859-1)$/i.test(encoding)) {
      throw new Error(`Unsupported GCG encoding: ${encoding}.`);
    }
    if (file && encoding && /^iso/i.test(encoding)) {
      text = Array.from(new Uint8Array(bytes), (byte) =>
        String.fromCharCode(byte),
      ).join("");
    } else if (file && text.includes("\uFFFD")) {
      if (encoding) {
        throw new Error("The GCG contains invalid UTF-8 text.");
      }
      text = Array.from(new Uint8Array(bytes), (byte) =>
        String.fromCharCode(byte),
      ).join("");
    }
    // The WASM boundary sends UTF-8, including decoded legacy GCGs.
    text =
      "#character-encoding UTF-8\n" +
      text.replace(/^#character-encoding[^\r\n]*[\r\n]*/i, "");
    if (text.includes("\0")) {
      throw new Error("The file is not a text GCG.");
    }
    if ((text.match(/^>/gm) || []).length > 1000) {
      throw new Error("GCG files may contain at most 1,000 events.");
    }
    const lexicon =
      text.match(/^#lexicon\s+(\S+)/im)?.[1] || defaultLexicon;
    if (!["CSW24", "NWL23"].includes(lexicon)) {
      throw new Error(
        `This preview supports CSW24 and NWL23; this game uses ${lexicon}.`,
      );
    }
    self.postMessage({ text, lexicon });
  } catch (failure) {
    self.postMessage({ error: failure.message });
  }
};
