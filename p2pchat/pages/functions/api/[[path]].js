export async function onRequest(context) {
  const { request, env } = context;
  if (!env.SIGNAL_WORKER) {
    return new Response(JSON.stringify({ ok: false, error: "signal_worker_binding_missing" }), {
      status: 500,
      headers: {
        "content-type": "application/json; charset=utf-8",
        "cache-control": "no-store",
      },
    });
  }

  const url = new URL(request.url);
  const upstreamPath = url.pathname.replace(/^\/api/, "") || "/";
  const upstreamUrl = new URL(upstreamPath + url.search, "https://signal.internal");
  const upstreamReq = new Request(upstreamUrl.toString(), request);
  return env.SIGNAL_WORKER.fetch(upstreamReq);
}
