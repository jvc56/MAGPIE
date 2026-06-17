// Show the version (styled like the MAGPIE wordmark) plus a small build stamp
// (date + commit hash) next to the site title, so screenshots and content
// dumps can be tied back to a specific version.
document$.subscribe(() => {
  const topic = document.querySelector(".md-header__title .md-header__topic");
  if (!topic || topic.querySelector(".md-version, .md-build")) {
    return;
  }
  const add = (selector, className) => {
    const meta = document.querySelector(selector);
    if (!meta || !meta.content.trim()) {
      return;
    }
    const span = document.createElement("span");
    span.className = className;
    span.textContent = meta.content.trim();
    topic.appendChild(span);
  };
  add('meta[name="magpie-version"]', "md-version");
  add('meta[name="magpie-build"]', "md-build");
});
