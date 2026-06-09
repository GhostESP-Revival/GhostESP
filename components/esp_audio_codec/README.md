# ESP_AUDIO_CODEC

Espressif Audio Codec (ESP_AUDIO_CODEC) is the official audio encoding and decoding processing module developed by Espressif Systems for SoCs. 

The ESP Audio Encoder provides a common encoder interface that allows you to register multiple encoders, such as AAC, AMR-NB, AMR-WB, ADPCM, G711A, G711U, PCM, OPUS, ALAC. User can create one or multiple encoder instances based on the encoder interfaces, these instances can run simultaneous encoding. Meanwhile user can also call specified encoder API directly to have less call depth. 

The ESP Audio Decoder provides a common decoder interface that allows you to register multiple decoders, such as AAC, MP3, AMR-NB, AMR-WB, ADPCM, G711A, G711U, VORBIS, OPUS, ALAC. You can create one or multiple decoder instances using the provided interfaces, enabling simultaneous decoding. Meanwhile user can also call specified decoder API directly to have less call depth. ESP Audio Decoder can only process audio frame data (which means input data is frame boundary).

To simplify the decoding process for parsing and locating audio frames, we utilize the ESP Audio Simple Decoder. This decoder employs a parser to easily and conveniently aggregate and structure audio frames, which are subsequently decoded using the ESP Audio Decoder. Users can input data of varying lengths. Supported audio containers include AAC, MP3, WAV, FLAC, AMRNB, AMRWB, M4A. 

