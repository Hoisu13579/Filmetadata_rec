// 시연용: NFC Tag 2 Click(MIKROE-2462, NTAG I2C plus) 메모리에
//         더미 컷 기록을 NDEF Text 레코드로 써넣는 스케치
// ESP32-C3 SuperMini, I2C: SDA=GPIO8, SCL=GPIO9 (추가 라이브러리 필요 없음)

#include <Wire.h>

const uint8_t NTAG_ADDR = 0x55;   // NTAG I2C 기본 주소 (7비트)

// 앱이 읽는 형식 그대로: 'F번호, YYYY-MM-DD HH:MM:SS' 를 줄바꿈으로 구분
const char *DUMMY_LOG =
  "F01, 2026-09-09 17:30:12\n"
  "F02, 2026-09-09 17:31:05\n"
  "F03, 2026-09-09 17:35:40";

// NTAG 메모리 블록 하나(16바이트)를 통째로 쓰기
bool writeBlock(uint8_t block, const uint8_t *data) {
  Wire.beginTransmission(NTAG_ADDR);
  Wire.write(block);            // 몇 번째 블록에 쓸지
  Wire.write(data, 16);         // 16바이트 데이터
  bool ok = (Wire.endTransmission() == 0);
  delay(5);                     // EEPROM 기록 시간 기다리기 (약 4ms)
  return ok;
}

// 로그 문자열 → NDEF 바이트 배열 만들기. 만든 길이를 돌려줌
int buildNdef(const char *text, uint8_t *buf) {
  int textLen = strlen(text);
  int payloadLen = 3 + textLen;  // 상태바이트 1 + 'en' 2 + 본문
  int recordLen = 4 + payloadLen;
  int i = 0;

  buf[i++] = 0x03;               // TLV 태그: "여기부터 NDEF 메시지"
  buf[i++] = recordLen;          // TLV 길이

  buf[i++] = 0xD1;               // 레코드 헤더: 처음이자 마지막, 짧은 레코드, Well-known 타입
  buf[i++] = 0x01;               // 타입 길이: 1글자
  buf[i++] = payloadLen;         // payload 길이
  buf[i++] = 'T';                // 타입: Text 레코드

  buf[i++] = 0x02;               // 상태 바이트: UTF-8, 언어코드 길이 2
  buf[i++] = 'e';
  buf[i++] = 'n';
  memcpy(&buf[i], text, textLen);
  i += textLen;

  buf[i++] = 0xFE;               // TLV 끝 표시
  return i;
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Wire.begin(8, 9);  // SDA, SCL

  uint8_t ndef[256] = {0};       // 0으로 채워두면 마지막 블록 남는 칸도 0
  int len = buildNdef(DUMMY_LOG, ndef);
  if (len > 250) {
    Serial.println("로그가 너무 김: 줄 수를 줄이세요");
    return;
  }

  // 블록 0은 칩 설정 영역이라 건드리지 않고, 블록 1부터 사용자 데이터
  int blocks = (len + 15) / 16;  // 16바이트 단위로 올림
  for (int b = 0; b < blocks; b++) {
    if (!writeBlock(1 + b, &ndef[b * 16])) {
      Serial.print("쓰기 실패: 블록 ");
      Serial.println(1 + b);
      return;
    }
  }

  Serial.print("NDEF 기록 완료: ");
  Serial.print(len);
  Serial.println("바이트. 이제 폰으로 태그를 읽어보세요.");
}

void loop() {
  // 할 일 없음: 한 번 써두면 전원을 꺼도 칩 안에 남아 있음
}
