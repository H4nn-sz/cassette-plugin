# Cassette

Plugin VST3 / Standalone de cinta lo-fi y espacio, hecho con JUCE 8 (interfaz en HTML dentro de WebView2).

- Wow & flutter estéreo, hiss, head bump, dropouts, saturación, bitcrush
- Crackle de vinilo y ancho estéreo (0–200 %)
- Room / Hall / Space con reverb, pre-delay, brillo y ancho
- Un knob de **Edad** que escala toda la cinta; buscador y guardado de tus propias cintas

## Compilar (Windows)

```
cmake -S . -B build
cmake --build build --config Release --target CassettePlugin_VST3 CassettePlugin_Standalone
```

El instalador (Inno Setup 6): `installer/Cassette_Setup.iss`.

## Estructura

- `Source/` motor DSP y plugin (JUCE)
- `ui/index.html` interfaz completa (se embebe en el binario)
- `installer/` script del instalador
- `promo/` composición HyperFrames del video promo (sin los medios)
