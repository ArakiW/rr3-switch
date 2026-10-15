#pragma once

#include <functional>

/*
 * One-frame-deferred answers for the Cloudcell facades.
 *
 * On Android every one of these classes replies from somewhere else: a worker
 * thread (HttpThread), an AsyncTask, or a runOnUiThread post. Nothing ever
 * answers inside the call that started the operation, and the native side is
 * written on that assumption.
 *
 * CC_HttpRequest_Class.post() is the case that proves it. It runs inside
 * CC_HttpRequestManager_Class::QueueRequest (0x93b0a4), between the manager
 * taking its lock and the store of the freshly built worker pointer into
 * request[136] at 0x93a87a. The completion path deletes request[136]; answering
 * synchronously would run it while that word is still zero, against a worker
 * whose constructor has not returned. The manager's mutex is recursive (its
 * ctor at 0x93a824 passes 1 to CC_Mutex_Class), so this is not a deadlock - it
 * is a use of half-constructed state, which is worse because it does not stop.
 *
 * So every stub that has to fire a native callback enqueues it here, and
 * cloudcell_pump() drains the queue from the game thread, outside every
 * Cloudcell lock, and early enough that the same frame's
 * CC_HttpRequestManager_Class::Update() (0x93ae04) sees the completion.
 *
 * The drain is now invoked from source/guest_view_probe.cpp, immediately before
 * and after each of onViewCreatedJNI / onViewChangedJNI / onViewRenderJNI and
 * once per frame in the persistent render loop - the header used to claim
 * src/main.cpp did it, and no such call existed anywhere in the tree.
 *
 * `site` is a label for the log line only. With `always_log` set (the default,
 * and what the six bracketing pumps around the view callbacks use) every pump
 * logs its depth, so a run in which the depth is always 0 is a readable
 * negative result rather than an absence of evidence. The per-frame drain
 * passes false: an always-empty queue would otherwise emit one line per frame
 * and bury everything else, so it logs only when there was something to drain.
 */
void cloudcell_defer(std::function<void()> fn);
void cloudcell_pump(const char *site, bool always_log = true);
