# Lua

## 加载 Lua 模块

​`insmod lua`

## 进入 Lua 交互编程模式

​在控制台中输入 `lua`

## 执行 Lua 脚本

​`lua /path/to/script.lua`

## grub 函数库

- grub.run (`string` command)

  执行 GRUB 命令，若执行成功，则返回零，否则返回非零值。

- grub.script (`string` script)

  执行一行 GRUB 脚本，若脚本执行成功则返回零。

- grub.getenv (`string` variable)

  获取 GRUB 环境变量的值，若环境变量存在，则返回变量的值，否则，返回 `nil`。

- grub.setenv (`string` variable, `string` value)

  设置 GRUB 环境变量的值。第一个参数为变量名，第二个参数为变量内容。

- grub.exportenv (`string` variable\[, `string` value\])

  设置 GRUB 全局环境变量的值。第一个参数为变量名，第二个参数可选，为变量内容。

- grub.enum_device (`function` (`string` device\[, `string` fs, `string` uuid, `string` label, `string` size\]))

  枚举 GRUB 的磁盘设备。

  参数为枚举执行的函数，可获取设备名，文件系统，UUID 和卷标。

- grub.enum_file (`function` (`string` filename\[, `int` isdir\]), `string` path)

  枚举某目录下的文件和文件夹 (包括 `.` 和 `..`)

- `userdata` file = grub.file_open (`string` filename\[, `string` flag\])

  打开文件，返回值为 `userdata` 类型的文件句柄。

  第一个参数是文件名。第二个参数为打开方式，若为 “w” 则以可写模式打开。

- grub.file_close (`userdata` file)

  关闭文件。

- grub.file_seek (`userdata` file, `integer` offset)

  设置文件句柄读写的位置偏移，返回值为偏移量。

- grub.file_read (`userdata` file, `integer` length)

  读取文件，长度为 “length”，返回文件内容。

- grub.file_write (`userdata` file, `string` data)

  将字符串 “data” 写入文件。

- grub.file_getline (`userdata` file)

  从文件读取一行文本，返回值为该文本。

- grub.file_getsize (`userdata` file)

  返回文件大小。

- grub.file_getpos (`userdata` file)

  返回文件当前偏移量。

- grub.file_eof (`userdata` file)

  判断文件是否已读到末尾。

- grub.file_exist (`string` filename)

  判断文件是否存在。

- `string` buf, `string` hex = grub.hexdump (`userdata` file, `integer` skip, `integer` length)

  获取文件指定位置的十六进制数据。

- grub.add_menu (`string` source, `string` title\[, …\])

  添加菜单项，菜单内容为 “source”，标题为 “title”。

- grub.add_icon_menu (`string` icon, `string` source, `string` title\[, …\])

  添加带图标 (–class) 的菜单项。

- grub.add_hidden_menu (`string` hotkey, `string` source, `string` title\[, …\])

  添加隐藏菜单项，热键为 “hotkey”。

- grub.clear_menu (`nil`)

  清空菜单。

- grub.cls (`nil`)

  清屏。

- grub.setcolorstate (`integer` state)

- grub.refresh (`nil`)

  执行 `grub_refresh` 函数

- `string` text = grub.gettext (`string` src)

  翻译字符串。

- `integer` rand = grub.random (`integer` m)

  返回一个小于 “m” 的随机数。

- grub.enum_block (`function` (`string` block)\[, `int` part_start\])

  枚举文件的块列表，形式为 sector+size。若存在 `part_start` 参数，则扇区号基于分区起始扇区计算。

## ini 函数库

- `userdata` inifile = ini.load (`string` filename)

  加载 ini 配置文件。

- ini.free (`userdata` inifile)

  释放 ini 配置文件。

- `string` ini.get (`userdata` inifile, \[`string` section, \] `string` key)

  从 ini 中读取配置项。

## input 函数库

- `integer` ascii_code, `integer` scan_code = input.getkey (`nil`)

  等待用户按键，返回 ASCII 码和扫描码。

- `integer` ascii_code, `integer` scan_code = input.getkey_noblock (`nil`)

  返回 ASCII 码和扫描码 (在循环中使用)。

- `string` line = input.read (`nil`)

  等待用户输入一行字符串。

## video 函数库

