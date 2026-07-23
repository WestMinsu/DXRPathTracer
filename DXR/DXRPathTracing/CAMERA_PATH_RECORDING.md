# 카메라 경로 녹화와 RTXPT 재생

## 자유 카메라

- W/A/S/D: 전후좌우 이동
- Q/E: 아래/위 이동
- Shift: 빠른 이동
- 왼쪽/오른쪽 방향키: yaw 회전
- 위/아래 방향키: pitch 회전
- Ctrl + 방향키: 설정된 회전 속도의 25%로 정밀 회전
- 마우스 오른쪽 드래그: 자유 시점 회전

ImGui의 Arrow-key turn speed로 방향키 회전 속도를 조절한다.

## 경로 녹화

1. Recording output JSON에 저장 경로를 입력한다. 기본값은
   Config\recorded_camera_path.json이다.
2. Start camera recording을 누르고 카메라를 움직인다.
3. Stop and save camera recording을 누른다.
4. Playback path JSON에 재생할 JSON 경로를 입력한다.
5. Load and play camera path를 눌러 입력한 경로를 불러와 재생한다.

녹화는 렌더링 FPS와 분리된 60 Hz 자세로 보간 저장된다. 저장 경로와 재생
경로는 서로 독립적이며, Load and play를 누를 때마다 Playback path JSON에
입력한 파일을 다시 읽는다. 저장된 경로는 다음과 같이 벤치마크에도 사용할 수 있다.

    .\x64\Debug\DXRPathTracing.exe --camera-path Config\recorded_camera_path.json --benchmark --benchmark-output BenchmarkOutput\recorded_camera.csv --vsync 0

## RTXPT scene으로 변환

현재 Sponza 변환과 동일하게 DXR 좌표의 Z축을 반전하고, 각
position/target 자세를 RTXPT의 translation/quaternion 애니메이션으로
변환한다.

    python .\Tools\CameraPath\export_to_rtxpt.py --input .\Config\recorded_camera_path.json --scene .\Validation\SponzaPbr\RTXPT\sponza-pbr.scene.json --output .\Validation\SponzaPbr\RTXPT\sponza-recorded.scene.json --warmup-seconds 1

생성된 scene 파일을 RTXPT의 Assets 루트에 복사한다. 기존
Models/SponzaPbr 자산도 같은 Assets 트리에 있어야 한다. RTXPT에서는
애니메이션을 켠 상태로 scene을 열면 1초 정지 후 녹화한 움직임이 시작된다.

결정론적 프레임 시퀀스는 다음 형식으로 캡처한다.

    Rtxpt.exe --scene sponza-recorded.scene.json --overrideToRealtimeMode --captureSequence --capturePath C:\Capture\rtxpt_recorded.png --sequenceWarmupStart 0 --sequenceRecordStart 1 --sequenceFPS 60 --sequenceFrameCount <녹화된 pose 수> --nonInteractive

--stopAnimations를 사용하면 카메라 경로도 정지하므로 함께 지정하지 않는다.
다른 장면이 이미 DXR과 동일한 좌표계를 사용한다면 변환 명령에
--coordinate-conversion identity를 추가한다.
