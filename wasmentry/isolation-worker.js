// Header adapter only: no asset cache, analytics, remote computation or auth
// changes. Leave cross-origin requests and opaque/redirect responses alone.
self.addEventListener("install", () => self.skipWaiting());
self.addEventListener("activate", (event) =>
  event.waitUntil(self.clients.claim()),
);
self.addEventListener("fetch", (event) => {
  const request = event.request;
  if (
    new URL(request.url).origin !== self.location.origin ||
    (request.cache === "only-if-cached" && request.mode !== "same-origin")
  )
    return;
  event.respondWith(
    (async () => {
      const response = await fetch(request);
      if (
        response.status === 0 ||
        new URL(response.url).origin !== self.location.origin
      )
        return response;
      const headers = new Headers(response.headers);
      headers.set("Cross-Origin-Opener-Policy", "same-origin");
      headers.set("Cross-Origin-Embedder-Policy", "require-corp");
      headers.set("Cross-Origin-Resource-Policy", "same-origin");
      return new Response(response.body, {
        status: response.status,
        statusText: response.statusText,
        headers,
      });
    })(),
  );
});
