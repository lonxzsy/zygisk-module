# Universal Zygisk Dumper (Unity IL2CPP & Unreal Engine 4/5)

Универсальный Zygisk-модуль для дампа игр на **Unity (IL2CPP)** и **Unreal Engine 4 / 5** в реальном времени с внутриигровым оверлеем **Dear ImGui** (поддержка **OpenGL ES 2/3** и **Vulkan**).

## Возможности

- **Автоопределение движка**:
  - **Unity (IL2CPP)**: `libil2cpp.so`, а также автоматический поиск переименованных библиотек по экспортам (`il2cpp_init`, `il2cpp_domain_get`) или через `custom_lib=`.
  - **Unreal Engine 4 и 5**: `libUE4.so` и `libUnreal.so` (UE 4.11–4.27 и UE 5.0–5.5+).
- **Unity IL2CPP Дамп**:
  - Генерация `dump.cs` (классы, вложенные типы `DeclaringType`, поля со смещениями, значения констант `enum`, C#-свойства `get/set`, методы с RVA/VA) и `script.json` для IDA/Ghidra.
  - Разрешение символов как через `.dynsym`, так и через `.symtab` (через `xdl` / `shadowhook_dlsym`).
  - Извлечение расшифрованного `global-metadata.dat` из памяти (по `0xFAB11BAF` или структурной реконструкции заголовка при затёртых магических байтах).
  - Дамп памяти `libil2cpp_mem.so` и автопоиск `Il2CppCodeRegistration` при полностью удалённых экспортах.
- **Unreal Engine 4 / 5 Дамп**:
  - Автопоиск `GNames` (`FNamePool` для UE 4.23+/UE5 и `TNameEntryArray` для UE 4.11–4.22) и `GUObjectArray` (`Chunked` и `Fixed`).
  - Автокалибровка смещений `UObject`, `UStruct`, `UField`/`FField`, `FProperty`, а также поддержка 4-байтовых `FName` (`UE_FNAME_OUTLINE_NUMBER` в UE5) и `double`-векторов (24 байта в UE5).
  - Автокалибровка `UFunction::Func` и генерация `SDK_Dump.hpp`, `ObjectsDump.txt`, `NamesDump.txt`, `FunctionsDump.txt`, `Offsets_Info.txt` и `script.json` для импорта нативных функций UE4/UE5 в IDA/Ghidra.
- **Интерактивный оверлей (GLES & Vulkan)**:
  - Кнопки **`[ Dump Now / Re-Dump Now ]`**, **`[-] Minimize`** (сворачивание в компактную плавающую кнопку `[+] Dumper`), **`Clear Log`** и **`Auto-scroll`**.
  - Корректный перехват касаний через `AInputQueue` и поиск `UnityPlayer.nativeInjectEvent` прямо в памяти (работает даже при `extractNativeLibs="false"` и split APK).
  - Возможность работы в headless-режиме (`overlay=0`) или отложенного ручного дампа (`auto_dump=0`).

## Сборка

Требуется Windows PowerShell, CMake и Android NDK (определяется автоматически через `ANDROID_NDK_HOME` или в стандартных путях):

```powershell
.\build.ps1
```

Готовый архив для установки в Magisk / KernelSU (ZygiskNext) / APatch создаётся в `dist/zygisk_il2cpp_dumper.zip`.

## Настройка (`config.txt`)

Файл конфигурации на устройстве находится по пути `/data/adb/modules/zygisk_il2cpp_dumper/config.txt`:

```ini
package=com.example.game
output=auto
auto_dump=1
overlay=1
init_delay_ms=5000
dump_cs=1
dump_script_json=1
dump_metadata=1
dump_so=1
```

Быстрая установка целевого пакета через `adb shell su`:
```sh
sh /data/adb/modules/zygisk_il2cpp_dumper/set_target.sh com.example.game
```
