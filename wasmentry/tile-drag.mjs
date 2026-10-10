// Pointer events give mouse, pen and touch the same transaction: do not mutate
// the position until a valid drop. Cancellation needs no rollback.
export function installTileDrag({board, rack, sourceFor, targetFor, onDrop, onTap}) {
  let drag = null, suppressClick = false, scrollFrame = 0;
  const caret = document.createElement("div");
  caret.className = "rack-drop-caret";
  caret.setAttribute("aria-hidden", "true");
  caret.hidden = true;
  document.body.append(caret);

  function clearTarget() {
    board.querySelector(".drop-target")?.classList.remove("drop-target");
    rack.classList.remove("drop-target");
    caret.hidden = true;
  }
  function cleanup() {
    const previous = drag;
    drag = null;
    cancelAnimationFrame(scrollFrame);
    clearTarget();
    previous?.ghost?.remove();
    previous?.source.element.classList.remove("drag-source");
    document.body.classList.remove("tile-dragging");
    if (previous?.capture.hasPointerCapture(previous.id)) previous.capture.releasePointerCapture(previous.id);
    return previous;
  }
  function cancel() {
    if (drag?.moved) suppressClick = true;
    cleanup();
  }
  function update() {
    if (!drag?.moved) return;
    const {x, y, source, ghost, offsetX, offsetY, touch} = drag;
    ghost.style.left = `${x - offsetX}px`;
    ghost.style.top = `${y - offsetY - (touch ? 36 : 0)}px`;
    clearTarget();
    drag.target = targetFor(x, y, source);
    if (drag.target?.square) drag.target.square.classList.add("drop-target");
    if (drag.target?.rack) {
      rack.classList.add("drop-target");
      const tiles = [...rack.children];
      const next = tiles[drag.target.gap], previous = tiles[drag.target.gap - 1];
      const rect = (next || previous || rack).getBoundingClientRect();
      const left = next ? rect.left - 3 : previous ? rect.right + 3 : rect.left + rect.width / 2;
      caret.style.left = `${left}px`;
      caret.style.top = `${rect.top}px`;
      caret.style.height = `${rect.height}px`;
      caret.hidden = false;
    }
    ghost.classList.toggle("invalid-drop", !drag.target);
  }
  function autoScroll() {
    if (!drag?.moved) return;
    // Reach the upper board or rack on short phone screens without releasing.
    const margin = 48;
    const delta = drag.y < margin ? -Math.ceil((margin - drag.y) / 4)
      : drag.y > innerHeight - margin ? Math.ceil((drag.y - innerHeight + margin) / 4) : 0;
    if (delta) { window.scrollBy(0, delta); update(); }
    scrollFrame = requestAnimationFrame(autoScroll);
  }
  function start(event) {
    if (drag || event.isPrimary === false || event.button !== 0) return;
    suppressClick = false;
    const source = sourceFor(event.target);
    if (!source) return;
    const rect = source.element.getBoundingClientRect();
    drag = {source, capture:event.currentTarget, id:event.pointerId, x:event.clientX, y:event.clientY,
      startX:event.clientX, startY:event.clientY, offsetX:event.clientX - rect.left,
      offsetY:event.clientY - rect.top, width:rect.width, height:rect.height,
      touch:event.pointerType === "touch", moved:false};
  }
  function move(event) {
    if (!drag || event.pointerId !== drag.id) return;
    drag.x = event.clientX; drag.y = event.clientY;
    if (!drag.moved && Math.hypot(drag.x - drag.startX, drag.y - drag.startY) < (drag.touch ? 8 : 5)) return;
    event.preventDefault();
    if (!drag.moved) {
      drag.moved = true;
      drag.capture.setPointerCapture(drag.id);
      const source = drag.source.element;
      const style = getComputedStyle(source);
      const ghost = source.cloneNode(true);
      ghost.removeAttribute("id");
      ghost.removeAttribute("role");
      ghost.tabIndex = -1;
      ghost.setAttribute("aria-hidden", "true");
      ghost.classList.remove("cursor", "chosen");
      ghost.classList.add("drag-tile");
      Object.assign(ghost.style, {width:`${drag.width}px`, height:`${drag.height}px`, background:style.backgroundColor, color:style.color});
      drag.ghost = ghost;
      document.body.append(ghost);
      source.classList.add("drag-source");
      document.body.classList.add("tile-dragging");
      scrollFrame = requestAnimationFrame(autoScroll);
    }
    update();
  }
  function finish(event) {
    if (!drag || event.pointerId !== drag.id) return;
    // Pointerup can be the first event at the final coordinate.
    drag.x = event.clientX; drag.y = event.clientY;
    update();
    const previous = cleanup();
    if (!previous.moved) { onTap(previous.source); return; }
    suppressClick = true;
    if (previous.target) onDrop(previous.source, previous.target);
  }
  for (const element of [board, rack]) {
    element.addEventListener("pointerdown", start);
    element.addEventListener("pointercancel", event => { if (event.pointerId === drag?.id) cancel(); });
    element.addEventListener("lostpointercapture", event => { if (event.pointerId === drag?.id && event.target === drag.capture) cancel(); });
    element.addEventListener("dragstart", event => event.preventDefault());
  }
  // Capture only after crossing the drag threshold so ordinary board clicks
  // retain their square as the click target. Track initial motion globally.
  document.addEventListener("pointermove", move, {passive:false});
  document.addEventListener("pointerup", finish);
  document.addEventListener("click", event => {
    if (!suppressClick || event.detail === 0) return;
    suppressClick = false;
    event.preventDefault(); event.stopImmediatePropagation();
  }, true);
  // A new gesture must not inherit suppression if a canceled drag had no click.
  document.addEventListener("pointerdown", () => { if (!drag) suppressClick = false; }, true);
  document.addEventListener("keydown", event => {
    if (drag && event.key === "Escape") {
      cancel(); event.preventDefault(); event.stopImmediatePropagation();
    }
  }, true);
  window.addEventListener("blur", cancel);
  window.addEventListener("resize", cancel);
  document.addEventListener("visibilitychange", () => { if (document.hidden) cancel(); });
  return {cancel};
}