The licenses of the third-party copyrights are recorded in [Copyrights and Licenses](http://docs.espressif.com/projects/esp-adf/en/latest/COPYRIGHT.html).

# Highlights

- **User-Friendly Interface**  
  The ESP Audio Codec module features a user-friendly interface designed for simple usability.
  
- **Lightweight with High Performance**  
  The module is optimized for high performance while maintaining a lightweight footprint and minimal memory usage.
  
- **Dual-Level Decoder API**  
  User can use ESP Audio Decoder when input data already at frame boundary, or use ESP Audio Simple Decoder to process any input length of data. Both decoder provide similar APIs make it easy to transition between them.
  
- **High Customization through Registration**  
  Through registration API, user can add customized decoder, encoder or simple decoder easily, meanwhile user can overwrite the default decoder or encoder without change for application code.
  
# Features

The ESP Audio Codec supports the following features:   

## Encoder   

* Following encoders are supported:
  - AAC
  - AMR-NB and AMR-WB
  - ADPCM
  - G711A and G711U
  - PCM
  - ALAC
  - OPUS
  - LC3
  - SBC
  - G722
* Supports operate all encoder through common API see [esp_audio_enc.h](include/encoder/esp_audio_enc.h)
* Supports customized encoder through `esp_audio_enc_register` or overwrite default encoder
* Supports register all supported encoder through `esp_audio_enc_register_default` and manager it by menuconfig

Details for the supported encoders are as follow:  
**AAC**     
- AAC low complexity profile encode (AAC-LC)
- Encoding sample rates (Hz): 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000    
- Encoding channel num: mono, dual     
- Encoding bits per sample: 16 bits    
- Constant bitrate encoding from 12 Kbps to 160 Kbps    
- Choosing whether to write ADTS header or not   

**AMR**       
- Encoding narrow band (NB) and wide band (WB)   
- AMRNB encoding at the sampling rate of 8 kHz       
- AMRWB encoding at the sampling rate of 16 kHz     
- Encoding channel num: mono    
- Encoding bits per sample: 16 bits    
- AMRNB encoding bitrate (Kbps): 4.75, 5.15, 5.9, 6.7, 7.4, 7.95, 10.2, 12.2    
- AMRWB encoding bitrate (Kbps): 6.6, 8.85, 12.65, 14.25, 15.85, 18.25, 19.85, 23.05, 23.85      
- Discontinuous transmission (DTX)     

**ADPCM**   
- Encoding sample rates (Hz): all    
- Encoding channel num: mono, dual    
- Encoding bits per sample: 16 bits    

**G711**    
- Encoding A-LAW and U-LAW      
- Encoding sample rates (Hz): all    
- Encoding channel num: all    
- Encoding bits per sample: 16 bits    

**OPUS**    
- Encoding sample rates (Hz): 8000, 12000, 16000, 24000, 48000    
- Encoding channel num: mono, dual    
- Encoding bits per sample: 16 bits    
- Constant bitrate encoding from 20Kbps to 510Kbps      
- Encoding frame duration (ms): 2.5, 5, 10, 20, 40, 60, 80, 100, 120       
- Application mode for VoIP and music       
- Encoding complexity adjustment, from 0 to 10      
- Inband forward error correction (FEC)     
- Discontinuous transmission (DTX)
- Variable Bit Rate (VBR)

**ALAC**    
- Encoding sample rates from 1 kHz to 384 kHz     
- Encoding channel num: [1, 8]  
- Encoding bits per sample: 16, 24, 32 bits  

**SBC**    
- Encoding sample rates (Hz): 16000, 32000, 44100, 48000    
- Encoding channel mode: mono, dual, stereo, joint stereo 
- Encoding bits per sample: 16 bits  
- Encoding sbc mode: standard, msbc
- Encoding block length: 4, 8, 12, 16 
- Encoding subbands number: 4, 8
- Encoding allocation method: loudness, SNR
- Encoding bitpool range: 2 to 250

**LC3**    
- Encoding sample rates (Hz): 8000, 16000, 24000, 32000, 44100, 48000    
- Encoding full range of channel
- Encoding bits per sample: 16, 24, 32 bits  
- Encoding frame decimilliseconds: 75, 100 dms
- Encoding nbyte range: 20 to 400
- Support 2-byte length prefix before each encoded frame

**G722**    
- Encoding sample rates (Hz): 8000, 16000    
- Encoding channel num: mono    
- Encoding bits per sample: 16 bits    
- Encoding bitrate (bps): 48000, 56000, 64000
- Support packed mode for 48 kbps and 56 kbps
- Configurable frame duration

## Decoder   

* Following decoders are supported:
  - AAC
  - MP3
  - AMR-NB and AMR-WB
  - ADPCM
  - G711A and G711U
  - OPUS
  - VORBIS
  - FLAC
  - ALAC
  - SBC
  - LC3
  - G722
* Supports operate all decoder through common API see [esp_audio_dec.h](include/encoder/esp_audio_dec.h)
* Supports customized decoder through `esp_audio_dec_register` or overwrite default decoder
* Supports register all supported decoder through `esp_audio_dec_register_default` and manager it by menuconfig

Details for the supported decoders are as follow:  
**AAC**     
- Supports AAC-LC, HE-AAC, HE-AACv2
- Use can control AAC-Plus decoding behavior, to decrease cpu and memory usage if no need AAC-Plus decoding
- Supports sample rates (Hz): 96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000
- Supports channel num: mono, dual only
- Supports bits per sample: 16 bits
- Supports decoding both with and without ADTS header
 
**AMR**       
- AMRNB decoding at the sampling rate of 8 kHz       
- AMRWB decoding at the sampling rate of 16 kHz     
- Decoding channel num: mono    
- Supports bits per sample: 16 bits  

**ADPCM**   
- Decoding sample rates (Hz): all    
- Decoding channel num: mono, dual    
- Decoding bits per sample: 16 bits  
- Supports IMA-ADPCM only

**G711**    
- Decoding A-LAW and U-LAW      
- Decoding sample rates (Hz): all    
- Decoding channel num: all    
- Decoding bits per sample: 16 bits    

**OPUS**    
- Decoding sample rates (Hz): 8000, 12000, 16000, 24000, 48000    
- Decoding channel num: mono, dual    
- Decoding bits per sample: 16 bits      
- Supports decoding self delimited packet also

**ALAC**    
- Decoding sample rates (kHz): [1, 384]   
- Decoding channel num: [1, 8]     
- Decoding bits per sample: 16, 24, 32 bits 
  
**FLAC**  
- Supports sample rates (kHz): [8, 192]   
- Decoding channel num: [1, 8]
- Decoding bits per sample: 16, 24, 32 bits 

**VORBIS**  
- Supports sample rates (Hz): 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000 
- Decoding channel num: mono, dual    
- Decoding bits per sample: 16 bits
- Supports decode VORBIS frame only, need remove OGG header
- User need provide common header information firstly

**SBC**    
- Decoding sample rates (Hz): 16000, 32000, 44100, 48000    
- Decoding channel: mono, dual 
- Decoding bits per sample: 16 bits  
- Decoding sbc mode: standard, msbc
- Packet Loss Concealment (PLC)

**LC3**    
- Decoding sample rates (Hz): 8000, 16000, 24000, 32000, 44100, 48000    
- Decoding full range of channel
- Decoding bits per sample: 16, 24, 32 bits  
- Decoding frame decimilliseconds: 75, 100 dms
- Decoding nbyte range: 20 to 400
- Support decoding of 2-byte length-prefixed frame data
- Packet Loss Concealment (PLC)

**G722**    
- Decoding sample rates (Hz): 8000, 16000    
- Decoding channel num: mono    
- Decoding bits per sample: 16 bits    
- Decoding bitrate (bps): 48000, 56000, 64000
- Support packed mode decoding

## Simple Decoder   

* Supports audio frame finding and decoding
* Supports common parser, user can add customized parser according parser rules
* Supports customized simple decoder to handle new file format
* Supports customized parser and decoder pair: Use default parser but with customized decoder
* Supports streaming decode only not support seek

Details for the supported audio containers are as follow:
| Audio Container| Notes                                                       |
|       --       |  --                                                         |
|       AAC      | Supports AAC-Plus controlled by configuration               |
|       MP3      | Supports layer 1, 2, 3                                      |
|       AMRNB    | Supports files with AMRNB file header only                  |
|       AMRWB    | Supports files with AMRWB file header only                  |
|       FLAC     | Supports files with FLAC file header only                   |
|       ADPCM    | Supports IMA-ADPCM only                                     |
|       WAV      | Supports G711A, G711U, PCM, ADPCM                           |
|       M4A      | Supports MP3, AAC, ALAC <br> Supports MDAT after MOOV only  |
|       TS       | Supports MP3, AAC                                           |
|       G711     | Supports G711A, G711U                                       |
|       SBC      | Supports SBC and MSBC                                       |
|       LC3      | Supports LC3                                                |
|       OPUS     | Supports raw opus only                                      |
|       PCM      | Supports raw pcm only                                       |
|       ALAC     | Supports raw alac only                                      |
|       OGG      | Supports VORBIS, OPUS                                       |
|       VORBIS   | Supports raw vorbis only                                    |

# Performance

The following results are tested and gathered on ESP32-S3R8 platform. The data can be obtained by running `Encode to Decode chain test` in the test code, sample log output is as follow:
```
I (12599) CODEC_TEST: Start to do chain test for AAC sample_rate 48000 channel 2
I (12879) DEC_TEST: Encoder for AAC cpu: 12.91%
I (12880) DEC_TEST: Encoder AAC compress ratio: 5.86% heap usage: 51400
I (12914) DEC_TEST: Decode for AAC cpu: 6.75% heap usage: 51168
```
To get performance under other sample rate, channel or complexity, please change the parameters in the test codes ( mainly located in function `chain_test_thread` and `get_encoder_config` ).

## Encoder
| Encoder Codec | Sample Rate (Hz) | Channel | Memory (KB) | CPU loading (%) |
| --            |  --              | --      | --          | --              |
| AAC           | 48000            | 2       | 51.4        | 12.9            |
| G711-A        | 8000             | 1       | 0.06        | 0.32            |
| G711-U        | 8000             | 1       | 0.06        | 0.33            |
| AMR-NB        | 8000             | 1       | 3.3         | 17.81           |
| AMR-WB        | 16000            | 1       | 5.6         | 37.69           |
| ADPCM         | 48000            | 2       | 0.01        | 2.69            |
| OPUS          | 48000            | 2       | 29.4        | 24.9            |
| SBC           | 48000            | 2       | 1.85        | 9.55            |
| LC3           | 48000            | 2       | 3.67        | 46.57           |
| G722          | 16000            | 1       | 20.8        | 9.74            |

**Notes:**   
Encoder cpu usage is highly dependent on certain encoding settings (like bitrate or complexity)  
 1) For AAC encoder, tested under bitrate 90kbps
 2) For AMR-NB and AMR-WB encoders, test bitrate under ARM-NB: 12.2kbps, ARM-WB: 8.85kbps.
 3) For OPUS encoder test bitrate set as 90kbps, complexity set as 0.
 4) Memory usage only consider heap usage, stack usage is not included, to support all encoders the running on stack size should about 40k.

