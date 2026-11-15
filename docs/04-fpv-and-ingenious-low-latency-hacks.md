# Técnicas FPV, Bare-Metal e Hacks de Ultra-Baixa Latência para Raspberry Pi 3B (V4L2 + DRM/KMS)

Este documento consolida técnicas não convencionais de redução extrema de latência (*glass-to-glass*) oriundas de sistemas digitais de FPV (OpenHD, EZ-WifiBroadcast, RubyFPV, WFB-ng, PixelPilot), reprodutores bare-metal (`hello_video`, `rpicam-apps`, `rpi-ffmpeg`, `mpv`, Kodi) e clientes de streaming de baixa latência (`moonlight-qt`, RetroArch KMS, Chiaki-ng, Parsec), cruzadas diretamente com o estado atual do código em `moonlight-embedded-rpi3-v4l2`.

> **Convenção de rigor técnico:** afirmações baseadas diretamente em arquivos lidos neste workspace ou em documentação/fontes verificadas são marcadas com referência exata; otimizações dependentes de comportamento específico de firmware ou versão de kernel no Raspberry Pi 3B são marcadas explicitamente como **Hipótese / Experimento a Medir**.

---

## 0. Raio-X Atual do Repositório: Onde a Latência Está Escondida Hoje

Uma auditoria direta dos caminhos quentes em `src/video/`, `libgamestream/` e `third_party/moonlight-common-c/` revela múltiplas fontes concretas de latência evitável, cópias redundantes de memória e contenção de locks:

