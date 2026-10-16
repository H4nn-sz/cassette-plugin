# Cassette

Plugin VST3 / Standalone de cinta lo-fi y espacio, hecho con JUCE 8 (interfaz en HTML dentro de WebView2).

- Wow & flutter estéreo, hiss, head bump, dropouts, saturación, bitcrush
- Crackle de vinilo y ancho estéreo (0–200 %)
- Room / Hall / Space con reverb, pre-delay, brillo y ancho
- Un knob de **Edad** que escala toda la cinta; buscador y guardado de tus propias cintas

## Descargar

[**Descargar Cassette (instalador para Windows)**](https://drive.google.com/file/d/1-SUwo3lwjG75coa9njn6u3gQl8Hg1-Dm/view?usp=sharing)

**Clave de prueba (trial):** `CASSETTE-TRIAL-1D6D-DED7`

## Compilar (Windows)

```
cmake -S . -B build
cmake --build build --config Release --target CassettePlugin_VST3 CassettePlugin_Standalone
```

El instalador (Inno Setup 6): `installer/Cassette_Setup.iss`.

## Licencias (OFFSZN)

Serial `CASSETTE-(FULL|TRIAL|SUB)-XXXXXXXX-XXXXXXXX`, activación online contra `https://offszn.lat` (`/api/plugin/...`),
firma Ed25519 exigida (política 2), registro sellado en `%APPDATA%\OFFSZN\Cassette.lic` + espejo en `HKCU\Software\OFFSZN\Cassette`.
Sin licencia el audio pasa limpio (señal seca alineada) y la interfaz muestra la pantalla de activación; la prueba y la
suscripción muestran una insignia con los días restantes. C++ (`Source/licensing/`) es la única puerta; la UI solo muestra estado.

**Para que active de verdad** el backend OFFSZN necesita una fila `cassette` (prefijo `CASSETTE`, nombres `cassette`)
en `server/src/shared/config/pluginRegistry.js` y los ids de producto de la tienda.

Pruebas: `CassetteTest` (motor) y `CassetteLicenseTests` (25 pruebas de licencias con reloj/red/disco simulados; necesita Node).
Para E2E con servidor simulado: `-DCASSETTE_API_BASE_OVERRIDE=http://127.0.0.1:8787` (nunca distribuir ese binario).

## Estructura

- `Source/` motor DSP y plugin (JUCE)
- `Source/licensing/` cliente de licencias (Ed25519 + sello local)
- `ui/index.html` interfaz completa (se embebe en el binario)
- `installer/` script del instalador
- `promo/` composición HyperFrames del video promo (sin los medios)
