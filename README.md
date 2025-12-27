# Wii Project - Tributo PS1 & Escena 3D Isométrica

Este proyecto es una aplicación homebrew avanzada para **Nintendo Wii**, desarrollada en **C** utilizando las librerías **devkitPro** y **libogc**. 

El objetivo principal de esta aplicación es recrear fielmente la icónica animación de inicio de la consola PlayStation 1 (PS1), seguida de una transición fluida hacia una segunda escena ("Scene 2") que demuestra capacidades de renderizado mixto (modelos 3D + sprites 2D) con iluminación dinámica, todo sincronizado con una pista de audio de alta calidad.

## 🚀 Características Principales

*   **Animación Procedural**: La intro no es un video; es una animación generada en tiempo real utilizando primitivas gráficas de la GPU de Wii (GX).
*   **Audio Sincronizado**: Utiliza un sistema de audio PCM RAW de 16-bit a 48kHz, cargado directamente en la memoria para una latencia mínima y sincronización perfecta con los eventos visuales.
*   **Renderizado Híbrido**: La "Escena 2" combina un modelo 3D (formato OBJ) con iluminación de dos puntos (Sol + Relleno) y sprites 2D posicionados dinámicamente.
*   **Sistema de Sprites Configurable**: Las posiciones y recortes de los elementos gráficos se definen en un archivo de texto externo (`scene2.txt`), permitiendo ajustes rápidos sin recompilar.
*   **Gestión de Energía**: La aplicación está diseñada para salir limpiamente al Menú del Sistema de Wii exactamente cuando termina la pista de audio (~20 segundos), gestionando correctamente el apagado del subsistema de video y audio.

---

## 🛠️ Requisitos de Compilación

Para compilar este proyecto, necesitas un entorno de desarrollo configurado para Wii:

1.  **devkitPro**: El toolchain estándar para desarrollo homebrew.
2.  **libogc**: Librería base para acceso al hardware de GameCube/Wii.
3.  **ffmpeg** (Opcional): Si deseas cambiar y convertir nuevos archivos de audio `.mp3` al formato `.raw` requerido.

### Entorno Recomendado (macOS/Linux)
Asegúrate de tener las variables de entorno `DEVKITPRO` y `DEVKITPPC` correctamente definidas en tu shell.

---

## 📂 Estructura del Proyecto

El proyecto sigue una estructura organizada para facilitar el mantenimiento:

```
wii-project/
├── source/                 # Código Fuente (.c / .h)
│   ├── main.c              # Punto de entrada, animación de intro y lógica principal
│   ├── scene2.c            # Lógica específica de la segunda escena (3D/2D)
│   └── scene2.h            # Cabecera para compartir funciones entre escenas
├── audio/                  # Recursos de Sonido
│   └── startup.raw         # Audio PCM 48kHz (generado desde MP3)
├── images/                 # Recursos Gráficos
│   ├── sprites.png         # Atlas de texturas para los elementos 2D
│   └── ps1/                # Recursos específicos de la intro
├── scene2.txt              # Configuración de regiones de sprites (Hot-loading embebido)
├── Makefile                # Script de compilación automatizado
├── meta.xml                # Metadatos para el Homebrew Channel (nombre, autor, versión)
└── README.md               # Esta documentación
```

---

## ⚙️ Funcionamiento Interno

### 1. Motor Gráfico (GX)
La Wii utiliza una API gráfica propietaria de bajo nivel llamada **GX**. Este proyecto interactúa directamente con GX para:
*   **Display Lists**: Los modelos 3D estáticos se compilan en listas de comandos (Display Lists) para un rendimiento óptimo, enviando geometría a la GPU en bloques pre-calculados.
*   **Proyección Ortográfica**: Se utiliza `guOrtho` para lograr el aspecto isómetrico/plano tanto en la intro como en la disposición de sprites.
*   **Z-Buffering**: Se gestiona manualmente la profundidad (`GX_SetZMode`) para asegurar que los modelos 3D se rendericen correctamente sobre (o detrás de) los elementos 2D.