| Camada | Arquivo / Símbolo Observado | Comportamento Atual | Impacto na Latência (RPi 3B / Cortex-A53) |
| :--- | :--- | :--- | :--- |
| **FEC / Debug** | [`CMakeLists.txt`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/CMakeLists.txt#L65-L67), [`RtpVideoQueue.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/RtpVideoQueue.c#L4-L10) | Builds com `CMAKE_BUILD_TYPE=Debug` definem `LC_DEBUG`, ativando `FEC_VALIDATION_MODE` (`neededPackets += queue->fecPercentage ? 1 : 0`). | **+1 pacote de paridade obrigatório por frame** com FEC, mesmo sem perda de rede (atraso direto na entrega do frame). |
| **Entrada Bitstream (Cópia Dupla)** | [`v4l2_submit_decode_unit()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L232-L259), [`v4l2_codec_feed_packet()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L199-L274) | Copia fragmentos `PLENTRY` para `v4l2_instance.bitstream_buf` (`malloc(256*1024)`), depois executa **segundo `memcpy`** para `ctx->output_bufs[free_idx].start`. | Desperdício de banda de RAM LPDDR2 (~900 MHz) e *cache thrashing* em L1/L2 no Cortex-A53 em I-frames grandes. |
| **Contenção de Mutex** | [`v4l2_submit_decode_unit()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L233-L259), [`v4l2_codec_feed_packet()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L215-L251) | Segura `v4l2_instance.state_mutex` durante o loop de até `4 × poll(&pfd, 1, 3)` (`POLLOUT`). | Bloqueia transições de estado e outras operações sincronizadas por **até 12 ms** por tentativa de envio. |
| **Depacketizer / AUD** | [`VideoDepacketizer.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/VideoDepacketizer.c#L668-L670), [`v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L265-L274) | `isAccessUnitDelimiter` e `isSeiNal` iniciais são descartados via `skipToNextNal`; `VIDIOC_QBUF` envia `buf.flags = 0` por AU sem trailing AUD no `v4l2_submit_decode_unit()`. | Risco de o firmware `video_decode` segurar AU até chegada de NALU seguinte (**~16,67 ms** a 60 fps) caso não haja flush imediato sem delimitador (a validar em teste A/B). |
| **Patch SPS / VUI** | [`gs_sps_fix()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c#L56-L77), [`h264_stream.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/h264bitstream/h264_stream.c#L1213-L1217) | Não força `vui_parameters_present_flag = 1`; só zera `num_reorder_frames` quando `!vui.bitstream_restriction_flag`; nunca configura `low_delay_hrd_flag`. | Se o host omitir VUI ou enviar `bitstream_restriction_flag=1` com `num_reorder_frames > 0`, o patch de baixa latência é **ignorado ou incompleto**. |
| **Profundidade de Filas V4L2** | [`v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L97) | `OUTPUT` solicita `req.count = 12`; `CAPTURE` solicita `min_bufs + 8` (mínimo `12`, linhas 373–375). | Permite acúmulo oculto de frames em trânsito (*buffer bloat*) se a drenagem/consumo atrasar. |
| **Alocação DRM por Frame** | [`commit_atomic_flip()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c#L342-L368) | Chama `drmModeAtomicAlloc()` e `drmModeAtomicFree()` em **todo frame** apresentado. | Alocação/liberação repetitiva de estruturas de propriedades no caminho crítico de flip. |
| **Scheduling RT e Page Faults** | [`v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L178-L179), [`Platform.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/Platform.c#L221) | Apenas `v4l2_event_thread` recebe `SCHED_FIFO` (prio 45); `VideoRecv`, `AudioRecv`, `InputSend`, `ControlRecv` ficam em `SCHED_OTHER`; não há `mlockall()`. | Preempção das threads de rede/input por processos em background e possíveis *major/minor page faults*. |
| **Polling / Syscalls de Rede** | [`PlatformSockets.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/PlatformSockets.c), [`v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L81) | `recvUdpSocket` usa `recvfrom()` (1 syscall/pacote) com `SO_RCVTIMEO = 100 ms`; `v4l2_event_thread` usa `poll(pfd, 2, 50)`. | Overhead de syscalls por segundo em bitrates altos no ARMv8 sem batching (`recvmmsg()`); (`SO_BUSY_POLL` é *no-op* em `eth0` `smsc95xx`). |
| **Slicing Falso (RPi 3B)** | [`decoder_callbacks_v4l2`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L277), [`RtpVideoQueue.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/RtpVideoQueue.c#L193-L251) | Anuncia `CAPABILITY_SLICES_PER_FRAME(4)`, mas `reconstructFrame` aguarda o frame inteiro e `bcm2835-codec` opera por Access Unit. | Sem ganho de pipelining por slice no cliente; gera apenas overhead extra de cabeçalhos de slice e reinicializações de CABAC no VideoCore IV. |

---

## Parte 1 — Hacks de Bitstream H.264 (FPV & Bare-Metal)

### 1.1. O Que `VideoDepacketizer.c` e `libgamestream/sps.c` Fazem Hoje

1. **Remoção de AUDs e SEIs no Depacketizer:**
   Em [`VideoDepacketizer.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/VideoDepacketizer.c#L668-L670) e linhas `987-995`, o depacketizer procura ativamente delimitadores de início de frame (`isAccessUnitDelimiter` e `isSeiNal`) e avança o ponteiro com `skipToNextNal`, descartando AUDs e SEIs antes de entregar a `decodeUnit->bufferList` para `VideoCallbacks.submitDecodeUnit`.
   Em [`v4l2_submit_decode_unit()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L247-L256), quando `entry->bufferType == BUFFER_TYPE_SPS`, é chamado `gs_sps_fix(entry, GS_SPS_BITSTREAM_FIXUP, ...)`, e os demais fragmentos (`PPS`, fatias VCL) são concatenados sem qualquer NALU final delimitadora.

2. **Três lacunas reais em [`gs_sps_fix()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c#L56-L77):**
   Inspecionando `third_party/h264bitstream/h264_stream.c` ([leitura/escrita do SPS](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/h264bitstream/h264_stream.c#L326-L330) e [serialização da VUI](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/h264bitstream/h264_stream.c#L1213-L1217) + [campos HRD/restrições](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/h264bitstream/h264_stream.c#L1307-L1332)), ficam evidentes três problemas:
   - **Lacuna A (`vui_parameters_present_flag`):** `write_seq_parameter_set_rbsp()` só invoca `write_vui_parameters()` se `sps->vui_parameters_present_flag == 1`. Contudo, `gs_sps_fix()` **nunca** força `h264_stream->sps->vui_parameters_present_flag = 1`. Se o host emitir um SPS sem VUI (o que ocorre em certos perfis/encoders), todo o patch em `sps->vui.*` é silenciosamente perdido na re-serialização.
   - **Lacuna B (`num_reorder_frames` condicional):** Em [`sps.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c#L61-L71):
     ```c
     if (!h264_stream->sps->vui.bitstream_restriction_flag) {
       ...
       h264_stream->sps->vui.num_reorder_frames = 0;
     }
     h264_stream->sps->vui.max_dec_frame_buffering = 1;
     ```
     Se o host já enviar `bitstream_restriction_flag = 1` com `num_reorder_frames = 1` ou `2` (padrão conservador de vários encoders), `gs_sps_fix()` **não sobrescreve `num_reorder_frames = 0`**, deixando `num_reorder_frames > max_dec_frame_buffering` (inválido per ITU-T H.264) ou mantendo reordenamento!
   - **Lacuna C (`low_delay_hrd_flag` ausente):** Em [`h264_stream.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/h264bitstream/h264_stream.c#L1307-L1320), `bs_write_u1(b, sps->vui.low_delay_hrd_flag)` só é gravado no bitstream quando `sps->vui.nal_hrd_parameters_present_flag || sps->vui.vcl_hrd_parameters_present_flag` for verdadeiro. `gs_sps_fix()` nunca inspeciona nem define `low_delay_hrd_flag = 1`.

---

### 1.2. Injeção de AUD (`00 00 00 01 09 F0`) e Delimitação de Access Unit (OpenHD / EZ-WifiBroadcast / `hello_video`)

**Por que decoders H.264 seguram 1 frame?**
Em um *byte stream* Annex B padrão, um parser H.264 não sabe que todas as fatias (*slices*) do frame $N$ terminaram até encontrar o *start code* (`00 00 00 01` ou `00 00 01`) de uma NALU pertencente ao frame $N+1$ (por exemplo, novo `first_mb_in_slice == 0` ou mudança de `frame_num`/POC). Sem um delimitador explícito de fim de Access Unit (AU) ao final do buffer entregue, um parser conservador retém o frame $N$ por **1 período de frame completo** (**16,67 ms a 60 fps**, **8,33 ms a 120 fps**).

**Como `hello_video`, `EZ-WifiBroadcast` (`mmormota`), `OpenHD` e `RubyFPV` resolvem isso:**
- **No legado OpenMAX IL / MMAL (`hello_video` original em `raspberrypi/userland`, fork `mmormota` em `EZ-WifiBroadcast`, `OpenHD`):**
  - Alimentam o stream via pipe/`stdin` diretamente na porta de entrada (`Port 130` do `OMX.broadcom.video_decode` ou porta input do `vc.ril.video_decode`), removendo componentes de relógio/agendamento que retêm frames por PTS (usando `OMX_BUFFERFLAG_TIME_UNKNOWN`) e sinalizando explicitamente o fim de cada unidade de acesso com **`OMX_BUFFERFLAG_ENDOFFRAME`** (ou `MMAL_BUFFER_HEADER_FLAG_FRAME_END` em MMAL, como já faz [`src/video/mmal.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c#L268) e [`src/video/pi.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/pi.c#L233)).
  - Em pipelines de streaming em *byte stream* bruto (ou parsers V4L2/FPV que não recebem flags out-of-band por frame), **Access Unit Delimiters (AUD)** com cabeçalho Annex B:
    ```text
    00 00 00 01 09 F0
    ```
    onde:
    - `00 00 00 01` = *start code* de 4 bytes;
    - `09` = `nal_unit_type = 9` (AUD, `nal_ref_idc = 0`, `forbidden_zero_bit = 0`);
    - `F0` (`111 1 0000` em binário) = `primary_pic_type = 7` (`111`, permitindo qualquer tipo de slice I/P/B) + `rbsp_trailing_bits` (`10000`).

**Como o `bcm2835-codec` (`drivers/staging/vc04_services/bcm2835-codec/bcm2835-v4l2-codec.c` / `drivers/media/platform/bcm2835/bcm2835-codec.c`) lida com isso:**
- O driver `bcm2835-codec` é um wrapper **stateful V4L2 M2M** sobre MMAL/VCHIQ no VideoCore IV; em [`v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L265-L274), cada `VIDIOC_QBUF` envia 1 Access Unit completo com `buf.flags = 0` (`MMAL_BUFFER_HEADER_FLAG_EOS` no kernel `device_run()` destina-se apenas ao fluxo de drain via `V4L2_DEC_CMD_STOP`, não devendo ser usado por frame — ver `#14`).
- Adicionalmente, o driver V4L2 não expõe `MMAL_PARAMETER_VIDEO_MAX_NUM_CALLBACKS` para restringir callbacks internos no firmware MMAL (`#14`).
- **Hipótese / Experimento a Medir no RPi 3B:** Deve-se medir com câmera de alta velocidade/fotodiodo se:
  1. Anexar um AUD sintético (`00 00 00 01 09 F0`) ao final da cadeia VCL antes do `VIDIOC_QBUF` força descarga imediata da frame anterior caso o parser do firmware `video_decode` retenha 1 frame aguardando nova NALU; **atenção:** validar se o firmware VideoCore IV trata o `09 F0` pós-slice como terminador do frame atual ou como cabeçalho do *próximo* AU (o que deixaria o novo AU aberto sem efeito adicional, ou geraria erro/buffer espúrio).
  2. Garantir que `[SPS][PPS]` + `[IDR slice]` sejam submetidos **juntos no mesmo buffer V4L2 `OUTPUT`** (comportamento já seguido quando `decodeUnit->bufferList` os encadeia), nunca em `VIDIOC_QBUF` separados.

---

### 1.3. Endurecimento da SPS VUI: `max_dec_frame_buffering` (`0` vs `1`), `num_reorder_frames=0` e `low_delay_hrd_flag`

Segundo a especificação **ITU-T H.264 Anexo E (`E.2.1`)**:
- `max_dec_frame_buffering` define o tamanho mínimo requerido do DPB (*Decoded Picture Buffer*) em unidades de frame. Embora a especificação determine que o tamanho efetivo obedeça $\text{Max}(1, \text{max\_dec\_frame\_buffering})$, ela também impõe a restrição normativa:
  $$\text{max\_dec\_frame\_buffering} \ge \text{max\_num\_ref\_frames}\ (\text{num\_ref\_frames})$$
- **Por que `0` vs `1` importa na prática:**
  - Como streams do Moonlight/Sunshine sem RFI usam `num_ref_frames = 1` ([`SdpGenerator.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/SdpGenerator.c#L422-L478) / [`libgamestream/sps.c:47`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c#L47)), definir `max_dec_frame_buffering = 0` viola formalmente `max_dec_frame_buffering >= num_ref_frames`. Decoders estritos ou o firmware VideoCore IV podem rejeitar o SPS ou interpretar `0` como "não especificado", revertendo para o `MaxDpbFrames` calculado pelo `level_idc` (**4 a 5 frames = 66–83 ms de buffering a 60 fps**).
  - Portanto, para streams I/P com 1 referência, a combinação segura e determinística é **`max_dec_frame_buffering = 1`** + **`num_reorder_frames = 0`** + **`vui_parameters_present_flag = 1`** + **`bitstream_restriction_flag = 1`**. (`0` só deve ser testado em modo puramente I-frame onde `num_ref_frames = 0`.)
- **Sobre `low_delay_hrd_flag` e `timing_info`:**
  - `low_delay_hrd_flag = 1` sinaliza ao modelo HRD que não há atraso de remoção para reordenamento no DPB.
  - Em `h264bitstream`, esse bit só existe no bitstream se `nal_hrd_parameters_present_flag == 1` ou `vcl_hrd_parameters_present_flag == 1`. Se o host já enviar HRD, devemos forçar `low_delay_hrd_flag = 1`; se o host **não** enviar HRD, é possível testar (como experimento) habilitar `vcl_hrd_parameters_present_flag = 1` mínimo com `cpb_cnt_minus1 = 0` e `low_delay_hrd_flag = 1` — validando se o firmware do VideoCore IV responde com menor `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE`.

#### Patch Proposto em C para `libgamestream/sps.c` (Somente Referência — Não Aplicado)

```c
/* Proposta de endurecimento para gs_sps_fix() em libgamestream/sps.c */
if ((flags & GS_SPS_BITSTREAM_FIXUP) == GS_SPS_BITSTREAM_FIXUP) {
  /* Corrige bug crítico: write_seq_parameter_set_rbsp() ignora vui.* se flag == 0 */
  h264_stream->sps->vui_parameters_present_flag = 1;

  /* Sempre força bitstream_restriction_flag = 1 e zera reordenamento,
     mesmo que o host já tenha enviado bitstream_restriction_flag == 1 */
  if (!h264_stream->sps->vui.bitstream_restriction_flag) {
    h264_stream->sps->vui.bitstream_restriction_flag = 1;
    h264_stream->sps->vui.motion_vectors_over_pic_boundaries_flag = 1;
    h264_stream->sps->vui.log2_max_mv_length_horizontal = 16;
    h264_stream->sps->vui.log2_max_mv_length_vertical = 16;
  }

  /* Elimina fila de reordenamento e limita DPB ao mínimo válido para 1 referência */
  h264_stream->sps->vui.num_reorder_frames = 0;
  h264_stream->sps->vui.max_dec_frame_buffering =
      h264_stream->sps->num_ref_frames > 0 ? h264_stream->sps->num_ref_frames : 1;

  h264_stream->sps->vui.max_bytes_per_pic_denom = 2;
  h264_stream->sps->vui.max_bits_per_mb_denom = 1;

  /* low_delay_hrd_flag só é escrito por h264_stream.c se nal_hrd || vcl_hrd == 1 */
  if (h264_stream->sps->vui.nal_hrd_parameters_present_flag ||
      h264_stream->sps->vui.vcl_hrd_parameters_present_flag) {
    h264_stream->sps->vui.low_delay_hrd_flag = 1;
  }
}
```

---

### 1.4. Intra-Refresh (Rolling I-Slices) vs IDRs Gigantes no Sunshine / AMD VAAPI

**Por que FPV usa Intra-Refresh:**
Em links FPV como OpenHD/WFB-ng (`raspivid -if cyclicrows`) e RubyFPV, um **IDR keyframe** completo é 10× a 30× maior que um P-frame. No Raspberry Pi 3B, o chip Ethernet/USB **SMSC LAN9514 (`smsc95xx`) compartilha um único barramento USB 2.0 (`dwc_otg`)**: rajadas repentinas de pacotes UDP + reconstrução FEC de um IDR gigante causam *head-of-line blocking* na rede, jitter na recepção de pacotes e picos no tempo de decode do VideoCore IV. Com **Intra-Refresh** (colunas/linhas I rolantes em frames P consecutivos), a taxa de bits permanece quase constante quadro a quadro.

**A Realidade no Sunshine e no Stack AMD VAAPI (Linux/Mesa):**
- No ecossistema **LizardByte/Sunshine**, suporte a intra-refresh / recuperação avançada tem evolução fragmentada: por exemplo, configurações de intra-refresh foram adicionadas no Sunshine para **NVENC no Windows** (via PR #5091 por `andygrundman`), enquanto no backend **Linux FFmpeg VAAPI (`h264_vaapi` via Mesa)** o suporte completo a rolling intra-refresh no Sunshine encontra barreiras técnicas:
  1. Embora `vainfo -a` possa expor `VAConfigAttribEncIntraRefresh` / `VA_ENC_INTRA_REFRESH_P_FRAME` em GPUs AMD, o wrapper VAAPI do Sunshine e o protocolo Moonlight esperam recuperação rápida via **IDR sob demanda** ou **RFI** (`CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC` / `HEVC`), e estruturas de referência de longo prazo combinadas com intra-refresh no Mesa/libva historicamente provocam instabilidade ou inflação de frames.
  2. **Excursão de bitrate em cenas estáticas no AMD VAAPI:** Um problema documentado do encoder VAAPI da AMD no Linux é disparar a taxa de bits em cenas estáticas/menus (causando perda de pacotes no RPi 3B via USB-Ethernet). Mitigação prática verificada no Sunshine: ativar **`vaapi_strict_rc_buffer = enabled`** no Sunshine para restringir o modelo HRD/VBV do rate control e impedir bursts que excedam a janela USB 2.0 do RPi 3B.
  3. **Limitação real de Slices (Cliente RPi 3B vs Host AMD VAAPI):** Conforme verificado na análise do binário `/usr/bin/sunshine` (`v2026.1018.5342`, `#15` / `docs/03-sunshine-rx580-and-network-pipeline.md`), o Sunshine limita `AVCodecContext::slices` via `VAConfigAttribEncMaxSlices` (evitando falha de perfil na RX 580). O motivo real para reduzir `CAPABILITY_SLICES_PER_FRAME(4)` para `1` está inteiramente no cliente: evitar cabeçalhos extras de slice e reinicializações de contexto CABAC por frame no VideoCore IV, já que o cliente não paraleliza decodificação por slice.

---

## Parte 2 — VBlank Chasing, Clock Locking e Timing Adaptativo de Display

Mesmo com decode instantâneo, um monitor 60 Hz com VSync tradicional introduz um atraso variável de **0 a 16,67 ms** (média de **8,33 ms**) devido ao desalinhamento de fase entre a chegada do frame decodificado e o intervalo *VBlank* do display. Pior: se o host produzir a `60,000 Hz` e o cristal HDMI do RPi 3B operar a `59,940 Hz` (ou vice-versa com drift de ppm), a fase desliza continuamente em padrão dente-de-serra (*beat frequency*) até causar um salto ou frame repetido.

### 2.1. Como Outros Projetos Atacam Esse Problema

| Projeto | Técnica Principal | Mecanismo Técnico Verificado | Custo / Trade-off |
| :--- | :--- | :--- | :--- |
| **Kodi (`popcornmix`)** | **Genlock por PLL / `vc_gencmd` / `VideoReferenceClock`** | Ajusta dinamicamente o **pixel clock (PLL HDMI do VideoCore)** e intervalos de blanking (`vtotal` / `htotal`) para casar a taxa do HDMI exatamente com a fonte (ex.: `23.976024 Hz`), travando `VideoReferenceClock` no VBlank e reamostrando áudio com `libsamplerate`. | Requer controle da PLL ou reprogramação de `drm_mode_crtc` / modelines; em streaming interativo de rede sem buffer grande, não elimina offset de fase inicial sem *phase-locking*. |
| **Moonlight-qt (`DrmRenderer` + `Pacer`)** | **Pacing sincronizado via `clientRefreshRateX100`** | Envia a taxa exata do display ao host em centésimos de Hz (ex.: `5994` vs `6000` via RTSP `x-nv-video[0].clientRefreshRateX100` em [`SdpGenerator.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/SdpGenerator.c)); `Pacer` decide descartar/reter na borda do VBlank e `DrmRenderer` usa KMS direto com `drmModePageFlip` sem compositor. | Ainda limitado ao próximo VBlank caso não haja VRR (HDMI do RPi 3B não suporta VRR/HDMI 2.1). |
| **RetroArch (`drm_ctx.c`)** | **Flip Assíncrono (`DRM_MODE_PAGE_FLIP_ASYNC`)** | Chama `drmModePageFlip()` com `DRM_MODE_PAGE_FLIP_ASYNC \| DRM_MODE_PAGE_FLIP_EVENT`, trocando o ponteiro de framebuffer no controlador imediatamente (*mid-scanout*), reduzindo a espera de VBlank de 0..16,67 ms para **~0 ms**. | **Introduz *screen tearing*** visível quando o flip ocorre durante a varredura ativa; exige suporte do driver DRM. |
| **Parsec** | **`client_vsync=0`** | Desativa completamente a espera por VSync na apresentação, exibindo o último frame decodificado imediatamente. | Mesmo trade-off de *tearing* em troca do piso absoluto de latência. |
| **`mpv`** | **`--video-latency-hacks=yes`, `--untimed`, `--vo=dmabuf-wayland` / `--vo=drm`, `--hwdec=v4l2m2m`** | Remove lookahead de sincronização/A-V e usa temporização imediata baseada no stream para evitar filas de 1–2 frames. | Pode gerar jitter se o fluxo de rede oscilar; `--vo=drm` ainda prefere modo bare-metal sem compositor. |
| **PixelPilot_rk / `rpicam-apps` (`drm_preview.cpp`)** | **Zero-Copy DMA-BUF + Reciclagem Imediata** | Importa `dmabuf fd` uma única vez (`drmPrimeFDToHandle` + `drmModeAddFB2`) com cache de buffers por `fd`, envia ao plano hardware sem compositor e recicla o `fd` anterior logo que o novo entra em scanout. | Exige execução fora de X11/Wayland (TTY/KMS direto). |
| **Chiaki-ng** | **KMSDRM + DPB mínimo** | Bypassa compositores (Mutter/KWin) via KMSDRM/GLES e restringe buffering de rede/decodificador para evitar acúmulo de fila no Remote Play. | Requer suporte KMSDRM do sistema. |

### 2.2. Aplicabilidade Real ao Raspberry Pi 3B (`vc4` DRM/KMS + Moonlight)

1. **VBlank Alignment via `clientRefreshRateX100` + Medição de Fase Host↔Cliente (Sem Tearing):**
   - [`SdpGenerator.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/SdpGenerator.c#L422-L478) suporta o envio de `x-nv-video[0].clientRefreshRateX100`. Garantir que `moonlight-embedded` passe exatamente a taxa medida do modo DRM ativo no RPi 3B (não apenas inteiro `60`, pois muitos painéis 60 Hz rodam a `59.94 Hz` / `5994` ou `60.02 Hz` / `6002` conforme `clock / (htotal * vtotal)`) alinha o cadenciador do Sunshine à taxa real do HDMI do Pi.
   - **Micro-nudge de Fase (*VBlank Chasing* — Hipótese Experimental):** Se o controlador DRM `vc4` do RPi 3B gerar timestamps precisos via `drmWaitVBlank` / evento de `DRM_MODE_PAGE_FLIP_EVENT`, o cliente conhece exatamente a distância temporal $\Delta t_{\text{vblank}}$ entre `DQBUF` do frame decodificado e o próximo VBlank. Em cenários onde `DRM_MODE_PAGE_FLIP_ASYNC` não for desejado (ou for rejeitado pelo driver para planos YUV/NV12), é teoricamente possível aplicar micro-ajustes no `vtotal` (alterando `vfp`/`vbp` ou pixel clock via modo customizado/PLL — técnica análoga ao genlock do Kodi/popcornmix) para deslizar a fase do VBlank do RPi 3B até alinhar o instante de VBlank logo após a chegada típica do frame da rede. *Aviso:* alterar modo CRTC completo via `DRM_MODE_ATOMIC_ALLOW_MODESET` causa *blanking* do HDMI na maioria das TVs; só é utilizável de forma contínua se o driver permitir mudança *seamless* de clock/blanking sem renegotiar link TMDS (precisa ser validado no hardware/kernel alvo do Pi 3B; em `vc4` mainline isso geralmente requer `ALLOW_MODESET`, tornando inviável o ajuste frame-a-frame via atomic modeset padrão).

2. **`DRM_MODE_PAGE_FLIP_ASYNC` no Driver `vc4` e Alternativa *Just-In-Time* (Com vs Sem Tearing):**
   - **Fato Verificado de Kernel Linux DRM:**
     - Antes do **Linux 6.8**, `drmModeAtomicCommit()` **rejeitava** `DRM_MODE_PAGE_FLIP_ASYNC` com `-EINVAL` — apenas a API legada `drmModePageFlip(fd, crtc_id, fb_id, DRM_MODE_PAGE_FLIP_ASYNC | DRM_MODE_PAGE_FLIP_EVENT, data)` suportava flip assíncrono (como faz `gfx/drivers_context/drm_ctx.c` do RetroArch).
     - A partir do **Linux 6.8**, `drmModeAtomicCommit()` passou a aceitar `DRM_MODE_PAGE_FLIP_ASYNC`, **sob regras estritas**: **apenas a propriedade `FB_ID` do plano primário** pode mudar no commit; alterar `CRTC_X/Y/W/H`, `SRC_X/Y/W/H` ou outros planos causa rejeição (`-EINVAL`) pelo `atomic_async_check` do kernel!
     - **Atenção específica ao driver `vc4` com `DRM_FORMAT_NV12`:** conforme reportado pelo agente `@drm-hvs` no board (#7, análise de `vc4_plane.c` / `vc4_kms.c`), embora o `vc4` anuncie `async_page_flip = true`, o caminho assíncrono nativo (`vc4_plane_async_set_fb()` / `vc4_plane_atomic_async_check()`) atualiza apenas o ponteiro do plano 0 (`ptr0_offset[0]` / `drm_fb_dma_get_gem_obj(fb, 0)`) e **rejeita flips assíncronos multi-planares (`DRM_FORMAT_NV12`) na `dlist` com `-EINVAL`** sem patch no kernel.
     - **Implicação prática:** Sem patch no kernel `vc4` (ou conversão de scanout para formato *single-plane*), a alternativa mais eficaz em `vc4` é substituir o *eager commit* imediato pós-VBlank de [`v4l2_drm.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c#L342-L368) por **VBlank Chasing / *Just-In-Time (JIT) Commit*** (~1–1,5 ms antes do VBlank previsto), permitindo que frames mais novos substituam os anteriores no staging antes do fechamento da janela de scanout.

3. **Por Que `CAPABILITY_SLICES_PER_FRAME(4)` Não Entrega Pipelining por Slice no RPi 3B:**
   - [`src/video/v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L277) anuncia `CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLICES_PER_FRAME(4)`.
   - Porém, duas barreiras impedem qualquer ganho real de slice-level pipelining no RPi 3B:
     1. **Lado de rede (`RtpVideoQueue.c`):** [`reconstructFrame()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/RtpVideoQueue.c#L193-L251) bloqueia a submissão (`submitCompletedFrame`) até que **todos os pacotes de dados do frame completo** (ou paridade FEC suficiente) estejam reunidos; nenhuma slice individual é entregue ao `v4l2_submit_decode_unit()` antes do frame terminar de chegar pela rede.
     2. **Lado do decodificador (`bcm2835-codec`):** O VideoCore IV expõe uma interface V4L2 M2M **stateful** baseada em Access Units (`V4L2_PIX_FMT_H264`), não uma Request API stateless por slice. Alimentar slices parciais sem delimitador de Access Unit confunde o firmware MMAL e gera stalls/timeouts; e, no lado do host, o Sunshine (`v2026.1018.5342`) já limita `AVCodecContext::slices` via `VAConfigAttribEncMaxSlices` sem falhar perfil na RX 580 (`#15`). Portanto, o ganho concreto de alterar [`src/video/v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L277) para **`CAPABILITY_SLICES_PER_FRAME(1)`** é eliminar cabeçalhos de slice redundantes e reinicializações de CABAC no front-end do VideoCore IV, mantendo decodificação limpa quadro-a-quadro (`#14`).

---

## Parte 3 — Zero-Copy na Entrada, Contenção de Locks e Eliminação de Buffering Oculto

### 3.1. Zero-Intermediate-Copy Direto no Buffer `mmap` do V4L2 `OUTPUT`

**Problema Observado:**
Atualmente, cada frame decodificado percorre dois buffers em RAM antes de entrar no kernel:
1. `VideoReceiveThreadProc` (`"VideoRecv"`) remonta os fragmentos RTP/FEC em `PLENTRY` (`decodeUnit->bufferList`) e chama diretamente [`v4l2_submit_decode_unit()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L232-L259) (pois `CAPABILITY_DIRECT_SUBMIT` está ativo).
2. `v4l2_submit_decode_unit()` percorre a lista encadeada e copia cada fragmento (`gs_sps_fix` ou `memcpy`) para `v4l2_instance.bitstream_buf` ([`v4l2.c:247-256`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L247-L256)).
3. Em seguida, [`v4l2_codec_feed_packet()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L263) executa **outro `memcpy`** de `v4l2_instance.bitstream_buf` para `ctx->output_bufs[free_idx].start` (buffer de 1 MiB já mapeado em memória via `mmap` do `/dev/video10`, conforme [`v4l2.h:35`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.h#L35)).

**Solução Concreta (Zero-Intermediate-Copy + Desbloqueio de `state_mutex`):**
1. Tornar `ctx->running` e `ctx->streaming` atômicos (`_Atomic bool` em `<stdatomic.h>`, lidos via `atomic_load_explicit(..., memory_order_acquire)`) para que `v4l2_stop()` e o hot path de submissão não sofram *data race* ao consultar estado fora de `state_mutex`.
2. Dividir `v4l2_codec_feed_packet()` em duas primitivas:
   - `v4l2_codec_acquire_output_buf(ctx, &buf_idx, &dst_ptr, &dst_cap)`: drena `VIDIOC_DQBUF` disponíveis e, se precisar aguardar `POLLOUT`, faz `poll()` **sem segurar `state_mutex`** (checando `atomic_load_explicit(&ctx->streaming, memory_order_acquire)` para abortar no stop).
   - Montar a cadeia `PLENTRY` (`gs_sps_fix()` + `memcpy`) **diretamente** dentro de `dst_ptr` (`ctx->output_bufs[buf_idx].start`), eliminando por completo `v4l2_instance.bitstream_buf` e a segunda cópia.
   - `v4l2_codec_queue_output_buf(ctx, buf_idx, bytes_written)`: dispara `VIDIOC_QBUF` com `bytesused = bytes_written`.
3. **Normalização estrita de retorno (`DR_OK` / `DR_NEED_IDR`):** Conforme `Limelight.h` (`DR_OK = 0`, `DR_NEED_IDR = -1`) e `VideoDepacketizer.c` (`#9`), o retorno de `submitDecodeUnit` **nunca** pode propagar `-errno` bruto (como `-EINVAL`/`-EBUSY`), pois valores distintos de `0` e `-1` quebram a máquina de estados (`idrFrameProcessed`). Além disso, em caso de *backlog* temporário ao adquirir buffer `OUTPUT`, deve-se retornar **`DR_OK`** (descarte silencioso alinhado a `src/video/v4l2.c`) para evitar uma **tempestade de IDRs** que agravaria o congestionamento do `bcm2835-codec`.

#### Esboço em C da Remoção da Dupla Cópia e do Lock no `poll()` (Somente Referência)

```c
#include <stdatomic.h>

/* Em v4l2_codec.c: aquisicao do buffer mmap'd sem manter state_mutex no poll()
 * Pré-requisito: ctx->running e ctx->streaming declarados como `_Atomic bool`. */
int v4l2_codec_acquire_output_buf(struct v4l2_ctx *ctx, int *out_idx,
                                  uint8_t **out_ptr, size_t *out_cap) {
  for (int attempt = 0; attempt < 4; attempt++) {
    if (!atomic_load_explicit(&ctx->running, memory_order_acquire) ||
        !atomic_load_explicit(&ctx->streaming, memory_order_acquire))
      return -EPIPE;

    struct v4l2_buffer buf = {0};
    struct v4l2_plane planes[1] = {{0}};
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = 1;

    while (ioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) == 0) {
      if ((int)buf.index < ctx->num_output_bufs)
        ctx->output_bufs[buf.index].queued = false;
      memset(&buf, 0, sizeof(buf));
      memset(planes, 0, sizeof(planes));
      buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
      buf.memory = V4L2_MEMORY_MMAP;
      buf.m.planes = planes;
      buf.length = 1;
    }

    for (int i = 0; i < ctx->num_output_bufs; i++) {
      if (!ctx->output_bufs[i].queued) {
        *out_idx = i;
        *out_ptr = (uint8_t *)ctx->output_bufs[i].start;
        *out_cap = ctx->output_bufs[i].length;
        return 0;
      }
    }

    struct pollfd pfd = { .fd = ctx->v4l2_fd, .events = POLLOUT };
    poll(&pfd, 1, 3); /* fora de state_mutex! */
  }
  return -EBUSY; /* backlog temporário na fila OUTPUT */
}

/* Em v4l2.c: montagem zero-intermediate-copy direto em output_bufs[idx].start */
static int v4l2_submit_decode_unit_zerocopy(PDECODE_UNIT decodeUnit) {
  if (!atomic_load_explicit(&v4l2_instance.streaming, memory_order_acquire) ||
      !atomic_load_explicit(&v4l2_instance.running, memory_order_acquire))
    return DR_OK;

  int buf_idx = -1;
  uint8_t *dst = NULL;
  size_t cap = 0;
  int acq = v4l2_codec_acquire_output_buf(&v4l2_instance, &buf_idx, &dst, &cap);
  if (acq != 0) {
    /* Backlog (-EBUSY) retorna DR_OK de propósito para evitar tempestade de IDRs
     * via VideoDepacketizer.c; apenas desconexão/stop também retorna DR_OK. */
    return DR_OK;
  }

  /* Garante espaço inclusive para expansão do SPS + eventual AUD de 6 bytes */
  if ((size_t)decodeUnit->fullLength + 256 > cap) {
    /* Não enfileira buffer truncado; retorna DR_OK se P-frame descartável ou
     * DR_NEED_IDR se precisar ressincronizar estado do fluxo. */
    return DR_NEED_IDR;
  }

  uint32_t length = 0;
  for (PLENTRY entry = decodeUnit->bufferList; entry != NULL; entry = entry->next) {
    if (entry->bufferType == BUFFER_TYPE_SPS) {
      gs_sps_fix(entry, GS_SPS_BITSTREAM_FIXUP, dst, &length);
    } else {
      memcpy(dst + length, entry->data, entry->length);
      length += entry->length;
    }
  }

  int ret = v4l2_codec_queue_output_buf(&v4l2_instance, buf_idx, length);
  if (ret != 0) {
    /* Normaliza qualquer erro do ioctl (nunca retornar -errno bruto!):
     * se falhou apenas por congestionamento/stop, DR_OK; se corrompeu cadeia, DR_NEED_IDR. */
    return (ret == -EAGAIN || ret == -EBUSY) ? DR_OK : DR_NEED_IDR;
  }
  return DR_OK;
}
```

> **Nota sobre `V4L2_BUF_FLAG_TIMESTAMP_COPY` / `poll(50)` em `v4l2_event_thread`:**
> Em [`v4l2.c:81`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L81), `v4l2_event_thread` usa `poll(pfd, 2, 50)`. Quando a saída é orientada a eventos (`POLLIN | POLLPRI` no `/dev/video10` e `POLLIN` no `/dev/dri/card0`), reduzir o timeout ou utilizar um `eventfd` dedicado para acordar imediatamente a thread no `v4l2_stop()` evita qualquer atraso de encerramento sem gastar CPU em *idle* (pois eventos de captura e *page flip* já acordam `poll()` instantaneamente).

---

### 3.2. Reuso de `drmModeAtomicReqPtr` com `drmModeAtomicGetCursor` / `drmModeAtomicSetCursor`

Em [`src/video/v4l2_drm.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c#L342-L368), `commit_atomic_flip()` aloca (`drmModeAtomicAlloc()`) e libera (`drmModeAtomicFree()`) o objeto de requisição atômica em cada frame.

A `libdrm` disponibiliza **`drmModeAtomicGetCursor()`** e **`drmModeAtomicSetCursor()`** para eliminar alocações dinâmicas no hot path, mas é preciso respeitar o ciclo de vida pós-DRC (*Dynamic Resolution Change*):
1. Em `v4l2_drm_init()`, alocar `atomic_req = drmModeAtomicAlloc()` uma única vez (liberando apenas no cleanup).
2. **Atenção crítica ao DRC (`v4l2_codec_handle_drc()`):** conforme verificado pelo `@verifier` (`#9`), após mudança dinâmica de resolução `v4l2_codec_handle_drc()` chama `v4l2_drm_disable_plane_sync()`, que **zera `FB_ID` e `CRTC_ID` do plano**! Portanto, um commit curto enviando apenas `FB_ID` falhará (ou não exibirá o plano) no primeiro frame pós-init ou pós-DRC.
3. Solução correta: manter um booleano `plane_needs_full_props` (inicializado em `true` no init e resetado para `true` dentro de `v4l2_drm_disable_plane_sync()`):
   - Quando `plane_needs_full_props == true` (1º frame pós-init ou pós-DRC), resetar o cursor para `0` e enviar `CRTC_ID`, propriedades de geometria (`SRC_*`, `CRTC_*`), cor e `FB_ID`, marcando `plane_needs_full_props = false` após o commit bem-sucedido.
   - Nos frames seguintes em regime permanente, chamar `drmModeAtomicSetCursor(atomic_req, 0)` e adicionar **apenas `FB_ID`** (o que também satisfaz a exigência de *async atomic flip* no Linux $\ge$ 6.8). Alternativamente, preencher as propriedades estáticas até `base_cursor = drmModeAtomicGetCursor(atomic_req)` e usar `drmModeAtomicSetCursor(atomic_req, base_cursor)` antes de anexar `FB_ID`, reconstruindo o prefixo sempre após DRC.

```c
/* Padrão ilustrativo de reuso em commit_atomic_flip() (libdrm) */
drmModeAtomicSetCursor(atomic_req, 0); /* descarta entradas anteriores sem malloc/free */
if (plane_needs_full_props) {
  /* Obrigatório no 1º frame após init ou v4l2_drm_disable_plane_sync() (pós-DRC):
   * reenvia CRTC_ID, SRC_X/Y/W/H, CRTC_X/Y/W/H e propriedades de cor. */
  add_static_plane_properties(atomic_req, ctx);
}
drmModeAtomicAddProperty(atomic_req, plane_id, prop_fb_id, fb_id);
int ret = drmModeAtomicCommit(ctx->drm_fd, atomic_req, flags, ctx);
if (ret == 0) {
  plane_needs_full_props = false;
}
```

---

### 3.3. Redução da Profundidade de Filas no `bcm2835-codec`

- **Fila `OUTPUT` ([`v4l2_codec.c:97`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L97)):** atualmente `req.count = 12`. Para um decodificador direto sem B-frames recebendo 1 frame por vez, `4` buffers `OUTPUT` (cada um de 1 MiB, totalizando 4 MiB em vez de 12 MiB em um RPi 3B de apenas 1 GiB de RAM!) são mais que suficientes para cobrir sobreposição de submissão e retorno do MMAL.
- **Fila `CAPTURE` ([`v4l2_codec.c:373-375`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L373-L375)):** atualmente `req_count = min_bufs + 8` com piso `12`. Como `bcm2835-codec` reporta via `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` o mínimo exigido pelo DPB do firmware + pipeline, somar `+ 8` desperdiça CMA (*Contiguous Memory Allocator* — memória preciosa no Pi 3B) e esconde atrasos de drenagem. Em `rpicam-apps`, `jc-kynesim/rpi-ffmpeg` e `PixelPilot`, recomenda-se manter apenas o estritamente necessário para DPB + buffer ativo em scanout + 1 staging (`min_bufs + 2` ou `min_bufs + 3`), monitorando se ocorre *starvation* de capture buffers durante VBlank.

---

### 3.4. Armadilhas Ocultas em `moonlight-common-c` e Tunings de Sistema no RPi 3B

1. **Desativar `LC_DEBUG` / `FEC_VALIDATION_MODE`:**
   Em [`third_party/moonlight-common-c/CMakeLists.txt:65-67`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/CMakeLists.txt#L65-L67), compilar em modo `Debug` injeta `-DLC_DEBUG`, o que ativa `#define FEC_VALIDATION_MODE` em [`RtpVideoQueue.c:4-10`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/RtpVideoQueue.c#L4-L10) e força `neededPackets += queue->fecPercentage ? 1 : 0` ([`RtpVideoQueue.c:200-205`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/RtpVideoQueue.c#L200-L205)). Isso força o receptor a esperar pelo menos **+1 pacote de paridade FEC em todo frame** mesmo com 0% de perda de pacotes! **Regra obrigatória:** compilar sempre com `-DCMAKE_BUILD_TYPE=Release` (ou `RelWithDebInfo`).

2. **Criptografia de Vídeo (`SS_ENC_VIDEO`) no BCM2837:**
   Inspecionando [`src/cpu.c:73-120`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/cpu.c#L73-L120) e [`src/config.c:354-364`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/config.c#L354-L364), o Cortex-A53 do RPi 3B **não possui extensão de hardware ARMv8 AES (`HWCAP_AES` / `HWCAP2_AES`)**, então `has_fast_aes()` retorna `false` e o cliente escolhe `ENCFLG_AUDIO`. Contudo, em [`SdpGenerator.c:284-288`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/SdpGenerator.c#L284-L288), se o servidor (Sunshine) exigir `EncryptionFeaturesRequested & SS_ENC_VIDEO`, `EncryptionFeaturesEnabled |= SS_ENC_VIDEO` ainda será ativado, obrigando [`VideoStream.c:95-98`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/VideoStream.c#L95-L98) a descriptografar cada pacote de vídeo em software na CPU. Em rede local dedicada/isolada, desativar criptografia de stream no Sunshine (ou usar `ENCFLG_NONE` quando aceitável) evita overhead de AES-GCM em CPU.

3. **Hierarquia `SCHED_FIFO` em Todas as Threads Críticas + `mlockall()`:**
   Atualmente, [`PltCreateThread()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/Platform.c#L221) cria `VideoRecv` ([`VideoStream.c:341`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/VideoStream.c#L341)), `InputSend` ([`InputStream.c:665`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/InputStream.c#L665)), `ControlRecv` ([`ControlStream.c:1796`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/ControlStream.c#L1796)) e `AudioRecv` ([`AudioStream.c:449`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/AudioStream.c#L449)) como `SCHED_OTHER`, enquanto somente `v4l2_event_thread` recebe `SCHED_FIFO` prioridade `45` ([`v4l2.c:178-179`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L178-L179)).
   - **Proposta:** Aplicar `mlockall(MCL_CURRENT | MCL_FUTURE)` na inicialização (para travar páginas de código e buffers de rede/bitstream na RAM sem *page faults*) e configurar hierarquia `SCHED_FIFO` (mantendo prioridades abaixo de `50` para não competir com `irq/*` de kernel se RT-patched):
     - `InputSend`: `SCHED_FIFO` prio `48` (latência de joystick/mouse para o host nunca deve ser preempida por decodificação).
     - `VideoRecv`: `SCHED_FIFO` prio `46` (como `CAPABILITY_DIRECT_SUBMIT` está ativo, essa thread também executa `VideoDepacketizer` e alimenta o `ioctl(VIDIOC_QBUF)`!).
     - `v4l2_event_thread`: `SCHED_FIFO` prio `45` (consome `CAPTURE` e envia `drmModeAtomicCommit`).
     - `AudioRecv` / `AudioDec` / `ControlRecv`: `SCHED_FIFO` prio `40–42`.
   - Requer `CAP_SYS_NICE` (e `CAP_IPC_LOCK` ou `ulimit -l unlimited` para `mlockall`), aplicável pontualmente via `pkexec setcap cap_sys_nice,cap_ipc_lock+ep /usr/local/bin/moonlight` sem rodar o cliente inteiro como root.

4. **Syscalls de Socket UDP (`PlatformSockets.c`) e Hardware USB-Ethernet do RPi 3B:**
   - [`PlatformSockets.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/PlatformSockets.c) usa `recvfrom()` com timeout de `100 ms` (`UDP_RECV_POLL_TIMEOUT_MS`). Em rajadas de vídeo, substituir por **`recvmmsg()`** (drenando vários datagramas RTP disponíveis em 1 única syscall) reduz o custo de chamadas de sistema por quadro no Cortex-A53.
   - **Por que `SO_BUSY_POLL` NÃO funciona na `eth0` do RPi 3B:** conforme verificado nos drivers Linux `drivers/net/usb/usbnet.c` + `smsc95xx.c` (`#11`/`#13`/`#15`), o adaptador USB-Ethernet `smsc95xx` entrega pacotes via `usbnet_skb_return()` $\to$ `netif_rx()` **sem suporte NAPI (`napi_id`)**. Logo, `SO_BUSY_POLL` é um *no-op* na `eth0` do RPi 3B e não deve entrar no pipeline de tunings desse hardware.
   - **Kernel Cmdline específico do RPi 3B (`/boot/cmdline.txt` ou `/boot/firmware/cmdline.txt`):**
     - **`smsc95xx.turbo_mode=N`**: no RPi 3B, o controlador Ethernet LAN9514/LAN9512 por padrão agrupa múltiplos pacotes Ethernet recebidos em uma única transferência USB (`turbo_mode=Y`), adicionando buffering/jitter deliberado para maximizar throughput bruto. Desligar com `N` entrega cada pacote UDP assim que chega.
     - **`dwc_otg.fiq_enable=1`** (em kernel `CONFIG_PREEMPT` padrão — **não** usar `PREEMPT_RT`, pois conforme observado por `@drm-hvs` no board `#7`, `PREEMPT_RT` conflita com o handler FIQ do `dwc_otg` e força `fiq_enable=0`, elevando o uso de CPU pelo USB/Ethernet `smsc95xx`): prioriza o processamento de transações USB via FIQ (*Fast Interrupt Request*) de ARM em CPU 0, reduzindo *latency spikes* compartilhados entre Ethernet e dongles/controles USB.
     - Isolamento de cores (`isolcpus=2,3 nohz_full=2,3 rcu_nocbs=2,3 irqaffinity=0`): mantém o FIQ/IRQs USB-Ethernet no core 0 enquanto fixa `VideoRecv` + `v4l2_event_thread` + `InputSend` nos cores isolados 2 e 3.

---

## Parte 4 — Comparação Pura de Latência no RPi 3B: `bcm2835-codec` (V4L2 + `vc4` DRM) vs Legado MMAL (`mmal.c`) vs OpenMAX IL (`pi.c` / `hello_video`)

O próprio repositório contém as implementações legadas em [`src/video/mmal.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c) e [`src/video/pi.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/pi.c), permitindo uma comparação técnica direta com o stack V4L2/DRM atual (`src/video/v4l2*.c`):

| Dimensão Técnica | Legado OpenMAX IL (`src/video/pi.c` / `hello_video`) | Legado MMAL (`src/video/mmal.c`) | Moderno V4L2 + `vc4` DRM (`src/video/v4l2*.c`) |
| :--- | :--- | :--- | :--- |
| **Comunicação com VideoCore IV** | VCHIQ direto via userland Broadcom (`OMX.broadcom.video_decode` + `video_render`). | VCHIQ direto via MMAL (`vc.ril.video_decode` + `vc.ril.video_render`). | Kernel driver `bcm2835-codec` traduzindo V4L2 `videobuf2` $\leftrightarrow$ MMAL $\leftrightarrow$ VCHIQ. |
| **Sinalização de Fim de Frame na Entrada** | `OMX_BUFFERFLAG_ENDOFFRAME \| OMX_BUFFERFLAG_EOS` ([`pi.c:233`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/pi.c#L233)). | `MMAL_BUFFER_HEADER_FLAG_FRAME_START` / `MMAL_BUFFER_HEADER_FLAG_FRAME_END` ([`mmal.c:251,268`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c#L251-L268)). | `v4l2_codec.c:265-274` envia `buf.flags = 0` via `VIDIOC_QBUF` (`V4L2_PIX_FMT_H264` stateful = 1 AU/buffer); `MMAL_BUFFER_HEADER_FLAG_EOS` no driver ocorre só em drain (`V4L2_DEC_CMD_STOP`, `#14`). |
| **Caminho Decoder $\to$ Display** | Túnel interno na GPU / clock com `OMX_IndexConfigLatencyTarget` (`nTarget=4000`, `pi.c:118-134`) para `DispmanX`. | `MMAL_ENCODING_OPAQUE` ([`mmal.c:119,140`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c#L119-L140)), `output[0]->buffer_num = 3` ([`mmal.c:125`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c#L125)), envio direto ao `renderer->input[0]` (`mmal.c:74`) via DispmanX. | Zero-copy de pixels via **DMA-BUF** (`VIDIOC_EXPBUF` $\to$ `drmPrimeFDToHandle` $\to$ `drmModeAddFB2` NV12 $\to$ `drmModeAtomicCommit` no HVS/CRTC do `vc4`). |
| **Overhead de Kernel / Scheduling** | Muito baixo (callback direto VCHIQ $\to$ firmware GPU; DispmanX fora do KMS Linux). | Muito baixo (callback direto, opcionalmente `MMAL_PARAMETER_VIDEO_MAX_NUM_CALLBACKS` para limitar acúmulo no firmware). | Maior: múltiplas trocas de contexto `ioctl(QBUF/DQBUF)` $\to$ worker V4L2 M2M $\to$ VCHIQ $\to$ `v4l2_event_thread` $\to$ DRM atomic commit + fila VBlank. |
| **Compatibilidade 64-bit / Kernel Atual** | Depreciado e quebrado em userland `aarch64` / kernels modernos `vc4-kms-v3d`. | Depreciado e restrito a userland 32-bit legacy (incompatível com `vc4-kms-v3d` padrão atual). | **Único caminho suportado em 64-bit (`aarch64`) e Raspberry Pi OS / Arch ARM modernos com `vc4-kms-v3d`.** |
| **Referência Empírica FPV (câmera + RF + display Pi 3B)** | ~70–110 ms *glass-to-glass* típicos em sistemas OpenHD / EZ-WifiBroadcast com `hello_video`. | Equivalente ao OpenMAX quando tunado com `OPAQUE` + `buffer_num` mínimo. | ~130–180+ ms em stacks genéricos (GStreamer/FFmpeg V4L2 sem tuning), reduzível drasticamente ao eliminar buffering oculto. |

### Onde `bcm2835-codec` + `vc4` Pode Empatar — e Onde Ainda Perde para MMAL / `hello_video`

1. **Onde o V4L2 + DRM empata com MMAL (após correções):**
   - **Zero-copy na saída:** já atingido por [`v4l2_drm.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c) via DMA-BUF (`NV12` direto para o plano do HVS/VC4), sem cópia em CPU.
   - **Zero-copy na entrada:** atingível ao eliminar `bitstream_buf` e montar diretamente em `output_bufs[idx].start`.
   - **DPB mínimo no bitstream:** corrigindo `gs_sps_fix()` (`vui_parameters_present_flag = 1`, `num_reorder_frames = 0` incondicional, `max_dec_frame_buffering = 1`).
2. **Lacunas estruturais que permanecem no `bcm2835-codec` vs MMAL/`hello_video`:**
   - **Tradução V4L2 M2M $\to$ VCHIQ:** Mesmo otimizado, o `bcm2835-codec` continua sendo uma ponte no kernel sobre o mesmo protocolo MMAL/VCHIQ (adicionando 2–3 transições kernel/user-space por frame: `QBUF(OUT)`, `DQBUF(CAP)` no thread de eventos, `drmModeAtomicCommit`), custando algo na ordem de frações de ms a ~1–2 ms em ARM Cortex-A53 sob carga.
   - **Ausência de `OMX_IndexConfigLatencyTarget` no HVS/DRM:** No legado `pi.c` / `hello_video`, o DispmanX e o componente de clock ajustavam suavemente a latência do pipeline diretamente no firmware. No `vc4` DRM/KMS, a apresentação fica sujeita ao VBlank discreto (0..16,67 ms a 60 Hz), a menos que se aceite *tearing* (`DRM_MODE_PAGE_FLIP_ASYNC`) ou se sincronize o host (`clientRefreshRateX100`) mantendo a fase estável.
   - **Limitação Stateful por Access Unit:** A ausência de um decodificador H.264 stateless no VideoCore IV impede decodificação sub-frame (slice-a-slice antes do fim do frame).

---

## 5. Checklist Prioritário: Fatos Prontos vs Hipóteses a Validar

### Fase A — Vitórias Imediatas e Seguras (Fatos Verificados no Código)
1. [ ] Garantir build em **`Release` / `RelWithDebInfo`** (nunca `Debug`) para não ativar `LC_DEBUG` / `FEC_VALIDATION_MODE` em `RtpVideoQueue.c`.
2. [ ] Corrigir [`gs_sps_fix()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c#L56-L77): forçar `vui_parameters_present_flag = 1`, forçar `num_reorder_frames = 0` fora do `if (!bitstream_restriction_flag)` e aplicar `low_delay_hrd_flag = 1` quando HRD presente.
3. [ ] Eliminar a dupla cópia e o bloqueio de `state_mutex` no `poll()` em [`v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L232-L259) e [`v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L199-L274), escrevendo diretamente no `mmap` de `output_bufs[idx].start`.
4. [ ] Pré-alocar `drmModeAtomicReqPtr` em `v4l2_drm_init()` e reutilizar com `drmModeAtomicSetCursor(..., 0)` em [`commit_atomic_flip()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c#L342-L368).
5. [ ] Reduzir `OUTPUT` `req.count` (`12` $\to$ `4`) e `CAPTURE` cushion (`min_bufs + 8` $\to$ `min_bufs + 2..3`).
6. [ ] Estender `SCHED_FIFO` para `InputSend`, `VideoRecv`, `AudioRecv` e `ControlRecv`, adicionar `mlockall(MCL_CURRENT | MCL_FUTURE)` (com `CAP_SYS_NICE` + `CAP_IPC_LOCK`) e configurar `smsc95xx.turbo_mode=N` + `dwc_otg.fiq_enable=1` no Pi 3B.
7. [ ] Configurar `vaapi_strict_rc_buffer = enabled` no Sunshine e alterar `CAPABILITY_SLICES_PER_FRAME(4)` $\to$ `CAPABILITY_SLICES_PER_FRAME(1)` em [`v4l2.c:277`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L277) para eliminar overhead de cabeçalhos de slice e resets CABAC no VideoCore IV; evitar `SS_ENC_VIDEO` no Cortex-A53.

### Fase B — Experimentos Radicais (Hipóteses a Medir no Hardware RPi 3B)
1. [ ] **AUD Flush (`00 00 00 01 09 F0`):** Medir *input-to-photon* com e sem trailing AUD após o último fragmento de cada frame no `/dev/video10` (`bcm2835-codec`) para confirmar se o firmware `video_decode` ancora algum frame por 16,67 ms ou já descarrega imediatamente.
2. [ ] **HRD Sintético (`low_delay_hrd_flag = 1`):** Testar se sintetizar parâmetros HRD mínimos quando ausentes reduz o `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` reportado pelo VideoCore IV.
3. [ ] **JIT VBlank Chasing vs `DRM_MODE_PAGE_FLIP_ASYNC`:** Implementar e medir *JIT atomic commit* (~1–1,5 ms antes do VBlank) para eliminar retenção em `staged_frame_idx` sem *tearing*; validar se *async flip* em `vc4` exige patch multi-planar NV12 no kernel (`#7`) ou conversão de formato de scanout.
4. [ ] **`recvmmsg()` no `VideoRecv`:** Medir redução de CPU e jitter em 720p60/1080p60 no Cortex-A53 drenando múltiplos pacotes RTP por syscall (não usar `SO_BUSY_POLL`, pois é *no-op* em `smsc95xx`).

---

## 6. Referências Verificadas

### Arquivos Analisados no Repositório
- [`src/video/v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c) e [`src/video/v4l2.h`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.h)
- [`src/video/v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c) e [`src/video/v4l2_drm.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c)
- [`src/video/mmal.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c) e [`src/video/pi.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/pi.c)
- [`src/video/video.h`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/video.h), [`src/config.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/config.c) e [`src/cpu.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/cpu.c)
- [`libgamestream/sps.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c) e [`third_party/h264bitstream/h264_stream.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/h264bitstream/h264_stream.c)
- [`third_party/moonlight-common-c/CMakeLists.txt`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/CMakeLists.txt)
- [`third_party/moonlight-common-c/src/VideoDepacketizer.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/VideoDepacketizer.c), [`RtpVideoQueue.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/RtpVideoQueue.c), [`VideoStream.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/VideoStream.c), [`InputStream.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/InputStream.c), [`ControlStream.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/ControlStream.c), [`SdpGenerator.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/SdpGenerator.c), [`Platform.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/Platform.c) e [`PlatformSockets.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/PlatformSockets.c)

### Projetos Externos e Repositórios Verificados
- Raspberry Pi Kernel (`bcm2835-codec` / `vc4`): https://github.com/raspberrypi/linux
- Raspberry Pi Userland (`hello_video` / OpenMAX / MMAL): https://github.com/raspberrypi/userland e https://github.com/adafruit/pi_hello_video
- Raspberry Pi `rpicam-apps` (`drm_preview.cpp`): https://github.com/raspberrypi/rpicam-apps
- `rpi-ffmpeg` (`jc-kynesim`): https://github.com/jc-kynesim/rpi-ffmpeg
- EZ-WifiBroadcast (`Befinitiv` / `mmormota`): https://github.com/Befinitiv/EZ-WifiBroadcast
- OpenHD: https://github.com/OpenHD , https://github.com/OpenHD/QOpenHD e https://github.com/MarioFPVdev/Open.HD
- RubyFPV: https://github.com/RubyFPV/RubyFPV
- WFB-ng e OSD: https://github.com/svpcom/wfb-ng e https://github.com/svpcom/osd
- PixelPilot (`OpenIPC`): https://github.com/OpenIPC/PixelPilot_rk e https://github.com/OpenIPC/PixelPilot
- RetroArch (`gfx/drivers_context/drm_ctx.c`): https://github.com/libretro/RetroArch/blob/master/gfx/drivers_context/drm_ctx.c
- `mpv` (`--video-latency-hacks`): https://github.com/mpv-player/mpv
- `chiaki-ng`: https://github.com/chiaki-ng/chiaki-ng
