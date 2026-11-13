# HMocapTool

언리얼 엔진 에디터에서 영상 파일로 바디 애니메이션을 만드는 에디터 플러그인.
영상을 목록에 넣고 처리하면 MetaHuman 바디 트래커로 동작을 뽑고, IK 리타게터로 대상 스켈레톤에 옮긴 애니메이션 시퀀스를 저장한다.

## 기능

- 영상 여러 개를 대기 목록에 넣고 차례로 처리
- IK 리타게터를 지정해 대상 스켈레톤용 애니메이션으로 변환
- 골반 이동을 root 뼈로 옮기는 루트 모션 변환
- 시작 위치를 원점으로, 시작 방향을 정면으로 맞추는 옵션 (각각 따로 선택)
- 영상과 결과 애니메이션 미리 보기

메뉴 위치: `HTools > 애니메이션 > 영상 모캡`

## 필요한 것

- Unreal Engine 5.8
- **MetaHuman Animator Markerless Motion Capture** 플러그인 (Fab에서 따로 받는다. 이 저장소에는 들어 있지 않다.)
  - 프로젝트의 `Plugins/MetaHumanBodyTracker`에 두고 플러그인을 켠다.
  - 없으면 탭은 열리지만 처리 시작 버튼이 꺼져 있고 안내문이 나온다.
- 엔진 기본 플러그인: MetaHuman, MetaHumanCoreTech, CaptureManagerEditor, CaptureData, ControlRig, IKRig (플러그인을 켜면 같이 켜진다.)

## 설치

### 미리 컴파일한 배포본 (C++ 빌드 불필요)

1. [Releases](../../releases)에서 `HMocapTool_UE5.8.zip`을 받는다.
2. 압축을 풀어 나온 `HMocapTool` 폴더를 프로젝트의 `Plugins` 폴더에 넣는다.
3. 에디터에서 플러그인을 켜고 다시 시작한다.

배포본은 에픽 런처로 설치한 언리얼 5.8 전용이다. 소스에서 직접 빌드한 엔진에서는 쓸 수 없으니 아래 방법을 쓴다.

### 소스에서 빌드

1. 이 저장소를 프로젝트의 `Plugins/HMocapTool`에 받는다.
2. 프로젝트 파일을 다시 생성하고 에디터 타깃을 빌드한다.

C++ 프로젝트여야 한다.