## Decoder
| Decoder Codec | Sample Rate (Hz) | Channel | Memory (KB) | CPU loading (%) |
| --            |  --              | --      | --          | --              |
| AAC           | 48000            | 2       | 51.2        | 6.75            |
| G711-A        | 8000             | 1       | 0.04        | 0.14            |
| G711-U        | 8000             | 1       | 0.04        | 0.13            |
| AMR-NB        | 8000             | 1       | 1.8         | 4.23            |
| AMR-WB        | 16000            | 1       | 5.4         | 9.5             |
| ADPCM         | 48000            | 2       | 0.11        | 2.43            |
| OPUS          | 48000            | 2       | 26.6        | 5.86            |
| MP3           | 44100            | 2       | 28          | 8.17            |
| FLAC          | 44100            | 2       | 89.4        | 8.0             |
| SBC           | 48000            | 2       | 0.21        | 8.14            |
| LC3           | 48000            | 2       | 1.36        | 17.5            |
| G722          | 16000            | 1       | 0.55        | 9.21            |

**Notes:** 
 1) MP3 and FLAC decoders are tested with real audio data. All other codes are tested with encoded data from sin wav PCM. 
 2) For AAC decoder, tested file is encoded in AAC-LC profile, decoding AAC-Plus profile will have higher memory and CPU usage.
 3) Only the heap usage is considered here. To support all decoders, the task running the decoder should have stack size of about 20K.

