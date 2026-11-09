# clearMax v1.16 릴리스 노트

### 🚀 신규 기능 및 개선 사항
- **스마트폰(휴대폰 / MTP) 실시간 감지 및 통합 열거**:
  - WPD(Windows Portable Devices) API 연동으로 USB 연결된 스마트폰(Galaxy S23+, iPhone 등)을 감지하여 파쇄/복구/공간분석 드라이브 목록에 명확히 표출 (`[휴대폰] FriendlyName (Model, MTP)`).
- **USB 외장 드라이브 및 SD 카드 정밀 분류**:
  - `IOCTL_STORAGE_QUERY_PROPERTY` 조회를 통해 USB 외장 HDD/SSD, USB 플래시 메모리, SD 카드를 100% 정밀 식별하고 볼륨명, 파일시스템(NTFS, FAT32, exFAT), 총 용량을 함께 상세 표시.
- **실시간 핫플러그(장치 연결/분리) 자동 동기화 (`WM_DEVICECHANGE`)**:
  - 프로그램이 실행 중인 상태에서 USB나 스마트폰을 꽂거나 뽑아도 파쇄/복구/공간분석 3개 탭의 드라이브 목록이 실시간으로 즉시 자동 새로고침.
- **USB 파일 복구 모듈 완결성 향상**:
  - 복구 탭에서 파일 복구 시 FAT32/exFAT 파일시스템 Fallback(`FatRecovery::recoverFile`)을 완벽 연동하여 USB 메모리 복구 지원.
- **안전 경로 파서 및 버퍼 오버플로우 방어**:
  - 상세 라벨 환경에서도 백엔드 엔진에 순수 드라이브 경로만 안전하게 분기 전달하며 버퍼를 256자 이상으로 방어.
