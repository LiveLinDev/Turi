# `web` · Pila web oficial de alto rendimiento para Turi

Paquete opcional (`require web 0.1.0 "../Turi/lib/web"`) compilado con `--web` (o automáticamente al enlazar llamadas del runtime `@tz_web_*`). Quienes no lo usan pagan **cero bytes en el binario y cero sobrecarga en los programas de cómputo**.

## Arquitectura

- **Windows:** Motor asíncrono **IOCP** con `AcceptEx` pre-publicado, `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS`, `GetQueuedCompletionStatusEx` por lotes y *slabs* pre-asignados por conexión.
- **Linux:** Modelo *thread-per-core* sin estado compartido usando **`epoll` Edge-Triggered + `SO_REUSEPORT` + `eventfd`**.
- **Peticiones y pipelining:** Analizador HTTP/1.1 de copia cero con soporte de múltiples peticiones en un mismo paquete (*pipelining*), `keep-alive`, `Transfer-Encoding: chunked` y tabla hash O(1) para rutas estáticas más rutas paramétricas (`:id`, `*ruta`).
- **Memoria automática y cero fugas:** Buffers reutilizables por conexión con reencogimiento automático y constructor JSON directo (`jsonInicio`/`jsonCampo*`/`jsonFin`) sin asignaciones intermedias.

## Módulos

- `mime`: tipos MIME y sanitización contra *path traversal*.
- `http`: constantes, query strings, cookies, codificación URL y `multipart/form-data`.
- `json`: analizador y serializador RFC 8259 completo (`parsearJson`, `serializarJson`).
- `router`: validación y emparejamiento de rutas paramétricas en Turi puro.
- `servidor`: servidor HTTP/1.1 de alto rendimiento (`Servidor`, rutas estáticas pre-enmarcadas, archivos estáticos con `ETag`/`304`, middleware).
- `ws`: WebSockets RFC 6455 (`rutaWs`, `enviarTexto`, `difundirTexto`, `claveAceptacionWs`).
- `cliente`: cliente HTTP/1.1 saliente con pool *keep-alive*.
- `sqlite`: carga dinámica de `sqlite3.dll`/`libsqlite3.so` con modo `WAL` automático y consultas parametrizadas.
- `tls`: detección y carga dinámica opcional de OpenSSL (`libssl`).

## Ejemplo mínimo

```turi
import servidor

extern "C" fn hola(ctx: MutPtr<void>) {
    servidor.jsonInicio(ctx)
    servidor.jsonCampoTexto(ctx, "estado", "ok")
    servidor.jsonFin(ctx, 200)
}

fn main() {
    let srv = servidor.nuevo("127.0.0.1", 8080, 0)
    servidor.estatica(srv, "/ping", 200, "text/plain; charset=utf-8", "pong")
    servidor.get(srv, "/api/estado", fnPtr(hola))
    let _ = servidor.ejecutar(srv)
}
```
