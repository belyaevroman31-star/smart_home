/* Service Worker для панели умного дома */
const CACHE_NAME = 'smarthome-v1';
const APP_SHELL = [
  './smart_home.html',
  './manifest.webmanifest',
  './icon-192.png',
  './icon-512.png'
];

// Установка: кэшируем оболочку приложения сразу
self.addEventListener('install', function (event) {
  event.waitUntil(
    caches.open(CACHE_NAME).then(function (cache) {
      return cache.addAll(APP_SHELL).catch(function (err) {
        console.warn('[SW] часть оболочки не закэширована:', err);
      });
    })
  );
  self.skipWaiting();
});

// Активация: удаляем старые кэши
self.addEventListener('activate', function (event) {
  event.waitUntil(
    caches.keys().then(function (keys) {
      return Promise.all(
        keys
          .filter(function (k) { return k !== CACHE_NAME; })
          .map(function (k) { return caches.delete(k); })
      );
    }).then(function () { return self.clients.claim(); })
  );
});

// Запросы: для шрифтов и иконок — кэш-сначала, для остального — сеть с фолбэком на кэш
self.addEventListener('fetch', function (event) {
  const url = event.request.url;

  if (event.request.method !== 'GET') return;

  // Навигация (открытие приложения) — сеть сначала, при ошибке кэш
  if (event.request.mode === 'navigate') {
    event.respondWith(
      fetch(event.request)
        .then(function (resp) {
          const copy = resp.clone();
          caches.open(CACHE_NAME).then(function (cache) { cache.put(event.request, copy); });
          return resp;
        })
        .catch(function () {
          return caches.match(event.request).then(function (c) {
            return c || caches.match('./smart_home.html');
          });
        })
    );
    return;
  }

  // Шрифты Google Fonts и внешние ресурсы — кэш-сначала, сеть как фолбэк
  if (url.indexOf('fonts.googleapis.com') !== -1 || url.indexOf('fonts.gstatic.com') !== -1 || url.indexOf('fonts.cdnfonts.com') !== -1) {
    event.respondWith(
      caches.match(event.request).then(function (cached) {
        return cached || fetch(event.request).then(function (resp) {
          const copy = resp.clone();
          caches.open(CACHE_NAME).then(function (cache) { cache.put(event.request, copy); });
          return resp;
        });
      })
    );
    return;
  }

  // Локальные статические ресурсы (иконки PWA) — кэш-сначала
  if (url.indexOf('icon-') !== -1 || url.indexOf('manifest.webmanifest') !== -1) {
    event.respondWith(
      caches.match(event.request).then(function (cached) {
        return cached || fetch(event.request);
      })
    );
  }
});