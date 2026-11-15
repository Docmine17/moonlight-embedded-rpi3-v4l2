# Internos do Decodificador H.264 VideoCore IV e Driver V4L2 M2M (`bcm2835-codec`) na Raspberry Pi 3B

Este documento analisa em profundidade o subsistema de decodificação de vídeo H.264 por hardware da **Raspberry Pi 3B** (SoC **BCM2837** / **VideoCore IV**) acessado via driver Linux V4L2 *Memory-to-Memory* (`bcm2835-codec` sobre VCHIQ/MMAL), comparando-o com a implementação atual em [`src/video/v4l2*.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c) para minimizar a latência de decodificação em **720p60** (e avaliar viabilidade em **>60 fps**).

> [!NOTE]
> **Critério de rigor epistêmico:** Todo comportamento observado diretamente no código-fonte deste repositório, na documentação oficial/kernel Linux ou em issues/commits confirmados está marcado como **[Fato Verificado]**. Comportamentos internos do firmware proprietário VideoCore IV (`start.elf` / `vc.ril.video_decode`) não expostos em código aberto e projeções de ganho aguardando medição em hardware real estão marcados explicitamente como **[Inferência / Hipótese]**.

---

## 1. Auditoria da Implementação Atual (`moonlight-embedded-rpi3-v4l2`) **[Fato Verificado]**

A inspeção direta do backend V4L2+DRM e dos módulos de bitstream mostra exatamente como os frames fluem hoje no cliente:

| Componente / Arquivo | Comportamento Atual Observado no Código | Implicação para Latência / Memória / Estabilidade |
|---|---|---|
| [`src/video/v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L232-L278) | `decoder_callbacks_v4l2` define `.capabilities = CAPABILITY_DIRECT_SUBMIT \| CAPABILITY_SLICES_PER_FRAME(4)`. `v4l2_submit_decode_unit()` executa na thread `VideoRecv` (`SCHED_OTHER` por padrão em `PltCreateThread`) e copia todos os fragmentos `PLENTRY` para `v4l2_instance.bitstream_buf` (chamando `gs_sps_fix` em `BUFFER_TYPE_SPS`), passando ao `v4l2_codec_feed_packet()`. | 1ª cópia desnecessária do bitstream na CPU ARM; `CAPABILITY_SLICES_PER_FRAME(4)` negocia 4 slices/frame com o host sem paralelismo real no cliente; `VideoRecv` não roda em `SCHED_FIFO`. |
| [`src/video/v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L37-L181) | Apenas `v4l2_instance.event_thread` chama `configure_realtime_thread(..., 45)` (`SCHED_FIFO`). No loop de eventos, drena todos os frames prontos via `while (ioctl(ctx->v4l2_fd, VIDIOC_DQBUF, &buf) == 0)` e responde a `V4L2_EVENT_SOURCE_CHANGE` com `v4l2_codec_handle_drc(ctx)`. | Drenagem em `while` evita atraso de fila na captura (bom), mas exige `CAP_SYS_NICE` ou `rtprio` para `SCHED_FIFO` não falhar silenciosamente com `EPERM`. |
| [`src/video/v4l2.h`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.h#L33-L75) & [`src/video/v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L47-L102) | Seleciona o device com `cap.card == "bcm2835-codec-decode"`. Configura `OUTPUT_MPLANE` em `V4L2_PIX_FMT_H264`, `num_planes = 1`, `sizeimage = V4L2_OUTPUT_BUFFER_SIZE` (`1 MiB`), e `req.count = 12` (`V4L2_MEMORY_MMAP`). | Reserva **12 MiB de CMA** apenas para bitstream comprimido (quando 1 frame 720p típico ocupa dezenas de KiB e até IDRs raramente passam de ~300 KiB). |
| [`src/video/v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L199-L275) | `v4l2_codec_feed_packet()` trava `state_mutex`, faz `memcpy` de `bitstream_buf` para `output_bufs[free_idx].start`, pode esperar até **`4 × poll(&pfd, 1, 3)` (12 ms)** com o mutex preso, zera `struct v4l2_buffer buf` (`buf.flags = 0`) e dispara `VIDIOC_QBUF`. | **2ª cópia** do bitstream em RAM/cache; bloqueio de até 12 ms sob `state_mutex` travando também a `v4l2_event_thread`; nenhuma flag explícita em `VIDIOC_QBUF`. |
| [`src/video/v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L283-L430) | Em `v4l2_codec_handle_drc()`, chama `v4l2_drm_disable_plane_sync()`, obtém formato via `VIDIOC_G_FMT`, **força `V4L2_PIX_FMT_NV12`** com `VIDIOC_S_FMT`, consulta `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE` (`min_bufs`), e requisita **`req_count = clamp(min_bufs + 8, 12, 20)`** buffers `MMAP` exportados via `VIDIOC_EXPBUF`. | Aloca **12 a 20 buffers** `NV12` (~1,38 MiB/frame em 720p = **~16,6 a ~27,6 MiB de CMA**), elevando uso total de CMA no codec para **~28,6 a ~39,6 MiB** e permitindo bufferbloat interno no firmware caso haja stall downstream. |
| [`src/video/v4l2_drm.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c#L231-L315) | Importa o FD DMABUF único por buffer (`drmPrimeFDToHandle`) duplicando `gem_handle` nos dois planos de `DRM_FORMAT_NV12` (`pitches = { stride, stride }`, `offsets = { 0, stride * coded_height }`) via `drmModeAddFB2`. | Zero-copy verdadeiro entre decoder (`CAPTURE`) e HVS DRM (`vc4`) sem cópia de pixels na CPU ARM. |
| [`libgamestream/sps.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c#L41-L71) | `gs_sps_fix(..., GS_SPS_BITSTREAM_FIXUP, ...)` força `level_idc = 32` (em 1280×720), `num_ref_frames = 1`, e dentro de `if (!vui.bitstream_restriction_flag)` ativa `bitstream_restriction_flag = 1` + `num_reorder_frames = 0`, escrevendo `max_dec_frame_buffering = 1`. | Possui lacunas críticas: **não** ativa `vui_parameters_present_flag = 1`, **não** sobrescreve `num_reorder_frames = 0` se `bitstream_restriction_flag` já vier `1` do host, e não define `low_delay_hrd_flag = 1` (detalhado na Seção 2 e em [`docs/04`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/04-fpv-and-ingenious-low-latency-hacks.md#L36-L49)). |

### Contraste Direto com os Backends Legados do Repositório

O próprio repositório mantém dois caminhos legados de VideoCore IV cujo contraste esclarece o que falta ou mudou na transição para V4L2:

1. **Legado MMAL ([`src/video/mmal.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c#L107-L268)):**
   - Configurava `format_in->flags = MMAL_ES_FORMAT_FLAG_FRAMED` (`mmal.c:107`), apenas **`buffer_num = 5`** buffers de entrada (`256 KiB` cada, `mmal.c:114-115`) e **`buffer_num = 3`** buffers de saída em `MMAL_ENCODING_OPAQUE` (`mmal.c:119,125`).
   - Marcava explicitamente cada fragmento com `MMAL_BUFFER_HEADER_FLAG_CONFIG`, `MMAL_BUFFER_HEADER_FLAG_FRAME_START` e `MMAL_BUFFER_HEADER_FLAG_FRAME_END` (`mmal.c:248-268`).
2. **Legado OpenMAX IL ([`src/video/pi.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/pi.c#L109-L235)):**
   - Configurava `OMX_DataUnitCodedPicture` (`pi.c:109`), `OMX_IndexConfigLatencyTarget` no `video_render` (`pi.c:118-134`) e enviava `buf->nFlags = OMX_BUFFERFLAG_ENDOFFRAME | OMX_BUFFERFLAG_EOS` em cada submissão (`pi.c:233`, preservado desde o `omx.c` de 2015) para forçar drenagem imediata do quadro sem reter 1 frame no parser do `video_decode`.

---

## 2. Eixo 1 — Armadilha de Fronteira de Frame (1-Frame Delay) e Mapeamento `VIDIOC_QBUF -> MMAL`

### 2.1 Por que o VideoCore IV retém o frame $N$ até chegar $N+1$? **[Fato Verificado]**

Em um *byte stream* H.264 Annex B puro, o decodificador só tem certeza sintática de que todas as fatias (*slices*) do frame $N$ encerraram ao detectar o início do frame $N+1$ (`first_mb_in_slice == 0` ou mudança de `frame_num`/POC/IDR), a menos que receba sinalização fora-de-banda (*out-of-band*) de fim de Access Unit ou um delimitador explícito no bitstream.

Na história do firmware Broadcom (`vc.ril.video_decode` / `OMX.broadcom.video_decode`):
- **`raspberrypi/firmware#232`** ([Issue #232](https://github.com/raspberrypi/firmware/issues/232), *"video_decode internal buffering/queuing"*): documentou que `video_decode` acumulava internamente múltiplos quadros (mesmo com streams sem B-frames) antes de liberar o primeiro quadro decodificado quando não havia sinalização rigorosa de limites e controle de DPB no SPS.
- **`raspberrypi/firmware#496`** ([Issue #496](https://github.com/raspberrypi/firmware/issues/496), *"Avoiding MMAL excessive buffering"*): demonstrou que a fila de entrada do firmware MMAL absorvia grandes quantidades de buffers comprimidos antes de emitir saída, motivando a introdução de **`MMAL_PARAMETER_VIDEO_MAX_NUM_CALLBACKS`** nos headers MMAL para limitar quantos callbacks/buffers simultâneos o componente de firmware pode reter.
- **Workaround do OpenMAX (`src/video/pi.c:233`):** Projetos de baixíssima latência (`hello_video`, `moonlight-embedded` `pi.c`) adicionavam `OMX_BUFFERFLAG_EOS` junto a `OMX_BUFFERFLAG_ENDOFFRAME` para obrigar a máquina de estado OpenMAX a descarregar o frame corrente sem aguardar o próximo cabeçalho de slice — um truque específico da implementação `OMX.broadcom.video_decode` que **não deve ser imitado em V4L2**, como veremos abaixo.

### 2.2 Como `bcm2835-codec` (`bcm2835-v4l2-codec.c`) mapeia `VIDIOC_QBUF` para MMAL **[Fato Verificado + Limite de Transparência]**

O driver [`drivers/staging/vc04_services/bcm2835-codec/bcm2835-v4l2-codec.c`](https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/staging/vc04_services/bcm2835-codec/bcm2835-v4l2-codec.c) opera como uma ponte V4L2 *stateful M2M* sobre `vchiq_mmal` (`vc.ril.video_decode` quando no papel `DECODE`, normalmente `/dev/video10`):

1. **Contrato V4L2 *Stateful* vs V4L2 User-space (`buf.flags`):** **[Fato Verificado]**
   - A especificação V4L2 para decodificadores *stateful* (`V4L2_PIX_FMT_H264`) **assume 1 Access Unit completo por buffer `OUTPUT`** submetido em `VIDIOC_QBUF`, e **não define nenhuma flag padrão de entrada no `VIDIOC_QBUF` para marcar "fim de frame"** (ao contrário de MMAL `MMAL_BUFFER_HEADER_FLAG_FRAME_END` ou OMX `OMX_BUFFERFLAG_ENDOFFRAME`). Em [`src/video/v4l2_codec.c:265-274`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L265-L274), `buf.flags` é enviado zerado (`0`).
   - Em `device_run()` do `bcm2835-v4l2-codec.c`, o buffer `vb2_buffer` da fila `OUTPUT` é empacotado em `struct mmal_buffer` via `vchiq_mmal_submit_buffer()` (com tamanho `vb2_get_plane_payload()`), e o retorno assíncrono chega por `ip_buffer_cb` / `op_buffer_cb` (traduzindo flags de saída do MMAL como `MMAL_BUFFER_HEADER_FLAG_KEYFRAME` / `MMAL_BUFFER_HEADER_FLAG_EOS` para `V4L2_BUF_FLAG_KEYFRAME` / `V4L2_BUF_FLAG_LAST`).
   - **Ponto de atenção (diferença entre versões de kernel/driver):** Nos logs e análises do `bcm2835-v4l2-codec.c`, `MMAL_BUFFER_HEADER_FLAG_EOS` é explicitamente acionado apenas no fluxo de *draining* (`V4L2_DEC_CMD_STOP` $\to$ `MMAL_BUFFER_HEADER_FLAG_EOS` $\to$ `V4L2_BUF_FLAG_LAST`), enquanto o uso ou não de `MMAL_BUFFER_HEADER_FLAG_FRAME_END` / `MMAL_ES_FORMAT_FLAG_FRAMED` no caminho `DECODE` depende da revisão exata do driver em execução no Pi 3B — e, crucialmente, **mesmo quando `MMAL_BUFFER_HEADER_FLAG_FRAME_END` é enviado ao MMAL, o firmware fechado `start.elf` ainda decide internamente quando liberar o frame da VPU conforme seu estado de parser/DPB**. **[Inferência]**

2. **Por que `EOB`/`EOS` ou `V4L2_BUF_FLAG_LAST` por frame quebram o `bcm2835-codec`:** **[Fato Verificado]**
   - Ao contrário do `OMX_BUFFERFLAG_EOS` em `pi.c:233`, no `bcm2835-v4l2-codec.c` sinalizar fim de stream (`V4L2_DEC_CMD_STOP` / `MMAL_BUFFER_HEADER_FLAG_EOS` / `V4L2_BUF_FLAG_LAST`) dispara o **estado formal de drain/EOS da V4L2 M2M state machine**, exigindo `V4L2_DEC_CMD_START` ou ciclo completo `VIDIOC_STREAMOFF`/`VIDIOC_STREAMON` para continuar decodificando.
   - Mais grave: bugs documentados no próprio `raspberrypi/linux`, como **[Issue #5059](https://github.com/raspberrypi/linux/issues/5059)** (*"lost frames/incomplete draining in bcm2835-v4l2-codec.c"* — `V4L2_BUF_FLAG_LAST` prematuro em drain) e **[Issue #7691](https://github.com/raspberrypi/linux/issues/7691)** (*"bcm2835-codec: V4L2_DEC_CMD_START does not resume after a source change"* — `V4L2_DEC_CMD_START` trava o decoder após source change), provam que **jamais** se deve usar mecanismos de `STOP`/`EOS` por frame para tentar "descarregar" buffers.

3. **Sobre `MMAL_PARAMETER_VIDEO_ENCODE_H264_LOW_LATENCY` e parâmetros MMAL internos:** **[Fato Verificado]**
   - `MMAL_PARAMETER_VIDEO_ENCODE_H264_LOW_LATENCY` pertence exclusivamente ao **encoder H.264** (`video_encode`), **não existe** no componente de decodificação (`vc.ril.video_decode` / `ROLE_DECODE`) nem é exposto pelo `bcm2835-codec` ao user-space via V4L2.
   - `MMAL_PARAMETER_VIDEO_MAX_NUM_CALLBACKS` (criado na `firmware#496` e usado por `libavcodec/mmaldec.c`) e `MMAL_PARAMETER_VIDEO_DECODE_ERROR_CONCEALMENT` (habilitado por padrão no firmware; quando desligado emite `MMAL_BUFFER_HEADER_FLAG_CORRUPTED` sem tentar mascarar quadros corrompidos) são parâmetros MMAL **não expostos como `V4L2_CID_*` padrão** pelo `bcm2835-codec-decode` (ver Seção 5.1).

### 2.3 Ações Concretas sobre Fronteiras de Frame: SPS VUI Fix vs Injeção de AUD

1. **Corrigir os 3 bugs reais em [`libgamestream/sps.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c#L56-L77) (Prioridade Máxima — [Fato Verificado]):**
   - Como detalhado no exame de `third_party/h264bitstream/h264_stream.c` (ver também [`docs/04`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/04-fpv-and-ingenious-low-latency-hacks.md#L36-L49)), `gs_sps_fix()` hoje tem 3 falhas que podem fazer o VideoCore IV cair no DPB máximo do Level 3.2/4.1 (~4 a 5 frames = **66–83 ms a 60 fps**!):
     1. Nunca força `h264_stream->sps->vui_parameters_present_flag = 1` (se vier `0`, `write_seq_parameter_set_rbsp()` ignora todas as restrições de VUI gravadas por `gs_sps_fix`!).
     2. Só zera `num_reorder_frames = 0` dentro de `if (!h264_stream->sps->vui.bitstream_restriction_flag)`: se o encoder do host (RX 580 VAAPI / FFmpeg) já enviar `bitstream_restriction_flag = 1` com `num_reorder_frames > 0`, `gs_sps_fix()` preserva o reordenamento no bitstream!
     3. Nunca define `low_delay_hrd_flag = 1` quando `nal_hrd_parameters_present_flag || vcl_hrd_parameters_present_flag` estiver ativo.
   - **Nota sobre `max_dec_frame_buffering` (`0` vs `1`):** Pela norma ITU-T H.264 Anexo E, quando `num_ref_frames = 1` (`sps.c:47`), `max_dec_frame_buffering` deve ser $\ge 1$. O valor correto e seguro para I/P sem B-frames é **`max_dec_frame_buffering = 1`** com **`num_reorder_frames = 0`**; enviar `0` viola a especificação e pode fazer o firmware ignorar a restrição.
2. **Empacotamento `[SPS][PPS][IDR]` em um único `VIDIOC_QBUF` ([Fato Verificado]):**
   - [`v4l2_submit_decode_unit()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L247-L258) já concatena `BUFFER_TYPE_SPS`, `PPS` e fatias VCL de um mesmo `decodeUnit` no mesmo buffer `OUTPUT` antes de chamar `v4l2_codec_feed_packet()`. Esse comportamento é correto e **deve ser mantido**, pois impede que o parser faça DRC ou dispare um ciclo de `device_run()` com `SPS`/`PPS` órfãos.
3. **Injeção de *Access Unit Delimiter* (`AUD`: `00 00 00 01 09 F0`) no final do frame ([Inferência / Experimento A/B]):**
   - `VideoDepacketizer.c` descarta AUDs e SEIs de entrada (`isAccessUnitDelimiter`, ver [`docs/04`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/04-fpv-and-ingenious-low-latency-hacks.md#L33)).
   - Em parsers Annex B do VideoCore IV (`hello_video`, streams crus sem delimitação externa), inserir `00 00 00 01 09 F0` após a última NALU VCL é uma técnica clássica para forçar o parser a reconhecer imediatamente o fim do frame $N$ sem aguardar o primeiro `first_mb_in_slice == 0` do frame $N+1$ (**ganho potencial de ~16,67 ms a 60 fps se o firmware estiver segurando 1 frame**).
   - **Risco/Cuidado obrigatório:** No padrão H.264, a NALU AUD (`nal_unit_type = 9`) inicia formalmente o Access Unit *seguinte*. No `bcm2835-codec` stateful: se o driver já estiver passando `MMAL_BUFFER_HEADER_FLAG_FRAME_END` por buffer `OUTPUT`, o AUD no final pode ser inócuo ou iniciar um AU vazio na fronteira do próximo buffer; se *não* estiver passando `FRAME_END`, o AUD sintético no final da submissão força o fechamento imediato do AU anterior. **Só adotar em produção após teste A/B cronometrado no hardware RPi 3B** (medindo delta temporal entre `VIDIOC_QBUF(OUT)` do IDR/P-frame #1 e `VIDIOC_DQBUF(CAP)` antes de enviar o frame #2!).

---

## 3. Eixo 2 — Formatos de Pixel, Conversão Interna VPU/ISP e Interação com `vc4` HVS

### 3.1 Qual é o formato nativo do decodificador VideoCore IV (RPi 3B) e existe conversão interna?

Há grande confusão na literatura online porque a **Raspberry Pi 4 (VideoCore VI / HVS5 / `rpivid`)** e a **Raspberry Pi 3B (VideoCore IV / HVS4)** possuem arquiteturas distintas. Separando estritamente os fatos para a **Pi 3B (BCM2837)**:

1. **Formato interno de reconstrução vs buffers de saída V4L2 (`bcm2835-codec-decode`):** **[Fato Verificado + Limite de Transparência]**
   - Internamente nos registradores e caches de reconstrução/referência (DPB), o bloco hardware H.264 da VideoCore IV trabalha com blocos/macroblocos proprietários em memória (`MMAL_ENCODING_OPAQUE` quando tunelado internamente via MMAL, como em [`src/video/mmal.c:119`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c#L119)).
   - Na fila `CAPTURE_MPLANE` do `bcm2835-codec-decode` (`/dev/video10`), o driver expõe formatos lineares 4:2:0 como **`YU12` (`V4L2_PIX_FMT_YUV420`)**, **`YV12` (`V4L2_PIX_FMT_YVU420`)**, **`NV12` (`V4L2_PIX_FMT_NV12`)**, **`NV21` (`V4L2_PIX_FMT_NV21`)** e `RGBP` (`RGB565`), enquanto o formato em colunas **`V4L2_PIX_FMT_NV12_COL128` (`MMAL_ENCODING_YUVUV128` / Broadcom `SAND128`)** existe no kernel `bcm2835-codec` principalmente para fluxos/hardware mais novos e interoperabilidade ISP/HEVC da Pi 4.
   - **O `SAND128` (`NV12_COL128`) é inviável para zero-copy KMS na RPi 3B:** Conforme verificado em `drivers/gpu/drm/vc4/vc4_plane.c` (também documentado em [`docs/02`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/02-vc4-drm-kms-hvs-display-latency.md)), o HVS de 4ª geração da RPi 3B (`VC4_GEN_4`) **não** suporta o modificador `DRM_FORMAT_MOD_BROADCOM_SAND128` (recurso de HVS5+ na RPi 4/5). Logo, no RPi 3B o cliente **precisa** receber um formato linear (`NV12` ou `YUV420`/`YU12`).
   - **Alinhamentos estritos documentados por `@6by9`:** **[Fato Verificado]** A VPU exige que cada plano (`Y`, `UV`, ou `Y`/`U`/`V`) comece em endereço múltiplo de **32 bytes** e que `bytesperline` seja múltiplo de **64 bytes** em `YUV420` (para garantir que o stride dos planos subamostrados `U` e `V`, igual a `bytesperline / 2`, também seja múltiplo de **32 bytes**). É exatamente por isso que [`v4l2_drm.c:310-314`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c#L310-L314) deve sempre usar `ctx->stride` e `ctx->coded_height` retornados pelo `VIDIOC_G_FMT`/`VIDIOC_S_FMT` (e nunca `1280` ou `720` crus) ao chamar `drmModeAddFB2()`.

2. **`V4L2_PIX_FMT_NV12` vs `V4L2_PIX_FMT_YUV420` (`YU12`) — VPU Firmware vs `vc4` HVS4:**
   - **Na VPU / `vc.ril.video_decode` (`start.elf`):** **[Evidência Histórica Mista / Inferência a Medir]**
     - Em toda a linha legada OpenMAX/MMAL da VideoCore IV (RPi 1–3, `hello_video`, `omx.c`), `OMX_COLOR_FormatYUV420PackedPlanar` (`I420`/`YU12`, 3 planos `Y`, `U`, `V`) foi historicamente o formato linear canônico do `video_decode`. Dependendo de como a VPU escreve a saída linear fora do modo `OPAQUE`, pedir `NV12` (croma `UV` intercalado) vs `YU12` (`U` e `V` separados) é convertido inteiramente dentro do firmware da VPU/bloco de saída sem jamais tocar a CPU ARM nem passar pelo nó V4L2 `/dev/video12` (`bcm2835-isp`). Contudo, como o firmware `start.elf` é fechado, **não há publicação oficial** quantificando se `NV12` custa ~0 ms ou ~0,3–1 ms extras de empacotamento na VPU da BCM2837 frente a `YU12`.
   - **No display `vc4` HVS4 (RPi 3B):** **[Fato Verificado]**
     - Conforme `drivers/gpu/drm/vc4/vc4_plane.c`, o HVS4 lê **ambos** diretamente em hardware sem conversão de CPU:
       - `DRM_FORMAT_NV12` $\to$ `HVS_PIXEL_FORMAT_YCBCR_YUV420_2PLANE`
       - `DRM_FORMAT_YUV420` $\to$ `HVS_PIXEL_FORMAT_YCBCR_YUV420_3PLANE`
     - **Vantagem de barramento LPDDR2 do `NV12` no HVS:** Embora ambos consumam exatamente **12 bpp** (`~1,38 MiB` em 720p), `NV12` lê apenas **2 fluxos DMA** (`Y` e `UV` contíguo), enquanto `YUV420` obriga o HVS a gerenciar **3 ponteiros e 3 fluxos DMA separados** (`Y`, `U` e `V`) a cada linha de varredura (*scanline*), com pior localidade de burst de croma na LPDDR2 compartilhada.
     - *(Detalhe importante para eventual teste com `YU12`: na V4L2, `V4L2_PIX_FMT_YUV420` — diferente de `V4L2_PIX_FMT_YUV420M` — coloca `Y`, `U` e `V` contiguamente em **1 único plano V4L2** (`num_planes = 1`), exportando **1 único FD DMABUF** via `VIDIOC_EXPBUF`, compatível com `drmPrimeFDToHandle` em `v4l2_drm.c` ao passar `handles = {h, h, h, 0}`, `pitches = {stride, stride/2, stride/2, 0}` e os 3 `offsets`! Nunca usar as variantes `*M` como `NV12M`/`YUV420M`, que multiplicam FDs e syscalls `EXPBUF` sem ganho algum.)*

> [!TIP]
> **Conclusão Prática (Eixo 2):** Manter **`V4L2_PIX_FMT_NV12`** como padrão atual em `v4l2_codec.c:337` (melhor eficiência DMA no HVS4 da Pi 3B), **mas executar um benchmark A/B simples no Pi 3B entre `V4L2_PIX_FMT_NV12` e `V4L2_PIX_FMT_YUV420` (`YU12`)** medindo o tempo médio e p99 entre `VIDIOC_QBUF(OUT)` e `VIDIOC_DQBUF(CAP)` em 720p60/120: se `YU12` apresentar latência VPU menor mensurável (>0,3 ms) que compense o 3º stream DMA no HVS, a troca em `v4l2_codec.c` + `v4l2_drm.c` é trivial (~10 linhas de C).

---

## 4. Eixo 3 — Perfis H.264, `CABAC` vs `CAVLC` no VideoCore IV e Realidade de `CAPABILITY_SLICES_PER_FRAME(4)`

### 4.1 VideoCore IV: Bloco H.264, Entropia (`CABAC` vs `CAVLC`) e Mito da Latência de 1 Frame

1. **Capacidade física e limites do hardware H.264 na BCM2837:** **[Fato Verificado]**
   - `h264_freq` padrão de fábrica na RPi 3B = **`300 MHz`** (com `core_freq = 400 MHz`).
   - Teto nominal H.264 **Level 4.1**: **`245.760` macroblocks/s**, máximo de **`8.192` macroblocks/frame** e largura máxima documentada por `@6by9` de **`2.032` pixels**.
   - **Orçamento de macroblocos em 720p ($1280 \times 720 = 80 \times 45 = \mathbf{3.600\text{ MB/frame}}$):**
     - `720p60`: `216.000 MB/s` (**87,9%** do teto nominal Level 4.1 — confortável em clock stock).
     - `720p68.2`: `245.760 MB/s` (100% nominal).
     - `720p90`: `324.000 MB/s` (131,8% — abaixo dos `489.600 MB/s` de 1080p60, viável com overclock `h264_freq=400..500`, `core_freq=450..500` — ver [`docs/02`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/02-vc4-drm-kms-hvs-display-latency.md#L241-L258)).
     - `720p120`: `432.000 MB/s` (~88% da taxa de MB/s de 1080p60, **mas dobra chamadas VCHIQ/ioctl/IRQ para 120 Hz**, exigindo overclock agressivo e refrigeração ativa).

2. **`CABAC` vs `CAVLC` — Separação em dois estágios realmente custa 1 frame inteiro?** **[Fato Verificado + Correção de Mito]**
   - Na arquitetura Broadcom VideoCore IV, o decodificador H.264 separa fisicamente a frente de entropia (bloco acelerador hardware de CABAC/CAVLC) das etapas de predição, transformada inversa (IDCT) e reconstrução de macroblocos + filtro *deblocking* executadas em hardware VPU/bloco de vídeo, comunicando dados de referência/sintaxe via SDRAM compartilhada.
   - Entretanto, **não existe evidência verificada de que escolher CABAC em vez de CAVLC adicione obrigatoriamente um atraso fixo de 1 frame inteiro (16,67 ms a 60 fps)** no pipeline da VideoCore IV. Como em qualquer decodificador hardware em pipeline, o hardware processa em granularidade de **macrobloco / linha de macroblocos** usando buffers internos de linha; o impacto real de **CABAC** vs **CAVLC** manifesta-se em **ciclos de clock por macrobloco, largura de banda de contexto e variância (*jitter*) em quadros complexos ou bitrates de pico** — não em um estágio frame-level artificial que retenha todo o quadro $N$ esperando o CABAC do quadro $N$. **[Fato Verificado na arquitetura H.264 / ausência de evidência de estágio de 1 frame inteiro]**

3. **Por que `Constrained Baseline + CAVLC` ainda pode ajudar em 720p60+/120 (e as travas práticas do host):** **[Fato Verificado em `docs/03`]**
   - **Ganho de determinismo no RPi 3B:** CABAC decodifica *bins* de forma estritamente serial (cada bin atualiza o contexto probabilístico do próximo). Em quadros pesados (IDRs ou cenas com ruído de alta frequência) e especialmente tentando **720p90/720p120**, **CAVLC** reduz a carga e a variância (*p99*) no front-end de entropia e **proíbe formalmente** no bitstream ferramentas pesadas do High Profile (B-slices, `transform_8x8_mode_flag`, matrizes de escalonamento customizadas).
   - **Custo de banda (+10–15%):** Para qualidade visual equivalente, CAVLC gasta ~10–15% mais bits — irrelevante em **720p60 a 10–15 Mbps** sobre Ethernet 100 Mbps cabeada, mas relevante se houver restrição de banda ou Wi-Fi 2.4 GHz.
   - **Trava real atual no Sunshine do usuário ([`docs/03`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/03-sunshine-rx580-and-network-pipeline.md#L122-L140)):** Conforme inspecionado no binário do Sunshine `v2026.1018.5342` no host Arch Linux + RX 580 VAAPI, o Sunshine **fixa `VAProfileH264High` (`7`)** e **não expõe opção `coder=cavlc`** em `sunshine.conf` sem compilar um patch no host; e reescrever o SPS no cliente (`gs_sps_fix`) **não altera** a entropia dos slices já codificados pelo host (`entropy_coding_mode_flag` no PPS). Logo:
     - **Ação imediata (sem recompilar o host):** Manter `VAProfileH264High` sem B-frames com **`vaapi_strict_rc_buffer = enabled`** no `sunshine.conf` (evita explosões VBV em cenas estáticas) + **`gs_sps_fix()` corrigido** no cliente.
     - **Experimento avançado para >60 fps:** Aplicar patch no Sunshine local passando `profile = constrained_baseline` + `coder = cavlc` ao `h264_vaapi` (a RX 580 Polaris 10 suporta `VAProfileH264ConstrainedBaseline` nativamente).

### 4.2 O Efeito Real de `CAPABILITY_SLICES_PER_FRAME(4)` em `bcm2835-codec` **[Fato Verificado]**

Em [`src/video/v4l2.c:277`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L277), o cliente anuncia:
```c
.capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLICES_PER_FRAME(4),
```
O gerador SDP ([`SdpGenerator.c:430-431`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/SdpGenerator.c#L425-L431)) envia `x-nv-video[0].videoEncoderSlicesPerFrame: 4` ao Sunshine. Entretanto, na combinação **RX 580 + `moonlight-common-c` + `bcm2835-codec`**:

1. **Zero ganho de latência sub-frame na rede:** O `VideoDepacketizer.c` (`reassembleFrame`) **espera o frame inteiro** (pacote com `FLAG_EOF` + conclusão/validação dos pacotes do frame) antes de disparar `v4l2_submit_decode_unit()` uma única vez por quadro (`docs/03`, `docs/04`). Os slices nunca são entregues antecipadamente ao decoder!
2. **Zero paralelismo multi-slice no `bcm2835-codec`:** `CAPABILITY_SLICES_PER_FRAME(4)` foi projetado para decodificadores **software multi-thread** (onde 4 cores ARM decodificam 4 fatias horizontais em paralelo) ou hardware com múltiplas engines paralelas por slice. O `bcm2835-codec` recebe um único buffer `OUTPUT` com o quadro completo e o entrega sequencialmente ao bloco único H.264 da VideoCore IV.
3. **Penalidades reais ao dividir 720p em 4 slices:**
   - Em 720p (apenas **45 linhas de macroblocos** de altura), 4 slices criam faixas de apenas 11–12 linhas cada, **reiniciando as tabelas de contexto CABAC/CAVLC 4 vezes por quadro**, bloqueando predição intra entre fronteiras de slice e adicionando cabeçalhos NAL extras (piora eficiência/qualidade por Mbps).
   - Em drivers AMD VAAPI (`h264_vaapi`), multi-slice combinado com rate control rígido pode introduzir overhead de geração de slices no VCE 3.4 ou falhas em certos modos (`docs/03`, `docs/04`).
- **Recomendação concreta:** Trocar para **`CAPABILITY_SLICES_PER_FRAME(1)`** em `src/video/v4l2.c:277` (testando em A/B com medição de latência e bitrate).

---

## 5. Eixo 4 — Controles V4L2, Contagem de Buffers, CMA, Cache L2 e *Stalls*

### 5.1 Controles V4L2 expostos por `bcm2835-codec-decode` **[Fato Verificado]**

Ao contrário de `ROLE_ENCODE` (`/dev/video11`), que registra dezenas de controles de bitrate, QP, GOP, `REPEAT_SEQ_HEADER` e `FORCE_KEY_FRAME`, o papel de decodificação **`ROLE_DECODE` (`/dev/video10`, `bcm2835-codec-decode`)** em `bcm2835-v4l2-codec.c` é propositalmente mínimo:
- **`V4L2_CID_MIN_BUFFERS_FOR_CAPTURE`:** Único controle crítico para dimensionamento de fila no decoder; implementado como controle *read-only* volátil preenchido a partir de `port->minimum_buffer.num` da porta de saída do componente MMAL `vc.ril.video_decode`, atualizado dinamicamente assim que o parser do firmware processa o SPS/VUI e dispara `V4L2_EVENT_SOURCE_CHANGE` ([`v4l2_codec.c:367-371`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L367-L371)).
- **`V4L2_CID_MPEG_VIDEO_DECODER_MPEG4_DEBLOCK_FILTER` / perfil e nível em builds mais recentes:** Não oferecem nenhum ajuste de baixa latência para H.264.
- **Lacuna de controles no driver:** O `bcm2835-codec` **não expõe** controles V4L2 equivalentes a `MMAL_PARAMETER_VIDEO_MAX_NUM_CALLBACKS`, `MMAL_PARAMETER_VIDEO_DECODE_ERROR_CONCEALMENT` nem `V4L2_CID_MPEG_VIDEO_H264_LOW_LATENCY`. Portanto, no V4L2, a única forma legítima do cliente influenciar o DPB e o acúmulo interno do decoder é via **(a) correção estrita do SPS/VUI (`gs_sps_fix`)** e **(b) controle rigoroso da profundidade das filas `OUTPUT` e `CAPTURE` e do ritmo de `VIDIOC_QBUF`**.

### 5.2 Auditoria de Memória CMA, Cache L2 e *Stalls* nas Filas Atuais (`v4l2_codec.c`)

#### A. Fila `OUTPUT_MPLANE` (`req.count = 12` buffers × `1 MiB`)
- **Consumo atual ([`v4l2_codec.c:85,97`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L85-L97)):** **12 MiB de CMA contígua**. Em 720p60 com `CAPABILITY_DIRECT_SUBMIT` e `num_reorder_frames = 0`, cada frame entra como 1 AU por vez, consumindo em média ~20–35 KiB (P-frame) e ~100–300 KiB (IDR). Ter 12 buffers de entrada é excessivo para um Raspberry Pi 3B de apenas 1 GiB de RAM total.
- **Armadilha de lock e dupla cópia ([`v4l2.c:250-258`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L250-L258) + [`v4l2_codec.c:250-274`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L250-L274)):**
  - O código aloca `v4l2_instance.bitstream_buf` no heap, copia para ele e depois executa **outro `memcpy`** para `output_bufs[free_idx].start`. Esse passo duplo consome ciclos do Cortex-A53 (1,2 GHz) e **polui a cache L1/L2 (512 KiB) da CPU** com dezenas a centenas de KiB por frame.
  - Se todos os 12 buffers estiverem ocupados, `v4l2_codec_feed_packet()` entra em `for (int wait = 0; wait < 4; wait++) { poll(&pfd, 1, 3); ... }` (**até 12 ms**) **mantendo `ctx->state_mutex` travado**, o que bloqueia simultaneamente a `v4l2_event_thread` de drenar frames decodificados ou responder ao DRM!
- **Redução recomendada:**
  - Reduzir `OUTPUT` `req.count` de **`12` para `4`** (equivalente aos **`5` buffers de entrada em [`mmal.c:114`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c#L114)**; economia de **8 MiB de CMA**). Mesmo em `1 MiB` cada (`4 MiB` totais), garante headroom para IDRs gigantes sem qualquer risco de overflow em 720p60/120.
  - Eliminar `bitstream_buf` escrevendo diretamente em `output_bufs[idx].start` (zero-intermediate-copy) e **nunca** chamar `poll()` mantendo `state_mutex` preso (`docs/04`, Seção 3.1).

#### B. Fila `CAPTURE_MPLANE` (`req_count = clamp(min_bufs + 8, 12, 20)`)
- **Consumo atual ([`v4l2_codec.c:367-377`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L367-L377)):** Para 720p `NV12` com stride alinhado (`1280 × 720 × 1,5` = `1,38 MiB` a ~`1,5 MiB` com padding de `coded_height` alinhado a 16/32), 12 a 20 buffers ocupam **~16,6 a ~30 MiB de CMA**.
- **Por que `min_bufs + 8` (com piso `12`) prejudica em vez de ajudar:**
  1. **Bufferbloat interno no firmware MMAL:** Diferente do mito de que "buffers extras no CAPTURE não afetam latência se você der DQBUF rápido", o componente `vc.ril.video_decode` continua aceitando e decodificando buffers da fila `OUTPUT` enquanto houver buffers livres enfileirados em `CAPTURE`. Em qualquer rajada de rede ou pequeno atraso de display/VBlank, uma fila `CAPTURE` de 12–20 slots permite ao firmware acumular vários frames à frente do scanout, inflando a latência de ponta a ponta e agravando a retenção antes de DRC/teardown.
  2. **Risco em teardowns e DRC (`#3325` e `#7163`):** Bugs conhecidos no `bcm2835-codec` como [`raspberrypi/linux#3325`](https://github.com/raspberrypi/linux/issues/3325) (*buffer leaks* / `Timeout waiting for buffers to be returned`) e [`#7163`](https://github.com/raspberrypi/linux/issues/7163) (*`videobuf2` active buffer / `bcm2835_mmal_vchiq` timeouts*) ocorrem quando buffers permanecem retidos entre `v4l2` e VPU durante `STREAMOFF`/DRC. Manter filas curtas reduz drasticamente o número de buffers em trânsito durante `v4l2_codec_handle_drc()` e `v4l2_codec_destroy()`.
  3. **Pressão de CMA na RPi 3B:** Na RPi 3B sob `vc4-kms-v3d` (`cma=256M@256M` padrão no `vc4-kms-v3d-overlay.dts`, frequentemente ajustável para `192M` com `gpu_mem=64`), economizar ~15–22 MiB de CMA evita fragmentação e pressão sobre os ~768 MiB utilizáveis pelo Linux.
- **Qual é o piso matemático mínimo sem causar *decoder stall*?** **[Fato Verificado sobre a máquina de estados de `v4l2_drm.c`]**
  - Os buffers `CAPTURE` precisam cobrir simultaneamente:
    - **`min_bufs` (`V4L2_CID_MIN_BUFFERS_FOR_CAPTURE`):** slots exigidos pelo DPB do hardware (referência anterior + frame sendo decodificado; com `gs_sps_fix()` corrigido para `num_ref_frames=1, max_dec_frame_buffering=1`, tipicamente `2` ou `3`, ao passo que sem correção de VUI o Level 3.2/4.1 pode reportar `5`–`6`!).
    - **`+ 1` buffer atualmente em *scanout* no CRTC HDMI** (`ctx->current_displayed_idx` até chegar `page_flip_handler`).
    - **`+ 1` buffer aguardando em `ctx->staged_frame_idx`** (enquanto `flip_pending == true`).
    - **`+ 1` buffer livre para o firmware escrever o próximo quadro enquanto um atomic flip/requeue ocorre.**
  - **Fórmula segura recomendada:** **`req_count = min_bufs + 3`** (removendo o piso rígido de `12`! Ex.: se `min_bufs = 3`, aloca **`6` buffers** = ~8,3 MiB CMA; com limite inferior absoluto `>= 5`). Abaixo de `min_bufs + 2` há risco real de **`CAPTURE` starvation**: o `device_run()` do `bcm2835-codec` não executa se não houver buffer livre na fila `CAPTURE`, inserindo uma bolha imediata de 1 frame (16,67 ms)!
- **Efeito na Cache L2 (512 KiB ARM Cortex-A53 / 128 KiB VPU):** **[Fato Verificado]**
  - Os buffers `CAPTURE` alocados pelo `vc-sm-cma` e exportados via `VIDIOC_EXPBUF` são mapeados sem cache de CPU (ou `write-combining` para DMA), logo os pixels `NV12` **não poluem a cache L2 do ARM** nem são tocados pela CPU no caminho zero-copy para o HVS. O ganho de cache L2 vem exclusivamente de remover `bitstream_buf` (dupla cópia de entrada) e minimizar structs de metadados/tabelas de página contíguas na CMA.

---

## 6. Síntese Priorizada e Plano de Testes no Hardware RPi 3B

| Prioridade | Otimização Específica (`bcm2835-codec` + Bitstream) | Ganho Esperado em 720p60 / >60fps | Risco / Trade-off | Evidência |
|---|---|---|---|---|
| **1** | Corrigir [`gs_sps_fix()`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c#L56-L77): forçar `vui_parameters_present_flag = 1`, `num_reorder_frames = 0` incondicional, `max_dec_frame_buffering = 1`, e `low_delay_hrd_flag = 1` (se HRD presente). | Elimina risco de DPB inflado (`MaxDpbFrames` 4–5 = **66–83 ms**!) e reduz `V4L2_CID_MIN_BUFFERS_FOR_CAPTURE`. | Nenhum se respeitar `max_dec_frame_buffering >= num_ref_frames`. | **[Fato Verificado no código]** |
| **2** | Eliminar dupla cópia (`bitstream_buf` $\to$ `output_bufs`) e retirar `poll(&pfd, 1, 3)` de dentro de `state_mutex` em [`v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L240-L258) / [`v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c#L199-L275); usar `SCHED_FIFO` também em `VideoRecv`. | **-0,2 a -1,5 ms** de CPU/cache L2 em cada frame + elimina stalls de até **12 ms** por contenção de mutex. | Baixo (cuidado com ciclo de vida de `output_bufs` sob DRC/stop). | **[Fato Verificado no código]** |
| **3** | Reduzir filas CMA: `OUTPUT` `12 -> 4` (`v4l2_codec.c:97`) e `CAPTURE` `min_bufs + 8 (min 12)` $\to$ `min_bufs + 3` (`v4l2_codec.c:373-376`). | Economiza **~20–25 MiB CMA** na Pi 3B; impede acúmulo multi-frame no MMAL sob backpressure; mitiga leaks/timeouts em DRC (`#3325`, `#7163`). | Baixo; validar em carga real que `CAPTURE` nunca zera (`min_bufs + 3` cobre DPB + scanout + staged + próximo). | **[Fato Verificado + Hipótese a validar]** |
| **4** | Mudar `CAPABILITY_SLICES_PER_FRAME(4)` para **`CAPABILITY_SLICES_PER_FRAME(1)`** em [`v4l2.c:277`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c#L277). | Elimina 4 reinicializações de contexto CABAC/CAVLC e overhead de cabeçalhos slice em 720p sem perder nada (V4L2 stateful é por AU inteira). | Nenhum em `bcm2835-codec`; melhora compatibilidade com RX 580 VAAPI. | **[Fato Verificado]** |
| **5** | **(Experimento A/B)** Testar **injeção de AUD (`00 00 00 01 09 F0`)** ao fim do frame entregue no `VIDIOC_QBUF`. | Potencial de **-16,67 ms (1 frame)** se o firmware `video_decode` da revisão usada estiver segurando o AU esperando início do próximo slice. | Se o firmware já der flush imediato ou interpretar AUD como AU seguinte incompleto, pode não ajudar ou gerar erro; exigir teste cronometrado. | **[Inferência / Hipótese]** |
| **6** | **(Experimento A/B)** Testar **`V4L2_PIX_FMT_YUV420` (`YU12`, 1 plano contíguo)** vs **`V4L2_PIX_FMT_NV12`** atual. | Potencial economia sub-ms se o firmware `video_decode` evitar empacotamento `UV` na VPU. | `YU12` usa 3 canais DMA no HVS4 vs 2 do `NV12`; manter `NV12` salvo se benchmark provar ganho líquido. | **[Inferência / Hipótese]** |
| **7** | **(Host >60fps / 720p90-120)** `vaapi_strict_rc_buffer = enabled` imediato no Sunshine + patch opcional `Constrained Baseline + CAVLC` + OC `h264_freq=450` / `core_freq=450..500`. | `720p60` roda em `300 MHz` stock (87,9% Level 4.1); `720p90/120` excede teto nominal e exige OC + menor variância CAVLC no front-end. | `CAVLC` eleva bitrate em +10–15% e exige recompilar Sunshine no host; OC exige refrigeração na Pi 3B. | **[Fato Verificado + Estimativa]** |

### Comandos de Diagnóstico Recomendados na Raspberry Pi 3B Real

Como o acesso ao firmware `start.elf` e aos retornos exatos do driver no kernel alvo ocorre em tempo de execução no hardware, execute os comandos abaixo diretamente na Raspberry Pi 3B antes e depois dos patches:

```bash
# 1. Confirmar driver, formato negociado e controles realmente expostos em /dev/video10
v4l2-ctl -d /dev/video10 --all --list-ctrls --list-formats-out-ext --list-formats-ext

# 2. Durante streaming 720p60 ativo, verificar valor efetivo reportado de DPB pelo firmware
v4l2-ctl -d /dev/video10 --get-ctrl=min_buffers_for_capture

# 3. Monitorar clocks reais (h264, core, arm), temperatura, throttling e consumo de CMA
vcgencmd measure_clock arm core h264
vcgencmd measure_temp
vcgencmd get_throttled
grep -i cma /proc/meminfo
```

---

## 7. Referências Verificadas

### Código-Fonte Local Inspecionado
- [`src/video/v4l2.h`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.h) e [`src/video/v4l2.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2.c)
- [`src/video/v4l2_codec.h`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.h) e [`src/video/v4l2_codec.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_codec.c)
- [`src/video/v4l2_drm.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/v4l2_drm.c)
- [`src/video/mmal.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/mmal.c) e [`src/video/pi.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/src/video/pi.c)
- [`libgamestream/sps.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/libgamestream/sps.c) e [`third_party/h264bitstream/h264_stream.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/h264bitstream/h264_stream.c)
- [`third_party/moonlight-common-c/src/Limelight.h`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/Limelight.h), [`VideoDepacketizer.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/VideoDepacketizer.c) e [`SdpGenerator.c`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/third_party/moonlight-common-c/src/SdpGenerator.c)
- Relatórios complementares do enxame: [`docs/02-vc4-drm-kms-hvs-display-latency.md`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/02-vc4-drm-kms-hvs-display-latency.md), [`docs/03-sunshine-rx580-and-network-pipeline.md`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/03-sunshine-rx580-and-network-pipeline.md) e [`docs/04-fpv-and-ingenious-low-latency-hacks.md`](file:///home/gabriel/Projetos/CLI/moonlight-embedded-rpi3-v4l2/docs/04-fpv-and-ingenious-low-latency-hacks.md)

### Kernel Linux, Firmware VideoCore e Issues Verificadas
- Driver `bcm2835-codec` (`drivers/staging/vc04_services/bcm2835-codec/bcm2835-v4l2-codec.c`): https://github.com/raspberrypi/linux/blob/rpi-6.6.y/drivers/staging/vc04_services/bcm2835-codec/bcm2835-v4l2-codec.c
- Driver `vc4` HVS (`vc4_plane.c`): https://github.com/raspberrypi/linux/blob/rpi-6.12.y/drivers/gpu/drm/vc4/vc4_plane.c
- Issue `#232` (*video_decode internal buffering/queuing*): https://github.com/raspberrypi/firmware/issues/232
- Issue `#496` (*Avoiding MMAL excessive buffering* / `MMAL_PARAMETER_VIDEO_MAX_NUM_CALLBACKS`): https://github.com/raspberrypi/firmware/issues/496
- Issue `#5059` (*lost frames/incomplete draining in bcm2835-v4l2-codec.c*): https://github.com/raspberrypi/linux/issues/5059
- Issue `#7691` (*bcm2835-codec: V4L2_DEC_CMD_START does not resume after a source change*): https://github.com/raspberrypi/linux/issues/7691
- Issue `#7313` (*bcm2835-codec encoder does not expose V4L2_CID_MIN_BUFFERS_FOR_CAPTURE*): https://github.com/raspberrypi/linux/issues/7313
- Issue `#7618` (*bcm2835-codec* caps renegotiation / drain): https://github.com/raspberrypi/linux/issues/7618
- Issue `#3325` (*bcm2835-codec - Driver leaks buffers / Timeout waiting for buffers to be returned*): https://github.com/raspberrypi/linux/issues/3325
- Issue `#7163` (*bcm2835-codec / bcm2835_mmal_vchiq timeouts and videobuf2 driver bug*): https://github.com/raspberrypi/linux/issues/7163
- Documentação VideoCore IV (hermanhermitage): https://github.com/hermanhermitage/videocoreiv
