# Objetivo
Crea una base de datos **key-value en memoria** en **C** (`C11` o `C17`) expuesta mediante una **API REST**, diseñada para ser **lo más ligera, simple y rápida posible**, **sin autenticación**, y utilizando **`yyjson`** para el parseo, validación y serialización de JSON.

# Requisitos funcionales y de diseño

1. **Almacén key-value en memoria:**
   - Tabla hash rápida y redimensionable (p. ej., direccionamiento abierto con *linear probing* / *Robin Hood* o encadenamiento limpio, con hash rápido como `wyhash` o `xxHash`).
   - Claves de tipo cadena y valores flexibles (almacenando JSON válido o valores serializables a JSON con `yyjson`).
   - Operaciones principales:
     - `PUT /keys/{key}` o `POST /keys` / `SET` vía REST para crear/actualizar clave y valor (con soporte opcional para TTL si se desea mantenerlo simple).
     - `GET /keys/{key}` para obtener el valor de una clave.
     - `DELETE /keys/{key}` para eliminar una clave.
     - `GET /keys` o `GET /stats` para listar claves o consultar métricas básicas/estado.
   - Respuestas HTTP con códigos adecuados (`200`, `201`, `400`, `404`, `405`, `500`) y cuerpo en JSON generado con `yyjson`.
   - **Sin autenticación**, sin *login*, sin *tokens* ni control de accesos.

2. **Servidor HTTP / API REST y uso de `yyjson`:**
   - Implementar un servidor HTTP/1.1 ligero y de bajo *overhead* en C (sobre sockets POSIX o una librería C minimalista/ligera si es necesaria, evitando *frameworks* pesados).
   - Usar `yyjson` para parsear y validar los cuerpos JSON entrantes y construir las respuestas JSON salientes de forma rápida.
   - Soporte limpio para `Content-Type: application/json` y reutilización de conexiones (`Keep-Alive`) cuando sea viable para maximizar el rendimiento.

3. **Rendimiento y ligereza:**
   - Minimizar *allocations* en el *hot path* (reutilización de buffers por conexión/hilo, *arena* o *slab/pool* si aporta claridad y velocidad).
   - Evitar abstracciones innecesarias o hilos superfluos; si se usa concurrencia, mantenerla acotada y segura (p. ej., *pool* de hilos o *sharding* de tabla hash con `pthread_rwlock_t`).
   - Código C limpio para Linux, compilable con `-O3` y sin *warnings* (`-Wall -Wextra -Werror`).

4. **Documentación y estructura del proyecto:**
   - Empieza proponiendo y redactando un `README.md` claro con:
     - Arquitectura y decisiones de diseño (estructura hash, servidor HTTP/REST, integración de `yyjson`, concurrencia y memoria).
     - Referencia de *endpoints* REST con ejemplos `curl`.
     - Estructura de directorios.
     - Instrucciones de compilación (`Makefile` o `CMakeLists.txt`, incluyendo `yyjson`) y ejecución.
   - Mantén el código modular (`src/`, `include/`, `tests/` o `Makefile`), con manejo explícito de errores y cero fugas de memoria (verificable con AddressSanitizer / Valgrind).

# Restricciones de trabajo
- Prioriza simplicidad, velocidad y claridad frente a sobreingeniería.
- No añadas autenticación, TLS complejo ni dependencias externas más allá de `yyjson` (y librerías estándar / `pthread` si aplica).
- Antes de escribir todo el código, presenta el plan y el `README.md`, y luego implementa los módulos paso a paso asegurando que compilan limpiamente.