### 2. Sistema de Audio (ASND)
En lugar de usar librerías de alto nivel complejas, utilizamos **ASNDLIB** para reproducir audio RAW.
*   El archivo `startup.mp3` se convierte a `startup.raw` (PCM s16be 48000Hz Stereo) durante el desarrollo.
*   Este archivo binario se incrusta en el ejecutable final (`boot.dol`) usando `bin2s` en el Makefile.
*   En tiempo de ejecución, `ASND_SetVoice` reproduce los datos directamente desde la memoria RAM, garantizando que no haya pausas de carga.

### 3. Lógica de Tiempos y Transiciones
La aplicación mantiene un contador de tiempo global (`appStartTime`) obtenido via `gettime()` al inicio.
*   **0.0s - 4.0s**: Pantalla Negra (Silencio visual).
*   **4.0s - 5.0s**: Fade-in a Blanco.
*   **5.0s - 12.0s**: Animación del Rombo (Estilo PS1) y aparición de sprites.
*   **12.0s**: Transición dura a **Scene 2** (llamada a `RunScene2`).
*   **12.0s - 19.8s**: Escena 2 activa (Modelo 3D + Sprites personalizados).
*   **19.8s**: **Trigger de Salida**. Al coincidir con el final del audio, se ejecuta `SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0)`.

---

## 📝 Configuración de Sprites (`scene2.txt`)

El archivo `scene2.txt` define qué partes de la textura `sprites.png` se dibujan y dónde. El formato es:

```text
NombreRegion PosX_Pantalla PosY_Textura Ancho Alto
```

*   **PosX_Pantalla**: Posición X relativa en la pantalla renderizada.
*   **PosY_Textura**: Coordenada Y superior en la imagen `sprites.png` (origen arriba-izquierda).
*   **Ancho/Alto**: Dimensiones del recorte.

La posición Y en pantalla se calcula dinámicamente en código (`scene2.c`) para permitir efectos de apilado automático, pero las coordenadas de textura son fijas según este archivo.

---

## 🔧 Compilación y Despliegue

### Paso 1: Limpiar
Si has hecho cambios previos, limpia los objetos compilados:
```bash
make clean
```

### Paso 2: Compilar
Genera el archivo `.dol`:
```bash
make
```
*Si la compilación es exitosa, verás "Build completo: boot.dol generado".*

### Paso 3: Ejecutar en Wii
1.  Copia la carpeta del proyecto a `/apps/wii-project/` en tu tarjeta SD o USB.
2.  Asegúrate de que `boot.dol` y `meta.xml` estén presentes.
3.  Inicia desde el **Homebrew Channel**.

### Paso 4: Ejecutar en Emulador (Dolphin)
Simplemente abre el archivo `boot.dol` con Dolphin Emulator.
> **Nota**: Dolphin puede no emular perfectamente los tiempos de carga de disco o caché de la Wii real, pero la lógica de la aplicación debería ser idéntica.

---

## 🐛 Solución de Problemas

*   **La pantalla se congela al inicio**: Verifica que los archivos embebidos (`startup.raw`) no sean demasiado grandes para la memoria MEM1/MEM2 de la Wii.
*   **El audio suena distorsionado**: Asegúrate de que la conversión a RAW usó **Big-Endian 16-bit** (`s16be`). La Wii usa arquitectura PowerPC (Big Endian).
*   **Gráficos corruptos**: Si editas `sprites.png`, asegúrate de que sus dimensiones sean potencias de 2 (ej. 512x512, 1024x1024) para máxima compatibilidad con GX.

---

## © Créditos

Desarrollado como un proyecto de demostración técnica para la comunidad homebrew de Wii.
*   Librerías: **devkitPro**, **libogc**, **stb_image**.
*   Inspiración: BIOS de Sony PlayStation 1.