- video.swap_buffers (`nil`)

- video.fill_rect (`table` color{`integer` r, `integer` g, `integer` b, `integer` a}, `integer` x, `integer` y, `integer` w, `integer` h)

  在指定位置绘制矩形。

- video.draw_string (`string` text, `string` font, `table` color{`integer` r, `integer` g, `integer` b, `integer` a}, `integer` x, `integer` y)

  在指定位置显示字符串。

- `string` video_mode = video.info (`nil`)

  获取图像模式列表。

- video.draw_pixel (`table` color{`integer` r, `integer` g, `integer` b, `integer` a}, `integer` x, `integer` y)

  在指定位置画一个像素点。

- `integer` x, `integer` y = video.get_info (`nil`)

  获取当前显示模式的宽度和高度。

- `userdata` bitmap = video.bitmap_load (`string` filename)

  加载图像文件 (支持 bmp, jpg, jpeg, png, tga)。

- video.bitmap_close (`userdata` bitmap)

  关闭图像文件。

- `integer` x, `integer` y = video.bitmap_info (`userdata` bitmap)

  获取图像的宽度和高度。

- video.bitmap_blit (`userdata` bitmap, `integer` x, `integer` y, `integer` offset_x, `integer` offset_y, `integer` w, `integer` h)

  在指定位置显示图像。

- `userdata` scaled_bitmap = video.bitmap_rescale (`userdata` bitmap, `integer` w, `integer` h)

  缩放指定图像。

## gbk 函数库

- `string` gbk_str = gbk.fromutf8 (`string` utf8_str)

  将 UTF-8 字符串转换为 GBK 编码字符串。

- `string` utf8_str = gbk.toutf8 (`string` gbk_str)

  将 GBK 编码字符串转换为 UTF-8 字符串。

- `string` utf8_simp = gbk.tosimp (`string` utf8_trad)

  将 UTF-8 繁体中文字符串转换为 UTF-8 简体中文字符串。

## disk 函数库

- `userdata` dev = disk.open (`string` diskname)

  打开磁盘，返回值为 `userdata` 类型的磁盘句柄。

- disk.close (`userdata` dev)

  关闭磁盘。

- `string` buf = disk.read (`userdata` dev, `integer` sector, `integer` offset, `integer` length)

  读取磁盘指定扇区/偏移的数据，返回字符串。

- disk.write (`userdata` dev, `integer` sector, `integer` offset, `integer` length, `string` buf)

  写入磁盘。

- `string` partmap = disk.partmap (`userdata` dev)

  获取分区表名称。

- `string` driver = disk.driver (`userdata` dev)

  获取磁盘驱动名。

- `string` fs = disk.fs (`userdata` dev)

  获取文件系统名。

- `string` uuid = disk.fsuuid (`userdata` dev)

  获取文件系统 UUID。

- `string` label = disk.label (`userdata` dev)

  获取磁盘卷标。

- `string` size = disk.size (`userdata` dev\[, flag\])

  获取磁盘大小。

- `boolean` boot = disk.bootable (`userdata` dev)

  判断分区是否有可启动标识。

## fatfs 函数库（历史接口，已退役）

以下是 a1ive 旧版 Lua API。当前分支采用统一的 `writefs.*` API，
不再暴露 `fat.*`；参见 [WriteFS API](../writefs.md)。


- fat.mount
- fat.umount
- fat.disk_status
- fat.get_label
- fat.set_label
- fat.mkdir
- fat.rename
- fat.unlink
- fat.open
- fat.close
- fat.read
- fat.write
- fat.lseek
- fat.tell
- fat.eof
- fat.size
- fat.truncate

## memrw 函数库

- `integer` value = memrw.read_byte (`integer` addr)

  从内存地址读取一个字节数据。

- `integer` value = memrw.read_word (`integer` addr)

  从内存地址读取双字节数据。

- `integer` value = memrw.read_dword (`integer` addr)

  从内存地址读取双字数据。

- memrw.write_byte (`integer` addr, `integer` value)

  向内存地址写入一个字节数据。

- memrw.write_word (`integer` addr, `integer` value)

  向内存地址写入双字节数据。

- memrw.write_dword (`integer` addr, `integer` value)

  向内存地址写入双字数据。