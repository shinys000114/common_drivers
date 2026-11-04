ODROID-C5 미디어 시험 묶음
=========================

우리가 실험한 미디어·플레이어 조합을 스크립트 하나에 한 항목씩 실행하고 결과를 남긴다.
모두 sudo로 실행한다(화면 갱신 측정에 tracefs·debugfs가 필요). 플레이어는 데스크톱 사용자(odroid)로
GNOME Wayland 세션 안에서 뜬다. 보드 모니터 앞에서 GNOME 터미널로 실행해도 되고, SSH로 실행해도 된다.

  sudo ./01-gst-hevc-4k60-fullscreen.sh        항목 하나
  sudo ./run-all.sh                            전체(유튜브와 kmssink 제외)
  sudo ./run-all.sh --with-net --with-kms      유튜브(네트워크)와 kmssink(GDM 정지)까지
  sudo ./run-all.sh --with-stress              JPEG 인코더 시작/정지·강제 종료 반복(82)까지

결과
  results/<시각>-<항목>.log      시스템 정보(커널, GPU 드라이버, Mesa, mutter 종류, 세션 환경) + 측정값
  results/<시각>-<항목>.log.client  플레이어 출력(GStreamer, Showtime, Chromium, VLC)
  results/summary.tsv            한 줄 요약: 시각, 항목, 화면 갱신/s, 클라이언트 결과, plane, GPU 점유

측정값 읽는 법
  display: N updates/s   커널이 실제로 화면을 바꾼 횟수(s7d_frame RDMA 완료). 60이면 60 Hz 그대로.
                         primary가 AFBC일 때는 0으로 나온다(그 경로는 이 이벤트를 내지 않음).
  planes                 측정 중 가장 많이 본 plane 상태. OSD1:NV12 = 영상이 VD1으로 직접 나감(GPU 안 씀),
                         OSD1:XR24 VD1:NV12 = 데스크톱 위에 영상 overlay, OSD1:XR24만 = GPU 합성.
  gpu                    Panthor 클라이언트별 GPU 점유(libMali 부팅에서는 n/a).
  kernel:                시험 중 커널 로그에 찍힌 디코더·GE2D 오류. 요약 줄 끝에도 [kernel: ...]로 붙는다.
  hint:                  전체화면 영상이 VD1에 못 올라간 경우. 대화상자, 알림 배너, 독이 영상 위에 있으면
                         데스크톱이 합성으로 돌아간다. 닫고 다시 실행.

