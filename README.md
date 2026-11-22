# keyjson

Servidor REST en C11 para un almacén key-value en memoria. Usa sockets POSIX, un bucle de eventos de un solo hilo y `yyjson` para validar, compactar y generar JSON. No incluye autenticación ni dependencias de ejecución externas a `yyjson` y libc.

> **Seguridad:** el servicio no tiene autenticación ni TLS. La ejecución nativa enlaza por defecto a `127.0.0.1`; el contenedor escucha dentro del contenedor y el ejemplo publica el puerto solo en loopback. No lo expongas a redes no confiables.

## Créditos

Esta aplicación ha sido creada con Zed Agent y GPT-6 Luna.

## Licencia

El código propio de keyjson, incluido el cliente Python, se distribuye bajo la licencia MIT; consulta [`LICENSE`](LICENSE). `python/LICENSE` contiene la misma licencia para incluirla en la distribución del paquete. `yyjson` conserva su aviso MIT original en `third_party/yyjson/LICENSE`, con su titular independiente. La imagen del contenedor incluye ambos avisos en `/usr/share/doc/keyjson/`.

## Arquitectura y decisiones

- **Almacén:** tabla hash redimensionable con direccionamiento abierto, sondeo lineal, tombstones y `xxHash64` integrado en el proyecto. El factor de ocupación se mantiene por debajo del 70 % mediante rehash. `PUT` reemplaza de forma atómica el valor existente si falla una asignación.
- **TTL:** cada entrada puede llevar un vencimiento basado en `CLOCK_MONOTONIC`. Se comprueba la fecha más próxima en cada operación y solo se recorre la tabla cuando llega ese vencimiento; las claves sin TTL no consultan el reloj. No hay hilo de fondo.
- **Valores:** se acepta cualquier valor JSON válido (incluidos objetos, arrays, cadenas, números, booleanos y `null`). `yyjson` lo valida y se guarda como JSON compacto serializado; no se retiene un árbol DOM por entrada. El almacén y sus datos son volátiles y desaparecen al terminar el proceso.
- **HTTP:** HTTP/1.1 sobre sockets POSIX no bloqueantes y `poll()`. Un único hilo mantiene el almacén seguro sin locks; se atienden hasta 64 conexiones y se reutilizan conexiones persistentes. Se aceptan solicitudes con `Content-Length`; `Transfer-Encoding`/chunked no está implementado.
- **Swagger/OpenAPI:** `GET /docs` sirve Swagger UI y `GET /openapi.json` entrega un documento OpenAPI 3 integrado en el binario. La interfaz carga CSS/JavaScript desde jsDelivr (`swagger-ui-dist` v5); el navegador necesita acceso a Internet para mostrarla. No añade dependencias al servidor C.
- **Favicon:** `favicon.ico` es un icono ICO de 32×32 embebido en el binario y servido en `/favicon.ico`; Swagger UI lo enlaza desde su HTML.
- **Límites:** cabeceras de hasta 16 KiB, cuerpo de hasta 1 MiB y clave UTF-8 de hasta 1024 bytes después de decodificar escapes URL. Se cierra una conexión inactiva tras 30 segundos; una solicitud incompleta tiene un plazo de recepción absoluto de 30 segundos, que no se reinicia al recibir más bytes. El envío de la respuesta anterior no consume el plazo de una solicitud posterior en la misma conexión. No se admiten query strings.
- **JSON de respuesta:** `yyjson` genera errores, resultados CRUD, lista de claves y estadísticas. `GET /keys/{key}` devuelve directamente el JSON compacto previamente validado, no una cadena JSON escapada.
- **Dependencia:** `yyjson` 0.10.0 está vendorizado en `third_party/yyjson/` con su licencia MIT; no se requiere descargarlo ni instalar un paquete del sistema para compilar.

## API REST

`PUT` crea una clave con `201 Created` o reemplaza su valor con `200 OK`. Para `PUT`, el tipo de contenido debe ser `application/json` (se admite un parámetro como `charset=utf-8`). El encabezado opcional `X-TTL-Seconds` acepta un entero decimal positivo; al omitirlo, la clave queda permanente y se borra un TTL anterior al reemplazarla. Las claves expiradas se consideran inexistentes y se limpian perezosamente en operaciones posteriores, listado o estadísticas. Las claves se decodifican como segmentos URL; usa percent-encoding para caracteres especiales.

