# emu8950 (vendored, 未修改)

来源: https://github.com/digital-sound-antiques/emu8950  (branch: develop)
作者: Mitsutaka Okazaki
许可: MIT (见同目录 LICENSE)

用途: 作为 adplug `OPLCORE` 的一个后端, 替代过重的 DOSBox DBOPL,
      在 ESP32-S3 上做实时 OPL2 (YM3812) 合成, 播放仙剑 MUS.MKF 里的 RIX。

⚠ 本目录文件**保持上游原样**, 不做任何修改。
   所有适配 (采样率/时钟/寄存器补偿) 都在上一层的 `../emu8950_opl.cpp`,
   这样上游更新时可以整目录替换。