#  ESP_AUDIO_CODEC Release and SoC Compatibility

The following table shows the support of ESP_AUDIO_CODEC for Espressif SoCs. The "&#10004;" means supported, and the "&#10006;" means not supported. 

|Version      |  ESP32   |   ESP32-S2 |   ESP32-S3 |   ESP32-C2 |  ESP32-C3 |  ESP32-C5 |  ESP32-C6 |   ESP32-P4  |   ESP32-H4 |  ESP32-S31 |
|:-----------:|:--------:|:----------:|:----------:|:----------:|:---------:|:---------:|:---------:|:-----------:|:----------:|:----------:|
|   v1.0.0    | &#10004; |  &#10004;  |  &#10004;  |  &#10006;  | &#10004;  | &#10006;  | &#10006;  |  &#10006;   |  &#10006;  |  &#10006;  |
|   v2.0.0    | &#10004; |  &#10004;  |  &#10004;  |  &#10006;  | &#10004;  | &#10006;  | &#10004;  |  &#10004;   |  &#10006;  |  &#10006;  |
|   v2.1.0    | &#10004; |  &#10004;  |  &#10004;  |  &#10004;  | &#10004;  | &#10004;  | &#10004;  |  &#10004;   |  &#10006;  |  &#10006;  |
|   v2.2.0    | &#10004; |  &#10004;  |  &#10004;  |  &#10004;  | &#10004;  | &#10004;  | &#10004;  |  &#10004;   |  &#10006;  |  &#10006;  |
|   v2.3.0    | &#10004; |  &#10004;  |  &#10004;  |  &#10004;  | &#10004;  | &#10004;  | &#10004;  |  &#10004;   |  &#10004;  |  &#10006;  |
|   v2.4.0    | &#10004; |  &#10004;  |  &#10004;  |  &#10004;  | &#10004;  | &#10004;  | &#10004;  |  &#10004;   |  &#10004;  |  &#10006;  |
|   v2.5.0    | &#10004; |  &#10004;  |  &#10004;  |  &#10004;  | &#10004;  | &#10004;  | &#10004;  |  &#10004;   |  &#10004;  |  &#10004;  |

# Usage

## Encoder Usage
For sample usage, please refer to [audio_encoder_test.c](test_apps/audio_codec_test/main/audio_encoder_test.c)

## Decoder Usage
For sample usage, please refer to [audio_decoder_test.c](test_apps/audio_codec_test/main/audio_decoder_test.c)

## Simple Decoder Usage
For sample usage, please refer to [simple_decoder_test.c](test_apps/audio_codec_test/main/simple_decoder_test.c)
For customized parser and decoder usage refer to [simple_decoder_cust.c](test_apps/audio_codec_test/main/simple_decoder_cust.c)