| Método | Ruta | Respuesta |
| --- | --- | --- |
| `PUT` | `/keys/{key}` | Crea/reemplaza el valor JSON y devuelve `{"created":true/false}`. Opcional: `X-TTL-Seconds: 60`. |
| `GET` | `/keys/{key}` | Devuelve el valor JSON; `404` si no existe. |
| `DELETE` | `/keys/{key}` | Elimina la clave y devuelve `{"deleted":true}`; `404` si no existe. |
| `GET` | `/keys` | Array JSON con las claves (el orden no está garantizado). |
| `GET` | `/stats` | Métricas: cantidad de claves, capacidad, bytes almacenados y contadores de operaciones. |
| `GET` | `/docs` | Interfaz interactiva Swagger UI (también `/docs/`). |
| `GET` | `/openapi.json` | Especificación OpenAPI 3 de la API. |
| `GET` | `/favicon.ico` | Icono ICO del servicio, también enlazado desde `/docs`. |

Ejemplos (con el contenedor iniciado en `127.0.0.1:8080`):

```sh
curl -i -X PUT http://127.0.0.1:8080/keys/greeting \
  -H 'Content-Type: application/json' \
  --data '{"message":"hello","count":1}'

curl -i -X PUT http://127.0.0.1:8080/keys/temporary \
  -H 'Content-Type: application/json' \
  -H 'X-TTL-Seconds: 60' \
  --data '"temporary value"'

curl -i http://127.0.0.1:8080/keys/greeting
curl -i http://127.0.0.1:8080/keys
curl -i http://127.0.0.1:8080/stats
curl -i http://127.0.0.1:8080/docs
curl -i http://127.0.0.1:8080/openapi.json
curl -i http://127.0.0.1:8080/favicon.ico
curl -i -X DELETE http://127.0.0.1:8080/keys/greeting
```

El servidor devuelve JSON de error para solicitudes inválidas y usa, entre otros, `400`, `404`, `405` (incluye `Allow`), `413`, `415`, `417`, `431`, `500` y `505` según corresponda. Para respuestas de `PUT`, el JSON raíz puede ser cualquier tipo; no hace falta envolverlo en un objeto.

## Compilación, pruebas y ejecución

Requisitos de desarrollo: compilador C11 y `make`; `pthread` para `bench-http`; Podman rootless solo para los objetivos `container` y `run`. `yyjson` ya está incluido. La primera construcción puede descargar las imágenes base `gcc:14-bookworm` y `debian:bookworm-slim` de Docker Hub si no están en caché.

```sh
make all       # Build with -O3 -Wall -Wextra -Werror
make test      # Run key-value unit tests
make sanitize  # Run KV tests and build HTTP test client with ASan/UBSan
make container # Build localhost/keyjson:dev with Podman
make run       # Start the service in a rootless container
```

`make run` publica el puerto únicamente en `127.0.0.1:8080`; detén el servicio con `Ctrl-C`. Para ejecutarlo manualmente después de `make container`:

```sh
podman run --rm --name keyjson \
  -p 127.0.0.1:8080:8080 localhost/keyjson:dev
```

El binario acepta `[host] [port]`; el contenedor usa `0.0.0.0:8080` dentro de su espacio de red. El almacén no persiste datos entre reinicios.

## Estructura

```text
include/kv.h       API del almacén
include/http.h     API del servidor
src/kv.c           Tabla hash y operaciones CRUD
src/http.c         Parser HTTP, rutas REST y serialización JSON
src/favicon.h      ICO embebido para servir desde el binario
favicon.ico        Favicon 32×32 del proyecto
src/main.c         Configuración y punto de entrada
tests/test_kv.c    Pruebas funcionales del almacén
tests/test_http_robustness.c Corpus HTTP local de robustez
tests/bench_kv.c   Benchmark CRUD del almacén
tests/bench_http.c Cliente de carga HTTP concurrente
python/keyjson/    Biblioteca cliente REST de Python
python/tests/      Pruebas aisladas del cliente Python
python/pyproject.toml Metadatos de instalación del paquete
python/LICENSE     Copia MIT incluida en la distribución Python
third_party/yyjson/ yyjson 0.10.0 y licencia
LICENSE            Licencia MIT del proyecto
Containerfile      Imagen de ejecución
Makefile           Compilación, pruebas, benchmarks, sanitizer y Podman
```

## Benchmarks de rendimiento