항목
  01~06  GStreamer waylandsink 전체화면(HEVC, HEVC10, VP9, AV1, H.264 4K30, 1080p 확대): VD1 직접, 60 기대
  07~08  waylandsink 창(1080p, 4K): 원본 mutter는 GPU 합성
  10~13  Showtime 4K 창/전체화면/최대화(HDR10 12, SDR 13)(GTK 오프로드). 창 모드에는 ~/.config/gtk-4.0/gtk.css 사각 모서리가 필요
  12     Showtime 4K 최대화 창(dconf is-maximized로 최대화해 띄우고, 끝나면 되돌린다)
  14     Showtime SDR 4K 최대화 + 컨트롤 표시(lib/jiggle.py로 포인터를 계속 움직인다): 원본 mutter는 GPU 합성
         Showtime 시험은 시작할 때 lib/park.py로 포인터를 화면 가운데(영상 위)에 둔다. 헤더바 버튼 위에 멈춘
         포인터는 툴팁을, 하단 컨트롤 위의 포인터는 컨트롤을 영상 위에 남겨 원본 mutter가 합성으로 돌아간다.
         GStreamer 시험은 gst-launch -e로 끝낸다(EOS drain). EOS 없이 SIGINT로 끊으면 GStreamer가 정지 중에
         "Could not read from resource"를 낼 때가 있다(드라이버 문제 아님).
  20~23  Chromium(V4L2 디코더) 창·키오스크. 4K 화면 키오스크는 Chromium 자체 합성 때문에 느림.
         22(VP9 10비트), 23(H.264 4K)은 지금 Chromium이 소프트웨어로 디코드한다: 아래 "알려진 문제"
  24     Chromium 유튜브(네트워크 필요, 디코더 사용 여부만 확인)
  30     VLC + hevc_v4l2m2m, MP4(프레임 CPU 복사). 현재 실패: 아래 "알려진 문제"
  31     같은 영상을 MKV로 바꿔 VLC 재생(처음 실행 때 $MEDIA에 MKV를 만든다)
  40~42  FFmpeg v4l2m2m 디코드 속도(화면 없음)
  50     GE2D 4K -> 1080p 변환 속도(화면 없음)
  51~57  GE2D(v4l2convert)와 gtk4paintablesink 창 조합. 51/52 HEVC 4K60 -> 1080p NV12/BGRx waylandsink,
         53 gtk4paintablesink 4K60(GE2D 없음), 54 GE2D + gtk4paintablesink 4K60,
         55 H.264 4K30 GE2D BGRx waylandsink, 56 GE2D + gtk4paintablesink 4K30, 57 gtk4paintablesink 4K30.
         libMali 부팅에서 GTK가 FBO 수정판(4.24.1 또는 c5fbo 백포트)이면 shim 없이 돈다
  58     GE2D 다중 평면 노드(meson-ge2d-mplane): 같은 videotestsrc 1280x720을 단일 평면 노드는 NV12로,
         다중 평면 노드는 NV12M(휘도·색차가 버퍼 두 개)으로 받아 BGRx로 바꾼다. 두 출력이 바이트 단위로 같아야
         통과. GStreamer는 다중 평면 노드에서 NV12M을 스스로 고른다(로그의 "Desired format ... NM12")
  60     glmark2 4K 전체화면
  70     kmssink, 컴포지터 없는 기준값. GDM을 멈추므로 --stop-gdm을 붙여야 실행된다
  80     JPEG 인코더 왕복(lib/vjpeg.py): 소프트웨어로 디코드한 프레임을 v4l2jpegenc로 부호화하고 jpegdec로
         되돌려 평면별 PSNR. 1080p q75/q50/q95, NV21, 642x482, GE2D 출력 dma-buf를 인코더가 그대로 읽는 경우.
         통과: 모든 프레임 왕복, q75·NV21·GE2D에서 Y >= 38 dB. FRAMES로 프레임 수(기본 30)
  81     JPEG 인코더 속도: videotestsrc 1080p NV12 600장, MMAP(복사)와 GE2D dma-buf 가져오기, fps
  82     JPEG 인코더 시작/정지 반복(v4l2-20261029/vjrep.sh와 같은 네 경우) + 인코드 중 SIGKILL 두 번을 JREP회
         (기본 5, 감사 권고 20). 끝나고 한 번 더 인코드해 인코더가 살아 있는지 본다. HCODEC DMA 중 정지가
         예전에 보드를 멈춘 경로라 PowerMate를 준비하고 돌린다. run-all.sh에서는 --with-stress일 때만
  85     v4l2-compliance(스트리밍 시험 없음): 디코더, JPEG 인코더, GE2D 단일·다중 평면 노드. 노드별 통과 수와
         fail: 줄을 로그에 남긴다

환경 변수
  MEDIA=/var/tmp/claude-media   시험 영상 위치
  DUR=20 AT=8 WIN=4             플레이어 실행 시간, 측정 시작 시점, 측정 구간(초)
  WL_DISPLAY=wayland-0          플레이어를 띄울 Wayland 소켓. GNOME 말고 다른 컴포저에서 돌릴 때는
                                tools/testing/c5/desktop-20261102/altrun.sh(VT7) 또는 gdmsess.sh(GDM 세션)가
                                정해 준다. summary.tsv 항목 이름 앞에 컴포저 이름(ALT_COMP)이 붙는다

전제
  - gnome-shell 환경에 MUTTER_DEBUG_PAINT=ignore-color-state-for-direct-scanout (없으면 경고).
    넣는 방법: systemctl --user set-environment MUTTER_DEBUG_PAINT=ignore-color-state-for-direct-scanout
    후 로그아웃/로그인(또는 /etc/environment.d/ 에 파일로).
  - s7d_drm primary_afbc=0 (/etc/modprobe.d/claude-s7d.conf).
  - Chromium은 /var/tmp/claude-chromium-debian 에 풀어 둔 Debian 156 빌드.
  - 80~82는 HW 디코더를 쓰지 않는다(vjpeg.py가 v4l2 디코더 rank를 NONE으로). 디코더 세션이 DOS를 잡고 있으면
    JPEG 인코더가 -EBUSY다. 58·80·81은 s7d_ge2d 모듈을 올린다. 85는 v4l2-utils의 v4l2-compliance가 필요하다.

