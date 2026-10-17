// Apply the saved theme before the stylesheet paints the page.
(() => {
  const key = "magpie-preview-theme";
  const system = matchMedia("(prefers-color-scheme: dark)");
  let preference = "dark";
  try {
    const saved = localStorage.getItem(key);
    if (["light", "dark", "system"].includes(saved)) preference = saved;
  } catch {
    // Storage is optional.
  }
  function applyTheme() {
    const theme =
      preference === "system"
        ? system.matches
          ? "dark"
          : "light"
        : preference;
    document.documentElement.dataset.theme = theme;
    document.querySelector('meta[name="theme-color"]').content =
      theme === "light" ? "#f4f5f8" : "#1e1e2e";
  }
  applyTheme();
  system.addEventListener("change", applyTheme);
  document.addEventListener("DOMContentLoaded", () => {
    const select = document.getElementById("theme");
    select.value = preference;
    select.addEventListener("change", () => {
      preference = select.value;
      applyTheme();
      try {
        localStorage.setItem(key, preference);
      } catch {
        // Keep the selection for this page when storage is unavailable.
      }
    });
  });
})();