Los benchmarks son herramientas de medición, no tests con umbrales de aprobación: los resultados dependen del hardware, el compilador, la carga y, para HTTP, la red local y el forwarding de Podman.

- `make bench-kv` mide inserciones, lecturas con hit y miss, actualizaciones y borrados. Por defecto usa 100 000 entradas y 1 000 000 de operaciones por fase.
- `make bench-http` requiere que el servicio ya esté activo en `127.0.0.1:8080`. El cliente se limita a loopback, usa conexiones HTTP/1.1 persistentes y cuatro workers para medir `GET /stats`; no modifica las claves. Informa throughput y latencia media/máxima. El número total incluye el establecimiento inicial de conexiones. Se limita a 32 workers y 10 000 000 de solicitudes.
- El cliente HTTP usa `pthread`; el servidor sigue siendo de un solo hilo.

Para cambiar el tamaño de las cargas, compila el ejecutable y pásale parámetros:

```sh
make build/bench-kv
./build/bench-kv 10000 100000

make build/bench-http
./build/bench-http 8080 1000 2
```

La firma de los programas es `bench-kv [entries] [operations]` y `bench-http [port] [requests] [threads]`. El benchmark HTTP solo conecta con `127.0.0.1`; el puerto debe estar entre 1 y 65535. Empieza con cargas pequeñas y compara resultados usando la misma máquina y configuración de compilación.

## Pruebas de robustez HTTP

Con el contenedor activo en `127.0.0.1:8080`, ejecuta:

```sh
make test-http
```

El cliente solo conecta con `127.0.0.1` y no admite destinos remotos. Comprueba solicitudes malformadas, cabeceras duplicadas, `Transfer-Encoding`, `Expect`, TTL inválidos o fuera de `PUT`, JSON inválido, escapes de URL y claves con UTF-8 inválido, límites, bytes de control, `Keep-Alive`, pipelining y cierre. La carga final son 8 conexiones con 100 consultas `GET /stats` cada una. No crea ni borra claves; todos los `PUT` de prueba se rechazan antes de escribir.

`make test-http` requiere el servicio ya iniciado mediante Podman; no lo arranca ni lo detiene. Usa el puerto local 8080 por defecto; para otro puerto de loopback, ejecuta `make test-http TEST_HTTP_PORT=8081`. `make test` sigue siendo independiente y solo ejecuta las pruebas unitarias del almacén.

## Biblioteca cliente de Python

El paquete `python/` requiere Python 3.10 o posterior y no tiene dependencias de ejecución externas. `KeyJsonClient` usa la API REST existente; acepta cualquier valor JSON y valida las respuestas y las claves antes de devolverlas. `put()` devuelve `True` si creó la clave y `False` si la reemplazó; acepta `ttl=segundos` como entero positivo opcional. Omitir `ttl` deja la clave sin expiración y quita un TTL anterior al actualizarla. `get()` y `delete()` lanzan `KeyNotFoundError` para claves inexistentes. Los errores HTTP, de transporte y de protocolo se exponen como `KeyJsonHTTPError`, `KeyJsonTransportError` y `KeyJsonProtocolError` (todos heredan de `KeyJsonError`).

Crea un entorno virtual local e instala el paquete en modo editable:

```sh
python -m venv .venv
./.venv/bin/pip install -e ./python
```

Ejemplo de uso con el servicio activo en `127.0.0.1:8080`:

```python
from keyjson import KeyJsonClient

client = KeyJsonClient("http://127.0.0.1:8080", timeout=3.0)
created = client.put("profile", {"name": "Ada", "active": True})
client.put("session", {"user": "Ada"}, ttl=60)
profile = client.get("profile")
keys = client.keys()
stats = client.stats()
client.delete("profile")
```

Las pruebas no necesitan el contenedor y usan un servidor HTTP temporal en loopback:

```sh
PYTHONPATH=python ./.venv/bin/python -m unittest discover -s python/tests -v
```

## Checklist de implementación

- [x] Definir arquitectura y endpoints en este README.
- [x] Integrar `yyjson` de forma reproducible.
- [x] Implementar tabla hash, CRUD y pruebas unitarias.
- [x] Implementar HTTP/1.1, JSON, rutas, límites, `Keep-Alive` y TTL por clave.
- [x] Añadir punto de entrada, `Makefile`, `.gitignore` y ejecución rootless con Podman.
- [x] Documentar endpoints, ejemplos, compilación y ejecución.
- [x] Validar compilación estricta y pruebas con sanitizers.
- [x] Añadir benchmarks CRUD y HTTP con concurrencia configurable.
- [x] Documentar su uso y validar cargas pequeñas.
- [x] Añadir pruebas HTTP locales, no destructivas y con carga acotada.
- [x] Documentar `make test-http` y validar que el servidor siga respondiendo.

