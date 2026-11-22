# Cliente Python de keyjson

Paquete `keyjson-client` para acceder desde Python a la API REST del servidor keyjson. Se importa como `keyjson`, usa `urllib` y `json` de la biblioteca estándar y no tiene dependencias de ejecución externas.

El cliente se distribuye bajo la licencia MIT; el archivo `LICENSE` se incluye en el paquete.

## Requisitos

- Python 3.10 o posterior.
- Un servidor keyjson accesible por HTTP. Por defecto, el cliente usa `http://127.0.0.1:8080`.
- El servidor no tiene autenticación. No lo expongas a redes no confiables.

## Instalación

Desde la raíz del repositorio, crea un entorno virtual e instala el paquete en modo editable:

```sh
python -m venv .venv
./.venv/bin/pip install -e ./python
```

La instalación editable permite usar el paquete directamente desde este checkout.

## Inicio rápido

```python
from keyjson import KeyJsonClient, KeyNotFoundError

client = KeyJsonClient("http://127.0.0.1:8080", timeout=3.0)

created = client.put("profile", {"name": "Ada", "active": True})
profile = client.get("profile")
all_keys = client.keys()
server_stats = client.stats()

client.put("session", {"user": "Ada"}, ttl=60)
client.delete("profile")

try:
    client.get("missing")
except KeyNotFoundError:
    print("The key does not exist")
```

`KeyJsonClient` acepta una URL base HTTP o HTTPS y un timeout positivo en segundos. La URL puede incluir un prefijo de ruta, pero no credenciales, query string ni fragmento. El valor predeterminado es `KeyJsonClient()`.

## API del cliente

| Método | Resultado | Descripción |
| --- | --- | --- |
| `put(key, value, *, ttl=None)` | `bool` | Crea o reemplaza un valor JSON. Devuelve `True` si creó la clave y `False` si la reemplazó. |
| `get(key)` | Valor JSON de Python | Devuelve el valor o lanza `KeyNotFoundError`. |
| `delete(key)` | `None` | Elimina la clave o lanza `KeyNotFoundError`. |
| `keys()` | `list[str]` | Devuelve las claves; el orden no está garantizado. |
| `stats()` | `dict[str, int]` | Devuelve contadores y métricas de almacenamiento. |

`put()` acepta valores JSON representables como `dict`, `list`, `str`, `int`, `float`, `bool` o `None`, incluidos objetos anidados. Los valores no serializables y los números no finitos (`NaN` o `Infinity`) se rechazan.

Las claves deben ser cadenas UTF-8 no vacías, de hasta 1024 bytes, y no pueden contener caracteres de control. El cliente aplica percent-encoding al segmento de ruta.

## Tiempo de vida (TTL)

Pasa `ttl` como entero positivo de segundos:

```python
client.put("temporary", {"state": "active"}, ttl=60)
```

Esto envía `X-TTL-Seconds: 60` en `PUT /keys/temporary`. El TTL es opcional: si no se especifica, la clave queda sin expiración. Si actualizas una clave que tenía TTL y omites `ttl`, se elimina su expiración anterior. `ttl=0`, valores negativos, booleanos y números no enteros se rechazan localmente.

El servidor mide los vencimientos con un reloj monotónico y los limpia perezosamente, sin un hilo de fondo. Una clave expirada se comporta como inexistente (`get()` y `delete()` lanzan `KeyNotFoundError`) y no aparece en `keys()` ni en el contador `keys` de `stats()`.

## Errores

Todas las excepciones propias heredan de `KeyJsonError`:

- `KeyNotFoundError`: la clave solicitada no existe; hereda de `KeyJsonHTTPError`.
- `KeyJsonHTTPError`: el servidor devolvió un error HTTP. Expone `status_code` y, si estaba en la respuesta JSON, `error_code`.
- `KeyJsonTransportError`: no se pudo completar la conexión o la solicitud, incluido un timeout.
- `KeyJsonProtocolError`: la respuesta no era JSON válido o no tenía la forma esperada.

Los argumentos inválidos y valores que no pueden serializarse se notifican con `TypeError` o `ValueError` antes de enviar la solicitud.

```python
from keyjson import KeyJsonClient, KeyJsonHTTPError, KeyNotFoundError

client = KeyJsonClient()
try:
    value = client.get("unknown")
except KeyNotFoundError:
    value = None
except KeyJsonHTTPError as error:
    print(error.status_code, error.error_code)
```

## Pruebas

Las pruebas usan `unittest` y un servidor HTTP temporal en loopback. No requieren el contenedor keyjson ni modifican sus datos:

```sh
PYTHONPATH=python ./.venv/bin/python -m unittest discover -s python/tests -v
```

## Checklist

- [x] Especificar instalación y configuración del cliente.
- [x] Documentar operaciones, TTL y errores públicos.
- [x] Añadir ejemplos de uso y ejecución de pruebas.
- [x] Revisar el contenido contra el paquete y el servidor.
