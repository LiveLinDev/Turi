# `lienzo` · Biblioteca visual oficial de Turi

Paquete opcional (`require lienzo 0.1.0 "../Turi/lib/lienzo"`) para interfaces nativas adaptables con gráficos vectoriales antialias sobre Windows GDI+. Quienes no la importan pagan **cero bytes y cero sobrecarga**.

## Módulos

- `metrica`: cálculo proporcional (`px(escala, v)`, `construirMetrica(...)`) para evitar coordenadas o tamaños fijos.
- `disposicion`: cajas (`Caja`, `contiene`, `solapan`, `dentroDe`, `recortar`), rejillas (`columnasRejilla`) y barras adaptables (`planificarFila`: completo → solo icono → desborde).
- `iconos`: analizador de rutas SVG (`M m L l H h V v C c Q q Z z` y extensión `O cx cy r`) en caja `24×24`, con catálogo integrado de 26 iconos y caché en `Map<str, Icono>`.
- `trazo`: dibujo antialias GDI+ encapsulado (`iniciar`, `cerrar`, `caja`, `rellenarCaja`, `bordeCaja`, `degradadoCaja`, `circulo`, `linea`, `ondulada`, `dibujarIcono`, `dibujarLogo`, `iconoVentana`, `guardarPng`, `escribirIco`).
- `tipografia`: carga privada por proceso (`AddFontResourceExW(..., FR_PRIVATE)`) y verificación (`GetTextFaceW`) de las fuentes incluidas **Inter** (`UI`) y **JetBrains Mono** (`código`) con respaldo automático (`Segoe UI`, `Cascadia Mono`, `Consolas`).

## Fuentes incluidas (`fuentes/`)

En `fuentes/` se distribuyen TTF estáticos bajo licencia **SIL Open Font License 1.1** ([LICENSE-Inter.txt](fuentes/LICENSE-Inter.txt), [OFL-JetBrainsMono.txt](fuentes/OFL-JetBrainsMono.txt)): `Inter-Regular.ttf`, `Inter-SemiBold.ttf`, `Inter-Bold.ttf`, `JetBrainsMono-Regular.ttf`, `JetBrainsMono-Bold.ttf` y `JetBrainsMono-Italic.ttf`. Se cargan únicamente en el proceso activo sin instalarse en Windows y se liberan al salir.

## Reglas de seguridad y memoria

- **Cero handles GDI+ expuestos**: cada función de dibujo crea sus recursos (`GpGraphics`, `GpPath`, `GpPen`, `GpBrush`) a partir del `hdc` recibido y los destruye antes de retornar, incluidas las rutas de error.
- **Texto ClearType nítido**: los rótulos siguen renderizándose con GDI `ExtTextOutW` (`CLEARTYPE_NATURAL_QUALITY`) después o entre primitivas de `trazo`.

## Ejemplo mínimo

```turi
import metrica
import iconos
import trazo
import tipografia

fn pintarBoton(hdc: MutPtr<void>, dirFuentes: str, cat: Map<str, iconos.Icono>, m: metrica.Metrica, x: int, y: int) {
    let f = tipografia.cargarPrivadas(dirFuentes)
    let w = m.iconPx + m.pad * 2
    trazo.caja(hdc, x, y, w, m.controlH, m.radio, 0x252A38, 0x3B82F6, 1)
    let ic = iconos.buscarEnCatalogo(cat, "play")
    trazo.dibujarIcono(hdc, ic, x + m.pad, y + (m.controlH - m.iconPx) / 2, m.iconPx, 0xFFFFFF)
    tipografia.liberarPrivadas(f)
}
```

## Cómo añadir un icono SVG

1. Agrega el nombre en `nombresIconos()` dentro de `src/iconos.turi`.
2. Añade su rama en `definicionIcono(nombre)` llamando `parsearRutaEn(nombre, "M... ...", relleno)` dentro del `viewBox="0 0 24 24"`.
3. Ejecuta `veloz check lib/lienzo/src/lienzo.turi` para validar sintaxis y rango de coordenadas.