## Plan: pruebas de robustez HTTP

- Añadir un cliente de pruebas raw limitado a `127.0.0.1:8080`, con casos malformados y de frontera para cabeceras, cuerpos, rutas y conexiones persistentes.
- Añadir una carga concurrente pequeña y acotada; las pruebas no escribirán ni borrarán claves y no aceptarán hosts remotos.
- Exponerla como `make test-http`, documentar que requiere la app ejecutándose en Podman y verificar respuestas y disponibilidad posterior.

### Checklist

- [x] Implementar el corpus de solicitudes malformadas y de frontera.
- [x] Probar Keep-Alive, pipelining y conexiones concurrentes acotadas.
- [x] Integrar y documentar el objetivo `make test-http`.
- [x] Ejecutar pruebas funcionales y comprobar que la app sigue respondiendo.

## Plan: biblioteca cliente de Python

- Crear un paquete importable `keyjson` bajo `python/`, implementado con la biblioteca estándar y la API REST existente; no requiere dependencias externas.
- Exponer un cliente con URL base y timeout configurables para `PUT`, `GET`, `DELETE`, listado de claves y estadísticas, con JSON convertido automáticamente entre Python y HTTP.
- Definir errores públicos claros para claves inexistentes, respuestas HTTP inesperadas, errores de transporte y JSON/protocolo inválido.
- Añadir pruebas `unittest` contra un servidor HTTP local de prueba, sin depender del contenedor ni modificar datos reales; documentar instalación en un entorno virtual y ejemplos de uso.

### Checklist

- [x] Implementar el paquete y su API pública.
- [x] Añadir pruebas automatizadas aisladas para CRUD, errores y serialización JSON.
- [x] Documentar instalación y ejemplos de uso de la biblioteca.
- [x] Ejecutar pruebas Python y verificar compatibilidad con el servidor actual.

## Plan: TTL por clave

- Extender `PUT /keys/{key}` con el encabezado opcional `X-TTL-Seconds`; solo se aceptan enteros positivos. Si no se envía, la clave queda sin expiración y un `PUT` posterior sin TTL elimina cualquier expiración previa.
- Guardar vencimientos con reloj monotónico y caducidad perezosa, sin hilos de fondo. Las claves expiradas deben comportarse como inexistentes en `GET`, `DELETE`, `GET /keys` y estadísticas.
- Exponer `ttl` opcional en `KeyJsonClient.put`, documentar el protocolo y añadir pruebas de expiración, entradas inválidas y compatibilidad.

### Checklist

- [x] Añadir vencimientos al almacén y limpiar entradas expiradas sin cambiar la ruta rápida de claves sin TTL.
- [x] Validar `X-TTL-Seconds` en HTTP y cubrir las respuestas REST.
- [x] Añadir y documentar `ttl` opcional en la biblioteca Python.
- [x] Ejecutar pruebas C/Python y comprobar `GET /stats` con el servidor activo.

## Plan: reiniciar el contenedor con la versión TTL

- Verificar que `localhost/keyjson:dev` contiene la versión recién compilada y que `keyjson` sigue publicando `127.0.0.1:8080`.
- Reemplazar el contenedor antiguo por uno nuevo basado en esa imagen, manteniendo el nombre y la publicación loopback.
- Comprobar `GET /stats` y una escritura temporal con TTL que expire correctamente.

### Checklist

- [x] Confirmar la imagen actualizada y el contenedor existente en loopback.
- [x] Reemplazar el contenedor manteniendo el nombre `keyjson` y el puerto 8080.
- [x] Verificar `/stats` y la expiración de una clave de prueba.

## Plan: atribución de creación

- Añadir al README general la atribución a Agent Zed y GPT-6 Luna.

### Checklist

- [x] Registrar la atribución en el README.

## Plan: licencia del proyecto

- Añadir la licencia MIT para la aplicación y el paquete Python, distinguiéndola de las licencias de dependencias vendorizadas.
- Incluir los avisos de keyjson y `yyjson` en la imagen de ejecución.

### Checklist

