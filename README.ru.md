# bc250-async-compute-bazzite

Async compute для AMD BC-250 (GFX1013) на Bazzite и Fedora atomic.

Патченный RADV ставится отдельным драйвером Vulkan в
`/usr/local/lib/bc250-radv`. Системная Mesa не заменяется.

## Установка

Скачайте `bc250-async-compute-<версия>.tar.zst` из
[Releases](../../releases), затем:

```bash
tar --zstd -xf bc250-async-compute-*.tar.zst
cd bc250-async-compute-*/
sudo ./install.sh
```

Выйдите из системы и войдите снова. Параметры запуска играм не нужны.

## Проверка

```bash
bc250-async-compute status
```

Должно быть:

```
dedicated compute (ACE) queue families:
  system driver : 0
  this package  : 1
```

## Тесты

```bash
bc250-async-compute test    # короткий
bc250-async-compute soak    # длинный, с побитовой сверкой
```

Записать, использовала ли игра вычислительные очереди — в параметры запуска
игры:

```
bc250-game-trace %command%
```

Потом:

```bash
bc250-async-compute trace
```

## Удаление

```bash
sudo ./uninstall.sh
```

Если после установки не вернулся рабочий стол — Ctrl+Alt+F3 и:

```bash
sudo rm /etc/environment.d/95-bc250-async-compute.conf
sudo systemctl reboot
```

## Сборка драйвера самостоятельно

Нужен `podman`, занимает 20–40 минут. Команды — в [README.md](README.md#build-the-driver-yourself).

## Требования

- AMD BC-250
- Bazzite 44 или Fedora 44 atomic
- ядро 7.2.0-ogc4.1 или новее (идёт в составе Bazzite)

## Что патчится

Три патча к Mesa 26.2.1, все в `patches/`:

| Патч | Что делает |
|---|---|
| `0001` | открывает выделенную вычислительную очередь (ACE) на GFX1013 и направляет чип через существующий обход Iceland/Tonga |
| `0002` | необязательный вывод счётчиков очередей, `BC250_IP_DEBUG=1` |
| `0003` | `VK_EXT_pageable_device_local_memory`, для паритета с Mesa из Bazzite |

## Ограничения

- Патчи ядра не применяются. Проверено только на ядре OGC из Bazzite
  (`7.2.0-ogc4.1`, `7.2.0-ogc6.1`). На других ядрах — см. первоисточник, ему
  своя половина для ядра нужна.
- Прирост FPS здесь не измерялся.
- `VK_EXT_pageable_device_local_memory` требует `AMDGPU_GEM_OP_SET_PRIORITY` в
  ядре. В ядре Bazzite этой операции нет, поэтому расширение объявлено, но
  ничего не делает — ровно как у стокового драйвера. Проверяется программой
  `test/gemop-probe.c`.
- `dEQP-VK.synchronization2` не прогонялся.

## Благодарности и лицензия

См. [README.md](README.md#credits). MIT, патчи производны от Mesa.