알려진 문제(2026-11-03, 디코더 커밋 ab9f5be7b..5dbd0bde7 반영 후)
  - VLC 3.0.23은 MP4를 첫 IDR 다음 픽처부터 넘긴다. 디코더가 이제 첫 IDR/IRAP까지 건너뛰므로 30도 재생된다.
    단 H.264 참조 프레임이 빠진 스트림(frame_num 간격)은 아직 실패한다(커널 "decoder failed: -95").
  - ENUM_FRAMESIZES가 생겨 Chromium이 4K H.264도 V4L2로 디코드한다(23). 4K 키오스크가 느린 것은 Chromium 자체 합성.
    VP9 프로파일 2(10비트)는 Chromium이 V4L2로 보내지 않는다(22).
  - 디코더 세션은 한 번에 하나다. 두 번째 클라이언트는 OUTPUT S_FMT에서 -EBUSY를 받고, Chromium·GStreamer는 소프트웨어로 넘어간다.
  - 보드 RAM 3.6 GB. zram 스왑(systemd-zram-generator, zstd 1.8 GB)을 켰다.
    크래시 보고 대화상자, "Application Stopped" 알림, "Low Disk Space" 알림(루트 여유 공간 부족)이 떠 있으면
    전체화면이 합성으로 돌아간다(hint: 줄). 알림을 닫고 다시 실행한다.
  - Showtime은 마지막 창 상태를 기억한다. 시험이 시작할 때 최대화 상태를 끈다.
  - libMali 부팅: GTK FBO 수정판(c5fbo1)이 깔려 있으면 shim 없이 돈다. 순정 mutter에서는 GTK 오프로드가 화면을
    멈추게 하므로 GTK_EXTRA=GDK_DISABLE=offload로 돌린다(4K 직접 11회/s, GE2D 1080p 30~32회/s).

기준 결과(2026-11-03, 커널 #412, Panthor + Mesa 26.2.6, Ubuntu 원본 mutter, primary_afbc=0)
  01~06 전체화면           VD1 직접 59~60회/s(H.264 4K30은 30), GPU 0%
  07 1080p 창              60회/s, GPU 합성 51%
  08 HEVC10 4K 창          8회/s, GPU 107%: 창이 화면 크기 4K라 매 프레임 4K 합성(원본 mutter 한계)
  10 Showtime 4K 창        50~60회/s, GPU 합성 66~67%
  11 Showtime 4K 전체화면   VD1 직접 52~53회/s, GPU 0%
  12 Showtime 4K 최대화 창  7회/s, GPU 107%(화면 크기 4K 합성, 원본 mutter 한계)
  20 Chromium 1080p 창     59 fps, V4L2 사용 / 21 키오스크 16 fps(Chromium 4K 자체 합성)
  22, 23 Chromium 4K       VP9.2 5~8 fps(소프트웨어), H.264 4K 18 fps(새 디코더에서 V4L2, 이전 10)
  24 유튜브                V4L2 사용, 54회/s
  30 VLC MP4               (새 디코더) 60 pictures/s / 31 VLC MKV 37~63 pictures/s
  40~42 FFmpeg 디코드      H.264 1080p 198, HEVC 4K 71, VP9 4K 107 fps
  50 GE2D 4K->1080p        40 fps
  60 glmark2 4K            409 FPS
  80~85                    (새 항목, 아직 실행 기록 없음) 이전 값: JPEG q75 Y 38.5/UV 41.2 dB, q95 45.5/46.7,
                           q50 35.2/39.0(2026-10-30 모듈 266), v4l2-compliance JPEG 48/48, GE2D 48/48(2026-10-29)
  51~54 GE2D 4K60 창       22~38회/s: GE2D가 4K 입력에서 약 40 fps라 60 fps 영상은 늦어지고 싱크가 버린다
  53 gtk4paintablesink 4K60 창  60회/s, GPU 37%(GE2D 없이가 낫다)
  55~57 4K30 창            GE2D BGRx waylandsink 30(GPU 35%), GE2D + gtk4 30(GPU 20%), gtk4만 30(GPU 28%)
  04 AV1은 재생 중 정상(59~60)이고, SIGINT로 멈출 때 v4l2av1dec 오류가 찍힐 때가 있다(요약에 "; error:"로 붙음).
  11은 GDK_DEBUG=frames를 켜면(ST_GDK_DEBUG) GTK가 느려져 전체화면이 합성으로 남을 때가 있어 기본은 color-mgmt만 쓴다.

2026-11-03 저녁 정리: 기본 이미지는 Ubuntu 원본 mutter/GTK. 아래 MPO 결과는 참고용(패키지는 배포하지 않음).
  디코더는 GPU가 읽는 중인 CAPTURE 버퍼를 기다린 뒤 쓴다(합성 중 찢김 수정). 축소된 VD1은 2160~270 모든 높이를 받는다.

MPO mutter(50.1-0ubuntu2.5+c5mpo1) 설치 후(2026-11-03)
  12/13 Showtime 최대화      HDR 60, SDR 58회/s, VD1 overlay, GPU 0%(원래 mutter 8 / 31)
  08 waylandsink 4K 창       59~60회/s, VD1, GPU 0%(원래 8~12)
  07 waylandsink 1080p 창    60회/s, VD1, GPU 0.5%(원래 GPU 51%)
  10 Showtime 기본 창        합성 그대로(영상이 메인 표면 아래), 전체화면·glmark2·Chromium 변화 없음
