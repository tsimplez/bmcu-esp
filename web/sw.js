// 构建时按网页与 PWA 资源内容替换版本，缓存按作用域隔离
const CACHE_PREFIX = 'bmcu-pwa-' + encodeURIComponent(self.registration.scope) + '-'
const CACHE_NAME = CACHE_PREFIX + '__BMCU_PWA_VERSION__'
const OFFLINE_URL = new URL('offline.html', self.registration.scope).href

/**
 * 缓存独立离线说明并立即启用本次 Service Worker
 * @return {Promise<void>} 离线页缓存完成并请求启用的 Promise
 */
const installOfflinePage = async () => {
  const cache = await caches.open(CACHE_NAME)
  await cache.add(new Request(OFFLINE_URL, {cache: 'reload'}))
  await self.skipWaiting()
}

/**
 * 删除同一作用域的旧版本缓存并接管已有页面
 * @return {Promise<void>} 缓存清理与接管完成的 Promise
 * 接管后不刷新页面，避免打断正在编辑的参数或升级操作
 */
const activateOfflinePage = async () => {
  const names = await caches.keys()
  await Promise.all(names.filter(name => name.startsWith(CACHE_PREFIX) && name !== CACHE_NAME)
    .map(name => caches.delete(name)))
  await self.clients.claim()
}

/**
 * 首页导航始终请求当前固件页面，断线或服务端错误时返回独立离线说明
 * @param {Request} request 当前首页导航请求
 * @return {Promise<Response>} 最新控制台或独立离线说明响应
 * 不缓存控制台、设置或设备状态，离线页面不提供控制操作
 */
const navigateToConsole = async request => {
  try {
    const response = await fetch(request, {cache: 'no-store'})
    if (response.status < 500) return response
  } catch (error) {
    // 无网络或设备不可达时只显示离线说明
  }
  const cache = await caches.open(CACHE_NAME)
  const offline = await cache.match(OFFLINE_URL)
  return offline || new Response('未连接到 BMCU，请连接设备所在网络后重试', {
    status: 503,
    headers: {'Content-Type': 'text/plain; charset=utf-8', 'Cache-Control': 'no-store'}
  })
}

self.addEventListener('install', event => event.waitUntil(installOfflinePage()))
self.addEventListener('activate', event => event.waitUntil(activateOfflinePage()))
self.addEventListener('fetch', event => {
  if (event.request.method !== 'GET' || event.request.mode !== 'navigate') return
  const url = new URL(event.request.url)
  const scope = new URL(self.registration.scope)
  if (url.origin !== scope.origin ||
      (url.pathname !== scope.pathname && url.pathname !== scope.pathname + 'index.html')) return
  // 只处理控制台入口，其他导航（包括 GET /api/reboot）完全交给网络
  event.respondWith(navigateToConsole(event.request))
})