- [x] Añadir el texto completo de MIT en `LICENSE` y al paquete Python.
- [x] Documentar la licencia general del proyecto y la de `yyjson`.
- [x] Incluir ambos avisos en la imagen de ejecución.

## Plan: actualizar titular de copyright

- Reemplazar el titular genérico por `Jose Carlos Sanchez Gomez <toletum@gmail.com>` en la licencia del proyecto y en la copia incluida por el paquete Python.
- Mantener intacta la licencia de `yyjson`, que tiene un titular independiente.

### Checklist

- [x] Actualizar los dos avisos MIT del proyecto.
- [x] Comprobar que coinciden y que `yyjson` conserva su titular.

## Plan: publicar el proyecto en GitHub

- Crear un repositorio llamado `keyjson`; usar visibilidad privada si no se indica otra.
- Revisar el estado del repositorio y excluir entornos virtuales, compilados, cachés, secretos y datos locales antes de publicar.
- Configurar el remoto, crear un commit inicial con los archivos del proyecto y hacer push a la rama principal (`master`).

### Checklist

- [x] Confirmar la cuenta autenticada y que el nombre del repositorio está disponible.
- [x] Revisar los archivos que se incluirán y evitar publicar secretos o artefactos locales.
- [x] Crear el repositorio privado y configurar `origin`.
- [x] Crear el commit inicial y hacer push.

## Plan: documentación Swagger en `/docs`

- Publicar una especificación OpenAPI 3 en `/openapi.json` con los endpoints CRUD, listado, estadísticas, TTL y respuestas de error.
- Añadir Swagger UI en `/docs` con assets versionados servidos desde CDN y una URL relativa al OpenAPI; no añadir dependencias de compilación o ejecución al servidor.
- Cubrir las nuevas rutas con pruebas HTTP y documentar el acceso y el requisito de red del navegador para cargar Swagger UI.

### Checklist

- [x] Implementar y validar la especificación OpenAPI.
- [x] Servir `/docs` y `/openapi.json` con métodos, tipos MIME y respuestas correctos.
- [x] Añadir pruebas, actualizar el README y validar compilación/pruebas.

## Plan: arrancar la app con Swagger

- Comprobar si ya hay un contenedor o servicio escuchando en el puerto 8080; no reiniciar una instancia activa para conservar sus datos en memoria.
- Si no está activa, iniciar `localhost/keyjson:dev` en Podman rootless, publicando solo en `127.0.0.1:8080`.
- Verificar `/stats`, `/docs` y `/openapi.json`.

### Checklist

- [x] Comprobar contenedores e imagen disponibles.
- [x] Arrancar la app si es necesario, sin interrumpir una instancia activa.
- [x] Verificar los tres endpoints.

## Plan: reparar Swagger UI en navegador

- Revisar la inicialización de Swagger UI y confirmar que `/openapi.json` sigue siendo válido.
- Corregir la carga de recursos/presets para que la página se renderice en navegadores, manteniendo el servidor ligero.
- Probar la respuesta de `/docs`, los recursos CDN y el documento; antes de reemplazar el contenedor activo, comprobar `/stats` para evitar pérdida de datos en memoria.

### Checklist

- [x] Corregir la inicialización de Swagger UI y documentar cualquier requisito externo.
- [x] Validar `/docs`, el OpenAPI, los recursos CDN y el contenedor actualizado.

## Plan: añadir favicon

- Crear `favicon.ico` con un icono sencillo acorde al tema oscuro y al almacén key-value.
- Incluirlo en el binario del servidor y servir `GET /favicon.ico` con el MIME correcto, sin depender de archivos en tiempo de ejecución.
- Enlazarlo desde Swagger UI y añadir pruebas HTTP para la ruta y el formato ICO.

### Checklist

- [x] Crear el archivo ICO y el recurso embebido para el binario.
- [x] Servir el favicon y enlazarlo desde `/docs`.
- [x] Validar formato, compilación y tests HTTP.

## Plan: crear Pull Request para Swagger UI y favicon

- Revisar los cambios locales pendientes para incluir la reparación de Swagger UI y el favicon en una rama de trabajo, sin tocar `master`.
- Validar el estado y crear un commit con identidad del propietario del repositorio.
- Publicar la rama en GitHub, crear un PR contra `master` y verificar su URL y estado.

### Checklist

- [ ] Revisar cambios pendientes y confirmar tests.
- [ ] Crear commit y subir rama de trabajo.
- [ ] Crear y verificar el Pull Request.
