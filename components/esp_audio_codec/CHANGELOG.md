# Changelog

## v2.5.0

### Features

- Added OGG container support for simple decoder
- Added G722 encoder and decoder support
- Added helper API `esp_audio_simple_dec_get_name` for simple decoder
- Supported build for IDFv6.0

### Bug Fixes

- Improved SBC bitpool calculation accuracy when setting bitrate

## v2.4.1

### Features

- Added audio decoder default configuration

### Bug Fixes

- Fixed decoder reset API abnormal for codecs
- Fixed padding size in `frame_info` of es parser is not clear
- Fixed `AMR-NB` and `AMR-WB` reset forget to clear some AMR header flags
- Fixed `FLAC` not reset `info_set` flag which will cause audio information not update
- Fixed Encoder reset not clear `sample_num`
- Fixed can not skip decode error frame
- Fixed dead loop of ts parser when file corrupt in middle
- Added parse of mp3 padding data size in `Info` tag

## v2.4.0

### Features

- Added support for `AAC` decoder to decode spliced audio streams
- Added support for `FLAC` decoder to decode 32-bit audio streams
- Added extensible format parsing support in `WAV` parser
- Added `esp_xxx_enc_reset`, `esp_xxx_dec_reset` and `esp_audio_simple_dec_reset` APIs to reset decoders or encoders to initialized state
- Added `Vorbis` decoder to the simple decoder module
- Added `ESP_OPUS_BITRATE_AUTO` in `OPUS` encoder
- Added test code for customized simple decoder cases

### Bug Fixes

- Enhanced `MP3` and `AAC` parsers to inspect additional frames for more reliable frame validation
- Fixed `FLAC` parser error in getting valid frame size
- Fixed decoder error toleranced frame count not taking effect in `simple_decoder`
- Fixed `SBC` decoder reporting an incorrect frame size when returning `ESP_AUDIO_ERR_BUFF_NOT_ENOUGH`

## v2.3.0

### Features

- Added support for SBC codec
- Added support for LC3 codec
- Added compatibility with `esp_audio_codec` on `ESP32-H4`
- Added `esp_xxx_enc_get_frame_info_by_cfg` APIs to get frame information by configuration before creating encoder handle
- Added `esp_xxx_enc_set_bitrate` APIs to set bitrate for encoder
- Added `esp_audio_dec_check_audio_type`, `esp_audio_enc_check_audio_type`, `esp_audio_simple_check_audio_type` APIs to check whether the audio type is supported by the codec
- Use `esp_fourcc.h` to define audio codec type
- Added CO64 atom parsing support for `M4A` format
- Standardized bitrate units in `AAC` and `Opus` encoder headers
- Added `ALAC` decoder to the simple decoder module
- Added `frame_duration` configuration support to `G711` and `PCM` encoders

### Bug Fixes
- Fixed crash in `AMR-NB` encoder when DTX is enabled
- Fixed crash in `AAC` encoder when recommended output buffer size is insufficient
- Fixed `M4A` parsing error when extra boxes are present under the mdat atom

## v2.2.1

### Bug Fixes

- Compile libraries with optimial compilation option

## v2.2.0

### Features

- Support g711a, g711u, pcm, adpcm decoder in audio simple decoder
- Add pcm decoder in esp_audio_codec

### Bug Fixes

- Fix a bug that dtx is not effect in opus encoder
- Complete error log in esp_audio_codec

## v2.1.0

### Features

- Support raw opus decoder in audio simple decoder
- Add a breaking change for the Opus encoder to support VBR
- Add a breaking change to the Opus decoder to support a specific frame duration

### Bug Fixes

- Fix a bug that opus decoder not check out buffer length
- Add error log when decoder failed to decode frame
- Fix a bug that out_buf size is larger than in_buf size in audio encoder

## v2.0.3

### Features

- Fix opus decode error


## v2.0.2

### Features

- Support MP2 and MP1 decode

### Bug Fixes

- Fix FLAC mono channel decode noise


## v2.0.1

### Features

- Support esp32c2 and esp32c5 board

### Bug Fixes

- Decrease memory usage when decode AAC-Plus
- Clear search position when simple decoder find valid frame to avoid accumulate and generate decoder error


## v2.0.0

### Features

- Add audio decoder common APIs to operate all supported decoders
- Add audio decoder implementation for `AAC`, `MP3`, `OPUS`, `ADPCM`, `G711A`, `G711U`, `AMRNB`, `AMRWB`, `VORBIS`, `ALAC`
- Add audio simple decoder common APIs to operate all supported simple decoders
- Add audio simple decoder implementation for `AAC`, `MP3`, `M4A`, `TS`, `AMRNB`, `AMRWB`, `FLAC`, `WAV`
- Add `esp_es_parse` to easily parse and get audio frame
- Add audio encoder registration and customization support through `esp_audio_enc_register`, deprecate `esp_audio_enc_install` and `esp_audio_enc_uninstall` APIs
- Add audio decoder registration and customization support through `esp_audio_dec_register`
- Add audio simple decoder registration and customization support through `esp_audio_simple_dec_register`
- Add audio encoder support for `ALAC`
- Refine memory usage when not enable AAC-Plus support
- Reorganized code layout, separate implementation from common part
- Add default registration for supported audio encoders (controlled by menuconfig) through `esp_audio_enc_register_default`
- Add default registration for supported audio decoders (controlled by menuconfig) through `esp_audio_dec_register_default`
- Add default registration for supported audio simple decoders (controlled by menuconfig) through `esp_audio_simple_dec_register_default`
- Add memory management through `media_lib_sal` and can be traced through [mem_trace](https://github.com/espressif/esp-adf-libs/tree/master/media_lib_sal/mem_trace)

### Bug Fixes

- Fix audio encoder PTS calculation error when run long time


## v1.0.1

### Bug Fixes

- Fix `CMakeLists.txt` hardcode on prebuilt library name
- Refine test code in [README.md](README.md)


## v1.0.0

### Features

- Initial version of `esp-audio-codec`
- Add audio encoder common part
- Add audio encoder implementation for `AAC`, `OPUS`, `ADPCM`, `G711A`, `G711U`, `AMRNB`, `AMRWB`, `PCM`
