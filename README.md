## BlueField-2 DPU hardware compression performance evaluation

- HW deflate vs Zstd vs Zlib 
  - HW deflate (BF2): Compression processing using hardware accelerator
  - Zstd (HOST): Zstd compression on HOST machine core (1core)
  - Zstd (BF2): Zstd compression on BF2 core (1core)
  - Zlib (HOST): Zlib compression on HOST machine core (1core)
  - Zlib (BF2): Zlib compression on BF2 core (1core)

- Machine specification
  - BF2: BlueField-2 DPU eMMC 16GB, Arm core x8, 
  - HOST: Intel(R) Xeon(R) CPU E5-2630 v4 @ 2.20GHz x2 socket, x10 core/socket, RAM 128GB

- Result
  - Zstd (HOST):
  - Zstd (BF2): Zstd compression on BF2 core (1core)
  - Zlib (HOST): Zlib compression on HOST machine core (1core)
  - Zlib (BF2): Zlib compression on BF2 core (1core)

|-|QVAPORf01.bin (float)| COVID19 (text)
|---|---|---|
|HW deflate     |0.126||
|Zstd (BF2)     |0.351||
|Zstd (HOST)    |0.226||
|Zlib (BF2)     |9.027||
|Zlib (HOST)    |5.758||


```
########## On BlueField2 DPU
[15:13:03:603979][DOCA][INF][FILE_COMPRESSION::Core:978]: Started Listening, waiting for new connection
[15:13:06:251713][DOCA][DBG][FILE_COMPRESSION::Core:747]: [SERVER] Start receiving file from the client total: 24510 msgs
[15:13:06:348343][DOCA][DBG][FILE_COMPRESSION::Core:505]: [SERVER] HW Compression start
[15:13:06:475334][DOCA][INF][FILE_COMPRESSION::Core:512]: [SERVER] HW compression time 0.126884, len=86145786
[15:13:06:475445][DOCA][DBG][FILE_COMPRESSION::Core:514]: [SERVER] Zlib Compression start
[15:13:15:503380][DOCA][INF][FILE_COMPRESSION::Core:521]: [SERVER] Zlib compression time 9.027908, len=85890168
[15:13:15:503480][DOCA][DBG][FILE_COMPRESSION::Core:523]: [SERVER] Zstd Compression start
[15:13:15:855247][DOCA][DBG][FILE_COMPRESSION::Core:530]: [SERVER] Zstd Compression time 0.351747, len=86498223
[15:13:15:855356][DOCA][DBG][FILE_COMPRESSION::Core:796]: [SERVER] Compression done
[15:13:15:855371][DOCA][DBG][FILE_COMPRESSION::Core:800]: [SERVER] Send file to client
[15:13:15:855389][DOCA][DBG][FILE_COMPRESSION::Core:433]: total message 21201

############## On HOST
[00:13:06:252247][DOCA][INF][FILE_COMPRESSION::Core:968]: Connection to DPU was established successfully
[00:13:06:252389][DOCA][INF][FILE_COMPRESSION::Core:582]: File size: 100000000
[00:13:06:252412][DOCA][DBG][FILE_COMPRESSION::Core:433]: total message 24510
[00:13:06:345639][DOCA][DBG][FILE_COMPRESSION::Core:589]: send_file finished
[00:13:06:349227][DOCA][INF][FILE_COMPRESSION::Core:603]: OK: Server was done receiving messages
[00:13:15:856265][DOCA][DBG][FILE_COMPRESSION::Core:629]: [CLIENT] Receiving file from the server total: 21201 msgs
[00:13:15:907559][DOCA][DBG][FILE_COMPRESSION::Core:664]: [CLIENT] file to write-> ../dataset/QVAPORf01.bin.deflate
[00:13:16:014371][DOCA][DBG][FILE_COMPRESSION::Core:470]: [CLIENT] Zlib Compression start
[00:13:21:772799][DOCA][INF][FILE_COMPRESSION::Core:477]: [CLIENT] Zlib compression time 5.758369, len=85890168
[00:13:21:772828][DOCA][DBG][FILE_COMPRESSION::Core:479]: [CLIENT] Zstd Compression start
[00:13:21:999690][DOCA][DBG][FILE_COMPRESSION::Core:486]: [CLIENT] Zstd Compression time 0.226856, len=86498223
[00:13:21:999731][DOCA][DBG][FILE_COMPRESSION::Core:690]: [CLIENT] Finish Client


```