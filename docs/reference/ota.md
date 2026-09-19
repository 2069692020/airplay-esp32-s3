# OTA updates

!!! info "本 fork 的状态：OTA 可用，但默认关闭"

    分区表 `components/boards/partitions-16m-ota.csv` 提供 `ota_0`/`ota_1` 两个 4MB
    槽 + `otadata`，系统页「固件升级」上传 `airplay2-receiver.bin` 即可，写的是另一个槽、
    镜像验不过不会切过去、成功后自动重启。
    **没设升级口令时这个接口一律 403**（fail-closed）——先在系统页设一个口令才打开。
    另外：正在播放 SD 卡 / 网络音乐时会被拒绝（写 flash 会关 cache），先停止再升级。
    不想用网页升级也完全可以，USB 烧录照旧可用。

Once the device is on your network you can update its firmware over WiFi without
unplugging anything. USB is only needed for the very first flash.

1. Get the new firmware — either build it (`pio run -e <env>` or `idf.py build`) or
   download a `.bin` from the
   [releases page](https://github.com/rbouteiller/airplay-esp32/releases/latest).
2. Open the device's web interface. Find its IP in your router's list of connected clients.
3. Use the firmware upload page to flash the new version.

The device reboots into the new firmware automatically. Settings stored in NVS — device
name, WiFi credentials, volume, EQ — survive the update.

!!! warning "OTA cannot change the partition table"

    If you are upgrading from a firmware built before the SPIFFS `storage` partition
    existed, the first flash has to happen over serial, because the partition layout
    itself changes. See [SPIFFS filesystem](spiffs.md#flashing-the-image).

## Updating files without reflashing

Web pages, the ST7789 background image and TAS57xx hybrid flow programs live on SPIFFS and
can be replaced individually over HTTP, without touching the firmware:

```bash
curl -X POST "http://<device-ip>/api/fs/upload?path=/spiffs/www/index.html" \
     --data-binary @data/www/index.html
```

See the [file management API](spiffs.md#file-management-api) for the full set of endpoints.
